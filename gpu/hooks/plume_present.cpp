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
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "plume_render_interface.h"

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
    uint64_t frame = 0;
};

// Guest Present hook bumps this; the plume thread renders one frame per increment.
std::atomic<uint64_t> g_guestFrames{0};
std::atomic<bool> g_threadLaunched{false};
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
