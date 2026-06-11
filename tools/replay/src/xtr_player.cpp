#include "xtr_player.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <utility>
#include <vector>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/trace_protocol.h>
#include <rex/logging.h>
#include <rex/memory.h>

#include <snappy.h>

namespace nhl::replay {

namespace {

namespace gfx = rex::graphics;

// Little-endian cursor over the trace bytes with bounds checks. PM4 packet
// payloads are big-endian and copied verbatim into guest memory (which the
// command processor reads big-endian); everything else in the container is LE.
class Cursor {
 public:
  Cursor(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  bool CanRead(size_t n) const { return pos_ + n <= size_; }
  size_t pos() const { return pos_; }

  uint32_t U32() {
    uint32_t v = 0;
    std::memcpy(&v, data_ + pos_, sizeof(v));
    pos_ += sizeof(v);
    return v;
  }
  const uint8_t* Bytes(size_t n) {
    const uint8_t* p = data_ + pos_;
    pos_ += n;
    return p;
  }
  void Skip(size_t n) { pos_ += n; }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t pos_ = 0;
};

// Decompress (or copy) a trace payload. `dec_hint` is the known decoded length
// for commands that carry it (memory); for snappy we trust GetUncompressedLength.
std::vector<uint8_t> Decode(gfx::MemoryEncodingFormat enc, const uint8_t* src, uint32_t enc_len,
                            uint32_t dec_hint) {
  if (enc == gfx::MemoryEncodingFormat::kNone) {
    return std::vector<uint8_t>(src, src + enc_len);
  }
  size_t out_len = dec_hint;
  if (!snappy::GetUncompressedLength(reinterpret_cast<const char*>(src), enc_len, &out_len)) {
    out_len = dec_hint;
  }
  std::vector<uint8_t> buf(out_len);
  if (!buf.empty() &&
      !snappy::RawUncompress(reinterpret_cast<const char*>(src), enc_len,
                             reinterpret_cast<char*>(buf.data()))) {
    buf.clear();
  }
  return buf;
}

// Walks the trace command stream and drives the command processor. MUST run on
// the command processor's worker thread (via CallInThread): RestoreEdramSnapshot
// and ExecutePacket/IssueDraw touch the D3D12 command list + submission state
// owned by that thread.
void DriveStream(const std::vector<uint8_t>& data, rex::memory::Memory& memory,
                 gfx::CommandProcessor& cp, ReplayStats& stats) {
  Cursor c(data.data(), data.size());
  c.Skip(4 + 40 + 4);  // TraceHeader: version + build_commit_sha[40] + title_id

  // This is a FLATTENED execution trace. Every packet the GPU executed is
  // recorded as a PacketStart (inline big-endian PM4) + PacketEnd, with the
  // contents of indirect buffers spliced inline (bracketed by IndirectBuffer
  // Start/End). The INDIRECT_BUFFER pointer packets themselves carry a size of 0
  // (the real size isn't preserved), so executing them divides by zero in
  // ExecuteIndirectBuffer. We therefore replay every LEAF packet in stream order
  // via ExecutePacket(base,count) and SKIP INDIRECT_BUFFER (op 0x3F) packets,
  // whose targets already follow inline. Packets nest, so use a stack.
  struct Pending {
    uint32_t base, count;
    uint32_t exec_count;  // dwords to actually execute (== one packet; <= count)
    bool skip;
  };
  std::vector<Pending> packet_stack;
  // Both indirect-buffer pointer opcodes must be skipped: their target packets
  // are already spliced inline (bracketed by IndirectBufferStart/End) and the
  // pointer packet itself carries size 0, so executing it calls
  // ExecuteIndirectBuffer with a zero-length ring → divide-by-zero (0xC0000094).
  // 0x3F = INDIRECT_BUFFER (menu uses only this); 0x37 = INDIRECT_BUFFER_PFD
  // (prefetch — gameplay/replay scenes use this too, the menu never did).
  constexpr uint32_t kOpIndirectBuffer = 0x3F;
  constexpr uint32_t kOpIndirectBufferPfd = 0x37;
  // WAIT_REG_MEM (0x3C) blocks the CP until a register/memory value meets a
  // condition. In serial offline replay there is no concurrent GPU producer to
  // satisfy that condition, so an unsatisfied wait hangs forever (seen mid
  // instant-replay: the CP worker blocks, deadlocking the replay's future). The
  // ordering it enforces is already guaranteed because we execute every packet
  // sequentially, so skipping it is safe — scenes whose waits were already
  // satisfied (menu, gameplay) replayed identically with it executed.
  constexpr uint32_t kOpWaitRegMem = 0x3C;

  // Diagnostic: NHL_REPLAY_SWAP_ONLY skips executing every leaf packet except
  // XE_SWAP (op 0x4A), so the frontbuffer keeps the bytes the trace's MemoryReads
  // loaded into guest memory (instead of being overwritten by the replayed
  // draws+resolve). Isolates the present/capture path from the render path: if
  // this presents the captured image, rendering is the culprit; if still black,
  // the present/capture path is.
  const bool swap_only = std::getenv("NHL_REPLAY_SWAP_ONLY") != nullptr;
  // NHL_REPLAY_LOAD_ONLY: apply memory/registers/EDRAM but execute NO packets at
  // all. Lets the caller dump the frontbuffer the trace LOADED (ground-truth
  // target image) and validate untiling, with nothing overwriting it.
  const bool load_only = std::getenv("NHL_REPLAY_LOAD_ONLY") != nullptr;
  constexpr uint32_t kOpXeSwap = 0x4A;

  // NHL_REPLAY_DIAG=<startFrame>: from that swap-frame onward, log every leaf
  // packet (base, count, raw header dwords, decoded type/op/size) via REXLOG so
  // it interleaves with the SDK's own ExecutePacket errors in the log file — the
  // last leaf logged before an "ExecutePacketType0 overflow" is the culprit.
  const char* diag_env = std::getenv("NHL_REPLAY_DIAG");
  const uint32_t diag_from_frame = diag_env ? std::strtoul(diag_env, nullptr, 10) : 0xFFFFFFFFu;
  uint64_t leaf_index = 0;

  bool did_mem = false, did_edram = false, did_regs = false, did_exec = false;
  auto trace_once = [](bool& flag, const char* msg) {
    if (!flag) {
      flag = true;
      fprintf(stderr, "[replay] first %s ok\n", msg);
      fflush(stderr);
    }
  };

  while (c.CanRead(4)) {
    const auto type = static_cast<gfx::TraceCommandType>(c.U32());
    switch (type) {
      case gfx::TraceCommandType::kPrimaryBufferStart:
      case gfx::TraceCommandType::kIndirectBufferStart: {
        c.U32();  // base_ptr
        c.U32();  // count (0 — unused; content is the inline packets)
        break;
      }
      case gfx::TraceCommandType::kPrimaryBufferEnd:
      case gfx::TraceCommandType::kIndirectBufferEnd:
        break;

      case gfx::TraceCommandType::kPacketStart: {
        const uint32_t base_ptr = c.U32();
        const uint32_t count = c.U32();
        const uint8_t* inline_data = c.Bytes(static_cast<size_t>(count) * 4);
        // Stage the packet bytes into guest memory verbatim (big-endian PM4).
        std::memcpy(memory.TranslatePhysical<uint8_t*>(base_ptr), inline_data,
                    static_cast<size_t>(count) * 4);
        // Proven model: each flattened-trace leaf is one packet; stage its bytes
        // and ExecutePacket(base, count) it (skipping indirect-buffer pointers,
        // whose targets follow inline). An empty leaf (count 0) has nothing to
        // execute — skip it rather than calling ExecutePacket(base, 0), which
        // would re-parse stale guest memory at base.
        // NOTE: the instant-replay scene hits an "ExecutePacketType0 overflow" at
        // ~frame 64 from a malformed packet this flattened model can't represent;
        // that one scene's full replay is a known limitation (its inventory is
        // already captured). Do NOT add per-leaf packet-size "repair" here — it
        // mis-skips thousands of valid packets in the other scenes.
        bool skip = (count == 0);
        if (skip) {
          ++stats.skipped_empty_leaves;
        }
        if (count >= 1) {
          uint32_t head = (uint32_t(inline_data[0]) << 24) | (uint32_t(inline_data[1]) << 16) |
                          (uint32_t(inline_data[2]) << 8) | uint32_t(inline_data[3]);
          if ((head >> 30) == 3) {
            uint32_t op = (head >> 8) & 0x7F;
            if (op == kOpIndirectBuffer || op == kOpIndirectBufferPfd)
              skip = true;  // target follows inline
            if (op == kOpWaitRegMem) {
              skip = true;  // wait that can never be satisfied in serial replay
              ++stats.skipped_wait_reg_mem;
            }
            if (swap_only && op != kOpXeSwap) skip = true;  // diagnostic: present only
            if (load_only) skip = true;                     // diagnostic: load nothing executes
            if (op == 0x22 || op == 0x36) ++stats.draw_packets;
          }
          if (stats.frames >= diag_from_frame) {
            const uint32_t ptype = head >> 30;
            const uint32_t cf = (head >> 16) & 0x3FFF;       // count_field
            const uint32_t pkt_size = (ptype == 2) ? 1u : (cf + 2u);
            auto bedw = [&](uint32_t i) -> uint32_t {
              return (i + 1) <= count
                         ? (uint32_t(inline_data[i * 4]) << 24) |
                               (uint32_t(inline_data[i * 4 + 1]) << 16) |
                               (uint32_t(inline_data[i * 4 + 2]) << 8) |
                               uint32_t(inline_data[i * 4 + 3])
                         : 0u;
            };
            REXLOG_INFO(
                "[diag] leaf#{} f{} base={:08X} count={} ptype={} op={:02X} cf={} pkt_size={} "
                "skip={} | dw: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                leaf_index, stats.frames, base_ptr, count, ptype, (head >> 8) & 0x7F, cf, pkt_size,
                skip, bedw(0), bedw(1), bedw(2), bedw(3), bedw(4), bedw(5));
          }
        }
        ++leaf_index;
        packet_stack.push_back({base_ptr, count, count, skip});
        break;
      }
      case gfx::TraceCommandType::kPacketEnd: {
        if (!packet_stack.empty()) {
          const Pending p = packet_stack.back();
          packet_stack.pop_back();
          if (!p.skip) {
            trace_once(did_exec, "ExecutePacket (leaf)");
            cp.ExecutePacket(p.base, p.exec_count);
            ++stats.primary_packets;
          }
        }
        break;
      }

      case gfx::TraceCommandType::kMemoryRead:
      case gfx::TraceCommandType::kMemoryWrite: {
        const uint32_t base_ptr = c.U32();
        const auto enc = static_cast<gfx::MemoryEncodingFormat>(c.U32());
        const uint32_t enc_len = c.U32();
        const uint32_t dec_len = c.U32();
        const uint8_t* payload = c.Bytes(enc_len);
        std::vector<uint8_t> buf = Decode(enc, payload, enc_len, dec_len);
        if (buf.size() != dec_len) {
          fprintf(stderr,
                  "[replay] DECODE MISMATCH base=%08X enc=%u enc_len=%u dec_len=%u got=%zu\n",
                  base_ptr, static_cast<uint32_t>(enc), enc_len, dec_len, buf.size());
          fflush(stderr);
        }
        if (!buf.empty()) {
          trace_once(did_mem, "memory range write");
          std::memcpy(memory.TranslatePhysical<uint8_t*>(base_ptr), buf.data(), buf.size());
          cp.TracePlaybackWroteMemory(base_ptr, static_cast<uint32_t>(buf.size()));
          ++stats.memory_ranges;
        }
        break;
      }

      case gfx::TraceCommandType::kEdramSnapshot: {
        const auto enc = static_cast<gfx::MemoryEncodingFormat>(c.U32());
        const uint32_t enc_len = c.U32();
        const uint8_t* payload = c.Bytes(enc_len);
        std::vector<uint8_t> buf = Decode(enc, payload, enc_len, /*dec_hint=*/0);
        if (!buf.empty()) {
          trace_once(did_edram, "RestoreEdramSnapshot");
          cp.RestoreEdramSnapshot(buf.data());
          ++stats.edram_snapshots;
        }
        break;
      }

      case gfx::TraceCommandType::kRegisters: {
        const uint32_t first = c.U32();
        const uint32_t reg_count = c.U32();
        const uint32_t exec_and_pad = c.U32();  // bool execute_callbacks + 3 pad
        const auto enc = static_cast<gfx::MemoryEncodingFormat>(c.U32());
        const uint32_t enc_len = c.U32();
        const uint8_t* payload = c.Bytes(enc_len);
        std::vector<uint8_t> buf = Decode(enc, payload, enc_len, reg_count * 4u);
        if (buf.size() >= reg_count * 4u) {
          trace_once(did_regs, "RestoreRegisters");
          cp.RestoreRegisters(first, reinterpret_cast<const uint32_t*>(buf.data()), reg_count,
                              (exec_and_pad & 0xFF) != 0);
          ++stats.register_blocks;
        }
        break;
      }

      case gfx::TraceCommandType::kGammaRamp: {
        // component(uint8+3 pad) + enc + enc_len + payload. Gamma does not affect
        // IssueDraw; skip the restore for now (advance past the payload).
        c.U32();  // rw_component + pad
        c.U32();  // enc
        const uint32_t enc_len = c.U32();
        c.Skip(enc_len);
        break;
      }

      case gfx::TraceCommandType::kEvent: {
        const uint32_t event_type = c.U32();
        if (event_type == static_cast<uint32_t>(gfx::EventCommand::Type::kSwap)) {
          ++stats.frames;
        }
        break;
      }

      default:
        stats.error = "desync: unknown command type at offset " + std::to_string(c.pos() - 4);
        return;
    }
  }
  stats.ok = true;
}

}  // namespace

ReplayStats ReplayTrace(gfx::GraphicsSystem& graphics_system, const std::string& trace_path) {
  ReplayStats stats;

  std::ifstream f(trace_path, std::ios::binary | std::ios::ate);
  if (!f) {
    stats.error = "could not open trace file";
    return stats;
  }
  const std::streamsize len = f.tellg();
  f.seekg(0);
  std::vector<uint8_t> data(static_cast<size_t>(len));
  if (!f.read(reinterpret_cast<char*>(data.data()), len)) {
    stats.error = "could not read trace file";
    return stats;
  }

  auto* memory = graphics_system.memory();
  auto* cp = graphics_system.command_processor();
  if (!memory || !cp) {
    stats.error = "graphics system has no memory / command processor";
    return stats;
  }

  // Drive the stream on the command processor's worker thread. CallInThread
  // posts the work there; the future blocks us until it completes.
  std::promise<void> done;
  std::future<void> fut = done.get_future();
  cp->CallInThread([&]() {
    DriveStream(data, *memory, *cp, stats);
    done.set_value();
  });
  fut.wait();
  return stats;
}

}  // namespace nhl::replay
