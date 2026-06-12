// High-cut engine — Milestone H-2: first plume-driven frame, in-process, alongside
// the live recompiled game.
//
// Stands up a plume D3D12 device + its own Win32 window + swapchain (lazy, on the
// first guest Present), and renders an animated CLEAR into it once per guest frame.
// Driven by the guest Present hook (sub_827F1C88) in d3d9_resources.cpp via
// HighcutPlumeTick(), so the plume swapchain advances in lock-step with the game's
// real present cadence. This is the H-2 "start": prove plume links into the recomp,
// runs a device in-process, and is driven by the intercepted Present — without yet
// taking over the game's own presentation (we still pass through to the real swap).
//
// Env-gated by NHL_HIGHCUT_PRESENT (independent of the NHL_HIGHCUT observation gate);
// unset => HighcutPlumeTick() is a no-op and the build/run is unchanged. WIN32 app
// has no console: log via REXLOG -> logs/nhllegacy_*.log.
//
// API usage mirrors the validated standalone gpu/smoke/main.cpp.
//
// Threading: all plume Win32 + D3D12 work runs on a DEDICATED thread, NOT the guest
// present thread. The guest Present hook only bumps an atomic frame counter (cheap,
// never blocks). This matters: the first guest Present fires during the game's GPU
// init (creating a D3D12 device on the guest thread then collided with rexglue's
// concurrent D3D12 setup and reset its device). The plume thread instead waits until
// the game is steadily presenting before it touches the GPU, then renders exactly one
// frame per guest Present — driven by Present, but fully decoupled from guest threads.
//
// Backend: VULKAN by default. rexglue owns a live, actively-submitting D3D12 device;
// standing up a SECOND D3D12 device + swapchain in the same process triggers a GPU
// device reset (TDR, DXGI_ERROR_DEVICE_RESET 0x887A0007) on this driver — verified
// live. A plume *Vulkan* device sits on a separate driver stack and coexists with the
// D3D12 game cleanly (the standard overlay arrangement). Set NHL_HIGHCUT_PRESENT=d3d12
// to force the (conflicting) D3D12 backend for testing; any other value => Vulkan.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <rex/logging.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "plume_render_interface.h"
#include "plume_render_interface_builders.h"  // C-3a: descriptor-set / pipeline-layout builders

// High-cut C-1: embedded bring-up triangle shaders (compiled by plume's shader cmake into
// ${CMAKE_BINARY_DIR}/shaders, which is on the include path). SPIRV for the Vulkan backend
// (the in-process default), DXIL for the d3d12 test backend.
#include "shaders/triangleVert.hlsl.spirv.h"
#include "shaders/triangleFrag.hlsl.spirv.h"
#include "shaders/triangleVert.hlsl.dxil.h"
#include "shaders/triangleFrag.hlsl.dxil.h"
// C-3a: zero-input solid-color PS to pair with the translated Xenos VS for pipeline creation.
#include "shaders/solidFrag.hlsl.spirv.h"

namespace plume {
std::unique_ptr<RenderInterface> CreateD3D12Interface();
std::unique_ptr<RenderInterface> CreateVulkanInterface();
}

using namespace plume;

