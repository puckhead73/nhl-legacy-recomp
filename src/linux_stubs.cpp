// Linux-only host glue (this TU is only compiled off Windows - see the NOT WIN32
// source filter in CMakeLists.txt).

#include <cstdlib>

// No-op stand-ins for the Windows-only "high-cut" experiment.
//
// gpu/hooks/d3d9_resources.cpp calls HighcutPlumeTick() once per guest present
// to drive the in-process plume D3D12 swapchain (gpu/hooks/plume_present.cpp,
// itself env-gated by NHL_HIGHCUT_PRESENT and a no-op by default). That TU is
// D3D12-only, so off Windows the tick does nothing.
extern "C" void HighcutPlumeTick() {}

namespace {

// SteamOS ships RenderDoc's implicit Vulkan capture layer
// (/etc/vulkan/implicit_layer.d/renderdoc_capture.json) and it ends up active in
// the game process: on a Steam Deck it cost ~6% of the GPU command-processor
// thread, and turning it off took matches from ~29.4 to ~31.7 fps at equal draw
// counts. The layer's own opt-out env name is version-suffixed
// (DISABLE_VULKAN_RENDERDOC_CAPTURE_1_43), so use the loader's generic
// VK_LOADER_LAYERS_DISABLE instead. Runs at static-init time, before main() and
// thus before any Vulkan call. Skipped when RenderDoc capture is explicitly
// requested (ENABLE_VULKAN_RENDERDOC_CAPTURE=1) or the variable is already set.
struct DisableRenderDocLayer {
  DisableRenderDocLayer() {
    if (std::getenv("ENABLE_VULKAN_RENDERDOC_CAPTURE")) return;
    setenv("VK_LOADER_LAYERS_DISABLE", "VK_LAYER_RENDERDOC_Capture", /*overwrite=*/0);
  }
} g_disable_renderdoc_layer;

}  // namespace
