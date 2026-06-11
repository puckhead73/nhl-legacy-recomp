// See injection_registry.h.

#include "injection_registry.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>

#include <rex/logging.h>

#if NHLLEGACY_HAVE_RX2FFI
#include "rx2_ffi.h"
#endif

namespace nhllegacy {

InjectionRegistry& InjectionRegistry::Get() {
  static InjectionRegistry instance;
  return instance;
}

uint64_t InjectionRegistry::HashPrefix(const uint8_t* data, size_t len) {
  // FNV-1a 64-bit over the first kHashPrefix bytes (caller guarantees len >= prefix).
  const size_t n = std::min(len, kHashPrefix);
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) {
    h ^= data[i];
    h *= 1099511628211ull;
  }
  return h;
}

namespace {
// An all-zero prefix is degenerate: it is what a MISSING/empty texture (zero-filled
// guest RAM) and any asset with a transparent leading region both hash to, so it
// would become a catch-all that maps every empty address to one asset. Reject it on
// both the register and lookup sides — a real, resident texture is never all-zero.
bool AllZero(const uint8_t* data, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (data[i]) return false;
  }
  return true;
}
}  // namespace

void InjectionRegistry::RegisterRx2(const std::string& relpath, const uint8_t* rx2, size_t len) {
#if NHLLEGACY_HAVE_RX2FFI
  if (!rx2 || len == 0) {
    return;
  }
  // Iterate slots until the FFI reports the slot index is out of range. A `.rx2`
  // container that fails to parse (rc=kRx2FfiErrParse) yields nothing -> skipped.
  // 64 is an upper bound on the texlib slot count; the loop exits early on
  // kRx2FfiErrSlotRange, so the constant is only a safety cap, not the real count.
  for (uint32_t slot = 0; slot < 64; ++slot) {
    Rx2SlotInfo info{};
    uint8_t* out = nullptr;
    size_t out_len = 0;
    const int32_t rc = rx2ffi_decode_slot(rx2, len, slot, &info, &out, &out_len);
    if (rc == kRx2FfiErrSlotRange) {
      break;  // no more slots
    }
    if (rc != kRx2FfiOk || !out || out_len < kHashPrefix) {
      if (out) rx2ffi_free(out, out_len);
      if (rc == kRx2FfiErrParse) break;  // not a texlib we can decode
      continue;
    }
    if (AllZero(out, kHashPrefix)) {  // degenerate empty prefix -> not a useful key
      rx2ffi_free(out, out_len);
      continue;
    }
    const uint64_t h = HashPrefix(out, out_len);
    rx2ffi_free(out, out_len);
    std::lock_guard<std::mutex> guard(mutex_);
    auto [it, inserted] = by_hash_.try_emplace(h, InjectResolved{0, relpath, slot});
    if (!inserted && (it->second.relpath != relpath || it->second.slot != slot)) {
      // A genuine 64-bit prefix-hash collision between two distinct assets. First
      // writer wins (kept), but surface it — silently mapping both to one asset would
      // inject the wrong texture.
      REXLOG_WARN("[inject] prefix-hash 0x{:016X} collision: kept {}:slot{}, ignored {}:slot{}",
                  h, it->second.relpath, it->second.slot, relpath, slot);
    }
  }
#else
  (void)relpath;
  (void)rx2;
  (void)len;
#endif
}

size_t InjectionRegistry::ScanDirectory(const std::filesystem::path& root,
                                        const std::filesystem::path& relative_root) {
  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec)) {
    return 0;
  }
  const size_t before = registry_size();
  for (auto it = std::filesystem::recursive_directory_iterator(
           root, std::filesystem::directory_options::skip_permission_denied, ec);
       it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    const std::filesystem::path& p = it->path();
    if (!it->is_regular_file(ec)) continue;
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    if (ext != ".rx2") {  // fully case-insensitive (was only folding ext[1])
      continue;
    }
    std::ifstream f(p, std::ios::binary);
    if (!f) continue;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
    if (bytes.empty()) continue;
    // Use a shared relative root when scanning selected sibling categories so one
    // sidecar can reference all of them through a single injection root.
    const std::filesystem::path& rel_root = relative_root.empty() ? root : relative_root;
    std::string rel = std::filesystem::relative(p, rel_root, ec).string();
    std::replace(rel.begin(), rel.end(), '/', '\\');
    RegisterRx2(rel, bytes.data(), bytes.size());
  }
  return registry_size() - before;
}

bool InjectionRegistry::LookupBytes(const uint8_t* data, size_t len, std::string& out_relpath,
                                    uint32_t& out_slot) const {
  if (!data || len < kHashPrefix || AllZero(data, kHashPrefix)) {
    return false;  // missing/empty region -> never a valid correlation
  }
  const uint64_t h = HashPrefix(data, len);
  std::lock_guard<std::mutex> guard(mutex_);
  auto it = by_hash_.find(h);
  if (it == by_hash_.end()) {
    return false;
  }
  out_relpath = it->second.relpath;
  out_slot = it->second.slot;
  return true;
}

void InjectionRegistry::RecordResolved(uint32_t addr, const std::string& relpath, uint32_t slot) {
  std::lock_guard<std::mutex> guard(mutex_);
  resolved_.insert_or_assign(addr, InjectResolved{addr, relpath, slot});
}