namespace {

const char* g_gate = std::getenv("NHL_HIGHCUT_PRESENT");
const bool g_enabled = g_gate != nullptr;
// rexglue is D3D12; default plume to Vulkan to avoid the two-D3D12-device TDR.
const bool g_useD3D12 = g_gate != nullptr && std::strcmp(g_gate, "d3d12") == 0;

constexpr uint32_t kBufferCount = 2;
constexpr uint32_t kWidth = 1280;   // the game's logical size (proven in H-1)
constexpr uint32_t kHeight = 720;
constexpr RenderFormat kSwapFormat = RenderFormat::B8G8R8A8_UNORM;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CLOSE:   DestroyWindow(hwnd); return 0;
        case WM_DESTROY: PostQuitMessage(0);  return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

struct PlumeCtx {
    HWND hwnd = nullptr;
    std::unique_ptr<RenderInterface> ri;
    std::unique_ptr<RenderDevice> device;
    std::unique_ptr<RenderCommandQueue> queue;
    std::unique_ptr<RenderCommandList> cmd;
    std::unique_ptr<RenderCommandFence> fence;
    std::unique_ptr<RenderSwapChain> swap;
    std::unique_ptr<RenderCommandSemaphore> acquireSem;
    std::vector<std::unique_ptr<RenderCommandSemaphore>> releaseSems;
    std::vector<std::unique_ptr<RenderFramebuffer>> fbs;
    // C-1 geometry: minimal triangle pipeline + vertex buffer (proves the plume render path).
    std::unique_ptr<RenderPipelineLayout> pipelineLayout;
    std::unique_ptr<RenderShader> vs;
    std::unique_ptr<RenderShader> ps;
    std::unique_ptr<RenderPipeline> pipeline;
    std::unique_ptr<RenderBuffer> vbuf;
    RenderVertexBufferView vbView;
    RenderInputSlot inputSlot;
    // C-2: shader module created from a translated Xenos VS (the shader bridge), once.
    std::unique_ptr<RenderShader> xlatVS;
    bool xlatDone = false;
    // C-3a: a real graphics pipeline built from the translated VS + solid PS + the VS's
    // reflected descriptor layout (set0: shared-memory SSBO; set1: system/bool/fetch UBOs).
    std::unique_ptr<RenderShader> xlatPS;
    std::unique_ptr<RenderPipelineLayout> xlatLayout;
    std::unique_ptr<RenderPipeline> xlatPipeline;
    uint64_t frame = 0;
};

// Guest Present hook bumps this; the plume thread renders one frame per increment.
std::atomic<uint64_t> g_guestFrames{0};
std::atomic<bool> g_threadLaunched{false};

// C-2 shader bridge: the CP thread (RenderBetaOwnedDraw / P-3) translates a real Xenos vertex
// shader to SPIR-V and publishes the bytes here; the plume Vulkan thread picks them up and
// creates a shader module from them — proving the translate->plume bridge end to end, and that
// a real Vulkan driver accepts the ported translator's output. Mutex-guarded one-shot handoff.
std::mutex g_xlatMutex;
std::vector<uint8_t> g_xlatVS;
bool g_xlatVSReady = false;
// Don't touch D3D12 until the game is steadily presenting (past GPU init), or the
// second device creation races rexglue's init and resets its device.
constexpr uint64_t kInitAfterGuestFrames = 30;

HWND CreatePlumeWindow() {
    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "NhlHighcutPlumeWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);
    RECT r = {0, 0, (LONG)kWidth, (LONG)kHeight};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    const char* title = g_useD3D12 ? "NHL high-cut (plume D3D12)" : "NHL high-cut (plume Vulkan)";
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, title,
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                                wc.hInstance, nullptr);
    if (hwnd) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    return hwnd;
}

void CreateFramebuffers(PlumeCtx& c) {
    c.fbs.clear();
    for (uint32_t i = 0; i < c.swap->getTextureCount(); ++i) {
        const RenderTexture* color = c.swap->getTexture(i);
        RenderFramebufferDesc d;
        d.colorAttachments = &color;
        d.colorAttachmentsCount = 1;
        d.depthAttachment = nullptr;
        c.fbs.push_back(c.device->createFramebuffer(d));
    }
}

