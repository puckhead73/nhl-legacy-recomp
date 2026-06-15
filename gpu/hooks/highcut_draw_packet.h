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
//   for each of vs_texture_count: TexturePacketDesc desc; uint8_t linear_texels[desc.data_bytes]
//   for each of ps_sampler_count: SamplerPacketDesc          — (v9) PS sampler filter/clamp state
//   for each of vs_sampler_count: SamplerPacketDesc          — (v9) VS sampler filter/clamp state
//   index_blob[index_bytes]        — (v5) raw guest kGuestDMA indices (u16/u32 per index_format)

#pragma once
#include <cstdint>

namespace nhl::highcut {

constexpr uint32_t kDrawPacketMagic = 0x48334450;  // 'H3DP'
constexpr uint32_t kDrawPacketVersion = 10;         // C-5g: + cube textures (array_layers; env reflection)

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
    kTexRGBA8 = 0,    // R8G8B8A8_UNORM       (k_8_8_8_8 untiled + 32-bit endian-swapped)
    kTexBC1 = 1,      // BC1_UNORM            (k_DXT1)
    kTexBC2 = 2,      // BC2_UNORM            (k_DXT2_3)
    kTexBC3 = 3,      // BC3_UNORM            (k_DXT4_5)
    kTexRGBA32F = 4,  // R32G32B32A32_FLOAT   (k_32_32_32_32_FLOAT — the VS skinning bone palette)
    kTexBC5 = 5,      // BC5_UNORM            (k_DXN — two-channel NORMAL MAPS; was stubbed -> magenta)
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
    uint32_t fetch_base_addr;  // C-5d.3: guest texture base address (tf.base_address<<12). If a prior
                               // pass RESOLVED to this address (ResolveMarker.dest_addr), the replay
                               // binds OUR offscreen surface RT (host-copy) instead of this captured
                               // blob — so render-to-texture passes (reflection/shadow) feed this draw.
    uint32_t array_layers;     // C-5g: 1 = normal 2D; 6 = CUBE map (data is 6 faces of width*height
                               // concatenated, +X,-X,+Y,-Y,+Z,-Z). Player materials sample an env cube
                               // for reflection AND use its ALPHA as a material factor (NHL12 finding):
                               // a cube bound as a 2D placeholder gives garbage alpha -> the gold
                               // back-number decal (which rides on that term) drops out while the
                               // diffuse jersey stays correct. The plume side builds a real cube here.
};

// C-5f: per-sampler state, captured from the guest texture-fetch constant of each translated
// SamplerBinding (in SamplerBindings order, so sampler binding i -> set descriptor nTex+i). Bring-up
// hardcoded LINEAR+CLAMP for ALL PS textures, which silently broke the jersey NAMEPLATE LAYOUT map: a
// data texture the player PS POINT-samples to compute the back-number atlas UV. LINEAR-blending it
// produced a garbage UV -> the number sampled a blank atlas cell -> no number (while the chest crest,
// a normal LINEAR decal, rendered fine). Honor the guest filter/clamp per binding to fix it. Fields
// are the RAW Xenos enum VALUES (xenos::TextureFilter 0..3, xenos::ClampMode 0..7); the plume side
// maps them to RenderFilter / RenderTextureAddressMode.
struct SamplerPacketDesc {
    uint32_t fetch_slot;  // Xenos texture fetch slot this sampler reads (diagnostic)
    uint32_t mag_filter;  // xenos::TextureFilter (0=kPoint, 1=kLinear)
    uint32_t min_filter;  // xenos::TextureFilter
    uint32_t mip_filter;  // xenos::TextureFilter (2=kBaseMap => no mip)
    uint32_t clamp_x;     // xenos::ClampMode (0=kRepeat/WRAP, 2=kClampToEdge/CLAMP, ...)
    uint32_t clamp_y;     // xenos::ClampMode
    uint32_t clamp_z;     // xenos::ClampMode
    uint32_t aniso;       // xenos::AnisoFilter (max anisotropy; diagnostic / future use)
};

