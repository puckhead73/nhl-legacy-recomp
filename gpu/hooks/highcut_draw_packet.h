// High-cut path C: a self-describing binary packet that carries one decoded guest draw's
// data from the beta CommandProcessor thread (RenderBetaOwnedDraw) to the plume Vulkan thread,
// so the translated Xenos VS (+ PS, from C-4) can fetch + transform real vertices, sample the
// real guest texture, and render the draw.
//
// Bridged via a disk file (highcut_p3_draw.bin in the cwd) — like highcut_p3_vs.spv — because the
// beta-takeover and plume-present subsystems don't co-run cleanly in one process (beta fires the
// owned draw during early boot before plume's init). The plume thread loads the packet and fills
// the descriptor buffers (constants UBOs + shared-memory SSBO) and, for C-4, creates the pixel
// shader module + the sampled texture(s).
//
// VERSION 2 (C-4) payload layout, in order, immediately after the header:
//   fetch_constants[fetch_bytes]   — full 192-dword fetch register space -> uvec4[48] UBO
//   system_constants[sys_bytes]    — SpirvShaderTranslator::SystemConstants (incl. color_exp_bias)
//   shared_memory[shared_bytes]    — guest vertex bytes (the vertex fetch base is REBASED to 0)
//   bool_loop_constants[bool_bytes]— 40 dwords (8 bool + 32 loop) -> the b/loop UBO
//   vs_float_constants[vs_float_bytes] — PACKED VS float constants (ascending storage index)
//   ps_float_constants[ps_float_bytes] — PACKED PS float constants (ascending storage index)
//   pixel_shader_spirv[ps_spirv_bytes] — translated guest PS SPIR-V (0 => VS-only C-3 path)
//   for each of texture_count: TexturePacketDesc desc; uint8_t linear_texels[desc.data_bytes];
//
// The float constants are packed (not the full 256-entry bank) because the translator indexes them
// via GetPackedFloatConstantIndex — see SpirvShaderTranslator and the beta path's pack_floats.

#pragma once
#include <cstdint>

namespace nhl::highcut {

constexpr uint32_t kDrawPacketMagic = 0x48334450;  // 'H3DP'
constexpr uint32_t kDrawPacketVersion = 2;          // C-4: PS SPIR-V + textures + b/loop + floats

// Plume topology for the host draw (the translated VS's expected primitive). Xenos RectangleList
// is translated as kRectangleListAsTriangleStrip and drawn as a 4-vertex TRIANGLE_STRIP.
enum DrawTopology : uint32_t { kTopoTriangleList = 0, kTopoTriangleStrip = 1 };

// Plume-neutral texel format the untiled blob is in (the CP side mustn't depend on plume's
// RenderFormat enum; the plume thread maps these to the matching RenderFormat). Bring-up set.
enum PacketTexFormat : uint32_t {
    kTexRGBA8 = 0,  // R8G8B8A8_UNORM  (k_8_8_8_8 untiled + endian-swapped)
    kTexBC1 = 1,    // BC1_UNORM       (k_DXT1)
    kTexBC2 = 2,    // BC2_UNORM       (k_DXT2_3)
    kTexBC3 = 3,    // BC3_UNORM       (k_DXT4_5)
};

// One sampled texture for the pixel shader: an already-UNTILED LINEAR texel blob plus the
// metadata plume needs to create the RenderTexture + view + upload. One per PS texture binding
// (the translator emits a separate binding for the unsigned and signed views of a fetch constant;
// for bring-up both point at the same untiled bytes — see is_signed).
struct TexturePacketDesc {
    uint32_t width;            // logical texel width
    uint32_t height;           // logical texel height
    uint32_t tex_format;       // PacketTexFormat — the untiled blob's texel format
    uint32_t row_pitch_bytes;  // linear row pitch of the blob (blocks_x * bytes_per_block)
    uint32_t data_bytes;       // size of the linear texel blob that follows this struct
    uint32_t fetch_slot;       // Xenos texture fetch-constant slot (diagnostic)
    uint32_t is_signed;        // 0 = unsigned binding, 1 = signed binding
    uint32_t reserved;
};

struct DrawPacketHeader {
    uint32_t magic;            // kDrawPacketMagic
    uint32_t version;          // kDrawPacketVersion
    uint32_t vertex_count;     // host vertices to draw (drawInstanced count)
    uint32_t topology;         // DrawTopology — the plume primitive topology
    uint32_t fetch_bytes;      // size of the fetch-constants blob (192 dwords = 768)
    uint32_t sys_bytes;        // size of the SpirvShaderTranslator::SystemConstants blob
    uint32_t shared_bytes;     // size of the shared-memory (vertex) blob
    uint32_t bool_bytes;       // size of the bool/loop-constants blob (40 dwords = 160)
    uint32_t vs_float_bytes;   // size of the packed VS float-constants blob
    uint32_t ps_float_bytes;   // size of the packed PS float-constants blob
    uint32_t ps_spirv_bytes;   // size of the translated PS SPIR-V blob (0 => no PS / C-3 path)
    uint32_t texture_count;    // number of TexturePacketDesc + texel blobs that follow (PS textures)
    uint32_t ps_sampler_count; // number of PS sampler bindings (set 3, after the textures)
    uint32_t reserved1;
};

}  // namespace nhl::highcut