// C-1: build the triangle pipeline (shaders + input layout) and its vertex buffer. This is
// the smallest geometry render that proves plume's pipeline/vertex-buffer/draw path works in
// the in-process Vulkan setup — the foundation the real CP-decode -> plume bridge builds on.
bool CreateTriangle(PlumeCtx& c) {
    RenderPipelineLayoutDesc layoutDesc;
    layoutDesc.allowInputLayout = true;
    c.pipelineLayout = c.device->createPipelineLayout(layoutDesc);

    const RenderShaderFormat fmt = c.ri->getCapabilities().shaderFormat;
    if (fmt == RenderShaderFormat::SPIRV) {
        c.vs = c.device->createShader(triangleVertBlobSPIRV, sizeof(triangleVertBlobSPIRV), "VSMain", fmt);
        c.ps = c.device->createShader(triangleFragBlobSPIRV, sizeof(triangleFragBlobSPIRV), "PSMain", fmt);
    } else if (fmt == RenderShaderFormat::DXIL) {
        c.vs = c.device->createShader(triangleVertBlobDXIL, sizeof(triangleVertBlobDXIL), "VSMain", fmt);
        c.ps = c.device->createShader(triangleFragBlobDXIL, sizeof(triangleFragBlobDXIL), "PSMain", fmt);
    } else {
        REXLOG_ERROR("[highcut-plume] unsupported shader format {}", int(fmt));
        return false;
    }

    // Vertex layout: POSITION (float3) + COLOR (float4), matching triangle.hlsl.
    c.inputSlot = RenderInputSlot(0, sizeof(float) * 7);
    const RenderInputElement inputElements[] = {
        RenderInputElement("POSITION", 0, 0, RenderFormat::R32G32B32_FLOAT, 0, 0),
        RenderInputElement("COLOR", 0, 1, RenderFormat::R32G32B32A32_FLOAT, 0, sizeof(float) * 3),
    };
    RenderGraphicsPipelineDesc pd;
    pd.inputSlots = &c.inputSlot;
    pd.inputSlotsCount = 1;
    pd.inputElements = inputElements;
    pd.inputElementsCount = 2;
    pd.pipelineLayout = c.pipelineLayout.get();
    pd.vertexShader = c.vs.get();
    pd.pixelShader = c.ps.get();
    pd.renderTargetFormat[0] = kSwapFormat;
    pd.renderTargetBlend[0] = RenderBlendDesc::Copy();
    pd.renderTargetCount = 1;
    pd.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
    c.pipeline = c.device->createGraphicsPipeline(pd);
    if (!c.pipeline) { REXLOG_ERROR("[highcut-plume] triangle pipeline create failed"); return false; }

    // A large centered triangle in NDC, vertex-colored, so it's unmistakable on screen.
    const float verts[] = {
        0.0f,  0.6f, 0.0f,   1.0f, 0.0f, 0.0f, 1.0f,  // top    (red)
       -0.6f, -0.6f, 0.0f,   0.0f, 1.0f, 0.0f, 1.0f,  // left   (green)
        0.6f, -0.6f, 0.0f,   0.0f, 0.0f, 1.0f, 1.0f,  // right  (blue)
    };
    c.vbuf = c.device->createBuffer(RenderBufferDesc::VertexBuffer(sizeof(verts), RenderHeapType::UPLOAD));
    if (!c.vbuf) { REXLOG_ERROR("[highcut-plume] triangle vbuf create failed"); return false; }
    void* p = c.vbuf->map();
    std::memcpy(p, verts, sizeof(verts));
    c.vbuf->unmap();
    c.vbView = RenderVertexBufferView(c.vbuf.get(), sizeof(verts));
    REXLOG_INFO("[highcut-plume] C-1 triangle pipeline + vertex buffer ready (fmt={})", int(fmt));
    return true;
}

