// Linux-only host glue (this TU is only compiled off Windows - see the NOT WIN32
// source filter in CMakeLists.txt).

#if defined(__linux__)
#include <sys/prctl.h>
#endif
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

// NHL_ALLOW_PTRACE=1: let any process of the same user attach a debugger or
// stack dumper (eu-stack, gdb -p) to this process. Distributions that restrict
// ptrace to ancestors (kernel.yama.ptrace_scope=1, e.g. SteamOS) otherwise
// refuse, and starting the game under gdb is impractical there. This is the
// per-process opt-in Yama provides; it changes nothing system-wide and is off
// unless the variable is set.
struct AllowPtraceOptIn {
  AllowPtraceOptIn() {
#if defined(__linux__) && defined(PR_SET_PTRACER)
    const char* e = std::getenv("NHL_ALLOW_PTRACE");
    if (e && *e && *e != '0') prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
#endif
  }
} g_allow_ptrace_opt_in;

// When launched by Steam (SteamEnv=1), leave the Steam Deck's built-in
// controller to Steam Input and read its virtual gamepad instead. SDL's HIDAPI
// Steam Deck driver opens the hardware directly and keeps switching it out of
// "lizard" mode while Steam Input is managing the same device; Steam lists the
// device in SDL_GAMECONTROLLER_IGNORE_DEVICES, but that list is not applied to
// this driver, so the game still opened it and input dropped out for seconds
// at a time mid-game. Outside Steam the direct driver is the only gamepad
// source, so it stays on there. Runs before SDL is initialized.
struct PreferSteamInputOnDeck {
  PreferSteamInputOnDeck() {
    if (std::getenv("SteamEnv")) setenv("SDL_JOYSTICK_HIDAPI_STEAMDECK", "0", /*overwrite=*/0);
  }
} g_prefer_steam_input_on_deck;

}  // namespace