// C-5d.3: a guest EDRAM Resolve event (sub_827EF8E0 / RB_MODECONTROL.edram_mode==kCopy), captured in
// stream order alongside the draws so the replay can host-copy the just-finished surface RT into a
// texture keyed by the resolve DEST address. Written to a sidecar `highcut_resolves.bin`:
//   [uint32 count][ResolveMarker x count], rewritten after every resolve, reset at each frame boundary.
// dest_addr matches a later draw's TexturePacketDesc.fetch_base_addr; src_* identifies the source
// surface (the pass being resolved) so the replay knows which offscreen RT to copy.
struct ResolveMarker {
    uint32_t after_draw;      // resolve fires AFTER this many captured draws this frame (= capture idx)
    uint32_t dest_addr;       // guest dest (draw_util::GetResolveInfo copy_dest_base) — the sampled addr
    uint32_t is_depth;        // 1 = depth resolve (shadow/depth map), 0 = color
    uint32_t src_depth_base;  // source surface key (RB_DEPTH_INFO / RB_SURFACE_INFO at resolve time)
    uint32_t src_pitch;
    uint32_t src_msaa;
};
constexpr uint32_t kResolveSidecarMagic = 0x48335256;  // 'H3RV'

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
    // Per-draw guest scissor (PA_SC_WINDOW_SCISSOR), already scaled to the 1280x720 logical RT. The
    // game clips e.g. the right-side description text to its box and the bottom ticker to its bar;
    // without it those overflow (and a draw scissored to ~nothing renders full -> bleed).
    uint32_t sc_left, sc_top, sc_right, sc_bottom;
    // C-5c: per-draw depth/stencil/cull state (decoded from RB_DEPTHCONTROL / RB_STENCILREFMASK /
    // PA_SU_SC_MODE_CNTL). func/op fields are the RAW Xenos enum VALUES (xenos::CompareFunction 0..7,
    // xenos::StencilOp 0..7); the plume side maps them to RenderComparisonFunction / RenderStencilOp.
    // A 2D menu draw has depth_enable=0 (depth-disabled pipeline) so the flat-RT menu is unaffected.
    uint32_t depth_enable;        // RB_DEPTHCONTROL.z_enable
    uint32_t depth_write;         // RB_DEPTHCONTROL.z_write_enable
    uint32_t depth_func;          // RB_DEPTHCONTROL.zfunc (xenos::CompareFunction)
    uint32_t stencil_enable;      // RB_DEPTHCONTROL.stencil_enable
    uint32_t stencil_read_mask;   // RB_STENCILREFMASK.stencilmask (compare/read mask)
    uint32_t stencil_write_mask;  // RB_STENCILREFMASK.stencilwritemask
    uint32_t stencil_ref;         // RB_STENCILREFMASK.stencilref
    // Front/back stencil ops + compare (xenos::StencilOp / xenos::CompareFunction values). Xenos
    // names map to plume as: fail_op=stencilfail, pass_op=stencilzpass (stencil AND depth pass),
    // depth_fail_op=stencilzfail (stencil pass, depth fail).
    uint32_t front_fail_op, front_pass_op, front_depth_fail_op, front_func;
    uint32_t back_fail_op, back_pass_op, back_depth_fail_op, back_func;
    uint32_t cull_mode;           // 0=none, 1=front, 2=back
    uint32_t front_ccw;           // 1 => guest front face is counter-clockwise (PA_SU..face==0).
                                  // The replay bakes a y-flip into ndc (reverses on-screen winding),
                                  // so the plume side INVERTS this when picking RenderFrontFace.
    // C-5d: kGuestDMA index buffer. Most 3D meshes are INDEXED (index_buffer_type==kGuestDMA) — the
    // index buffer defines triangle connectivity; drawing them non-indexed connects vertices in
    // storage order -> exploded geometry. The raw guest indices (BIG-endian; the VS swaps
    // gl_VertexIndex via vertex_index_endian, mirroring the beta kGuestDMA path) are appended at the
    // VERY END of the packet (after all textures). vertex_count = the index count (drawIndexedInstanced).
    uint32_t index_format;        // 0 = none (drawInstanced / quad-expand), 1 = u16, 2 = u32
    uint32_t index_bytes;         // size of the raw index blob appended after the textures
    // C-5d: guest render-surface identity. Draws sharing a (color_base, depth_base, surface_pitch,
    // msaa) tuple target the SAME guest surface; the replay buckets them into ONE per-surface flat
    // plume RT (each with its own depth+stencil), instead of the single shared RT — which can't both
    // let a frame-start mask draw seed stencil AND keep its Always-depth write off the 3D geometry's
    // shared depth (Problem 1). color_base/depth_base are EDRAM tile indices used only as surface KEYS
    // (the flat path has no EDRAM addressing); color_format picks the per-surface plume RT format.
    uint32_t surface_color_base;   // RB_COLOR_INFO.color_base (+bit11) — EDRAM tile, color surface key
    uint32_t surface_depth_base;   // RB_DEPTH_INFO.depth_base — EDRAM tile, depth surface key
    uint32_t surface_pitch;        // RB_SURFACE_INFO.surface_pitch (logical surface width)
    uint32_t surface_msaa;         // RB_SURFACE_INFO.msaa_samples (0=1X, 1=2X, 2=4X)
    uint32_t surface_color_format; // RB_COLOR_INFO.color_format (xenos::ColorRenderTargetFormat)
    // C-5d.3: VERTEX-shader textures. Skinned meshes (players, most of the gameplay scene) fetch a
    // BONE-PALETTE texture IN THE VS; without it the skinning collapses every vertex to a point → zero
    // fragments (the "black depth=184 pass"). These bind to descriptor SET 2 (the translator's vertex-
    // texture set, an empty placeholder before C-5d.3). They're appended AFTER the `texture_count` PS
    // textures and BEFORE the index blob: [PS textures][VS textures][index]. Same TexturePacketDesc.
    uint32_t vs_texture_count;     // number of VS TexturePacketDesc+blobs (set2 textures)
    uint32_t vs_sampler_count;     // number of VS sampler bindings (set2, after the VS textures)
};

}  // namespace nhl::highcut
