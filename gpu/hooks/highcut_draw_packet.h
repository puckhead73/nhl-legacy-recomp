// High-cut path C, C-3b.2: a self-describing binary packet that carries one decoded guest draw's
// data from the beta CommandProcessor thread (RenderBetaOwnedDraw) to the plume Vulkan thread,
// so the translated Xenos VS can fetch + transform real vertices and render the draw.
//
// Bridged via a disk file (highcut_p3_draw.bin in the cwd) — like highcut_p3_vs.spv — because the
// beta-takeover and plume-present subsystems don't co-run cleanly in one process (beta fires the
// owned draw during early boot before plume's init). The plume thread loads the packet and fills
// the C-3b.1 descriptor buffers (system / fetch constants UBOs + shared-memory SSBO).
//
// Payload layout after the header: fetch_constants[fetch_bytes], system_constants[sys_bytes],
// shared_memory[shared_bytes]. The shared-memory bytes are the guest vertex buffer, and the
// vertex fetch constant's base address has been REBASED to 0 so the VS indexes from SSBO offset 0.

#pragma once
#include <cstdint>

namespace nhl::highcut {

constexpr uint32_t kDrawPacketMagic = 0x48334450;  // 'H3DP'
constexpr uint32_t kDrawPacketVersion = 1;

struct DrawPacketHeader {
    uint32_t magic;          // kDrawPacketMagic
    uint32_t version;        // kDrawPacketVersion
    uint32_t vertex_count;   // vertices to draw (drawInstanced count)
    uint32_t prim_type;      // xenos::PrimitiveType (raw) — mapped to plume topology
    uint32_t fetch_bytes;    // size of the fetch-constants blob (192 dwords = 768)
    uint32_t sys_bytes;      // size of the SpirvShaderTranslator::SystemConstants blob
    uint32_t shared_bytes;   // size of the shared-memory (vertex) blob
    uint32_t reserved;
};

}  // namespace nhl::highcut