bool Init(PlumeCtx& c) {
    const char* backend = g_useD3D12 ? "D3D12" : "Vulkan";
    c.hwnd = CreatePlumeWindow();
    if (!c.hwnd) { REXLOG_ERROR("[highcut-plume] window creation failed"); return false; }
    c.ri = g_useD3D12 ? CreateD3D12Interface() : CreateVulkanInterface();
    if (!c.ri) { REXLOG_ERROR("[highcut-plume] Create{}Interface failed", backend); return false; }
    REXLOG_INFO("[highcut-plume] backend = {}", backend);
    c.device = c.ri->createDevice();
    if (!c.device) { REXLOG_ERROR("[highcut-plume] createDevice failed"); return false; }
    c.queue = c.device->createCommandQueue(RenderCommandListType::DIRECT);
    c.fence = c.device->createCommandFence();
    c.swap = c.queue->createSwapChain(RenderSwapChainDesc(c.hwnd, kSwapFormat, kBufferCount));
    c.swap->resize();
    c.cmd = c.queue->createCommandList();
    c.acquireSem = c.device->createCommandSemaphore();
    CreateFramebuffers(c);
    if (!CreateTriangle(c)) {
        REXLOG_ERROR("[highcut-plume] triangle setup failed — falling back to clear-only");
    }
    REXLOG_INFO("[highcut-plume] device + swapchain ready: {}x{}, {} buffers",
                c.swap->getWidth(), c.swap->getHeight(), c.swap->getTextureCount());
    return true;
}

