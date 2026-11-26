// No-op stand-ins for the high-cut plume entry points, compiled when the D3D12 /
// plume side of the tree is off (NHLLEGACY_D3D12=OFF: Linux, ARM64, Switch).
//
// gpu/hooks/d3d9_resources.cpp is portable and stays in every build; it calls
// HighcutPlumeTick() once per guest Present. The real body lives in
// gpu/hooks/plume_present.cpp, which needs D3D12 + plume.
#include <cstdint>

extern "C" void HighcutPlumeTick() {}
