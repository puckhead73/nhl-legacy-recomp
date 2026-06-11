// C ABI for the Rust `tdb-rx2-ffi` static library (built from
// nhl-database-studio/crates/tdb-rx2-ffi). Converts loose `.dds` texture
// overrides into a game-valid `.rx2` at load time — used by the loose-asset
// device so textures can live as editable `.dds` instead of inside the
// proprietary `.rx2` container.
//
// Keep these declarations in sync with crates/tdb-rx2-ffi/src/lib.rs.

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {

// One texture-slot override: replace slot `slot` with the base mip from the
// `.dds` blob at dds_ptr[..dds_len].
struct Rx2Override {
  uint32_t slot;
  const uint8_t* dds_ptr;
  size_t dds_len;
};

// Result codes (mirror RX2FFI_* in the Rust crate). 0 == success.
enum {
  kRx2FfiOk = 0,
  kRx2FfiErrNullArg = 1,
  kRx2FfiErrParse = 2,
  kRx2FfiErrSlotRange = 3,
  kRx2FfiErrDdsInvalid = 4,
  kRx2FfiErrDimMismatch = 5,
  kRx2FfiErrFormatUnsupported = 6,
  kRx2FfiErrReplace = 7,
};

// Apply `n_overrides` .dds slot overrides to the .rx2 in rx2_ptr[..rx2_len].
// On success returns kRx2FfiOk and writes a heap buffer to *out_ptr/*out_len
// that the caller MUST release with rx2ffi_free. On error returns nonzero and
// sets *out_ptr = nullptr.
int32_t rx2ffi_replace_textures(const uint8_t* rx2_ptr, size_t rx2_len,
                                const Rx2Override* overrides, size_t n_overrides,
                                uint8_t** out_ptr, size_t* out_len);

// Descriptor for one decoded .rx2 texture slot (mirror Rx2SlotInfo in the
// Rust crate). All sizes in bytes.
struct Rx2SlotInfo {
  uint32_t width;
  uint32_t height;
  uint32_t format_byte;      // tdb PixelFormat byte: 0x52=DXT1,0x53=DXT3,0x54=DXT5,0x86=ARGB8888
  uint32_t xenos_format;     // xenos::TextureFormat: 18=DXT1/BC1,19=DXT2_3,20=DXT4_5/BC3,6=8_8_8_8
  uint32_t base_size;        // base-mip byte count
  uint32_t buffer_capacity;  // whole reserved slot buffer == returned payload length
  uint32_t tiled;            // 1 = X360-tiled+swap16 (DXT); 0 = linear (ARGB8888)
};

// Decode one slot of the .rx2 in rx2_ptr[..rx2_len] and return its RAW on-disk
// tiled payload — exactly the bytes the Xbox 360 GPU samples from guest RAM at
// the texture's fetch-constant base. On success returns kRx2FfiOk, fills
// *out_info, and writes a heap buffer (length == buffer_capacity) to
// *out_ptr/*out_len that the caller MUST release with rx2ffi_free. On error
// returns nonzero and sets *out_ptr = nullptr.
int32_t rx2ffi_decode_slot(const uint8_t* rx2_ptr, size_t rx2_len, uint32_t slot,
                           Rx2SlotInfo* out_info, uint8_t** out_ptr, size_t* out_len);

// Free a buffer returned by rx2ffi_replace_textures or rx2ffi_decode_slot
// (nullptr is a no-op).
void rx2ffi_free(uint8_t* ptr, size_t len);

}  // extern "C"