void RenderClear(PlumeCtx& c) {
    uint32_t idx = 0;
    if (c.swap->isEmpty() || !c.swap->acquireTexture(c.acquireSem.get(), &idx)) {
        c.swap->resize();
        CreateFramebuffers(c);
        return;
    }

    c.cmd->begin();
    RenderTexture* tex = c.swap->getTexture(idx);
    c.cmd->barriers(RenderBarrierStage::GRAPHICS,
                    RenderTextureBarrier(tex, RenderTextureLayout::COLOR_WRITE));
    c.cmd->setFramebuffer(c.fbs[idx].get());

    const uint32_t w = c.swap->getWidth(), h = c.swap->getHeight();
    c.cmd->setViewports(RenderViewport(0.0f, 0.0f, float(w), float(h)));
    c.cmd->setScissors(RenderRect(0, 0, w, h));

    // Animated clear so the plume window is obviously alive and synced to the game.
    const float t = (c.frame % 120) / 120.0f;
    c.cmd->clearColor(0, RenderColor(0.1f, t, 0.2f + 0.3f * t, 1.0f));

    // C-2: if the CP thread published a translated Xenos VS, create a plume shader module from
    // it (once). createShader runs the SPIR-V through vkCreateShaderModule on a real Vulkan
    // driver — the shader bridge proof. SPIRV backend only (the translator emits SPIR-V).
    if (!c.xlatDone) {
        std::vector<uint8_t> spv;
        {
            std::lock_guard<std::mutex> lk(g_xlatMutex);
            if (g_xlatVSReady) { spv.swap(g_xlatVS); g_xlatVSReady = false; }
        }
        // Fallback: load the P-3 dump from cwd. Lets C-2 run present-only (no beta takeover,
        // which fires the translation during early boot before plume is up) after a P-3 run.
        if (spv.empty()) {
            if (FILE* f = std::fopen("highcut_p3_vs.spv", "rb")) {
                std::fseek(f, 0, SEEK_END);
                long sz = std::ftell(f);
                std::fseek(f, 0, SEEK_SET);
                if (sz > 0) { spv.resize(size_t(sz)); if (std::fread(spv.data(), 1, size_t(sz), f) != size_t(sz)) spv.clear(); }
                std::fclose(f);
                if (!spv.empty()) REXLOG_INFO("[highcut-C2] loaded {} bytes from highcut_p3_vs.spv (disk fallback)", uint32_t(spv.size()));
            }
        }
        if (!spv.empty()) {
            c.xlatDone = true;
            const RenderShaderFormat fmt = c.ri->getCapabilities().shaderFormat;
            if (fmt == RenderShaderFormat::SPIRV) {
                uint32_t magic = spv.size() >= 4 ? *reinterpret_cast<const uint32_t*>(spv.data()) : 0u;
                c.xlatVS = c.device->createShader(spv.data(), spv.size(), "main", fmt);
                REXLOG_INFO("[highcut-C2] translated Xenos VS -> plume shader module: {} "
                            "({} bytes, magic=0x{:08X}). Any vkCreateShaderModule error would be on stderr.",
                            c.xlatVS ? "module object created" : "null", uint32_t(spv.size()), magic);

                // C-3a: build a real graphics pipeline from the translated VS + a solid-color PS,
                // with a pipeline layout matching the VS's reflected descriptor interface:
                //   set 0 -> binding 0: xe_shared_memory (StorageBuffer / byte-address buffer)
                //   set 1 -> bindings 0,3,4: system / bool-loop / fetch constants (UBOs)
                // Vulkan validates the layout, the float-controls capabilities, and VS<->PS stage
                // linking at pipeline creation — so success proves the translated shader is fully
                // pipeline-compilable on a real device (C-3b then binds decoded data + draws).
                // C-3a: build a graphics pipeline from the translated VS + solid PS. GATED by
                // NHL_HIGHCUT_C3 (the driver can crash compiling the pipeline, so keep it off the
                // stable C-2/present path). Value selects the pipeline layout for isolation:
                //   "empty" -> no descriptor sets (tests whether the SPIR-V compile alone crashes)
                //   else    -> the real layout (set0:SSBO@0, set1:CBV@0,3,4) matching the VS.
                const char* c3gate = std::getenv("NHL_HIGHCUT_C3");
                if (c.xlatVS && c3gate) {
                    const bool emptyLayout = std::strcmp(c3gate, "empty") == 0;
                    c.xlatPS = c.device->createShader(solidFragBlobSPIRV, sizeof(solidFragBlobSPIRV), "PSMain", fmt);
                    REXLOG_INFO("[highcut-C3a] step: solid PS module {} (layout mode='{}')",
                                c.xlatPS ? "ok" : "null", emptyLayout ? "empty" : "full");

                    RenderDescriptorSetBuilder set0, set1;
                    RenderPipelineLayoutBuilder lb;
                    lb.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
                    if (!emptyLayout) {
                        set0.begin(); set0.addByteAddressBuffer(0); set0.end();           // shared memory
                        set1.begin();                                                     // constants (gaps 1,2)
                        set1.addConstantBuffer(0); set1.addConstantBuffer(3); set1.addConstantBuffer(4);
                        set1.end();
                        lb.addDescriptorSet(set0);  // -> set 0
                        lb.addDescriptorSet(set1);  // -> set 1
                    }
                    lb.end();
                    c.xlatLayout = lb.create(c.device.get());
                    REXLOG_INFO("[highcut-C3a] step: pipeline layout {}", c.xlatLayout ? "ok" : "null");

                    if (c.xlatLayout && c.xlatPS) {
                        REXLOG_INFO("[highcut-C3a] step: creating graphics pipeline...");
                        // The Xenos VS fetches vertices from the SSBO via gl_VertexIndex — NO IA
                        // vertex input, so the pipeline has no input slots/elements.
                        RenderGraphicsPipelineDesc pd;
                        pd.pipelineLayout = c.xlatLayout.get();
                        pd.vertexShader = c.xlatVS.get();
                        pd.pixelShader = c.xlatPS.get();
                        pd.renderTargetFormat[0] = kSwapFormat;
                        pd.renderTargetBlend[0] = RenderBlendDesc::Copy();
                        pd.renderTargetCount = 1;
                        pd.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
                        c.xlatPipeline = c.device->createGraphicsPipeline(pd);
                        REXLOG_INFO("[highcut-C3a] graphics pipeline (layout={}): {} "
                                    "— any pipeline/driver error is on stderr.",
                                    emptyLayout ? "empty" : "set0=SSBO@0,set1=CBV@0,3,4",
                                    c.xlatPipeline ? "CREATED" : "FAILED (null)");
                    } else {
                        REXLOG_ERROR("[highcut-C3a] pipeline layout or solid PS creation failed");
                    }
                }
            } else {
                REXLOG_INFO("[highcut-C2] translated VS available but plume backend is not SPIRV "
                            "(fmt={}) — skipping (d3d12 cannot consume SPIR-V)", int(fmt));
            }
        }
    }

    // C-1: draw the bring-up triangle over the clear — proves plume rasterizes real geometry
    // (vertex buffer + pipeline + draw), not just clears. The vertex-colored triangle is the
    // readable signal that the plume render path is live before we feed it decoded guest draws.
    if (c.pipeline && c.vbuf) {
        c.cmd->setGraphicsPipelineLayout(c.pipelineLayout.get());
        c.cmd->setPipeline(c.pipeline.get());
        c.cmd->setVertexBuffers(0, &c.vbView, 1, &c.inputSlot);
        c.cmd->drawInstanced(3, 1, 0, 0);
    }

    c.cmd->barriers(RenderBarrierStage::NONE,
                    RenderTextureBarrier(tex, RenderTextureLayout::PRESENT));
    c.cmd->end();

    while (c.releaseSems.size() < c.swap->getTextureCount())
        c.releaseSems.emplace_back(c.device->createCommandSemaphore());

    const RenderCommandList* cl = c.cmd.get();
    RenderCommandSemaphore* wait = c.acquireSem.get();
    RenderCommandSemaphore* signal = c.releaseSems[idx].get();
    c.queue->executeCommandLists(&cl, 1, &wait, 1, &signal, 1, c.fence.get());
    c.swap->present(idx, &signal, 1);
    c.queue->waitForCommandFence(c.fence.get());
}

