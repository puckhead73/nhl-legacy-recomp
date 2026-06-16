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

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "plume_render_interface.h"
#include "plume_render_interface_builders.h"  // C-3a: descriptor-set / pipeline-layout builders
#include "highcut_draw_packet.h"              // C-3b.2: decoded-draw data bridge

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

// C-5j: forward decl (defined below near the xenos helpers) — opt-out for the signed-BC5 SNORM fix.
bool NhlBc5NoSnorm();
// C-5c: the shared depth+stencil attachment format. Every framebuffer carries one, so EVERY pipeline
// must declare this depthTargetFormat (else its renderpass is incompatible with the framebuffer's).
constexpr RenderFormat kDepthFormat = RenderFormat::D32_FLOAT_S8_UINT;

// ---------------------------------------------------------------------------------------------
// C-5g debug tooling: a minimal, dependency-free PNG writer + a .spv-from-disk loader. These let a
// headless replay (1) dump its final swapchain image to a file so the result is verifiable WITHOUT a
// human eyeballing the window, and (2) swap one draw's pixel shader for an instrumented .spv (to read
// the number-composition layer directly). Both are env-gated and one-shot — zero cost when unused.
// ---------------------------------------------------------------------------------------------

// CRC32 (PNG/zlib polynomial) — tiny table-free implementation; only used on a one-shot debug dump.
inline uint32_t Crc32(const uint8_t* p, size_t n, uint32_t crc = 0xFFFFFFFFu) {
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1) + 1));
    }
    return crc;
}

// Write an RGBA8 image as a PNG using STORED (uncompressed) DEFLATE blocks — no zlib dependency.
// src is `rowPitch`-strided; `bgra` swaps R/B (the swapchain is B8G8R8A8). Returns true on success.
bool WriteImagePNG(const char* path, uint32_t w, uint32_t h, uint32_t rowPitch,
                   const uint8_t* src, bool bgra) {
    // Raw filtered scanlines: each row = filter byte (0) + w*4 RGBA bytes.
    std::vector<uint8_t> raw;
    raw.reserve(size_t(h) * (1 + size_t(w) * 4));
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* row = src + size_t(y) * rowPitch;
        raw.push_back(0);
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* px = row + size_t(x) * 4;
            uint8_t r = px[0], g = px[1], b = px[2];
            if (bgra) { uint8_t t = r; r = b; b = t; }
            // Force opaque: this is a presented-image screenshot — the swapchain's alpha is meaningless
            // (often 0) and would make the whole dump look white when viewed over a white background.
            raw.push_back(r); raw.push_back(g); raw.push_back(b); raw.push_back(255);
        }
    }
    // zlib stream = 2-byte header + STORED deflate blocks + adler32 (over the raw bytes).
    std::vector<uint8_t> z;
    z.push_back(0x78); z.push_back(0x01);
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t chunk = std::min<size_t>(65535, raw.size() - pos);
        bool final = (pos + chunk >= raw.size());
        z.push_back(final ? 1 : 0);
        z.push_back(uint8_t(chunk & 0xFF)); z.push_back(uint8_t((chunk >> 8) & 0xFF));
        uint16_t nlen = uint16_t(~uint16_t(chunk));
        z.push_back(uint8_t(nlen & 0xFF)); z.push_back(uint8_t((nlen >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + chunk);
        pos += chunk;
    }
    uint32_t s1 = 1, s2 = 0;
    for (uint8_t b : raw) { s1 = (s1 + b) % 65521; s2 = (s2 + s1) % 65521; }
    uint32_t adler = (s2 << 16) | s1;
    z.push_back(uint8_t(adler >> 24)); z.push_back(uint8_t(adler >> 16));
    z.push_back(uint8_t(adler >> 8));  z.push_back(uint8_t(adler));

    auto be32 = [](std::vector<uint8_t>& v, uint32_t x) {
        v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16));
        v.push_back(uint8_t(x >> 8));  v.push_back(uint8_t(x));
    };
    auto chunkOut = [&](std::vector<uint8_t>& out, const char tag[4], const std::vector<uint8_t>& data) {
        be32(out, uint32_t(data.size()));
        std::vector<uint8_t> td(tag, tag + 4);
        td.insert(td.end(), data.begin(), data.end());
        out.insert(out.end(), td.begin(), td.end());
        be32(out, Crc32(td.data(), td.size()) ^ 0xFFFFFFFFu);
    };
    std::vector<uint8_t> ihdr;
    be32(ihdr, w); be32(ihdr, h);
    ihdr.push_back(8); ihdr.push_back(6); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    chunkOut(png, "IHDR", ihdr);
    chunkOut(png, "IDAT", z);
    chunkOut(png, "IEND", {});
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    std::fclose(f);
    return ok;
}