bool InjectionRegistry::CorrelateRecord(uint32_t addr, const uint8_t* data, size_t len) {
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (resolved_.count(addr)) {
      return true;  // already mapped this address
    }
  }
  std::string relpath;
  uint32_t slot = 0;
  if (!LookupBytes(data, len, relpath, slot)) {
    return false;
  }
  RecordResolved(addr, relpath, slot);
  return true;
}

size_t InjectionRegistry::registry_size() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return by_hash_.size();
}

size_t InjectionRegistry::resolved_count() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return resolved_.size();
}

bool InjectionRegistry::WriteSidecar(const std::filesystem::path& path) const {
  std::vector<InjectResolved> entries;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    entries.reserve(resolved_.size());
    for (const auto& [addr, r] : resolved_) entries.push_back(r);
  }
  std::sort(entries.begin(), entries.end(),
            [](const InjectResolved& a, const InjectResolved& b) { return a.addr < b.addr; });
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    return false;
  }
  f << "{\n  \"version\": 1,\n  \"captures\": [\n";
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto& e = entries[i];
    char addrbuf[16];
    std::snprintf(addrbuf, sizeof(addrbuf), "0x%08X", e.addr);
    // relpath: emit with forward slashes and escape backslashes -> forward slashes
    // (no other JSON-special chars occur in asset paths).
    std::string rel = e.relpath;
    std::replace(rel.begin(), rel.end(), '\\', '/');
    f << "    {\"addr\": \"" << addrbuf << "\", \"rx2\": \"" << rel << "\", \"slot\": " << e.slot
      << "}" << (i + 1 < entries.size() ? "," : "") << "\n";
  }
  f << "  ]\n}\n";
  return static_cast<bool>(f);
}

std::vector<InjectResolved> InjectionRegistry::ParseSidecar(const std::filesystem::path& path) {
  std::vector<InjectResolved> out;
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return out;
  }
  const std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  // Minimal object scan for the format WriteSidecar emits (one {"addr","rx2","slot"}
  // object per capture). We don't need a general JSON parser, but each entry's fields
  // MUST be read within that entry's own {...} braces: a field missing from object N
  // must NOT be satisfied by object N+1's field, or addr<->rx2 mis-pair and we inject
  // an asset into the wrong guest address (silent corruption).

  // Extract a "key": "value" string field searching ONLY within [from, end). Returns
  // false (not npos+1 arithmetic) if the colon or either quote is missing/out of range.
  auto find_str_field = [&](const char* key, size_t from, size_t end,
                            std::string& val) -> bool {
    const std::string pat = std::string("\"") + key + "\"";
    size_t k = s.find(pat, from);
    if (k == std::string::npos || k >= end) return false;
    size_t colon = s.find(':', k + pat.size());
    if (colon == std::string::npos || colon >= end) return false;
    size_t q1 = s.find('"', colon + 1);
    if (q1 == std::string::npos || q1 >= end) return false;
    size_t q2 = s.find('"', q1 + 1);
    if (q2 == std::string::npos || q2 >= end) return false;
    val = s.substr(q1 + 1, q2 - q1 - 1);
    return true;
  };
  // Extract an integer "key": N field within [from, end). Optional; default 0.
  auto find_uint_field = [&](const char* key, size_t from, size_t end,
                             uint32_t& val) -> bool {
    const std::string pat = std::string("\"") + key + "\"";
    size_t k = s.find(pat, from);
    if (k == std::string::npos || k >= end) return false;
    size_t colon = s.find(':', k + pat.size());
    if (colon == std::string::npos || colon >= end) return false;
    val = uint32_t(std::strtoul(s.c_str() + colon + 1, nullptr, 10));
    return true;
  };

  // Start scanning after the "captures" '[' so the outer object's brace isn't treated
  // as an entry; entries contain no nested braces, so the first '}' after each '{'
  // bounds that entry exactly.
  size_t pos = 0;
  size_t cap = s.find("\"captures\"");
  if (cap != std::string::npos) {
    size_t bracket = s.find('[', cap);
    if (bracket != std::string::npos) pos = bracket + 1;
  }
  while (true) {
    size_t obj_begin = s.find('{', pos);
    if (obj_begin == std::string::npos) break;
    size_t obj_end = s.find('}', obj_begin + 1);
    if (obj_end == std::string::npos) break;
    pos = obj_end + 1;

    std::string addr_s, rx2_s;
    if (!find_str_field("addr", obj_begin, obj_end, addr_s) ||
        !find_str_field("rx2", obj_begin, obj_end, rx2_s)) {
      // Missing a required field inside this object -> skip it rather than reach into
      // the next one. (Also skips any non-capture object encountered.)
      continue;
    }
    uint32_t slot = 0;
    find_uint_field("slot", obj_begin, obj_end, slot);  // optional

    InjectResolved e;
    e.addr = uint32_t(std::strtoul(addr_s.c_str(), nullptr, 0));
    e.relpath = rx2_s;
    e.slot = slot;
    out.push_back(std::move(e));
  }
  return out;
}

}  // namespace nhllegacy