// Dedicated plume thread: owns the window + D3D12 device + swapchain and its message
// pump. Waits for the game to be steadily presenting, inits plume, then renders one
// clear per guest Present (tracked via g_guestFrames). Decoupled from guest threads.
void PlumeThreadMain() {
    while (g_guestFrames.load(std::memory_order_relaxed) < kInitAfterGuestFrames)
        Sleep(20);

    PlumeCtx c;
    if (!Init(c)) {
        REXLOG_ERROR("[highcut-plume] init failed — plume present disabled");
        return;
    }

    uint64_t lastRendered = 0;
    for (;;) {
        MSG m;
        while (PeekMessageA(&m, c.hwnd, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessage(&m);
        }
        const uint64_t gf = g_guestFrames.load(std::memory_order_relaxed);
        if (gf != lastRendered) {
            c.frame = gf;
            RenderClear(c);
            lastRendered = gf;
            if (gf == kInitAfterGuestFrames + 1 || gf % 120 == 0)
                REXLOG_INFO("[highcut-plume] presented plume frame for guest Present {}", gf);
        } else {
            Sleep(1);  // idle until the next guest Present advances the counter
        }
    }
}

}  // namespace

// Called once per guest Present (sub_827F1C88) from d3d9_resources.cpp. Self-gating:
// a no-op unless NHL_HIGHCUT_PRESENT is set. Bumps the guest-frame counter and lazily
// launches the dedicated plume thread on the first call. Never blocks the guest.
extern "C" void HighcutPlumeTick() {
    if (!g_enabled) return;
    g_guestFrames.fetch_add(1, std::memory_order_relaxed);
    bool expected = false;
    if (g_threadLaunched.compare_exchange_strong(expected, true))
        std::thread(PlumeThreadMain).detach();
}

// C-2: called from the CP thread (RenderBetaOwnedDraw / P-3) to hand the just-translated Xenos
// vertex shader's SPIR-V to the plume Vulkan thread, which creates a shader module from it. A
// no-op-ish copy if the plume thread isn't running (nobody consumes it). Thread-safe.
extern "C" void HighcutPublishTranslatedVS(const uint8_t* data, size_t size) {
    if (!data || size == 0) return;
    std::lock_guard<std::mutex> lk(g_xlatMutex);
    g_xlatVS.assign(data, data + size);
    g_xlatVSReady = true;
}
