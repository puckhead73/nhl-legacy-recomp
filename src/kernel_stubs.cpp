// Host-side stubs for guest kernel imports not implemented by rexruntime 0.8.0.
//
// NHL Legacy imports the Xbox Live Vision camera API (XUsbcam*) from xam.xex.
// The game is fully playable without a camera, so report "no device".

#include "generated/default/nhllegacy_init.h"

namespace {
constexpr uint32_t kXErrorDeviceNotConnected = 0x48F;  // ERROR_DEVICE_NOT_CONNECTED
}

// DWORD XUsbcamGetState(): 0 = no camera present.
REX_EXTERN(__imp__XUsbcamGetState) {
  (void)base;
  ctx.r3.u64 = 0;
}

// DWORD XUsbcamSetConfig(XUSBCAM_CONFIG*): fail, no device attached.
REX_EXTERN(__imp__XUsbcamSetConfig) {
  (void)base;
  ctx.r3.u64 = kXErrorDeviceNotConnected;
}