// NHL_HIGHCUT_DEBUG_PS=<path.spv> + NHL_HIGHCUT_DEBUG_PS_DRAW=<idx>: replace the pixel shader of the
// given captured draw with an instrumented SPIR-V from disk. Returns the file bytes (empty if N/A).
std::vector<uint8_t> MaybeLoadDebugPS(uint32_t drawIndex) {
    static const char* dbgPath = std::getenv("NHL_HIGHCUT_DEBUG_PS");
    static const char* dbgDrawS = std::getenv("NHL_HIGHCUT_DEBUG_PS_DRAW");
    std::vector<uint8_t> out;
    if (!dbgPath || !dbgDrawS) return out;
    if (uint32_t(std::strtoul(dbgDrawS, nullptr, 10)) != drawIndex) return out;
    if (FILE* f = std::fopen(dbgPath, "rb")) {
        std::fseek(f, 0, SEEK_END); long sz = std::ftell(f); std::fseek(f, 0, SEEK_SET);
        if (sz > 0) { out.resize(size_t(sz)); if (std::fread(out.data(), 1, size_t(sz), f) != size_t(sz)) out.clear(); }
        std::fclose(f);
    }
    if (!out.empty())
        REXLOG_INFO("[highcut-C5g] DEBUG_PS: draw {} uses instrumented PS '{}' ({} bytes)",
                    drawIndex, dbgPath, uint32_t(out.size()));
    else
        REXLOG_INFO("[highcut-C5g] DEBUG_PS: draw {} requested '{}' but file is empty/missing",
                    drawIndex, dbgPath);
    return out;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CLOSE:   DestroyWindow(hwnd); return 0;
        case WM_DESTROY: PostQuitMessage(0);  return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// C-5a: a fully self-contained renderable draw — owns ALL its plume resources (its own VS+PS,
// constant/shared/float buffers, textures+sampler, pipeline layout + descriptor sets, and the
// graphics pipeline built with this draw's blend + topology). A captured frame = a vector of these,
// replayed in order into one flat RT with per-draw blend. (The C-4 single-draw path stays separate.)
struct RenderableDraw {
    // by-ID: shaders/pipeline/layout/textures are SHARED (cached in PlumeCtx, reused across draws+frames
    // so a 4000-draw scene doesn't recreate every GPU object every frame). shared_ptr keeps the render
    // loop's `.get()` calls unchanged; the per-draw buffers/descriptor-sets/samplers stay unique.
    std::shared_ptr<RenderShader> vs, ps;
    std::unique_ptr<RenderBuffer> sysBuf, boolBuf, fetchBuf, sharedBuf, vsFloatBuf, psFloatBuf;
    std::vector<std::shared_ptr<RenderTexture>> textures;
    std::vector<std::shared_ptr<RenderTextureView>> texViews;
    // C-5f: ONE sampler per PS sampler binding (was a single LINEAR+CLAMP for all). The jersey
    // nameplate-layout map is POINT-sampled by the guest; LINEAR-blending it broke the back number.
    std::vector<std::unique_ptr<RenderSampler>> samplers;
    // C-5d.3: VERTEX-shader textures (skinning bone palette) -> descriptor set 2.
    std::vector<std::shared_ptr<RenderTexture>> vsTextures;
    std::vector<std::shared_ptr<RenderTextureView>> vsTexViews;
    std::vector<std::unique_ptr<RenderSampler>> vsSamplers;  // C-5f: per-binding (was single)
    std::shared_ptr<RenderPipelineLayout> layout;
    std::unique_ptr<RenderDescriptorSet> set0, set1, set2, set3;
    std::shared_ptr<RenderPipeline> pipeline;
    std::unique_ptr<RenderBuffer> indexBuf;  // quad-list expansion (C-5a.1) OR kGuestDMA (C-5d)
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;  // >0 => drawIndexedInstanced (quad expand / DMA); else drawInstanced
    bool indexU32 = true;     // index buffer element size: true=R32_UINT (quad expand), false=R16_UINT
    bool textured = false;  // has set3 (textures+samplers) bound
    int32_t scLeft = 0, scTop = 0, scRight = 0, scBottom = 0;  // C-5b: per-draw clip (RT px)
    // C-5d.2: the guest render surface this draw targets (v6 packet). Draws are bucketed by this so a
    // non-primary surface (the 384^2 RTT pass, the 320x180 stencil-mask passes) renders into its OWN
    // offscreen RT instead of the swapchain — keeping its depth/stencil writes off the main scene.
    uint32_t surfDepthBase = 0, surfPitch = 0, surfMsaa = 0;
    float vpW = 0, vpH = 0;  // C-5h: per-draw viewport, for full-res primary-surface selection
    // C-5d.3 composition: PS texture slots whose fetch_base_addr matches a resolve dest_addr. At load
    // time (after the source surface RTs exist) these set3 slots are re-pointed at the source surface's
    // sampleable color/depth view, so the draw samples our rendered shadow map / reflection RTT.
    struct HostCopyBind { uint32_t slot; uint64_t srcKey; bool isDepth; };
    std::vector<HostCopyBind> hostCopy;
    // C-5n HUD overlay: a draw that samples the full-screen scene COLOR resolve (the game's final
    // composite/grade pass re-reads the rendered scene to tone-map it). `sceneGradeSkip` marks the
    // OPAQUE (One/Zero) ones that would overwrite our clean primary render — we skip those and keep our
    // scene, but still replay the rest of the overlay surface (stencil masks + the HUD elements:
    // scorebug, change-lines) so the HUD composites on top.
    bool samplesFullscreenSceneColor = false;
    bool sceneGradeSkip = false;
};

// C-5d.2: pack the v6 surface tuple into one key. color_base is always 0 in this title, so it's
// omitted; (depth_base, pitch, msaa) distinguishes every guest surface seen (640 main passes, the
// 800-pitch 384^2 RTT pass, the 320-pitch mask passes).
static inline uint64_t SurfaceKey(uint32_t depthBase, uint32_t pitch, uint32_t msaa) {
    return (uint64_t(depthBase) << 32) | (uint64_t(pitch) << 8) | (msaa & 0xFF);
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
    // C-5c: a single shared depth+stencil target (D32_FLOAT_S8_UINT) attached to every framebuffer,
    // so 3D draws get correct occlusion and stencil-masked draws behave. Recreated with the swapchain.
    std::unique_ptr<RenderTexture> depthTex;
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
    // C-3b: the VS's descriptor buffers + sets (set0 shared-memory SSBO; set1 system/bool/fetch
    // UBOs) so the translated VS can be drawn. C-3b.1 zero-fills them (proves bind+draw mechanics
    // validation-clean); C-3b.2 fills real decoded data.
    std::unique_ptr<RenderBuffer> xlatSysBuf, xlatBoolBuf, xlatFetchBuf, xlatSharedBuf;
    std::unique_ptr<RenderDescriptorSet> xlatSet0, xlatSet1;
    bool xlatDrawReady = false;
    uint32_t xlatVertexCount = 3;  // C-3b.2: real vertex count from the draw packet (else 3)
    // C-3b.2 verify: host-visible UAV the solid PS atomically increments per fragment.
    std::unique_ptr<RenderBuffer> xlatCounterBuf;
    std::unique_ptr<RenderDescriptorSet> xlatSet2;
    int xlatCounterReadsLeft = 4;  // read the count for the first few frames, then stop
    // C-4: the TRANSLATED guest pixel shader + its sampled texture(s), rendered over the C-3 solid
    // draw (the solid pass still writes the frag counter as the geometry proof; this pass samples
    // the real texture for the visual proof). Built from the v2 draw packet's PS SPIR-V + textures.
    std::unique_ptr<RenderShader> xlatTexPS;
    std::unique_ptr<RenderPipelineLayout> xlatTexLayout;
    std::unique_ptr<RenderPipeline> xlatTexPipeline;
    std::unique_ptr<RenderBuffer> xlatVsFloatBuf, xlatPsFloatBuf;
    std::vector<std::unique_ptr<RenderTexture>> xlatTextures;
    std::vector<std::unique_ptr<RenderTextureView>> xlatTexViews;
    std::unique_ptr<RenderSampler> xlatSampler;
    std::unique_ptr<RenderDescriptorSet> xlatTexSet0, xlatTexSet1, xlatTexSet3;
    bool xlatTexDrawReady = false;
    // by-ID resource caches (the dense-gameplay crash fix). These OWN the GPU objects; RenderableDraw
    // holds shared_ptr copies. Persist across frames + draws so a 4000-draw scene reuses the ~hundreds of
    // unique shaders/textures/pipelines instead of recreating thousands of GPU objects every frame
    // (which exhausted memory -> crash). Keyed by the producer's resource IDs (packet vs/ps_shader_id,
    // TexturePacketDesc.tex_id) + the small bits that vary the GPU object (texture swizzle/sign; full
    // pipeline state). Never cleared during a session (device persists across swapchain resize).
    std::unordered_map<uint64_t, std::shared_ptr<RenderShader>> shaderCache;
    struct CachedTex { std::shared_ptr<RenderTexture> tex; std::shared_ptr<RenderTextureView> view; };
    std::unordered_map<uint64_t, CachedTex> texCache;
    std::unordered_map<uint64_t, std::shared_ptr<RenderPipelineLayout>> layoutCache;
    std::unordered_map<uint64_t, std::shared_ptr<RenderPipeline>> pipelineCache;
    // Step 2: raw resource bytes streamed once via the dictionary (shader SPIR-V / texture blobs), keyed
    // by id. A packet referencing an id with 0 inline bytes resolves its bytes here. Persistent.
    std::unordered_map<uint64_t, std::vector<uint8_t>> resourceBytes;
    // C-5a: the captured frame's draws, replayed in order into the swapchain with per-draw blend.
    std::vector<RenderableDraw> c5draws;
    bool c5loaded = false;
    uint64_t liveSeqSeen = 0;  // C-6: last live-fed frame seq rebuilt (0 = none yet)
    uint64_t frame = 0;
    // C-5d.2: per-surface offscreen render targets. A draw to a NON-primary guest surface renders
    // into its own flat color+depth+stencil RT (cached here, keyed by SurfaceKey) instead of the
    // swapchain, so its writes don't pollute the main scene. Sized to the swapchain (the draws' ndc
    // fills clip space regardless of the guest surface size); the content isn't composited yet
    // (that's C-5d.3 Resolve=host-copy) — the win is ISOLATION. Recreated with the swapchain.
    struct SurfaceRT {
        std::unique_ptr<RenderTexture> color, depth;
        std::unique_ptr<RenderFramebuffer> fb;
        // C-5d.3 composition: sampleable views of the color (and depth) attachments, so a later draw
        // that samples this surface's resolve dest binds OUR rendered content (Resolve=host copy).
        std::unique_ptr<RenderTextureView> colorSRV, depthSRV;
        uint32_t w = 0, h = 0;           // sized to the GUEST surface (not the swapchain), so the
                                         // consumer's [0,1] UV maps onto the full rendered content.
        uint64_t clearedFrame = ~0ull;   // last frame cleared (clear once per frame, LOAD thereafter)
    };
    std::unordered_map<uint64_t, SurfaceRT> c5surfaces;
    uint64_t c5PrimaryKey = 0;  // the ONE guest surface (depth,pitch,msaa) shown on the swapchain
    bool c5ShotDone = false;    // C-5g: NHL_HIGHCUT_C5_SHOT one-shot framebuffer dump fired
    // C-5d.3 "Resolve = host copy": a guest EDRAM resolve copies a just-finished surface into a texture
    // at dest_addr; a later draw samples that addr. Map dest_addr -> the SOURCE surface that produced it
    // (its SurfaceKey + whether the resolve was depth), loaded from highcut_resolves.bin. When a draw's
    // texture fetch_base_addr matches a dest_addr, we bind that source surface's offscreen RT attachment
    // instead of the captured stub — feeding real shadow maps / reflection RTTs into the main pass.
    struct ResolveEntry { uint64_t srcKey; bool isDepth; };
    std::unordered_map<uint32_t, ResolveEntry> resolveMap;       // dest_addr -> source surface
    std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> surfaceDims;  // SurfaceKey -> (w,h)
    std::vector<uint64_t> sampledSrcOrder;  // sampled source SurfaceKeys, first-seen order (render order)
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

// C-6 LIVE FEED: the CP thread pushes each owned draw's packet bytes (same bytes it writes to
// highcut_frame_<N>.bin) into g_liveBuild, then commits the whole frame at the guest-present boundary.
// The plume thread swaps the committed frame in and rebuilds its renderable draws — so plume renders
// the LIVE game in real time instead of replaying one captured frame from disk. Double-buffered: the CP
// fills g_liveBuild (no lock needed — single CP thread), commit moves it into g_livePending under the
// lock + bumps the seq; the plume thread reads g_livePending when the seq changes.
std::mutex g_liveMutex;
std::vector<std::vector<uint8_t>> g_liveBuild;       // CP thread: this frame's draws, in progress
std::vector<std::vector<uint8_t>> g_livePendingDraws;// committed frame ready for the plume thread
std::vector<uint8_t> g_liveBuildResolves;            // CP thread: this frame's resolve sidecar bytes
std::vector<uint8_t> g_livePendingResolves;
std::atomic<uint64_t> g_liveSeq{0};                  // bumped on each committed frame
// Step 2 RESOURCE DICTIONARY: persistent, append-only channel (NOT double-buffered / latest-wins like
// draws). The producer pushes each unique shader SPIR-V and texture blob ONCE, keyed by its resource id;
// the consumer drains it before every rebuild and caches the raw bytes (c.resourceBytes), so a draw whose
// packet carries the id but 0 inline bytes still resolves. This removes the ~130KB shader + texture blobs
// the packet used to inline per draw (≈1 GB/frame of memcpy + bridge memory at 4000+ draws → the crash).
std::mutex g_resMutex;
std::vector<std::pair<uint64_t, std::vector<uint8_t>>> g_resourcePending;
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
    c.c5surfaces.clear();  // C-5d.2: offscreen surface RTs are swapchain-sized — rebuild on resize
    // C-5c: (re)create the shared depth+stencil target to match the swapchain size, then attach it to
    // every framebuffer. Pass only the texture — plume derives the correct both-aspect (depth+stencil)
    // attachment view internally for D32_FLOAT_S8_UINT. DEPTH_TARGET flag is set by DepthTarget().
    const uint32_t dw = c.swap->getWidth(), dh = c.swap->getHeight();
    c.depthTex = c.device->createTexture(RenderTextureDesc::DepthTarget(dw, dh, kDepthFormat));
    if (!c.depthTex) REXLOG_ERROR("[highcut-plume] depth-stencil target create failed");
    for (uint32_t i = 0; i < c.swap->getTextureCount(); ++i) {
        const RenderTexture* color = c.swap->getTexture(i);
        RenderFramebufferDesc d;
        d.colorAttachments = &color;
        d.colorAttachmentsCount = 1;
        d.depthAttachment = c.depthTex.get();
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
    pd.depthTargetFormat = kDepthFormat;  // C-5c: framebuffer has depth; declare it (depth test off)
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

// C-4: build the textured-draw pipeline from the v2 draw packet — the TRANSLATED guest PIXEL
// shader plus its sampled guest texture(s) — reusing the C-3 constant/shared buffers. Renders the
// real menu element textured (over the C-3 solid+counter pass). Returns true when ready. Requires
// the C-3 buffers (xlatSharedBuf/Sys/Bool/Fetch) to already exist + be filled. Safe to call when
// the packet has no PS (returns false; the C-3 solid path is unaffected). SPIRV backend only.
bool CreateTexturedDraw(PlumeCtx& c) {
    if (!c.xlatVS || !c.xlatSharedBuf || !c.xlatSysBuf || !c.xlatBoolBuf || !c.xlatFetchBuf)
        return false;
    const RenderShaderFormat fmt = c.ri->getCapabilities().shaderFormat;
    if (fmt != RenderShaderFormat::SPIRV) return false;

    FILE* pf = std::fopen("highcut_p3_draw.bin", "rb");
    if (!pf) return false;
    nhl::highcut::DrawPacketHeader hdr{};
    if (std::fread(&hdr, 1, sizeof(hdr), pf) != sizeof(hdr) ||
        hdr.magic != nhl::highcut::kDrawPacketMagic ||
        hdr.version != nhl::highcut::kDrawPacketVersion || hdr.ps_spirv_bytes == 0 ||
        hdr.texture_count == 0) {
        std::fclose(pf);
        return false;  // not a C-4 packet (VS-only / older version) — leave the C-3 path alone
    }
    // Skip fetch/sys/shared (already in the C-3 buffers); then read the rest in packet order.
    std::fseek(pf, long(hdr.fetch_bytes + hdr.sys_bytes + hdr.shared_bytes), SEEK_CUR);
    auto readVec = [&](uint32_t n) {
        std::vector<uint8_t> v(n);
        if (n && std::fread(v.data(), 1, n, pf) != n) v.clear();
        return v;
    };
    std::vector<uint8_t> boolBlob = readVec(hdr.bool_bytes);
    std::vector<uint8_t> vsFloat = readVec(hdr.vs_float_bytes);
    std::vector<uint8_t> psFloat = readVec(hdr.ps_float_bytes);
    std::vector<uint8_t> psSpirv = readVec(hdr.ps_spirv_bytes);
    struct TexLoad { nhl::highcut::TexturePacketDesc d; std::vector<uint8_t> blob; };
    std::vector<TexLoad> texs;
    for (uint32_t i = 0; i < hdr.texture_count; ++i) {
        TexLoad t{};
        if (std::fread(&t.d, 1, sizeof(t.d), pf) != sizeof(t.d)) { texs.clear(); break; }
        t.blob = readVec(t.d.data_bytes);
        if (t.d.data_bytes && t.blob.empty()) { texs.clear(); break; }
        texs.push_back(std::move(t));
    }
    std::fclose(pf);
    if (psSpirv.empty() || texs.empty()) return false;

    // The C-3 block already created + FILLED the bool/loop + VS/PS float UBOs (set1 bindings 1..4)
    // and shares them with this textured pass. REUSE them — recreating would free the buffers the
    // C-3 solid descriptor set references. boolBlob/vsFloat/psFloat were read above only to advance
    // the file position to the PS SPIR-V + textures.
    if (!c.xlatVsFloatBuf || !c.xlatPsFloatBuf) return false;
    (void)boolBlob; (void)vsFloat; (void)psFloat;

    // PS shader module.
    c.xlatTexPS = c.device->createShader(psSpirv.data(), psSpirv.size(), "main", fmt);
    if (!c.xlatTexPS) { REXLOG_ERROR("[highcut-C4] PS shader module create failed"); return false; }

    // Create + upload each texture via a transient command list (the frame's c.cmd is mid-record).
    auto mapFmt = [](uint32_t f, uint32_t is_signed) -> RenderFormat {
        const bool sgn = is_signed && !NhlBc5NoSnorm();
        switch (f) {
            case nhl::highcut::kTexBC1: return RenderFormat::BC1_UNORM;
            case nhl::highcut::kTexBC2: return RenderFormat::BC2_UNORM;
            case nhl::highcut::kTexBC3: return RenderFormat::BC3_UNORM;
            // C-5j: honor the guest TextureSign. A signed BC5/k_DXN binding must use a SNORM view (see
            // NhlBc5NoSnorm comment) or the normal fetch yields [0,1] where the shader expects [-1,1].
            case nhl::highcut::kTexBC5: return sgn ? RenderFormat::BC5_SNORM : RenderFormat::BC5_UNORM;
            case nhl::highcut::kTexR16: return RenderFormat::R16_UNORM;  // C-5h: k_16 data/mask map
            default: return RenderFormat::R8G8B8A8_UNORM;
        }
    };
    auto upCmd = c.queue->createCommandList();
    auto upFence = c.device->createCommandFence();
    std::vector<std::unique_ptr<RenderBuffer>> stagings;
    c.xlatTextures.clear();
    c.xlatTexViews.clear();
    upCmd->begin();
    for (auto& t : texs) {
        const RenderFormat rf = mapFmt(t.d.tex_format, t.d.is_signed);
        auto tex = c.device->createTexture(RenderTextureDesc::Texture2D(t.d.width, t.d.height, 1, rf));
        auto staging = c.device->createBuffer(RenderBufferDesc::UploadBuffer(t.blob.size()));
        if (staging) { void* p = staging->map(); std::memcpy(p, t.blob.data(), t.blob.size()); staging->unmap(); }
        if (tex && staging) {
            upCmd->barriers(RenderBarrierStage::COPY,
                            RenderTextureBarrier(tex.get(), RenderTextureLayout::COPY_DEST));
            RenderTextureCopyLocation dst = RenderTextureCopyLocation::Subresource(tex.get(), 0, 0);
            // rowWidth is in PIXELS (plume derives the byte pitch from format) — our blob is tight
            // (row_pitch = blocks_x * bytes_per_block), which matches width texels for both 8888/BCn.
            RenderTextureCopyLocation src = RenderTextureCopyLocation::PlacedFootprint(
                staging.get(), rf, t.d.width, t.d.height, 1, t.d.width, 0);
            upCmd->copyTextureRegion(dst, src, 0, 0, 0, nullptr);
            upCmd->barriers(RenderBarrierStage::GRAPHICS,
                            RenderTextureBarrier(tex.get(), RenderTextureLayout::SHADER_READ));
        }
        auto view = tex ? tex->createTextureView(RenderTextureViewDesc::Texture2D(rf)) : nullptr;
        c.xlatTextures.push_back(std::move(tex));
        c.xlatTexViews.push_back(std::move(view));
        stagings.push_back(std::move(staging));
    }
    upCmd->end();
    {
        const RenderCommandList* cl = upCmd.get();
        c.queue->executeCommandLists(&cl, 1, nullptr, 0, nullptr, 0, upFence.get());
        c.queue->waitForCommandFence(upFence.get());
    }

    // Sampler (bring-up: linear + clamp; honor the fetch constant's filter/clamp later).
    RenderSamplerDesc sd;
    sd.minFilter = RenderFilter::LINEAR;
    sd.magFilter = RenderFilter::LINEAR;
    sd.addressU = sd.addressV = sd.addressW = RenderTextureAddressMode::CLAMP;
    c.xlatSampler = c.device->createSampler(sd);
    if (!c.xlatSampler) return false;

    const uint32_t nTex = hdr.texture_count;
    const uint32_t nSamp = hdr.ps_sampler_count ? hdr.ps_sampler_count : 1u;

    // Pipeline layout = the union of the VS sets + the PS texture/sampler set (the SPIR-V hardcodes
    // these set numbers; the translator puts pixel textures on set 3, samplers after the images):
    //   set 0: shared memory (byte-address buffer @ 0)
    //   set 1: constants @ 0(sys),1(vs-float),2(ps-float),3(bool/loop),4(fetch) — superset; an
    //          unused binding bound to a valid buffer is allowed
    //   set 2: vertex textures — EMPTY here (placeholder so pixel textures land on set 3)
    //   set 3: PS textures @ 0..nTex-1, PS samplers @ nTex..nTex+nSamp-1
    auto buildSet1 = [](RenderDescriptorSetBuilder& b) {
        b.begin();
        for (uint32_t i = 0; i <= 4; ++i) b.addConstantBuffer(i);
        b.end();
    };
    auto buildSet3 = [&](RenderDescriptorSetBuilder& b) {
        b.begin();
        for (uint32_t i = 0; i < nTex; ++i) b.addTexture(i);
        for (uint32_t i = 0; i < nSamp; ++i) b.addSampler(nTex + i);
        b.end();
    };
    RenderDescriptorSetBuilder ls0, ls1, ls2, ls3;
    ls0.begin(); ls0.addByteAddressBuffer(0); ls0.end();
    buildSet1(ls1);
    ls2.begin(); ls2.end();  // empty vertex-texture set
    buildSet3(ls3);
    RenderPipelineLayoutBuilder lb;
    lb.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
    lb.addDescriptorSet(ls0);
    lb.addDescriptorSet(ls1);
    lb.addDescriptorSet(ls2);
    lb.addDescriptorSet(ls3);
    lb.end();
    c.xlatTexLayout = lb.create(c.device.get());
    if (!c.xlatTexLayout) { REXLOG_ERROR("[highcut-C4] textured pipeline layout create failed"); return false; }

    // Descriptor sets 0/1/3 (set 2 is empty — no object created/bound).
    { RenderDescriptorSetBuilder b; b.begin(); b.addByteAddressBuffer(0); b.end(); c.xlatTexSet0 = b.create(c.device.get()); }
    { RenderDescriptorSetBuilder b; buildSet1(b); c.xlatTexSet1 = b.create(c.device.get()); }
    { RenderDescriptorSetBuilder b; buildSet3(b); c.xlatTexSet3 = b.create(c.device.get()); }
    if (!c.xlatTexSet0 || !c.xlatTexSet1 || !c.xlatTexSet3) {
        REXLOG_ERROR("[highcut-C4] textured descriptor set create failed");
        return false;
    }
    c.xlatTexSet0->setBuffer(0, c.xlatSharedBuf.get(), 1u << 16);
    c.xlatTexSet1->setBuffer(0, c.xlatSysBuf.get(), 2048);
    c.xlatTexSet1->setBuffer(1, c.xlatVsFloatBuf.get(), 256 * 16);
    c.xlatTexSet1->setBuffer(2, c.xlatPsFloatBuf.get(), 256 * 16);
    c.xlatTexSet1->setBuffer(3, c.xlatBoolBuf.get(), 256);
    c.xlatTexSet1->setBuffer(4, c.xlatFetchBuf.get(), 768);
    for (uint32_t i = 0; i < nTex && i < c.xlatTextures.size(); ++i) {
        if (c.xlatTextures[i])
            c.xlatTexSet3->setTexture(i, c.xlatTextures[i].get(), RenderTextureLayout::SHADER_READ,
                                      c.xlatTexViews[i].get());
    }
    for (uint32_t i = 0; i < nSamp; ++i) c.xlatTexSet3->setSampler(nTex + i, c.xlatSampler.get());

    auto topo = (hdr.topology == nhl::highcut::kTopoTriangleStrip)
                    ? RenderPrimitiveTopology::TRIANGLE_STRIP
                    : RenderPrimitiveTopology::TRIANGLE_LIST;
    RenderGraphicsPipelineDesc pd;
    pd.pipelineLayout = c.xlatTexLayout.get();
    pd.vertexShader = c.xlatVS.get();
    pd.pixelShader = c.xlatTexPS.get();
    pd.renderTargetFormat[0] = kSwapFormat;
    pd.renderTargetBlend[0] = RenderBlendDesc::Copy();
    pd.renderTargetCount = 1;
    pd.depthTargetFormat = kDepthFormat;  // C-5c: framebuffer has depth; declare it (depth test off)
    pd.primitiveTopology = topo;
    c.xlatTexPipeline = c.device->createGraphicsPipeline(pd);
    if (!c.xlatTexPipeline) { REXLOG_ERROR("[highcut-C4] textured graphics pipeline create failed"); return false; }

    c.xlatTexDrawReady = true;
    REXLOG_INFO("[highcut-C4] textured pipeline READY: nTex={} nSamp={} ps_spirv={} verts={} topo={}",
                nTex, nSamp, uint32_t(psSpirv.size()), c.xlatVertexCount, hdr.topology);
    return true;
}

// C-5a: map the packet's plume-neutral blend factor/op (= Xenos enum values) to plume's enums.
RenderBlend toPlumeBlend(uint32_t f) {
    namespace hc = nhl::highcut;
    switch (f) {
        case hc::kBlendZero: return RenderBlend::ZERO;
        case hc::kBlendOne: return RenderBlend::ONE;
        case hc::kBlendSrcColor: return RenderBlend::SRC_COLOR;
        case hc::kBlendInvSrcColor: return RenderBlend::INV_SRC_COLOR;
        case hc::kBlendSrcAlpha: return RenderBlend::SRC_ALPHA;
        case hc::kBlendInvSrcAlpha: return RenderBlend::INV_SRC_ALPHA;
        case hc::kBlendDstColor: return RenderBlend::DEST_COLOR;
        case hc::kBlendInvDstColor: return RenderBlend::INV_DEST_COLOR;
        case hc::kBlendDstAlpha: return RenderBlend::DEST_ALPHA;
        case hc::kBlendInvDstAlpha: return RenderBlend::INV_DEST_ALPHA;
        case hc::kBlendSrcAlphaSat: return RenderBlend::SRC_ALPHA_SAT;
        case hc::kBlendConstColor: case hc::kBlendConstAlpha: return RenderBlend::BLEND_FACTOR;
        case hc::kBlendInvConstColor: case hc::kBlendInvConstAlpha: return RenderBlend::INV_BLEND_FACTOR;
        default: return RenderBlend::ONE;
    }
}
RenderBlendOperation toPlumeBlendOp(uint32_t o) {
    namespace hc = nhl::highcut;
    switch (o) {
        case hc::kBlendOpSubtract: return RenderBlendOperation::SUBTRACT;
        case hc::kBlendOpRevSubtract: return RenderBlendOperation::REV_SUBTRACT;
        case hc::kBlendOpMin: return RenderBlendOperation::MIN;
        case hc::kBlendOpMax: return RenderBlendOperation::MAX;
        default: return RenderBlendOperation::ADD;
    }
}

// C-5c: map the raw Xenos depth/stencil/cull enum VALUES the packet carries to plume's enums.
// xenos::CompareFunction: 0=Never 1=Less 2=Equal 3=LessEqual 4=Greater 5=NotEqual 6=GreaterEqual 7=Always.
RenderComparisonFunction toPlumeCompare(uint32_t f) {
    switch (f & 7) {
        case 0: return RenderComparisonFunction::NEVER;
        case 1: return RenderComparisonFunction::LESS;
        case 2: return RenderComparisonFunction::EQUAL;
        case 3: return RenderComparisonFunction::LESS_EQUAL;
        case 4: return RenderComparisonFunction::GREATER;
        case 5: return RenderComparisonFunction::NOT_EQUAL;
        case 6: return RenderComparisonFunction::GREATER_EQUAL;
        default: return RenderComparisonFunction::ALWAYS;
    }
}
// xenos::StencilOp: 0=Keep 1=Zero 2=Replace 3=IncrClamp 4=DecrClamp 5=Invert 6=IncrWrap 7=DecrWrap.
RenderStencilOp toPlumeStencilOp(uint32_t o) {
    switch (o & 7) {
        case 0: return RenderStencilOp::KEEP;
        case 1: return RenderStencilOp::ZERO;
        case 2: return RenderStencilOp::REPLACE;
        case 3: return RenderStencilOp::INCREMENT_AND_CLAMP;
        case 4: return RenderStencilOp::DECREMENT_AND_CLAMP;
        case 5: return RenderStencilOp::INVERT;
        case 6: return RenderStencilOp::INCREMENT_AND_WRAP;
        default: return RenderStencilOp::DECREMENT_AND_WRAP;
    }
}
RenderStencilFaceDesc toPlumeStencilFace(uint32_t fail_op, uint32_t pass_op, uint32_t depth_fail_op,
                                         uint32_t func) {
    RenderStencilFaceDesc d;
    d.failOp = toPlumeStencilOp(fail_op);
    d.passOp = toPlumeStencilOp(pass_op);
    d.depthFailOp = toPlumeStencilOp(depth_fail_op);
    d.compareFunction = toPlumeCompare(func);
    return d;
}

// C-5a: map a Xenos 3-bit texture-swizzle component (0=R,1=G,2=B,3=A,4=0,5=1) to plume's enum, and
// the 12-bit guest swizzle to a plume component mapping. Applied to the sampled view so e.g. the
// menu logo's BGRA 8888 reads as RGBA (else blue<->red -> the "orange logo").
RenderSwizzle xenosSwz(uint32_t s) {
    switch (s & 7) {
        case 0: return RenderSwizzle::R;
        case 1: return RenderSwizzle::G;
        case 2: return RenderSwizzle::B;
        case 3: return RenderSwizzle::A;
        case 4: return RenderSwizzle::ZERO;
        case 5: return RenderSwizzle::ONE;
        default: return RenderSwizzle::IDENTITY;
    }
}
// C-5j: A/B opt-out for the signed-BC5 -> BC5_SNORM view fix. The rexglue SPIR-V translator emits a
// separate TextureBinding per (fetch_constant, dimension, is_signed); a signed binding means the shader
// samples that texture expecting [-1,1], so the host must supply a signed-format view. The k_DXN goalie
// normal maps are bound signed; reading them through a BC5_UNORM view gives [0,1] -> wrong normals ->
// the green-on-dark equipment speckle (BC5 packs into R,G). Set NHL_HIGHCUT_BC5_NO_SNORM=1 to force the
// old UNORM-always behavior for a clean A/B against the fix.
bool NhlBc5NoSnorm() {
    static const bool v = std::getenv("NHL_HIGHCUT_BC5_NO_SNORM") != nullptr;
    return v;
}

RenderComponentMapping xenosSwizzleMapping(uint32_t swz) {
    return RenderComponentMapping(xenosSwz(swz), xenosSwz(swz >> 3), xenosSwz(swz >> 6),
                                  xenosSwz(swz >> 9));
}

// C-5f: map a guest xenos::TextureFilter (0=kPoint,1=kLinear,2=kBaseMap,3=kUseFetchConst) to plume.
// kPoint -> NEAREST, everything else -> LINEAR (kUseFetchConst can't survive into the live fetch
// constant we read; kBaseMap only applies to mip and reads as no-mip below).
RenderFilter xenosFilter(uint32_t f) {
    return f == 0u /*kPoint*/ ? RenderFilter::NEAREST : RenderFilter::LINEAR;
}
RenderMipmapMode xenosMipMode(uint32_t f) {
    // kPoint -> NEAREST, kLinear -> LINEAR, kBaseMap(2)/other -> NEAREST (sample base level only).
    return f == 1u /*kLinear*/ ? RenderMipmapMode::LINEAR : RenderMipmapMode::NEAREST;
}
// xenos::ClampMode -> plume RenderTextureAddressMode. 0=kRepeat,1=kMirroredRepeat,2=kClampToEdge,
// 3=kMirrorClampToEdge,4=kClampToHalfway,5=kMirrorClampToHalfway,6=kClampToBorder,7=kMirrorClampToBorder.
RenderTextureAddressMode xenosClamp(uint32_t c) {
    switch (c) {
        case 0: return RenderTextureAddressMode::WRAP;        // kRepeat
        case 1: return RenderTextureAddressMode::MIRROR;      // kMirroredRepeat
        case 3: return RenderTextureAddressMode::MIRROR_ONCE; // kMirrorClampToEdge
        case 6: return RenderTextureAddressMode::BORDER;      // kClampToBorder
        case 7: return RenderTextureAddressMode::MIRROR;      // kMirrorClampToBorder (no exact plume mode)
        default: return RenderTextureAddressMode::CLAMP;      // 2/4/5 clamp-to-edge family
    }
}

// C-5a: build ONE fully self-contained RenderableDraw from a v3 packet (in memory). Mirrors the C-4
// CreateTexturedDraw resource setup, but owns everything per-draw (its own VS/PS/buffers/textures/
// layout/sets/pipeline) so many can be replayed into one RT. Draws with no translated PS are skipped
// (return false) — C-5a needs a color PS. Returns false on any resource-creation failure.
bool BuildRenderableDraw(PlumeCtx& c, const std::vector<uint8_t>& bytes, RenderableDraw& d,
                         uint32_t drawIndex = ~0u) {
    namespace hc = nhl::highcut;
    if (bytes.size() < sizeof(hc::DrawPacketHeader)) return false;
    hc::DrawPacketHeader hdr{};
    std::memcpy(&hdr, bytes.data(), sizeof(hdr));
    if (hdr.magic != hc::kDrawPacketMagic || hdr.version != hc::kDrawPacketVersion) return false;
    // need a VS + a color PS. Step 2: the bytes may be 0 (streamed via the dictionary) — gate on the ids.
    if (hdr.vs_shader_id == 0 || hdr.ps_shader_id == 0) return false;
    const RenderShaderFormat fmt = c.ri->getCapabilities().shaderFormat;
    if (fmt != RenderShaderFormat::SPIRV) return false;

    size_t off = sizeof(hdr);
    auto take = [&](uint32_t n) -> const uint8_t* {
        if (off + n > bytes.size()) return nullptr;
        const uint8_t* p = bytes.data() + off;
        off += n;
        return p;
    };
    const uint8_t* fetch = take(hdr.fetch_bytes);
    const uint8_t* sysc = take(hdr.sys_bytes);
    const uint8_t* shared = take(hdr.shared_bytes);
    const uint8_t* boolc = take(hdr.bool_bytes);
    const uint8_t* vsf = take(hdr.vs_float_bytes);
    const uint8_t* psf = take(hdr.ps_float_bytes);
    const uint8_t* vsSpirv = take(hdr.vs_spirv_bytes);
    const uint8_t* psSpirv = take(hdr.ps_spirv_bytes);
    if (!fetch || !sysc || !boolc || !vsf || !psf || !vsSpirv || !psSpirv) return false;
    struct TexLoad { hc::TexturePacketDesc desc; const uint8_t* data; };
    std::vector<TexLoad> texs;
    for (uint32_t i = 0; i < hdr.texture_count; ++i) {
        const uint8_t* dp = take(uint32_t(sizeof(hc::TexturePacketDesc)));
        if (!dp) return false;
        TexLoad t{};
        std::memcpy(&t.desc, dp, sizeof(t.desc));
        t.data = take(t.desc.data_bytes);
        if (!t.data && t.desc.data_bytes) return false;
        texs.push_back(t);
    }
    // C-5d.3: VERTEX-shader textures (skinning bone palette) — after the PS textures, before the index.
    std::vector<TexLoad> vs_texs;
    for (uint32_t i = 0; i < hdr.vs_texture_count; ++i) {
        const uint8_t* dp = take(uint32_t(sizeof(hc::TexturePacketDesc)));
        if (!dp) return false;
        TexLoad t{};
        std::memcpy(&t.desc, dp, sizeof(t.desc));
        t.data = take(t.desc.data_bytes);
        if (!t.data && t.desc.data_bytes) return false;
        vs_texs.push_back(t);
    }
    // C-5f: per-sampler descs (PS then VS), between the textures and the index blob.
    std::vector<hc::SamplerPacketDesc> psSampDescs(hdr.ps_sampler_count), vsSampDescs(hdr.vs_sampler_count);
    for (uint32_t i = 0; i < hdr.ps_sampler_count; ++i) {
        const uint8_t* dp = take(uint32_t(sizeof(hc::SamplerPacketDesc)));
        if (!dp) return false;
        std::memcpy(&psSampDescs[i], dp, sizeof(hc::SamplerPacketDesc));
    }
    for (uint32_t i = 0; i < hdr.vs_sampler_count; ++i) {
        const uint8_t* dp = take(uint32_t(sizeof(hc::SamplerPacketDesc)));
        if (!dp) return false;
        std::memcpy(&vsSampDescs[i], dp, sizeof(hc::SamplerPacketDesc));
    }
    // C-5d: kGuestDMA index blob (raw guest indices, last in the packet).
    const uint8_t* idxData = hdr.index_bytes ? take(hdr.index_bytes) : nullptr;
    if (hdr.index_bytes && !idxData) return false;

    // by-ID: reuse the compiled shader module across draws/frames (keyed by the producer's shader id).
    // On a miss we compile the inline SPIR-V (Step 1 still ships bytes every draw; Step 2 ships once).
    auto getShader = [&](uint64_t id, const uint8_t* spv, uint32_t n) -> std::shared_ptr<RenderShader> {
        if (id) { auto it = c.shaderCache.find(id); if (it != c.shaderCache.end()) return it->second; }
        // Step 2: when the packet carries no inline bytes, resolve them from the streamed dictionary.
        if ((!spv || !n) && id) {
            auto r = c.resourceBytes.find(id);
            if (r != c.resourceBytes.end()) { spv = r->second.data(); n = uint32_t(r->second.size()); }
        }
        if (!spv || !n) return nullptr;  // not inline AND not (yet) in the dictionary
        std::shared_ptr<RenderShader> sh = c.device->createShader(spv, n, "main", fmt);
        if (sh && id) c.shaderCache.emplace(id, sh);
        return sh;
    };
    d.vs = getShader(hdr.vs_shader_id, vsSpirv, hdr.vs_spirv_bytes);
    // C-5g: optionally override this draw's PS with an instrumented .spv from disk (the number-layer
    // probe). The instrumented shader keeps the same descriptor interface, so all bindings still line up.
    std::vector<uint8_t> dbgPS = MaybeLoadDebugPS(drawIndex);
    const bool dbgPSactive = !dbgPS.empty();
    if (dbgPSactive)
        d.ps = c.device->createShader(dbgPS.data(), dbgPS.size(), "main", fmt);  // debug: fresh, uncached
    else
        d.ps = getShader(hdr.ps_shader_id, psSpirv, hdr.ps_spirv_bytes);
    if (!d.vs || !d.ps) return false;
    const bool textured = hdr.texture_count > 0;
    d.textured = textured;

    // C-5d.3: flag PS texture slots that sample a guest resolve dest AND whose captured blob is a 2x2
    // STUB — i.e. the capture could NOT read real data for that address (depth/shadow maps: k_24_8 is
    // unsupported by untileBindings -> stubbed). For those we re-point set3 (below, once the source RTs
    // exist) at the source surface's rendered RT. COLOR resolves, by contrast, were captured as real
    // full-size images straight from guest RAM (the resolved texels persist there), so the captured blob
    // is already correct — re-rendering them regresses (broken ice/reflections). Cube stubs
    // (array_layers==6) are the neutral-env case, handled separately and deferred to the cube step.
    for (uint32_t i = 0; i < hdr.texture_count && i < texs.size(); ++i) {
        const auto& td = texs[i].desc;
        auto rit = c.resolveMap.find(td.fetch_base_addr);
        if (rit == c.resolveMap.end()) continue;
        const bool capturedStub = td.width <= 2 && td.height <= 2 && td.array_layers == 1;
        if (!capturedStub) continue;  // only host-copy genuinely-stubbed bindings (depth/shadow)
        d.hostCopy.push_back({i, rit->second.srcKey, rit->second.isDepth});
    }
    // C-5n: flag a draw that samples the FULL-SCREEN scene COLOR resolve (the composite/grade pass).
    for (uint32_t i = 0; i < hdr.texture_count && i < texs.size(); ++i) {
        const auto& td = texs[i].desc;
        auto rit = c.resolveMap.find(td.fetch_base_addr);
        if (rit == c.resolveMap.end() || rit->second.isDepth) continue;   // color resolves only
        if (td.array_layers != 1 || td.width < 960) continue;             // full-screen 2D only
        d.samplesFullscreenSceneColor = true;
    }
    // The OPAQUE (One/Zero copy) scene-samplers are the grade pass that re-writes the whole frame; the
    // HUD overlay must skip them so our clean primary render survives. Alpha-blended overlay draws and
    // stencil-mask setups (which sample the scene but are masked/discarded) are kept.
    d.sceneGradeSkip = d.samplesFullscreenSceneColor &&
                       hdr.blend_src == nhl::highcut::kBlendOne &&
                       hdr.blend_dst == nhl::highcut::kBlendZero;

    constexpr uint64_t kSys = 2048, kBool = 256, kFetch = 768, kFloat = 256 * 16;
    // C-5c: size the shared-memory (vertex) SSBO to THIS draw's data, not a fixed 64K. 3D meshes
    // pack multiple vertex streams (position/normal/uv/...) and can be hundreds of KB — a fixed 64K
    // truncated them, dropping the tail vertices to garbage -> exploded geometry. Floor 64K (small
    // draws / safety), cap 16MB (matches the capture cap).
    const uint64_t kShared = std::min<uint64_t>(
        std::max<uint64_t>(hdr.shared_bytes, 1u << 16), 16u * 0x100000u);
    auto mkUbo = [&](uint64_t sz, const uint8_t* src, uint32_t srcN) {
        auto b = c.device->createBuffer(RenderBufferDesc::UploadBuffer(sz, RenderBufferFlag::CONSTANT));
        if (b) {
            void* p = b->map();
            std::memset(p, 0, sz);
            if (src && srcN) std::memcpy(p, src, std::min<uint64_t>(srcN, sz));
            b->unmap();
        }
        return b;
    };
    d.sysBuf = mkUbo(kSys, sysc, hdr.sys_bytes);
    d.boolBuf = mkUbo(kBool, boolc, hdr.bool_bytes);
    d.fetchBuf = mkUbo(kFetch, fetch, hdr.fetch_bytes);
    d.vsFloatBuf = mkUbo(kFloat, vsf, hdr.vs_float_bytes);
    d.psFloatBuf = mkUbo(kFloat, psf, hdr.ps_float_bytes);
    d.sharedBuf = c.device->createBuffer(RenderBufferDesc::UploadBuffer(kShared, RenderBufferFlag::STORAGE));
    if (d.sharedBuf) {
        void* p = d.sharedBuf->map();
        std::memset(p, 0, kShared);
        if (shared && hdr.shared_bytes) std::memcpy(p, shared, std::min<uint64_t>(hdr.shared_bytes, kShared));
        d.sharedBuf->unmap();
    }
    if (!d.sysBuf || !d.boolBuf || !d.fetchBuf || !d.vsFloatBuf || !d.psFloatBuf || !d.sharedBuf)
        return false;

    const uint32_t nTex = hdr.texture_count;
    const uint32_t nSamp = hdr.ps_sampler_count ? hdr.ps_sampler_count : 1u;
    const uint32_t nVsTex = hdr.vs_texture_count;                              // C-5d.3: VS (set2)
    // EXACT sampler count (NOT defaulted to 1 like the PS path): a skinning bone palette is read via
    // texel-fetch and may have 0 samplers. Adding a phantom sampler would make set2's layout mismatch
    // the translated VS's declared bindings.
    const uint32_t nVsSamp = hdr.vs_sampler_count;
    const bool vs_textured = nVsTex > 0;
    // C-4/C-5d.3: create + upload a list of guest textures (PS or VS) into plume textures+views.
    // Factored so PS textures (set3) and the VS skinning bone palette (set2) use one path. Samplers are
    // built separately (C-5f: one per guest SamplerBinding, honoring its filter/clamp).
    auto createTextures = [&](const std::vector<TexLoad>& srcTexs,
                              std::vector<std::shared_ptr<RenderTexture>>& outTex,
                              std::vector<std::shared_ptr<RenderTextureView>>& outView) -> bool {
        if (srcTexs.empty()) return true;
        auto mapFmt = [](uint32_t f, uint32_t is_signed) -> RenderFormat {
            const bool sgn = is_signed && !NhlBc5NoSnorm();
            switch (f) {
                case hc::kTexBC1: return RenderFormat::BC1_UNORM;
                case hc::kTexBC2: return RenderFormat::BC2_UNORM;
                case hc::kTexBC3: return RenderFormat::BC3_UNORM;
                // C-5j: honor the guest TextureSign — a signed BC5/k_DXN binding needs a SNORM view.
                case hc::kTexBC5: return sgn ? RenderFormat::BC5_SNORM : RenderFormat::BC5_UNORM;
                case hc::kTexR16: return RenderFormat::R16_UNORM;  // C-5h: k_16 data/mask map
                case hc::kTexRGBA32F: return RenderFormat::R32G32B32A32_FLOAT;  // C-5d.3 bone palette
                default: return RenderFormat::R8G8B8A8_UNORM;
            }
        };
        auto up = c.queue->createCommandList();
        auto fence = c.device->createCommandFence();
        std::vector<std::unique_ptr<RenderBuffer>> stagings;
        bool anyUpload = false;
        up->begin();
        for (auto& t : srcTexs) {
            const RenderFormat rf = mapFmt(t.desc.tex_format, t.desc.is_signed);
            // C-5g: a CUBE binding (array_layers==6) must be a real cube texture+view, else Vulkan
            // samples our 2D placeholder as garbage and the cube-alpha material term drops the number.
            const bool isCube = t.desc.array_layers == 6;
            const uint32_t layers = isCube ? 6u : 1u;
            // by-ID cache: reuse the uploaded texture+view across draws/frames. Fold in the bits that
            // change the GPU object (format/swizzle/cube). tex_id==0 (2x2 stub fallbacks) => uncached.
            uint64_t tkey = 0;
            if (t.desc.tex_id)
                tkey = t.desc.tex_id ^ (uint64_t(t.desc.swizzle) * 0x9E3779B1ull) ^
                       (uint64_t(uint32_t(rf)) << 40) ^ (uint64_t(isCube) << 52);
            if (tkey) {
                auto it = c.texCache.find(tkey);
                if (it != c.texCache.end()) {
                    outTex.push_back(it->second.tex);
                    outView.push_back(it->second.view);
                    continue;  // already uploaded in a prior draw/frame — skip recreate + upload
                }
            }
            // Step 2: the blob is inline (disk capture / 2x2 stubs) OR streamed via the dictionary (live).
            const uint8_t* texData = t.data;
            uint32_t texBytes = t.desc.data_bytes;
            if ((!texData || !texBytes) && t.desc.tex_id) {
                auto r = c.resourceBytes.find(t.desc.tex_id);
                if (r != c.resourceBytes.end()) { texData = r->second.data(); texBytes = uint32_t(r->second.size()); }
            }
            auto tex = isCube
                ? c.device->createTexture(RenderTextureDesc::Texture(
                      RenderTextureDimension::TEXTURE_2D, t.desc.width, t.desc.height, 1, 1, 6, rf,
                      RenderTextureFlag::CUBE))
                : c.device->createTexture(RenderTextureDesc::Texture2D(t.desc.width, t.desc.height, 1, rf));
            auto staging = c.device->createBuffer(RenderBufferDesc::UploadBuffer(texBytes ? texBytes : 4u));
            if (staging && texBytes) {
                void* p = staging->map();
                std::memcpy(p, texData, texBytes);
                // DIAGNOSTIC: NHL_HIGHCUT_C5_CUBE=R,G,B overwrites cube faces with a flat test color, to
                // confirm whether the (currently neutral-stubbed) env reflection cube drives a material's
                // shading. If the jersey/equipment lights up when this is bright, the cube is the missing
                // lighting input and the real fix is untiling its 6 faces from guest RAM (capture-side).
                static const bool cubeOverride = std::getenv("NHL_HIGHCUT_C5_CUBE") != nullptr;
                if (isCube && cubeOverride && t.desc.tex_format == hc::kTexRGBA8) {
                    int cr = 200, cg = 200, cb = 200;
                    if (const char* s = std::getenv("NHL_HIGHCUT_C5_CUBE")) std::sscanf(s, "%d,%d,%d", &cr, &cg, &cb);
                    uint8_t* bp = static_cast<uint8_t*>(p);
                    for (uint32_t o = 0; o + 4 <= texBytes; o += 4) {
                        bp[o + 0] = uint8_t(cr); bp[o + 1] = uint8_t(cg); bp[o + 2] = uint8_t(cb); bp[o + 3] = 255;
                    }
                }
                staging->unmap();
            }
            if (tex && staging) {
                up->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(tex.get(), RenderTextureLayout::COPY_DEST));
                // One PlacedFootprint per face/layer; faces are concatenated tight in the blob (the CP
                // writes them face-major), so layer N starts at N * faceBytes.
                const uint32_t faceBytes = texBytes / layers;
                for (uint32_t layer = 0; layer < layers; ++layer) {
                    up->copyTextureRegion(
                        RenderTextureCopyLocation::Subresource(tex.get(), 0, layer),
                        RenderTextureCopyLocation::PlacedFootprint(staging.get(), rf, t.desc.width,
                                                                   t.desc.height, 1, t.desc.width,
                                                                   uint64_t(layer) * faceBytes));
                }
                up->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(tex.get(), RenderTextureLayout::SHADER_READ));
                anyUpload = true;
            }
            RenderTextureViewDesc vd = isCube ? RenderTextureViewDesc::TextureCube(rf)
                                              : RenderTextureViewDesc::Texture2D(rf);
            vd.componentMapping = xenosSwizzleMapping(t.desc.swizzle);  // apply the guest swizzle
            std::shared_ptr<RenderTexture> texS = std::move(tex);
            std::shared_ptr<RenderTextureView> viewS;
            if (texS) viewS = texS->createTextureView(vd);
            if (tkey && texS) c.texCache.emplace(tkey, PlumeCtx::CachedTex{texS, viewS});
            outTex.push_back(texS);
            outView.push_back(viewS);
            stagings.push_back(std::move(staging));
        }
        up->end();
        if (anyUpload) {  // only submit when at least one texture was actually (re)uploaded this call
            const RenderCommandList* cl = up.get();
            c.queue->executeCommandLists(&cl, 1, nullptr, 0, nullptr, 0, fence.get());
            c.queue->waitForCommandFence(fence.get());
        }
        return true;
    };
    // C-5f: build `count` samplers from the guest SamplerBindings (honor filter+clamp per binding). The
    // descriptor set declares `count` sampler bindings at nTex+i; bind sampler i = descs[i]. When the
    // packet carries no descs (old capture / 0 bindings), fall back to `fallback` filter + CLAMP.
    auto buildSamplers = [&](const std::vector<hc::SamplerPacketDesc>& descs, uint32_t count,
                             RenderFilter fallback,
                             std::vector<std::unique_ptr<RenderSampler>>& out) -> bool {
        for (uint32_t i = 0; i < count; ++i) {
            RenderSamplerDesc sd;
            if (i < descs.size()) {
                const auto& g = descs[i];
                sd.magFilter = xenosFilter(g.mag_filter);
                sd.minFilter = xenosFilter(g.min_filter);
                sd.mipmapMode = xenosMipMode(g.mip_filter);
                sd.addressU = xenosClamp(g.clamp_x);
                sd.addressV = xenosClamp(g.clamp_y);
                sd.addressW = xenosClamp(g.clamp_z);
            } else {
                sd.magFilter = sd.minFilter = fallback;
                sd.mipmapMode = RenderMipmapMode::NEAREST;
                sd.addressU = sd.addressV = sd.addressW = RenderTextureAddressMode::CLAMP;
            }
            auto s = c.device->createSampler(sd);
            if (!s) return false;
            out.push_back(std::move(s));
        }
        return true;
    };
    if (!createTextures(texs, d.textures, d.texViews)) return false;
    if (!createTextures(vs_texs, d.vsTextures, d.vsTexViews)) return false;
    // C-5f: PS samplers honor the guest per-binding filter/clamp. The jersey nameplate-layout map is
    // POINT-sampled by the guest (LINEAR-blending it broke the back number); the bone palette likewise.
    if (!buildSamplers(psSampDescs, nSamp, RenderFilter::LINEAR, d.samplers)) return false;
    // C-5d.3: the VS skinning bone palette MUST be POINT-sampled — LINEAR blends adjacent bone-matrix
    // rows into garbage transforms. The guest sampler is POINT, so the descs carry it; NEAREST fallback.
    if (!buildSamplers(vsSampDescs, nVsSamp, RenderFilter::NEAREST, d.vsSamplers)) return false;

    auto bSet1 = [](RenderDescriptorSetBuilder& b) {
        b.begin();
        for (uint32_t i = 0; i <= 4; ++i) b.addConstantBuffer(i);
        b.end();
    };
    auto bSet2 = [&](RenderDescriptorSetBuilder& b) {  // C-5d.3: VS textures (skinning) then samplers
        b.begin();
        for (uint32_t i = 0; i < nVsTex; ++i) b.addTexture(i);
        for (uint32_t i = 0; i < nVsSamp; ++i) b.addSampler(nVsTex + i);
        b.end();
    };
    auto bSet3 = [&](RenderDescriptorSetBuilder& b) {
        b.begin();
        for (uint32_t i = 0; i < nTex; ++i) b.addTexture(i);
        for (uint32_t i = 0; i < nSamp; ++i) b.addSampler(nTex + i);
        b.end();
    };
    // set2 must EXIST whenever set3 does (so PS textures land on set 3) OR when the VS has textures.
    const bool need_set2 = vs_textured || textured;
    // by-ID: cache the pipeline LAYOUT by its shape (descriptor-set presence + texture/sampler counts) —
    // identical across all draws with the same counts, so build it once instead of per draw.
    const uint64_t layoutKey =
        (uint64_t(need_set2 ? 1 : 0)) | (uint64_t(vs_textured ? 1 : 0) << 1) |
        (uint64_t(textured ? 1 : 0) << 2) | (uint64_t(nTex & 0xFFu) << 8) |
        (uint64_t(nSamp & 0xFFu) << 16) | (uint64_t(nVsTex & 0xFFu) << 24) |
        (uint64_t(nVsSamp & 0xFFu) << 32);
    if (auto it = c.layoutCache.find(layoutKey); it != c.layoutCache.end()) {
        d.layout = it->second;
    } else {
        RenderPipelineLayoutBuilder lb;
        lb.begin(false, false);
        { RenderDescriptorSetBuilder s; s.begin(); s.addByteAddressBuffer(0); s.end(); lb.addDescriptorSet(s); }  // set0
        { RenderDescriptorSetBuilder s; bSet1(s); lb.addDescriptorSet(s); }                                       // set1
        if (need_set2) {                                                                                          // set2
            RenderDescriptorSetBuilder s;
            if (vs_textured) bSet2(s); else { s.begin(); s.end(); }  // VS textures, else empty placeholder
            lb.addDescriptorSet(s);
        }
        if (textured) { RenderDescriptorSetBuilder s; bSet3(s); lb.addDescriptorSet(s); }                         // set3
        lb.end();
        std::shared_ptr<RenderPipelineLayout> L = lb.create(c.device.get());
        if (!L) return false;
        c.layoutCache.emplace(layoutKey, L);
        d.layout = L;
    }
    if (!d.layout) return false;

    { RenderDescriptorSetBuilder s; s.begin(); s.addByteAddressBuffer(0); s.end(); d.set0 = s.create(c.device.get()); }
    { RenderDescriptorSetBuilder s; bSet1(s); d.set1 = s.create(c.device.get()); }
    if (!d.set0 || !d.set1) return false;
    d.set0->setBuffer(0, d.sharedBuf.get(), kShared);
    d.set1->setBuffer(0, d.sysBuf.get(), kSys);
    d.set1->setBuffer(1, d.vsFloatBuf.get(), kFloat);
    d.set1->setBuffer(2, d.psFloatBuf.get(), kFloat);
    d.set1->setBuffer(3, d.boolBuf.get(), kBool);
    d.set1->setBuffer(4, d.fetchBuf.get(), kFetch);
    if (vs_textured) {  // C-5d.3: bind the VS skinning textures to set2
        RenderDescriptorSetBuilder s; bSet2(s); d.set2 = s.create(c.device.get());
        if (!d.set2) return false;
        for (uint32_t i = 0; i < nVsTex && i < d.vsTextures.size(); ++i)
            if (d.vsTextures[i]) d.set2->setTexture(i, d.vsTextures[i].get(), RenderTextureLayout::SHADER_READ, d.vsTexViews[i].get());
        for (uint32_t i = 0; i < nVsSamp && i < d.vsSamplers.size(); ++i)
            d.set2->setSampler(nVsTex + i, d.vsSamplers[i].get());
    }
    if (textured) {
        RenderDescriptorSetBuilder s; bSet3(s); d.set3 = s.create(c.device.get());
        if (!d.set3) return false;
        for (uint32_t i = 0; i < nTex && i < d.textures.size(); ++i)
            if (d.textures[i]) d.set3->setTexture(i, d.textures[i].get(), RenderTextureLayout::SHADER_READ, d.texViews[i].get());
        for (uint32_t i = 0; i < nSamp && i < d.samplers.size(); ++i)
            d.set3->setSampler(nTex + i, d.samplers[i].get());
    }

    RenderBlendDesc blend;
    blend.blendEnabled = hdr.blend_enable != 0;
    blend.srcBlend = toPlumeBlend(hdr.blend_src);
    blend.dstBlend = toPlumeBlend(hdr.blend_dst);
    blend.blendOp = toPlumeBlendOp(hdr.blend_op);
    blend.srcBlendAlpha = toPlumeBlend(hdr.blend_src_a);
    blend.dstBlendAlpha = toPlumeBlend(hdr.blend_dst_a);
    blend.blendOpAlpha = toPlumeBlendOp(hdr.blend_op_a);
    blend.renderTargetWriteMask = uint8_t(hdr.color_write_mask & 0xF);
    auto topo = (hdr.topology == hc::kTopoTriangleStrip) ? RenderPrimitiveTopology::TRIANGLE_STRIP
                                                         : RenderPrimitiveTopology::TRIANGLE_LIST;
    // C-5a.1: quad-list (menu text) -> a TRIANGLE_LIST index buffer, {0,1,2,0,2,3} per 4-vert quad.
    if (hdr.topology == hc::kTopoTriangleListQuadExpand) {
        const uint32_t quads = hdr.vertex_count / 4;
        std::vector<uint32_t> idx;
        idx.reserve(size_t(quads) * 6);
        for (uint32_t q = 0; q < quads; ++q) {
            const uint32_t b = q * 4;
            idx.push_back(b + 0); idx.push_back(b + 1); idx.push_back(b + 2);
            idx.push_back(b + 0); idx.push_back(b + 2); idx.push_back(b + 3);
        }
        if (!idx.empty()) {
            const uint64_t bytes = idx.size() * sizeof(uint32_t);
            d.indexBuf = c.device->createBuffer(RenderBufferDesc::IndexBuffer(bytes, RenderHeapType::UPLOAD));
            if (d.indexBuf) { void* p = d.indexBuf->map(); std::memcpy(p, idx.data(), bytes); d.indexBuf->unmap(); }
            if (d.indexBuf) { d.indexCount = uint32_t(idx.size()); d.indexU32 = true; }
        }
    } else if (hdr.index_format != 0 && idxData && hdr.index_bytes) {
        // C-5d: kGuestDMA indexed draw — upload the raw guest indices verbatim (the VS swaps
        // gl_VertexIndex via vertex_index_endian, so no host byte-swap), then drawIndexedInstanced.
        // vertex_count carries the INDEX count for these draws.
        d.indexBuf = c.device->createBuffer(
            RenderBufferDesc::IndexBuffer(hdr.index_bytes, RenderHeapType::UPLOAD));
        if (d.indexBuf) { void* p = d.indexBuf->map(); std::memcpy(p, idxData, hdr.index_bytes); d.indexBuf->unmap(); }
        if (d.indexBuf) { d.indexCount = hdr.vertex_count; d.indexU32 = (hdr.index_format == 2); }
    }
    RenderGraphicsPipelineDesc pd;
    pd.pipelineLayout = d.layout.get();
    pd.vertexShader = d.vs.get();
    pd.pixelShader = d.ps.get();
    pd.renderTargetFormat[0] = kSwapFormat;
    pd.renderTargetBlend[0] = blend;
    pd.renderTargetCount = 1;
    pd.primitiveTopology = topo;
    pd.depthTargetFormat = kDepthFormat;
    // C-5c: per-draw depth/stencil/cull (the packet carries raw Xenos enum values). 2D menu draws
    // have depth_enable=0 -> depth test/write off, so the flat-RT menu composites exactly as before.
    pd.depthEnabled = hdr.depth_enable != 0;
    pd.depthWriteEnabled = hdr.depth_write != 0;
    pd.depthFunction = toPlumeCompare(hdr.depth_func);
    pd.depthClipEnabled = true;
    pd.stencilEnabled = hdr.stencil_enable != 0;
    pd.stencilReadMask = hdr.stencil_read_mask;
    pd.stencilWriteMask = hdr.stencil_write_mask;
    pd.stencilReference = hdr.stencil_ref;
    pd.stencilFrontFace = toPlumeStencilFace(hdr.front_fail_op, hdr.front_pass_op,
                                             hdr.front_depth_fail_op, hdr.front_func);
    pd.stencilBackFace = toPlumeStencilFace(hdr.back_fail_op, hdr.back_pass_op,
                                            hdr.back_depth_fail_op, hdr.back_func);
    // Cull. NHL_HIGHCUT_NOCULL force-disables it at replay time (bring-up). C-5e: use the guest front-
    // face winding (PA_SU_SC_MODE_CNTL.face) DIRECTLY — verified correct on a full face-off scene (ice +
    // closed player meshes). The old code inverted it "to compensate for the y-flip baked into ndc," but
    // that double-flipped: a y-flip reverses on-screen winding AND Vulkan's framebuffer-space front-face
    // determination already accounts for the flipped clip-space, so the net needs NO inversion. The
    // inverted version culled the camera-facing side of closed meshes (players seen front-from-behind,
    // logo mirrored) and culled the ice. NHL_HIGHCUT_FLIP_FACE now re-applies the OLD inversion for A/B.
    static const bool no_cull = std::getenv("NHL_HIGHCUT_NOCULL") != nullptr;
    static const bool flip_face = std::getenv("NHL_HIGHCUT_FLIP_FACE") != nullptr;
    pd.cullMode = no_cull ? RenderCullMode::NONE
                : (hdr.cull_mode == 1 ? RenderCullMode::FRONT
                : (hdr.cull_mode == 2 ? RenderCullMode::BACK : RenderCullMode::NONE));
    bool front_ccw_screen = (hdr.front_ccw != 0);  // guest convention, used directly
    if (flip_face) front_ccw_screen = !front_ccw_screen;
    pd.frontFace = front_ccw_screen ? RenderFrontFace::COUNTER_CLOCKWISE
                                    : RenderFrontFace::CLOCKWISE;
    // by-ID: cache the graphics PIPELINE — PSO creation is the most expensive per-draw op, and a
    // 4000-draw scene shares only a few hundred unique pipelines (this is the main consumer-crash fix).
    // Key = shaders + full pipeline state + layout shape. The debug-PS override uses a fresh uncached
    // module, so it bypasses the cache.
    if (dbgPSactive) {
        d.pipeline = c.device->createGraphicsPipeline(pd);
        if (!d.pipeline) return false;
    } else {
        uint64_t pk = 1469598103934665603ull;
        auto mix = [&](uint64_t v) { pk ^= v; pk *= 1099511628211ull; };
        mix(hdr.vs_shader_id); mix(hdr.ps_shader_id); mix(layoutKey);
        mix(hdr.blend_enable); mix(hdr.blend_src); mix(hdr.blend_dst); mix(hdr.blend_op);
        mix(hdr.blend_src_a); mix(hdr.blend_dst_a); mix(hdr.blend_op_a); mix(hdr.color_write_mask);
        mix(uint32_t(topo));
        mix(hdr.depth_enable); mix(hdr.depth_write); mix(hdr.depth_func);
        mix(hdr.stencil_enable); mix(hdr.stencil_read_mask); mix(hdr.stencil_write_mask); mix(hdr.stencil_ref);
        mix(hdr.front_fail_op); mix(hdr.front_pass_op); mix(hdr.front_depth_fail_op); mix(hdr.front_func);
        mix(hdr.back_fail_op); mix(hdr.back_pass_op); mix(hdr.back_depth_fail_op); mix(hdr.back_func);
        mix(hdr.cull_mode); mix(hdr.front_ccw);
        if (auto it = c.pipelineCache.find(pk); it != c.pipelineCache.end()) {
            d.pipeline = it->second;
        } else {
            std::shared_ptr<RenderPipeline> P = c.device->createGraphicsPipeline(pd);
            if (!P) return false;
            c.pipelineCache.emplace(pk, P);
            d.pipeline = P;
        }
    }
    d.vertexCount = hdr.vertex_count ? hdr.vertex_count : 3;
    d.surfDepthBase = hdr.surface_depth_base;  // C-5d.2: surface bucket key
    d.surfPitch = hdr.surface_pitch;
    d.surfMsaa = hdr.surface_msaa;
    d.vpW = hdr.vp_w; d.vpH = hdr.vp_h;  // C-5h: viewport, for full-res primary-surface pick
    d.scLeft = int32_t(hdr.sc_left); d.scTop = int32_t(hdr.sc_top);
    d.scRight = int32_t(hdr.sc_right); d.scBottom = int32_t(hdr.sc_bottom);
    if (d.scRight <= d.scLeft || d.scBottom <= d.scTop) {  // degenerate -> full RT
        d.scLeft = 0; d.scTop = 0; d.scRight = int32_t(kWidth); d.scBottom = int32_t(kHeight);
    }
    return true;
}

// Defined below LoadC5Frames; forward-declared so LoadC5Frames can call them (defaults live on the
// definitions). GetOrCreateSurfaceRT lazily creates a per-surface offscreen RT; LoadResolveGraph reads
// the C-5d.3 resolve sidecar.
PlumeCtx::SurfaceRT* GetOrCreateSurfaceRT(PlumeCtx& c, uint64_t key, uint32_t w, uint32_t h);
void LoadResolveGraph(PlumeCtx& c);
void ParseResolveGraphBytes(PlumeCtx& c, const std::vector<uint8_t>& buf);

// C-5a: load the captured frame (highcut_frame.count -> highcut_frame_0..N-1.bin) into renderable
// draws. Each packet is self-contained. C-6: with liveDraws != null, load the LIVE-FED frame's packet
// bytes (from the in-memory bridge) instead of disk — called fresh every time a new live frame arrives.
void LoadC5Frames(PlumeCtx& c, const std::vector<std::vector<uint8_t>>* liveDraws = nullptr,
                  const std::vector<uint8_t>* liveResolves = nullptr) {
    const bool live = liveDraws != nullptr;
    if (live) {  // a fresh live frame fully replaces the prior — clear derived per-frame state
        c.c5draws.clear(); c.resolveMap.clear(); c.surfaceDims.clear();
        c.sampledSrcOrder.clear(); c.c5surfaces.clear();
    }
    uint32_t count = 0;
    if (live) count = uint32_t(liveDraws->size());
    else if (FILE* cf = std::fopen("highcut_frame.count", "r")) {
        if (std::fscanf(cf, "%u", &count) != 1) count = 0;
        std::fclose(cf);
    }
    if (!count) { if (!live) REXLOG_INFO("[highcut-C5] no highcut_frame.count — run _c5dump.ps1 first"); return; }
    // C-5d.3: load the resolve graph BEFORE building draws, so BuildRenderableDraw can flag the PS
    // texture slots that sample a resolve dest (host-copy bindings) as it parses each packet.
    if (live) ParseResolveGraphBytes(c, *liveResolves);
    else LoadResolveGraph(c);
    uint32_t built = 0, skipped = 0;
    for (uint32_t i = 0; i < count; ++i) {
        std::vector<uint8_t> diskBytes;
        const std::vector<uint8_t>* bytes = nullptr;
        if (live) {
            bytes = &(*liveDraws)[i];
        } else {
            char path[64];
            std::snprintf(path, sizeof(path), "highcut_frame_%u.bin", i);
            if (FILE* f = std::fopen(path, "rb")) {
                std::fseek(f, 0, SEEK_END);
                long sz = std::ftell(f);
                std::fseek(f, 0, SEEK_SET);
                if (sz > 0) { diskBytes.resize(size_t(sz)); if (std::fread(diskBytes.data(), 1, size_t(sz), f) != size_t(sz)) diskBytes.clear(); }
                std::fclose(f);
            }
            bytes = &diskBytes;
        }
        if (bytes->empty()) { ++skipped; continue; }
        RenderableDraw d;
        if (BuildRenderableDraw(c, *bytes, d, i)) { c.c5draws.push_back(std::move(d)); ++built; }
        else ++skipped;
    }
    REXLOG_INFO("[highcut-C5{}] loaded {} renderable draws ({} skipped) of {} {} owned draws",
                live ? "-LIVE" : "", built, skipped, count, live ? "live-fed" : "captured");

    // C-5d.2: pick the PRIMARY guest surface = the (depth_base,pitch,msaa) tuple with the most draws
    // (the main scene). ONLY that surface renders to the swapchain; EVERY other surface — incl. a
    // second same-pitch surface that's a distinct guest depth buffer (e.g. 640/2X vs 640/1X) — renders
    // into its OWN offscreen color+depth RT. This matters: lumping two guest depth buffers onto one
    // shared host depth gives wrong occlusion (large areas depth-killed → the "squash"). Override the
    // pick with NHL_HIGHCUT_C5_PRIMARY_PITCH (+ optional NHL_HIGHCUT_C5_PRIMARY_DEPTH) to bisect which
    // surface is the final image; NHL_HIGHCUT_C5_NOSPLIT forces the old all-into-swapchain (= C-5c).
    // Default primary = the surface with the most TEXTURED draws (the opaque scene with real game art,
    // e.g. depth=360 here), NOT the most draws — the largest surface is often an UNTEXTURED aux pass
    // (a depth/shadow prepass or a reflection pass that samples a screen-size resolve) that renders
    // black on its own. Tie-break on total draws.
    // C-5h: the main camera renders at the LARGEST viewport (full resolution). In a normal scene that's
    // also the most-drawn/most-textured surface, but in an INSTANT-REPLAY / multi-pass frame a HALF-RES
    // aux pass (a reflection or picture-in-picture camera at e.g. 640x360) can out-COUNT the full-res
    // broadcast view (1280x720) — the old "most textured draws" pick then upscaled the wrong camera (it
    // looked like the view was sunk into the ice). Rank: any-textured beats none (skip untextured depth/
    // shadow prepasses), then LARGEST viewport area, then most textured. Manual override still wins below.
    std::unordered_map<uint64_t, uint32_t> keyCounts, keyTexCounts;
    std::unordered_map<uint64_t, uint64_t> keyMaxVpArea;
    for (const auto& d : c.c5draws) {
        const uint64_t k = SurfaceKey(d.surfDepthBase, d.surfPitch, d.surfMsaa);
        ++keyCounts[k];
        if (d.textured) ++keyTexCounts[k];
        const uint64_t area = uint64_t(d.vpW > 0 ? d.vpW : 0) * uint64_t(d.vpH > 0 ? d.vpH : 0);
        if (area > keyMaxVpArea[k]) keyMaxVpArea[k] = area;
        // C-5d.3: a surface's logical size = the largest per-draw viewport it carries (the surface is
        // rendered into an offscreen RT of this size so a host-copy consumer's [0,1] UV maps to it).
        auto& dim = c.surfaceDims[k];
        dim.first = std::max(dim.first, uint32_t(d.vpW > 0 ? d.vpW : 0));
        dim.second = std::max(dim.second, uint32_t(d.vpH > 0 ? d.vpH : 0));
    }
    uint64_t bestKey = 0, bestArea = 0; uint32_t bestTex = 0, bestN = 0;
    for (const auto& kv : keyCounts) {
        const uint32_t tex = keyTexCounts[kv.first];
        const uint64_t area = keyMaxVpArea[kv.first];
        bool better;
        if ((tex > 0) != (bestTex > 0))      better = (tex > 0);          // any textured beats none
        else if (area != bestArea)           better = (area > bestArea);  // then largest viewport
        else if (tex != bestTex)             better = (tex > bestTex);    // then most textured
        else                                 better = (kv.second > bestN);// then most draws
        if (bestKey == 0 || better) { bestKey = kv.first; bestArea = area; bestTex = tex; bestN = kv.second; }
    }
    if (const char* op = std::getenv("NHL_HIGHCUT_C5_PRIMARY_PITCH")) {
        const uint32_t wantPitch = uint32_t(std::strtoul(op, nullptr, 10));
        const char* od = std::getenv("NHL_HIGHCUT_C5_PRIMARY_DEPTH");
        const uint32_t wantDepth = od ? uint32_t(std::strtoul(od, nullptr, 10)) : 0xFFFFFFFFu;
        uint64_t k2 = 0; uint32_t n2 = 0;  // most-drawn surface matching the requested pitch (+depth)
        for (const auto& d : c.c5draws) {
            if (d.surfPitch != wantPitch) continue;
            if (wantDepth != 0xFFFFFFFFu && d.surfDepthBase != wantDepth) continue;
            const uint64_t k = SurfaceKey(d.surfDepthBase, d.surfPitch, d.surfMsaa);
            if (keyCounts[k] > n2) { n2 = keyCounts[k]; k2 = k; }
        }
        if (k2) { bestKey = k2; bestN = n2; bestTex = keyTexCounts[k2]; }
    }
    c.c5PrimaryKey = bestKey;
    auto unkPitch = [](uint64_t k) { return uint32_t((k >> 8) & 0xFFFFFF); };
    auto unkDepth = [](uint64_t k) { return uint32_t(k >> 32); };
    auto unkMsaa  = [](uint64_t k) { return uint32_t(k & 0xFF); };
    REXLOG_INFO("[highcut-C5d] PRIMARY surface = depth={} pitch={} msaa={} ({} draws, {} textured); "
                "{} distinct surfaces:", unkDepth(bestKey), unkPitch(bestKey), unkMsaa(bestKey),
                keyCounts[bestKey], bestTex, uint32_t(keyCounts.size()));
    for (const auto& kv : keyCounts)
        REXLOG_INFO("[highcut-C5d]   surface depth={} pitch={} msaa={} -> {} draws ({} textured) ({})",
                    unkDepth(kv.first), unkPitch(kv.first), unkMsaa(kv.first), kv.second,
                    keyTexCounts[kv.first], kv.first == c.c5PrimaryKey ? "PRIMARY -> swapchain" : "offscreen RT");

    // C-5d.3 "Resolve = host copy": for every draw that samples a resolve dest, create the SOURCE
    // surface's offscreen RT (sized to that surface) and re-point the draw's set3 slot at the source
    // RT's sampleable color/depth view. The render loop renders these source surfaces before the
    // primary pass and barriers them to SHADER_READ, so the consumer samples our rendered content.
    // C-5m: DEFAULT ON in "depth" mode. The host-copy re-points only genuinely-STUBBED bindings (see
    // BuildRenderableDraw `capturedStub`); since readback-resolve (C-5l) made the COLOR resolves sample
    // real guest RAM, the only stubbed resolve-dest bindings left are the k_24_8 DEPTH/shadow maps. So
    // the default re-points the goalie's shadow-map samples at the rendered shadow surface -> real self-
    // shadowing, verified (66 depth rebinds, 0 color, 0 Vulkan errors, no ice/jersey regression).
    // Opt out fully with NHL_HIGHCUT_NO_COMPOSITE. NHL_HIGHCUT_C5_COMPOSITE still OVERRIDES the mode for
    // diagnostics: "depth"=only depth, "color"=only color (reflection/RTT), any other value=both.
    if (std::getenv("NHL_HIGHCUT_NO_COMPOSITE")) return;
    const char* compEnv = std::getenv("NHL_HIGHCUT_C5_COMPOSITE");
    const char* mode = compEnv ? compEnv : "depth";  // default-on = depth-only
    const bool wantDepth = std::strcmp(mode, "color") != 0;
    const bool wantColor = std::strcmp(mode, "depth") != 0;
    uint32_t reboundColor = 0, reboundDepth = 0, deferred = 0, skippedClass = 0;
    std::unordered_set<uint64_t> sampledSeen;
    for (auto& d : c.c5draws) {
        for (const auto& hb : d.hostCopy) {
            // The HUD / self-sampling case (source == the primary swapchain surface) has no offscreen RT
            // and needs mid-pass snapshot ordering — defer it to the HUD step.
            if (hb.srcKey == c.c5PrimaryKey) { ++deferred; continue; }
            if (hb.isDepth ? !wantDepth : !wantColor) { ++skippedClass; continue; }
            auto dim = c.surfaceDims.count(hb.srcKey) ? c.surfaceDims[hb.srcKey] : std::make_pair(0u, 0u);
            PlumeCtx::SurfaceRT* rt = GetOrCreateSurfaceRT(c, hb.srcKey, dim.first, dim.second);
            if (!rt || !d.set3) continue;
            if (!sampledSeen.count(hb.srcKey)) { sampledSeen.insert(hb.srcKey); c.sampledSrcOrder.push_back(hb.srcKey); }
            if (hb.isDepth) {
                d.set3->setTexture(hb.slot, rt->depth.get(), RenderTextureLayout::SHADER_READ, rt->depthSRV.get());
                ++reboundDepth;
            } else {
                d.set3->setTexture(hb.slot, rt->color.get(), RenderTextureLayout::SHADER_READ, rt->colorSRV.get());
                ++reboundColor;
            }
        }
    }
    REXLOG_INFO("[highcut-C5d3] composite mode='{}': {} source surfaces; re-pointed {} color + {} depth "
                "bindings ({} deferred primary-source/HUD, {} skipped by mode)", mode,
                uint32_t(c.sampledSrcOrder.size()), reboundColor, reboundDepth, deferred, skippedClass);
}

// C-5d.2: lazily create + cache the offscreen color+depth+stencil RT for a NON-primary guest surface.
// Sized to the swapchain (the draws' ndc already fills clip space regardless of the guest surface
// size, and per-draw scissors are in swapchain coords). Color=kSwapFormat, depth=kDepthFormat at 1X so
// it stays renderpass-compatible with the per-draw pipelines (which declare exactly those formats).
PlumeCtx::SurfaceRT* GetOrCreateSurfaceRT(PlumeCtx& c, uint64_t key, uint32_t w = 0, uint32_t h = 0) {
    auto it = c.c5surfaces.find(key);
    if (it != c.c5surfaces.end()) return &it->second;
    // Size to the GUEST surface when known (so a consumer sampling [0,1] reads the full rendered
    // content), else fall back to the swapchain size (legacy isolation-only behavior).
    if (!w || !h) { w = c.swap->getWidth(); h = c.swap->getHeight(); }
    PlumeCtx::SurfaceRT s;
    s.w = w; s.h = h;
    s.color = c.device->createTexture(RenderTextureDesc::ColorTarget(w, h, kSwapFormat));
    s.depth = c.device->createTexture(RenderTextureDesc::DepthTarget(w, h, kDepthFormat));
    if (!s.color || !s.depth) { REXLOG_ERROR("[highcut-C5d] offscreen surface RT create failed"); return nullptr; }
    RenderFramebufferDesc fd;
    const RenderTexture* col = s.color.get();
    fd.colorAttachments = &col;
    fd.colorAttachmentsCount = 1;
    fd.depthAttachment = s.depth.get();
    s.fb = c.device->createFramebuffer(fd);
    if (!s.fb) { REXLOG_ERROR("[highcut-C5d] offscreen surface framebuffer create failed"); return nullptr; }
    // C-5d.3: sampleable views so a host-copy consumer can bind this surface as a texture. The view
    // aspect is derived from the texture flags (color vs depth) by plume; the depth view samples the
    // depth aspect of D32_FLOAT_S8_UINT (shadow/depth maps). identity component mapping.
    s.colorSRV = s.color->createTextureView(RenderTextureViewDesc::Texture2D(kSwapFormat));
    s.depthSRV = s.depth->createTextureView(RenderTextureViewDesc::Texture2D(kDepthFormat));
    auto res = c.c5surfaces.emplace(key, std::move(s));
    return &res.first->second;
}

// C-5d.3: load the guest EDRAM resolve graph (highcut_resolves.bin) and build dest_addr -> source
// surface. Last writer wins for a re-resolved dest (file order == capture stream order); the source
// SurfaceKey is what we'll render offscreen and host-copy from.
// C-6: parse the resolve sidecar from a byte buffer (disk file OR the live-feed commit) into resolveMap.
void ParseResolveGraphBytes(PlumeCtx& c, const std::vector<uint8_t>& buf) {
    namespace hc = nhl::highcut;
    if (buf.size() < 8) { REXLOG_INFO("[highcut-C5d3] no resolve graph — composition has none"); return; }
    uint32_t magic = 0, count = 0;
    std::memcpy(&magic, &buf[0], 4); std::memcpy(&count, &buf[4], 4);
    if (magic != hc::kResolveSidecarMagic) { REXLOG_INFO("[highcut-C5d3] resolve sidecar bad magic"); return; }
    size_t off = 8;
    for (uint32_t i = 0; i < count && off + sizeof(hc::ResolveMarker) <= buf.size(); ++i, off += sizeof(hc::ResolveMarker)) {
        hc::ResolveMarker m{};
        std::memcpy(&m, &buf[off], sizeof(m));
        const uint64_t srcKey = SurfaceKey(m.src_depth_base, m.src_pitch, m.src_msaa);
        c.resolveMap[m.dest_addr] = PlumeCtx::ResolveEntry{srcKey, m.is_depth != 0};
    }
    REXLOG_INFO("[highcut-C5d3] loaded {} resolve dest mappings from {} markers", uint32_t(c.resolveMap.size()), count);
}

void LoadResolveGraph(PlumeCtx& c) {
    namespace hc = nhl::highcut;
    std::vector<uint8_t> buf;
    if (FILE* f = std::fopen("highcut_resolves.bin", "rb")) {
        std::fseek(f, 0, SEEK_END); long sz = std::ftell(f); std::fseek(f, 0, SEEK_SET);
        if (sz > 0) { buf.resize(size_t(sz)); if (std::fread(buf.data(), 1, size_t(sz), f) != size_t(sz)) buf.clear(); }
        std::fclose(f);
    }
    ParseResolveGraphBytes(c, buf);
}

void RenderClear(PlumeCtx& c) {
    // C-5a: load the captured frame once, before touching the swapchain (resource creation +
    // texture uploads use the queue, independent of the frame). Gated NHL_HIGHCUT_C5.
    static const bool c5_mode = std::getenv("NHL_HIGHCUT_C5") != nullptr;
    // C-6 LIVE FEED: rebuild the renderable draws from the in-memory bridge whenever the CP thread
    // commits a new frame (g_liveSeq bumps). Replaces the once-from-disk load with a per-new-frame
    // rebuild so plume tracks the live game. (First cut rebuilds every draw's pipeline per frame —
    // correctness over speed; a draw/pipeline cache is the next increment.)
    static const bool live_feed = std::getenv("NHL_HIGHCUT_LIVE_FEED") != nullptr;
    if (c5_mode && live_feed) {
        const uint64_t seq = g_liveSeq.load(std::memory_order_acquire);
        if (seq != c.liveSeqSeen) {
            // Step 2: drain the resource dictionary FIRST (append-only, never dropped), so every shader/
            // texture id a draw references is in c.resourceBytes before the rebuild looks it up.
            {
                std::vector<std::pair<uint64_t, std::vector<uint8_t>>> drained;
                { std::lock_guard<std::mutex> lk(g_resMutex); drained.swap(g_resourcePending); }
                for (auto& kv : drained) c.resourceBytes[kv.first] = std::move(kv.second);
            }
            std::vector<std::vector<uint8_t>> draws;
            std::vector<uint8_t> resolves;
            { std::lock_guard<std::mutex> lk(g_liveMutex);
              draws.swap(g_livePendingDraws); resolves.swap(g_livePendingResolves); }
            if (!draws.empty()) { LoadC5Frames(c, &draws, &resolves); c.liveSeqSeen = seq; }
        }
    } else if (c5_mode && !c.c5loaded) { c.c5loaded = true; LoadC5Frames(c); }
    uint32_t idx = 0;
    if (c.swap->isEmpty() || !c.swap->acquireTexture(c.acquireSem.get(), &idx)) {
        c.swap->resize();
        CreateFramebuffers(c);
        return;
    }

    c.cmd->begin();
    RenderTexture* tex = c.swap->getTexture(idx);
    // C-5c: transition color -> COLOR_WRITE and the shared depth-stencil -> DEPTH_WRITE for this pass.
    {
        const RenderTextureBarrier b[] = {
            RenderTextureBarrier(tex, RenderTextureLayout::COLOR_WRITE),
            RenderTextureBarrier(c.depthTex.get(), RenderTextureLayout::DEPTH_WRITE),
        };
        c.cmd->barriers(RenderBarrierStage::GRAPHICS, b, c.depthTex ? 2u : 1u);
    }
    c.cmd->setFramebuffer(c.fbs[idx].get());

    const uint32_t w = c.swap->getWidth(), h = c.swap->getHeight();
    c.cmd->setViewports(RenderViewport(0.0f, 0.0f, float(w), float(h)));
    c.cmd->setScissors(RenderRect(0, 0, w, h));
    // C-5c: clear depth to the guest far value (1.0) + stencil to 0 every frame. The renderpass
    // load-ops LOAD the attachment, so it must hold a defined value; do it once per frame here.
    if (c.depthTex) c.cmd->clearDepthStencil(true, true, 1.0f, 0);

    // C-5: clear to BLACK — the guest framebuffer base behind the menu. (C-1/C-3 used an animated
    // teal/green clear as a "window alive" indicator, but that green showed THROUGH the menu's
    // low-alpha overlays — e.g. the bottom nav bars, draws 121/122: a black BC3 texture at alpha
    // ~0.13 over the clear = ~87% clear = bright green. Real game clears black there -> dark bars.)
    if (c5_mode) {
        // Diagnostic: NHL_HIGHCUT_C5_CLEAR=R,G,B (0..255) overrides the black clear, so a pass whose
        // draws output black (opaque One/Zero) shows as BLACK SILHOUETTES on the bright clear — reveals
        // whether a "black" surface actually has full-screen geometry (the scene) or is itself a band.
        static const RenderColor c5clear = []() {
            if (const char* s = std::getenv("NHL_HIGHCUT_C5_CLEAR")) {
                int r = 0, g = 0, b = 0; std::sscanf(s, "%d,%d,%d", &r, &g, &b);
                return RenderColor(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
            }
            return RenderColor(0.0f, 0.0f, 0.0f, 1.0f);
        }();
        c.cmd->clearColor(0, c5clear);
    } else {
        const float t = (c.frame % 120) / 120.0f;
        c.cmd->clearColor(0, RenderColor(0.1f, t, 0.2f + 0.3f * t, 1.0f));
    }

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
                    // C-3b.3: peek the draw packet's topology so the pipeline is created with the
                    // primitive the translated VS expects (RectangleList -> rect-strip VS -> a
                    // 4-vertex TRIANGLE_STRIP; else TRIANGLE_LIST).
                    auto pktTopo = RenderPrimitiveTopology::TRIANGLE_LIST;
                    if (FILE* tf = std::fopen("highcut_p3_draw.bin", "rb")) {
                        nhl::highcut::DrawPacketHeader th{};
                        if (std::fread(&th, 1, sizeof(th), tf) == sizeof(th) &&
                            th.magic == nhl::highcut::kDrawPacketMagic &&
                            th.topology == nhl::highcut::kTopoTriangleStrip)
                            pktTopo = RenderPrimitiveTopology::TRIANGLE_STRIP;
                        std::fclose(tf);
                    }
                    c.xlatPS = c.device->createShader(solidFragBlobSPIRV, sizeof(solidFragBlobSPIRV), "PSMain", fmt);
                    REXLOG_INFO("[highcut-C3a] step: solid PS module {} (layout mode='{}')",
                                c.xlatPS ? "ok" : "null", emptyLayout ? "empty" : "full");

                    RenderDescriptorSetBuilder set0, set1;
                    RenderPipelineLayoutBuilder lb;
                    lb.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
                    RenderDescriptorSetBuilder set2;  // C-3b.2: PS fragment-counter UAV (space2/u0)
                    if (!emptyLayout) {
                        set0.begin(); set0.addByteAddressBuffer(0); set0.end();           // shared memory
                        set1.begin();                                                     // constants 0..4
                        for (uint32_t b = 0; b <= 4; ++b) set1.addConstantBuffer(b);       // sys,vsF,psF,bool,fetch
                        set1.end();
                        set2.begin(); set2.addReadWriteByteAddressBuffer(0); set2.end();  // frag counter
                        lb.addDescriptorSet(set0);  // -> set 0
                        lb.addDescriptorSet(set1);  // -> set 1
                        lb.addDescriptorSet(set2);  // -> set 2
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
                        pd.depthTargetFormat = kDepthFormat;  // C-5c: framebuffer has depth (test off)
                        pd.primitiveTopology = pktTopo;
                        c.xlatPipeline = c.device->createGraphicsPipeline(pd);
                        REXLOG_INFO("[highcut-C3a] graphics pipeline (layout={}): {} "
                                    "— any pipeline/driver error is on stderr.",
                                    emptyLayout ? "empty" : "set0=SSBO@0,set1=CBV@0..4,set2=UAV@0",
                                    c.xlatPipeline ? "CREATED" : "FAILED (null)");

                        // C-3b.1: create + bind the VS's descriptor buffers so it can be drawn.
                        // Zero-filled for now (proves the bind+draw path is validation-clean before
                        // real decoded data goes in at C-3b.2). Sizes: system constants UBO (>= the
                        // std140 SystemConstants struct), bool/loop UBO (10 uvec4), fetch UBO (48
                        // uvec4 = 32 fetch constants x 6 dwords), shared-memory SSBO (scratch).
                        if (c.xlatPipeline && !emptyLayout) {
                            auto mkUbo = [&](uint64_t sz) {
                                auto b = c.device->createBuffer(
                                    RenderBufferDesc::UploadBuffer(sz, RenderBufferFlag::CONSTANT));
                                if (b) { void* p = b->map(); std::memset(p, 0, sz); b->unmap(); }
                                return b;
                            };
                            constexpr uint64_t kSysSize = 2048, kBoolSize = 256, kFetchSize = 768,
                                               kSharedSize = 1u << 16, kFloatSize = 256 * 16;
                            c.xlatSysBuf = mkUbo(kSysSize);
                            c.xlatBoolBuf = mkUbo(kBoolSize);
                            c.xlatFetchBuf = mkUbo(kFetchSize);
                            // C-4: VS/PS float-constant UBOs (set1 bindings 1/2). The solid+counter
                            // pass needs the VS floats too (most real VSs put their transform matrix
                            // here) or every vertex collapses to 0 -> 0 fragments. Shared with the
                            // textured pass (CreateTexturedDraw reuses these, doesn't recreate).
                            c.xlatVsFloatBuf = mkUbo(kFloatSize);
                            c.xlatPsFloatBuf = mkUbo(kFloatSize);
                            c.xlatSharedBuf = c.device->createBuffer(
                                RenderBufferDesc::UploadBuffer(kSharedSize, RenderBufferFlag::STORAGE));
                            if (c.xlatSharedBuf) { void* p = c.xlatSharedBuf->map(); std::memset(p, 0, kSharedSize); c.xlatSharedBuf->unmap(); }

                            constexpr uint64_t kCounterSize = 256;
                            c.xlatCounterBuf = c.device->createBuffer(RenderBufferDesc::UploadBuffer(
                                kCounterSize, RenderBufferFlag::STORAGE | RenderBufferFlag::UNORDERED_ACCESS));
                            if (c.xlatCounterBuf) { void* p = c.xlatCounterBuf->map(); std::memset(p, 0, kCounterSize); c.xlatCounterBuf->unmap(); }
                            RenderDescriptorSetBuilder ds0, ds1, ds2;
                            ds0.begin(); ds0.addByteAddressBuffer(0); ds0.end();
                            ds1.begin(); for (uint32_t b = 0; b <= 4; ++b) ds1.addConstantBuffer(b); ds1.end();
                            ds2.begin(); ds2.addReadWriteByteAddressBuffer(0); ds2.end();
                            c.xlatSet0 = ds0.create(c.device.get());
                            c.xlatSet1 = ds1.create(c.device.get());
                            c.xlatSet2 = ds2.create(c.device.get());
                            if (c.xlatSet0 && c.xlatSet1 && c.xlatSet2 && c.xlatSysBuf && c.xlatBoolBuf &&
                                c.xlatFetchBuf && c.xlatSharedBuf && c.xlatCounterBuf && c.xlatVsFloatBuf &&
                                c.xlatPsFloatBuf) {
                                c.xlatSet0->setBuffer(0, c.xlatSharedBuf.get(), kSharedSize);
                                c.xlatSet1->setBuffer(0, c.xlatSysBuf.get(), kSysSize);
                                c.xlatSet1->setBuffer(1, c.xlatVsFloatBuf.get(), kFloatSize);
                                c.xlatSet1->setBuffer(2, c.xlatPsFloatBuf.get(), kFloatSize);
                                c.xlatSet1->setBuffer(3, c.xlatBoolBuf.get(), kBoolSize);
                                c.xlatSet1->setBuffer(4, c.xlatFetchBuf.get(), kFetchSize);
                                c.xlatSet2->setBuffer(0, c.xlatCounterBuf.get(), kCounterSize);
                                c.xlatDrawReady = true;

                                // C-3b.2: load the decoded-draw packet and fill the buffers with
                                // REAL data (system + fetch constants, shared-memory vertex bytes).
                                // If absent, the buffers stay zeroed (C-3b.1 mechanics-only).
                                if (FILE* pf = std::fopen("highcut_p3_draw.bin", "rb")) {
                                    nhl::highcut::DrawPacketHeader hdr{};
                                    if (std::fread(&hdr, 1, sizeof(hdr), pf) == sizeof(hdr) &&
                                        hdr.magic == nhl::highcut::kDrawPacketMagic) {
                                        auto fillBuf = [&](RenderBuffer* b, uint64_t cap, uint32_t n) {
                                            if (!b || !n) return;
                                            std::vector<uint8_t> tmp(n);
                                            if (std::fread(tmp.data(), 1, n, pf) != n) return;
                                            void* p = b->map();
                                            std::memcpy(p, tmp.data(), n < cap ? n : cap);
                                            b->unmap();
                                        };
                                        // Read in packet order: fetch, sys, shared, bool, vs-float,
                                        // ps-float (the rest — ps_spirv + textures — is read by
                                        // CreateTexturedDraw, which re-opens the file).
                                        fillBuf(c.xlatFetchBuf.get(), kFetchSize, hdr.fetch_bytes);
                                        fillBuf(c.xlatSysBuf.get(), kSysSize, hdr.sys_bytes);
                                        fillBuf(c.xlatSharedBuf.get(), kSharedSize, hdr.shared_bytes);
                                        fillBuf(c.xlatBoolBuf.get(), kBoolSize, hdr.bool_bytes);
                                        fillBuf(c.xlatVsFloatBuf.get(), kFloatSize, hdr.vs_float_bytes);
                                        fillBuf(c.xlatPsFloatBuf.get(), kFloatSize, hdr.ps_float_bytes);
                                        c.xlatVertexCount = hdr.vertex_count ? hdr.vertex_count : 3;
                                        REXLOG_INFO("[highcut-C3b2] filled buffers from packet: verts={} "
                                                    "fetch={} sys={} shared={} bool={} vsF={} psF={} topo={}",
                                                    hdr.vertex_count, hdr.fetch_bytes, hdr.sys_bytes,
                                                    hdr.shared_bytes, hdr.bool_bytes, hdr.vs_float_bytes,
                                                    hdr.ps_float_bytes, hdr.topology);
                                    }
                                    std::fclose(pf);
                                } else {
                                    REXLOG_INFO("[highcut-C3b] no draw packet — buffers zeroed (mechanics only)");
                                }
                                // C-4: if the packet carries a translated PS + texture(s), build the
                                // textured pipeline (samples the real guest texture). No-op for a
                                // VS-only (C-3) packet — the solid+counter path above is unaffected.
                                CreateTexturedDraw(c);
                            } else {
                                REXLOG_ERROR("[highcut-C3b] descriptor buffer/set creation failed");
                            }
                        }
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
    // C-1 bring-up triangle (vertex-colored) — kept as a "window alive" control alongside the
    // C-3b translated draw (solid orange), so both are visible in the plume window.
    if (!c5_mode && c.pipeline && c.vbuf) {
        c.cmd->setGraphicsPipelineLayout(c.pipelineLayout.get());
        c.cmd->setPipeline(c.pipeline.get());
        c.cmd->setVertexBuffers(0, &c.vbView, 1, &c.inputSlot);
        c.cmd->drawInstanced(3, 1, 0, 0);
    }

    // C-5a/d.2: replay the captured frame's owned draws, routed by guest render surface. DEFAULT: only
    // the PRIMARY surface (the visible scene) renders to the swapchain; every other surface (the
    // depth=184 pass, the 384^2 shadow/RTT pass, the 320x180 stencil masks) is DROPPED — the resolve
    // graph showed nothing samples them, so a mask's Always-depth write and the shadow pass can't
    // pollute the main scene (Problem 1) AND we don't pay to render them. Each primary draw carries its
    // own pipeline + per-draw blend/depth/stencil/cull; ndc fills clip space so the full swapchain
    // viewport is correct. Gates:
    //   NHL_HIGHCUT_C5_NOSPLIT    — render ALL surfaces to the swapchain (= C-5c baseline).
    //   NHL_HIGHCUT_C5_OFFSCREEN  — render the non-primary surfaces into their OWN offscreen color+
    //                               depth+stencil RTs (the C-5d.3 Resolve=host-copy foundation; pure
    //                               overhead today since nothing samples them).
    //   NHL_HIGHCUT_C5_PRIMARY_PITCH/_DEPTH — override which surface is the main scene.
    if (c5_mode && !c.c5draws.empty()) {
        // Bisection: replay only draws [MINDRAW, MAXDRAW) to isolate an artifact to a draw index.
        static const uint32_t c5_min = []() { const char* s = std::getenv("NHL_HIGHCUT_C5_MINDRAW"); return s ? uint32_t(std::strtoul(s, nullptr, 10)) : 0u; }();
        static const uint32_t c5_max = []() { const char* s = std::getenv("NHL_HIGHCUT_C5_MAXDRAW"); return s ? uint32_t(std::strtoul(s, nullptr, 10)) : 0xFFFFFFFFu; }();
        static const bool no_scissor = std::getenv("NHL_HIGHCUT_NO_SCISSOR") != nullptr;
        static const bool no_split = std::getenv("NHL_HIGHCUT_C5_NOSPLIT") != nullptr;
        // Opt-in: render non-primary surfaces offscreen (C-5d.3 foundation). Default OFF — they're
        // never sampled, so rendering them is wasted GPU work; dropping them gives the same image.
        static const bool render_offscreen = std::getenv("NHL_HIGHCUT_C5_OFFSCREEN") != nullptr;
        auto inWindow = [&](uint32_t i) { return i >= c5_min && i < c5_max; };
        // Bind+draw one renderable draw into the currently-bound framebuffer (its own pipeline carries
        // blend/depth/stencil/cull + topology). The per-draw scissor is in swapchain coords; offscreen
        // RTs are swapchain-sized so it stays in range.
        auto renderDraw = [&](RenderableDraw& d, const RenderRect* scOverride = nullptr) {
            if (scOverride) c.cmd->setScissors(*scOverride);  // composite source pass: full-surface clip
            else if (no_scissor) c.cmd->setScissors(RenderRect(0, 0, kWidth, kHeight));
            else c.cmd->setScissors(RenderRect(d.scLeft, d.scTop, d.scRight, d.scBottom));
            c.cmd->setGraphicsPipelineLayout(d.layout.get());
            c.cmd->setPipeline(d.pipeline.get());
            c.cmd->setGraphicsDescriptorSet(d.set0.get(), 0);
            c.cmd->setGraphicsDescriptorSet(d.set1.get(), 1);
            if (d.set2) c.cmd->setGraphicsDescriptorSet(d.set2.get(), 2);  // C-5d.3: VS skinning textures
            if (d.textured) c.cmd->setGraphicsDescriptorSet(d.set3.get(), 3);
            if (d.indexCount > 0) {  // quad-list expansion (R32) or kGuestDMA (R16/R32)
                const uint32_t istride = d.indexU32 ? 4u : 2u;
                RenderIndexBufferView iv(d.indexBuf.get(), d.indexCount * istride,
                                         d.indexU32 ? RenderFormat::R32_UINT : RenderFormat::R16_UINT);
                c.cmd->setIndexBuffer(&iv);
                c.cmd->drawIndexedInstanced(d.indexCount, 1, 0, 0, 0);
            } else {
                c.cmd->drawInstanced(d.vertexCount, 1, 0, 0);
            }
        };
        auto isPrimary = [&](const RenderableDraw& d) {
            return no_split || SurfaceKey(d.surfDepthBase, d.surfPitch, d.surfMsaa) == c.c5PrimaryKey;
        };

        // C-5d.3 "Resolve = host copy": render each SAMPLED source surface (shadow map, reflection RTT)
        // into its own correctly-sized offscreen RT, then barrier it to SHADER_READ. The primary draws
        // that sample its resolve dest were re-pointed (at load) at this RT's color/depth view, so they
        // now read our rendered content. Render order = first-seen sampling order; each source surface
        // is a full pass (own viewport+clear). C-5m: DEFAULT ON (opt out NHL_HIGHCUT_NO_COMPOSITE) —
        // matches the load-time re-point gate. Needs the full draw range (the source surface's own draws
        // must replay), so a single-draw isolation (-Draw) skips it. The HUD / self-sampling case
        // (source == primary) is deferred and handled in the HUD step.
        static const bool composite = std::getenv("NHL_HIGHCUT_NO_COMPOSITE") == nullptr;
        if (composite && !c.sampledSrcOrder.empty()) {
            for (uint64_t k : c.sampledSrcOrder) {
                PlumeCtx::SurfaceRT* s = GetOrCreateSurfaceRT(c, k);  // created at load with guest dims
                if (!s) continue;
                const RenderTextureBarrier bb[] = {
                    RenderTextureBarrier(s->color.get(), RenderTextureLayout::COLOR_WRITE),
                    RenderTextureBarrier(s->depth.get(), RenderTextureLayout::DEPTH_WRITE),
                };
                c.cmd->barriers(RenderBarrierStage::GRAPHICS, bb, 2);
                c.cmd->setFramebuffer(s->fb.get());
                c.cmd->setViewports(RenderViewport(0.0f, 0.0f, float(s->w), float(s->h)));
                const RenderRect full(0, 0, int32_t(s->w), int32_t(s->h));
                c.cmd->setScissors(full);
                c.cmd->clearColor(0, RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
                c.cmd->clearDepthStencil(true, true, 1.0f, 0);
                for (uint32_t i = 0; i < c.c5draws.size(); ++i) {
                    if (!inWindow(i)) continue;
                    auto& d = c.c5draws[i];
                    if (SurfaceKey(d.surfDepthBase, d.surfPitch, d.surfMsaa) == k) renderDraw(d, &full);
                }
                const RenderTextureBarrier rb[] = {
                    RenderTextureBarrier(s->color.get(), RenderTextureLayout::SHADER_READ),
                    RenderTextureBarrier(s->depth.get(), RenderTextureLayout::SHADER_READ),
                };
                c.cmd->barriers(RenderBarrierStage::GRAPHICS, rb, 2);
            }
            // Re-bind the swapchain (still COLOR_WRITE/DEPTH_WRITE from RenderClear; its earlier clear is
            // LOADed) for the primary pass, which now samples the source RTs via the re-pointed set3.
            c.cmd->setFramebuffer(c.fbs[idx].get());
            c.cmd->setViewports(RenderViewport(0.0f, 0.0f, float(w), float(h)));
            c.cmd->setScissors(RenderRect(0, 0, w, h));
        }

        // C-5d.2 (opt-in via NHL_HIGHCUT_C5_OFFSCREEN): render each NON-primary surface's draws into its
        // own offscreen RT (grouped by surface, capture order preserved within a surface). Each is its
        // own render pass (barriers + setFramebuffer end the prior pass; a fresh clear+draws begin a new
        // one), so the mask/RTT passes get their OWN depth+stencil and never touch the swapchain. This
        // is the C-5d.3 Resolve=host-copy foundation; off by default (nothing samples the result yet).
        if (!no_split && render_offscreen) {
            std::vector<uint64_t> order;
            std::unordered_map<uint64_t, char> seen;
            for (uint32_t i = 0; i < c.c5draws.size(); ++i) {
                if (!inWindow(i) || isPrimary(c.c5draws[i])) continue;
                const auto& d = c.c5draws[i];
                const uint64_t k = SurfaceKey(d.surfDepthBase, d.surfPitch, d.surfMsaa);
                if (!seen[k]) { seen[k] = 1; order.push_back(k); }
            }
            for (uint64_t k : order) {
                PlumeCtx::SurfaceRT* s = GetOrCreateSurfaceRT(c, k);
                if (!s) continue;
                const RenderTextureBarrier bb[] = {
                    RenderTextureBarrier(s->color.get(), RenderTextureLayout::COLOR_WRITE),
                    RenderTextureBarrier(s->depth.get(), RenderTextureLayout::DEPTH_WRITE),
                };
                c.cmd->barriers(RenderBarrierStage::GRAPHICS, bb, 2);
                c.cmd->setFramebuffer(s->fb.get());
                c.cmd->setViewports(RenderViewport(0.0f, 0.0f, float(w), float(h)));
                c.cmd->clearColor(0, RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
                c.cmd->clearDepthStencil(true, true, 1.0f, 0);
                for (uint32_t i = 0; i < c.c5draws.size(); ++i) {
                    if (!inWindow(i) || isPrimary(c.c5draws[i])) continue;
                    auto& d = c.c5draws[i];
                    if (SurfaceKey(d.surfDepthBase, d.surfPitch, d.surfMsaa) == k) renderDraw(d);
                }
            }
            // Re-bind the swapchain for the primary draws; its color+depth keep their earlier clear
            // (renderpass LOAD), and they were left in COLOR_WRITE/DEPTH_WRITE (only offscreen RTs were
            // transitioned), so no extra barrier is needed.
            c.cmd->setFramebuffer(c.fbs[idx].get());
            c.cmd->setViewports(RenderViewport(0.0f, 0.0f, float(w), float(h)));
        }

        // Primary surface -> swapchain (the main scene; == C-5c when NHL_HIGHCUT_C5_NOSPLIT).
        for (uint32_t i = 0; i < c.c5draws.size(); ++i) {
            if (!inWindow(i)) continue;
            if (!isPrimary(c.c5draws[i])) continue;  // non-primary went to its offscreen RT (or skipped)
            renderDraw(c.c5draws[i]);
        }

        // C-5n HUD overlay (default on; opt out NHL_HIGHCUT_NO_HUD). The game's final composite/HUD pass
        // renders to a SEPARATE full-res surface that we don't present as primary (presenting it samples
        // the scene from guest RAM and degrades it vs our re-render). Instead, replay that overlay
        // surface's HUD elements (scorebug, change-lines: stencil-masked quads over small UI atlases) ON
        // TOP of our clean swapchain scene, SKIPPING the opaque scene-grade draws (sceneGradeSkip) that
        // would overwrite it. Overlay surface(s) = any NON-primary surface containing a full-screen
        // scene-color sampler (the composite pass).
        static const bool hud_overlay = std::getenv("NHL_HIGHCUT_NO_HUD") == nullptr;
        if (hud_overlay) {
            std::unordered_set<uint64_t> hudKeys;
            for (const auto& d : c.c5draws) {
                if (!d.samplesFullscreenSceneColor) continue;
                const uint64_t k = SurfaceKey(d.surfDepthBase, d.surfPitch, d.surfMsaa);
                if (k != c.c5PrimaryKey) hudKeys.insert(k);
            }
            if (!hudKeys.empty()) {
                c.cmd->setFramebuffer(c.fbs[idx].get());
                c.cmd->setViewports(RenderViewport(0.0f, 0.0f, float(w), float(h)));
                // Fresh stencil for the HUD pass (its masks were authored on the guest overlay surface,
                // not our swapchain). Keep color (our scene) + depth.
                c.cmd->clearDepthStencil(false, true, 1.0f, 0);
                uint32_t hudDrawn = 0, hudSkipped = 0;
                for (uint32_t i = 0; i < c.c5draws.size(); ++i) {
                    if (!inWindow(i)) continue;
                    auto& d = c.c5draws[i];
                    const uint64_t k = SurfaceKey(d.surfDepthBase, d.surfPitch, d.surfMsaa);
                    if (!hudKeys.count(k)) continue;                  // only the overlay/HUD surface(s)
                    if (d.sceneGradeSkip) { ++hudSkipped; continue; } // keep our clean scene
                    renderDraw(d);
                    ++hudDrawn;
                }
                static bool hudLogged = false;
                if (!hudLogged) {
                    hudLogged = true;
                    REXLOG_INFO("[highcut-C5n] HUD overlay: {} surface(s), drew {} overlay draws, "
                                "skipped {} scene-grade", uint32_t(hudKeys.size()), hudDrawn, hudSkipped);
                }
            }
        }
    }

    // C-3b.1: draw the TRANSLATED Xenos VS pipeline with its descriptor sets bound. With zeroed
    // buffers this renders nothing visible (degenerate positions), but it exercises the full
    // bind+draw path so the validation layer can confirm the descriptor sets match the shader.
    // C-3b.2 fills the buffers with real decoded data -> actual geometry.
    if (c.xlatDrawReady) {
        // C-3b.2 verify: zero the fragment counter before this frame's draw (GPU is idle here — the
        // prior frame's fence was waited at the end of RenderClear), so the post-present read gives
        // this frame's rasterized-pixel count.
        if (c.xlatCounterReadsLeft > 0 && c.xlatCounterBuf) {
            if (void* p = c.xlatCounterBuf->map()) { *static_cast<uint32_t*>(p) = 0; c.xlatCounterBuf->unmap(); }
        }
        c.cmd->setGraphicsPipelineLayout(c.xlatLayout.get());
        c.cmd->setPipeline(c.xlatPipeline.get());
        c.cmd->setGraphicsDescriptorSet(c.xlatSet0.get(), 0);
        c.cmd->setGraphicsDescriptorSet(c.xlatSet1.get(), 1);
        c.cmd->setGraphicsDescriptorSet(c.xlatSet2.get(), 2);
        c.cmd->drawInstanced(c.xlatVertexCount, 1, 0, 0);
    }

    // C-4: draw the same geometry with the TRANSLATED guest PIXEL shader, sampling the real guest
    // texture — over the C-3 solid pass (whose frag counter already proved the geometry). This is
    // the textured menu element; bind sets 0 (shared) / 1 (constants) / 3 (textures+samplers). Set
    // 2 (vertex textures) is an empty layout slot, so nothing is bound there.
    if (c.xlatTexDrawReady) {
        c.cmd->setGraphicsPipelineLayout(c.xlatTexLayout.get());
        c.cmd->setPipeline(c.xlatTexPipeline.get());
        c.cmd->setGraphicsDescriptorSet(c.xlatTexSet0.get(), 0);
        c.cmd->setGraphicsDescriptorSet(c.xlatTexSet1.get(), 1);
        c.cmd->setGraphicsDescriptorSet(c.xlatTexSet3.get(), 3);
        c.cmd->drawInstanced(c.xlatVertexCount, 1, 0, 0);
    }

    // C-5g: NHL_HIGHCUT_C5_SHOT=<path.png> — one-shot readback of the final swapchain image to a PNG,
    // so a headless replay's result is verifiable without a human at the window. Warm up a few frames
    // first (the scene/uploads settle), then copy tex -> a READBACK buffer this frame; mapped + written
    // after the fence wait below. Row pitch is 256-byte aligned (D3D12-safe; harmless on Vulkan).
    static const char* c5shot = std::getenv("NHL_HIGHCUT_C5_SHOT");
    // FILMSTRIP (restart §5.2 — "see the live output per screen"): NHL_HIGHCUT_FILMSTRIP=<dir> dumps the
    // presented swapchain to <dir>/f<frame>.png every NHL_HIGHCUT_FILMSTRIP_EVERY frames (default 30), so
    // a whole live run becomes a sequence of readable PNGs (one per ~screen) without a human at the window.
    // Same readback path as the one-shot C5_SHOT; recurring instead of one-shot. Zero cost when unset.
    static const char* filmDir = std::getenv("NHL_HIGHCUT_FILMSTRIP");
    static const uint32_t filmEvery = []() {
        const char* s = std::getenv("NHL_HIGHCUT_FILMSTRIP_EVERY");
        uint32_t n = s ? uint32_t(std::strtoul(s, nullptr, 10)) : 0u;
        return n ? n : 30u;
    }();
    static bool filmDirMade = false;
    if (filmDir && !filmDirMade) { CreateDirectoryA(filmDir, nullptr); filmDirMade = true; }
    const bool doFilm = filmDir && c.frame >= 16 && (c.frame % filmEvery == 0);
    std::unique_ptr<RenderBuffer> shotBuf;
    uint32_t shotPitch = 0;
    const bool doShot = c5shot && !c.c5ShotDone && c.frame >= 16;
    if (doShot || doFilm) {
        shotPitch = ((w * 4u) + 255u) & ~255u;
        shotBuf = c.device->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(shotPitch) * h));
        if (shotBuf) {
            c.cmd->barriers(RenderBarrierStage::COPY,
                            RenderTextureBarrier(tex, RenderTextureLayout::COPY_SOURCE));
            RenderTextureCopyLocation dstL = RenderTextureCopyLocation::PlacedFootprint(
                shotBuf.get(), kSwapFormat, w, h, 1, shotPitch / 4u, 0);
            RenderTextureCopyLocation srcL = RenderTextureCopyLocation::Subresource(tex, 0, 0);
            c.cmd->copyTextureRegion(dstL, srcL, 0, 0, 0, nullptr);
        }
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

    // C-5g: GPU is idle (fence waited) — map the readback buffer and write the PNG once.
    if (doShot && shotBuf) {
        if (const void* p = shotBuf->map()) {
            bool ok = WriteImagePNG(c5shot, w, h, shotPitch, static_cast<const uint8_t*>(p), /*bgra=*/true);
            shotBuf->unmap();
            REXLOG_INFO("[highcut-C5g] framebuffer shot {} -> {} ({}x{})",
                        ok ? "WROTE" : "FAILED", c5shot, w, h);
        }
        c.c5ShotDone = true;
    }
    // FILMSTRIP: write this frame's presented image to <dir>/f<frame>.png. (doShot may also have fired
    // this frame; they share the one readback buffer — write both.)
    if (doFilm && shotBuf) {
        if (const void* p = shotBuf->map()) {
            char fpath[1024];
            std::snprintf(fpath, sizeof(fpath), "%s\\f%06llu.png", filmDir,
                          static_cast<unsigned long long>(c.frame));
            bool ok = WriteImagePNG(fpath, w, h, shotPitch, static_cast<const uint8_t*>(p), /*bgra=*/true);
            shotBuf->unmap();
            REXLOG_INFO("[highcut-film] {} {} (guest frame {}, {} live draws this rebuild)",
                        ok ? "WROTE" : "FAILED", fpath, static_cast<unsigned long long>(c.frame),
                        uint32_t(c.c5draws.size()));
        }
    }

    // C-3b.2 verify: the GPU is now idle (fence waited), so the PS's atomic writes to the host-
    // visible counter are complete + visible. Read this frame's rasterized-pixel count.
    if (c.xlatDrawReady && c.xlatCounterReadsLeft > 0 && c.xlatCounterBuf) {
        --c.xlatCounterReadsLeft;
        if (const void* p = c.xlatCounterBuf->map()) {
            uint32_t frags = *static_cast<const uint32_t*>(p);
            c.xlatCounterBuf->unmap();
            REXLOG_INFO("[highcut-C3b2] translated-draw rasterized {} fragments this frame "
                        "(>0 => the Xenos VS produced on-screen geometry; verts={})",
                        frags, c.xlatVertexCount);
        }
    }
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

// C-6 live feed: CP thread appends one owned draw's packet bytes to the in-progress frame. No lock —
// only the CP thread touches g_liveBuild between commits.
extern "C" void HighcutLivePushDraw(const uint8_t* data, size_t size) {
    if (!g_enabled || !data || !size) return;
    g_liveBuild.emplace_back(data, data + size);
}
// Step 2: CP thread streams a unique shader/texture's bytes ONCE (append-only, persistent). The plume
// thread drains g_resourcePending into c.resourceBytes before each rebuild.
extern "C" void HighcutLivePushResource(uint64_t id, const uint8_t* data, size_t size) {
    if (!g_enabled || !data || !size) return;
    std::lock_guard<std::mutex> lk(g_resMutex);
    g_resourcePending.emplace_back(id, std::vector<uint8_t>(data, data + size));
}
// C-6 live feed: CP thread commits the in-progress frame at the guest-present boundary — move it into
// g_livePending under the lock and bump the seq so the plume thread picks it up. resolves may be null.
extern "C" void HighcutLiveCommitFrame(const uint8_t* resolves, size_t rsize) {
    if (!g_enabled) { g_liveBuild.clear(); g_liveBuildResolves.clear(); return; }
    if (g_liveBuild.empty()) return;  // nothing accumulated (e.g. a non-rendered frame)
    {
        std::lock_guard<std::mutex> lk(g_liveMutex);
        g_livePendingDraws.swap(g_liveBuild);
        g_livePendingResolves.assign(resolves ? resolves : (const uint8_t*)nullptr,
                                     resolves ? resolves + rsize : (const uint8_t*)nullptr);
        g_liveSeq.fetch_add(1, std::memory_order_release);
    }
    g_liveBuild.clear();
    g_liveBuildResolves.clear();
}
