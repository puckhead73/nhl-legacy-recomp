// No-op stand-ins for the Windows-only "high-cut" experiment on non-Windows
// builds (see the NOT WIN32 source filter in CMakeLists.txt).
//
// gpu/hooks/d3d9_resources.cpp calls HighcutPlumeTick() once per guest present
// to drive the in-process plume D3D12 swapchain (gpu/hooks/plume_present.cpp,
// itself env-gated by NHL_HIGHCUT_PRESENT and a no-op by default). That TU is
// D3D12-only, so off Windows the tick does nothing.

extern "C" void HighcutPlumeTick() {}
