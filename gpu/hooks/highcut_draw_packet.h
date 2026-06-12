// High-cut path C: a self-describing binary packet that carries one decoded guest draw's data from
// the beta CommandProcessor thread (RenderBetaOwnedDraw) to the plume Vulkan thread, so the
// translated Xenos VS+PS can fetch+transform real vertices, sample the real guest texture, and
// render the draw. Bridged via a disk file (so beta-takeover and plume-present need not co-run).
//
// C-4 used ONE packet (highcut_p3_draw.bin). C-5a (NHL_HIGHCUT_FRAME_CAPTURE) writes ONE packet PER
// owned draw of a frame — highcut_frame_<N>.bin, N = 0..(highcut_frame.count-1) — and the plume
// thread replays them ALL in order into one flat RT with per-draw blend + viewport.
//
// VERSION 3 payload layout, in order, immediately after the header:
//   fetch_constants[fetch_bytes]   — full 192-dword fetch register space -> uvec4[48] UBO
//   system_constants[sys_bytes]    — SpirvShaderTranslator::SystemConstants (color_exp_bias, vte
//                                    w-division flags, y-flipped ndc)
//   shared_memory[shared_bytes]    — guest vertex bytes (vertex fetch base REBASED to 0)
//   bool_loop_constants[bool_bytes]— 40 dwords (8 bool + 32 loop) -> the b/loop UBO
//   vs_float_constants[vs_float_bytes] — PACKED VS float constants (ascending storage index)
//   ps_float_constants[ps_float_bytes] — PACKED PS float constants (ascending storage index)
//   vertex_shader_spirv[vs_spirv_bytes] — translated guest VS SPIR-V (masked interpolators), INLINE
//                                    per draw (0 => fall back to the shared highcut_p3_vs.spv)
//   pixel_shader_spirv[ps_spirv_bytes]  — translated guest PS SPIR-V (0 => VS-only / solid PS path)
//   for each of texture_count: TexturePacketDesc desc; uint8_t linear_texels[desc.data_bytes];

#pragma once
#include <cstdint>

namespace nhl::highcut {

constexpr uint32_t kDrawPacketMagic = 0x48334450;  // 'H3DP'
constexpr uint32_t kDrawPacketVersion = 3;          // C-5a: inline VS SPIR-V + viewport + blend

// Plume topology for the host draw. Xenos RectangleList -> kRectangleListAsTriangleStrip (4-vert
// strip). kQuadList (menu text/glyphs) has no host-shader expansion in the translator, so the plume
// side expands it with an index buffer ({0,1,2,0,2,3} per 4-vert quad) and a TRIANGLE_LIST pipeline;
// vertex_count carries the GUEST quad-vertex count (4 * #quads).
enum DrawTopology : uint32_t {
    kTopoTriangleList = 0,
    kTopoTriangleStrip = 1,
    kTopoTriangleListQuadExpand = 2,  // index-expanded quad list (vertex_count = guest 4*#quads)
};

// Plume-neutral texel format the untiled blob is in (the CP side mustn't depend on plume's
// RenderFormat enum; the plume thread maps these to the matching RenderFormat).
enum PacketTexFormat : uint32_t {
    kTexRGBA8 = 0,  // R8G8B8A8_UNORM  (k_8_8_8_8 untiled + endian-swapped)
    kTexBC1 = 1,    // BC1_UNORM       (k_DXT1)
    kTexBC2 = 2,    // BC2_UNORM       (k_DXT2_3)
    kTexBC3 = 3,    // BC3_UNORM       (k_DXT4_5)
};

// Plume-neutral blend factor/op — the xenos::BlendFactor / BlendOp enum VALUES (the CP decodes
// RB_BLENDCONTROL into these; the plume side maps them to RenderBlend / RenderBlendOperation).
enum PacketBlendFactor : uint32_t {
    kBlendZero = 0, kBlendOne = 1, kBlendSrcColor = 4, kBlendInvSrcColor = 5,
    kBlendSrcAlpha = 6, kBlendInvSrcAlpha = 7, kBlendDstColor = 8, kBlendInvDstColor = 9,
    kBlendDstAlpha = 10, kBlendInvDstAlpha = 11, kBlendConstColor = 12, kBlendInvConstColor = 13,
    kBlendConstAlpha = 14, kBlendInvConstAlpha = 15, kBlendSrcAlphaSat = 16,
};
enum PacketBlendOp : uint32_t {
    kBlendOpAdd = 0, kBlendOpSubtract = 1, kBlendOpMin = 2, kBlendOpMax = 3, kBlendOpRevSubtract = 4,
};

struct TexturePacketDesc {
    uint32_t width;            // logical texel width
    uint32_t height;           // logical texel height
    uint32_t tex_format;       // PacketTexFormat — the untiled blob's texel format
    uint32_t row_pitch_bytes;  // linear row pitch of the blob (blocks_x * bytes_per_block)
    uint32_t data_bytes;       // size of the linear texel blob that follows this struct
    uint32_t fetch_slot;       // Xenos texture fetch-constant slot (diagnostic)
    uint32_t is_signed;        // 0 = unsigned binding, 1 = signed binding
    uint32_t swizzle;          // Xenos 12-bit component swizzle (4x3-bit; 0=R,1=G,2=B,3=A,4=0,5=1).
                               // Applied to the plume view's component mapping (e.g. BGRA 8888).
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
    uint32_t vs_spirv_bytes;   // size of the inline VS SPIR-V (0 => shared highcut_p3_vs.spv)
    uint32_t ps_spirv_bytes;   // size of the translated PS SPIR-V blob (0 => no PS / solid path)
    uint32_t texture_count;    // number of TexturePacketDesc + texel blobs that follow (PS textures)
    uint32_t ps_sampler_count; // number of PS sampler bindings (set 3, after the textures)
    // Per-draw viewport (plume sets this before the draw): from vpi.xy_offset/xy_extent/z_min/z_max.
    float vp_x, vp_y, vp_w, vp_h, vp_zmin, vp_zmax;
    // Per-draw blend (decoded from RB_BLENDCONTROL0 + RB_COLOR_MASK). Factors/ops are PacketBlend*.
    uint32_t blend_enable;     // 1 => blend; (kBlendOne,kBlendZero,kBlendOpAdd) is identity (copy)
    uint32_t blend_src, blend_dst, blend_op;           // color
    uint32_t blend_src_a, blend_dst_a, blend_op_a;     // alpha
    uint32_t color_write_mask; // RGBA bits 0..3 (1=R,2=G,4=B,8=A)
};

}  // namespace nhl::highcut
