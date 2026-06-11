// xtr_player - self-written ReXGlue/Xenia .xtr trace player (renderer plan,
// Phase 4). The SDK's TraceReader/TracePlayer are not exported from
// rexruntime.dll, so this re-implements the replay loop over the *exported*
// CommandProcessor API (ExecutePacket / RestoreRegisters / RestoreEdramSnapshot
// / TracePlaybackWroteMemory) and a guest Memory we write captured ranges into.
//
// Format reference: rex/graphics/trace_protocol.h and tools/parse_gpu_trace.py.

#pragma once

#include <cstdint>
#include <string>

namespace rex::graphics {
class GraphicsSystem;
}

namespace nhl::replay {

struct ReplayStats {
  bool ok = false;
  uint32_t frames = 0;            // swap events seen
  uint32_t primary_packets = 0;   // primary-level packets executed
  uint32_t memory_ranges = 0;     // memory read/write ranges applied
  uint32_t register_blocks = 0;   // RestoreRegisters calls
  uint32_t edram_snapshots = 0;   // RestoreEdramSnapshot calls
  uint32_t draw_packets = 0;      // DRAW_INDX / DRAW_INDX_2 packets seen in stream
  uint32_t skipped_truncated_type0 = 0;  // malformed Type-0 leaves skipped (capture artifacts)
  uint32_t skipped_empty_leaves = 0;     // count==0 leaves skipped (would re-parse stale memory)
  uint32_t clamped_trailing_leaves = 0;  // leaves with trailing dwords after the packet (executed only the packet)
  uint32_t skipped_wait_reg_mem = 0;     // WAIT_REG_MEM packets skipped (would block serial replay forever)
  std::string error;
};

// Replays the whole trace file through graphics_system's command processor.
// graphics_system must already be set up (GPU initialized, command processor
// live). Returns counts gathered while parsing/driving the stream.
ReplayStats ReplayTrace(rex::graphics::GraphicsSystem& graphics_system,
                        const std::string& trace_path);

}  // namespace nhl::replay
