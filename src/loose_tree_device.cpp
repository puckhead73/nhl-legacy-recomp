// nhllegacy - read-only _compiled tree with per-file .dds -> .rx2 synth.
// See loose_tree_device.h.

#include "loose_tree_device.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <span>
#include <utility>

#include <rex/filesystem/file.h>
#include <rex/logging.h>
#include <rex/system/xtypes.h>

#if NHLLEGACY_HAVE_RX2FFI
#include "rx2_ffi.h"
#endif

#include "injection_registry.h"

namespace nhllegacy {

namespace fs = rex::filesystem;
using rex::X_STATUS;  // so X_STATUS_* macros resolve outside namespace rex

namespace {

bool EndsWithCi(std::string_view s, std::string_view suffix) {
  if (s.size() < suffix.size()) return false;
  return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin(), [](char a, char b) {
    return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
  });
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
}

// File backed by an in-memory buffer (synthesized .rx2).
class MemFile final : public fs::File {
 public:
  MemFile(uint32_t access, fs::Entry* entry,
          std::shared_ptr<const std::vector<uint8_t>> data)
      : fs::File(access, entry), data_(std::move(data)) {}
  void Destroy() override { delete this; }
  X_STATUS ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                    size_t* out) override {
    const std::vector<uint8_t>& d = *data_;
    if (byte_offset >= d.size()) {
      if (out) *out = 0;
      return X_STATUS_END_OF_FILE;
    }
    const size_t n = std::min(buffer.size(), d.size() - byte_offset);
    std::memcpy(buffer.data(), d.data() + byte_offset, n);
    if (out) *out = n;
    return X_STATUS_SUCCESS;
  }
  X_STATUS WriteSync(std::span<const uint8_t>, size_t, size_t* out) override {
    if (out) *out = 0;
    return X_STATUS_UNSUCCESSFUL;
  }

 private:
  std::shared_ptr<const std::vector<uint8_t>> data_;
};

// File backed by a host file on disk (streamed). Directory handles use this
// too (reads return EOF, which is fine — a dir handle is used for relative
// opens / enumeration, not reads).
class HostFile final : public fs::File {
 public:
  HostFile(uint32_t access, fs::Entry* entry, const std::filesystem::path& path)
      : fs::File(access, entry), stream_(path, std::ios::binary) {}
  void Destroy() override { delete this; }
  X_STATUS ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                    size_t* out) override {
    if (out) *out = 0;
    // The seek+read pair mutates the shared stream_, so serialize per handle: if the
    // SDK ever dispatches concurrent ReadSync on one File, an unlocked seek/read race
    // would return torn data (audit M6). Cheap insurance; reads are otherwise serial.
    std::lock_guard<std::mutex> guard(read_mu_);
    if (!stream_) return X_STATUS_END_OF_FILE;
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(byte_offset), std::ios::beg);
    if (!stream_) return X_STATUS_END_OF_FILE;
    stream_.read(reinterpret_cast<char*>(buffer.data()),
                 static_cast<std::streamsize>(buffer.size()));
    const size_t n = static_cast<size_t>(stream_.gcount());
    if (out) *out = n;
    return n == 0 ? X_STATUS_END_OF_FILE : X_STATUS_SUCCESS;
  }
  X_STATUS WriteSync(std::span<const uint8_t>, size_t, size_t* out) override {
    if (out) *out = 0;
    return X_STATUS_UNSUCCESSFUL;
  }

 private:
  std::ifstream stream_;
  std::mutex read_mu_;  // serializes seek+read on stream_ (see ReadSync)
};

// One node of the mirrored _compiled tree. Directories carry populated
// children (so the base Entry's GetChild/ResolvePath/IterateChildren work);
// a .rx2 file with a loose .dds override is synthesized on Open.
class LooseTreeEntry final : public fs::Entry {
 public:
  LooseTreeEntry(fs::Device* device, fs::Entry* parent, std::string_view path,
                 std::filesystem::path host, bool is_dir, size_t size)
      : fs::Entry(device, parent, path),
        host_(std::move(host)),
        is_dir_(is_dir) {
    size_ = size;
    allocation_size_ = size;
    attributes_ = is_dir_ ? fs::kFileAttributeDirectory
                          : (fs::kFileAttributeNormal | fs::kFileAttributeReadOnly);
    const size_t slash = path.find_last_of("\\/");
    name_ = std::string(slash == std::string_view::npos ? path
                                                        : path.substr(slash + 1));
  }

  void Populate() {
    std::error_code ec;
    for (const auto& de : std::filesystem::directory_iterator(host_, ec)) {
      if (ec) break;
      const bool dir = de.is_directory(ec);
      const std::string leaf = de.path().filename().string();
      const std::string child_rel = path().empty() ? leaf : path() + "\\" + leaf;
      // Use a dedicated error_code so a stat failure here doesn't pollute the
      // iterator's ec; on failure file_size() returns (uintmax_t)-1, so clamp to 0
      // rather than wrapping to SIZE_MAX (size_ is cosmetic, but should not be wrong).
      size_t sz = 0;
      if (!dir) {
        std::error_code sec;
        const auto fsz = de.file_size(sec);
        if (!sec) sz = static_cast<size_t>(fsz);
      }
      auto child = std::make_unique<LooseTreeEntry>(device_, this, child_rel,
                                                    de.path(), dir, sz);
      if (dir) child->Populate();
      children_.push_back(std::move(child));
    }
  }

  X_STATUS Open(uint32_t desired_access, fs::File** out_file) override {
    if (is_dir_) {
      *out_file = new HostFile(desired_access, this, host_);
      return X_STATUS_SUCCESS;
    }
    // TEMP diagnostic: log .db opens so we can see which databases the game
    // loads (and when) and confirm a grown DB is served. Remove after testing.
    if (EndsWithCi(name_, ".db")) {
      REXLOG_INFO("[db-open] {} ({} bytes)", path(), size_);
    }
#if NHLLEGACY_HAVE_RX2FFI
    if (EndsWithCi(name_, ".rx2")) {
      auto* dev = static_cast<LooseTreeDevice*>(device_);
      std::shared_ptr<const std::vector<uint8_t>> data;
      {
        std::lock_guard<std::mutex> guard(dev->synth_mutex());
        if (!synth_resolved_) {
          synth_ = dev->BuildSynth(path(), host_);
          synth_resolved_ = true;
        }
        data = synth_;
      }
      // Stage-1 capture seam (NHL_INJECT_CAPTURE): hash the served slot payloads so a
      // live trace capture can correlate guest-RAM texture bases back to this asset.
      // Hash exactly what the game will read into guest RAM: the synthesized buffer if
      // a .dds override is active, else the raw .rx2 on disk. Env-gated; the default
      // path never reads/decodes here. Done once per entry (registry dedupes by hash).
      if (!registered_ && std::getenv("NHL_INJECT_CAPTURE")) {
        if (data) {
          InjectionRegistry::Get().RegisterRx2(path(), data->data(), data->size());
          registered_ = true;
        } else {
          const std::vector<uint8_t> raw = ReadFile(host_);
          if (!raw.empty()) {
            InjectionRegistry::Get().RegisterRx2(path(), raw.data(), raw.size());
            registered_ = true;
          }
          // else: transient empty/locked read -> leave registered_ false so a later
          // Open retries instead of permanently dropping this asset.
        }
      }
      if (data) {
        *out_file = new MemFile(desired_access, this, data);
        return X_STATUS_SUCCESS;
      }
    }
#endif
    *out_file = new HostFile(desired_access, this, host_);
    return X_STATUS_SUCCESS;
  }

  bool can_map() const override { return false; }

 private:
  std::filesystem::path host_;
  bool is_dir_;
  std::shared_ptr<const std::vector<uint8_t>> synth_;
  bool synth_resolved_ = false;
  bool registered_ = false;  // Stage-1: registered served bytes with the registry once
};

}  // namespace

LooseTreeDevice::LooseTreeDevice(std::string_view mount_path,
                                 const std::filesystem::path& compiled_root,
                                 const std::filesystem::path& override_root)
    : fs::Device(mount_path),
      name_("\\Device\\LooseTree"),
      compiled_root_(compiled_root),
      override_root_(override_root) {}

LooseTreeDevice::~LooseTreeDevice() = default;

bool LooseTreeDevice::Initialize() {
  std::error_code ec;
  if (!std::filesystem::is_directory(compiled_root_, ec)) return false;
  auto root = std::make_unique<LooseTreeEntry>(this, /*parent=*/nullptr,
                                               /*path=*/"", compiled_root_,
                                               /*is_dir=*/true, /*size=*/0);
  root->Populate();
  root_ = std::move(root);
  return true;
}

void LooseTreeDevice::Dump(rex::string::StringBuffer*) {}

fs::Entry* LooseTreeDevice::ResolvePath(std::string_view path) {
  if (!root_) return nullptr;
  size_t i = 0;
  while (i < path.size() && (path[i] == '\\' || path[i] == '/')) ++i;
  return root_->ResolvePath(path.substr(i));
}

std::shared_ptr<const std::vector<uint8_t>> LooseTreeDevice::BuildSynth(
    const std::string& rel, const std::filesystem::path& original) {
#if NHLLEGACY_HAVE_RX2FFI
  if (override_root_.empty()) return nullptr;
  std::error_code ec;
  // Resolve the override location, accepting two layouts:
  //   A) a folder named exactly as the .rx2 INCLUDING the extension —
  //      "<rel>/<NN>.dds" (the original hand-authored loose-override convention);
  //   B) the bulk texture exporter's layout (tdb-gui-api export_all_textures):
  //      a folder named after the .rx2 STEM (no extension) holding
  //      "<NN>_<name>.dds" slot files, or — for a single-texture .rx2 — a flat
  //      sibling file "<stem>.dds" (treated as slot 0).
  // Slot index is the LEADING digits of the .dds stem, so stoul("07_diffuse")
  // yields 7 and the exporter's "<NN>_<name>.dds" naming works unchanged.
  const std::filesystem::path rel_path(rel);
  std::filesystem::path rel_noext = rel_path;
  rel_noext.replace_extension();  // drop ".rx2"

  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> dds;
  auto scan_dir = [&](const std::filesystem::path& dir) {
    if (!std::filesystem::is_directory(dir, ec)) return;
    for (const auto& de : std::filesystem::directory_iterator(dir, ec)) {
      if (ec) break;
      if (!de.is_regular_file(ec)) continue;
      const std::filesystem::path& p = de.path();
      if (!EndsWithCi(p.extension().string(), ".dds")) continue;
      uint32_t slot = 0;
      try {
        // On MSVC `unsigned long` is 32-bit, so stoul's range == uint32_t's and an
        // overflow throws std::out_of_range (caught below); the cast is lossless here.
        slot = static_cast<uint32_t>(std::stoul(p.stem().string()));
      } catch (...) {
        continue;  // no leading digits or out-of-range -> skip this slot file
      }
      std::vector<uint8_t> bytes = ReadFile(p);
      if (!bytes.empty()) dds.emplace_back(slot, std::move(bytes));
    }
  };

  // Folder A (with extension) wins; fall back to the exporter's stem folder (B).
  scan_dir(override_root_ / rel_path);
  if (dds.empty()) scan_dir(override_root_ / rel_noext);
  // Single-texture exporter output: a flat sibling "<stem>.dds" == slot 0.
  if (dds.empty()) {
    std::filesystem::path flat = override_root_ / rel_noext;
    flat += ".dds";
    if (std::filesystem::is_regular_file(flat, ec)) {
      std::vector<uint8_t> bytes = ReadFile(flat);
      if (!bytes.empty()) dds.emplace_back(0u, std::move(bytes));
    }
  }
  if (dds.empty()) return nullptr;

  const std::vector<uint8_t> orig = ReadFile(original);
  if (orig.empty()) return nullptr;

  std::vector<Rx2Override> overrides;
  overrides.reserve(dds.size());
  for (const auto& d : dds) {
    overrides.push_back(Rx2Override{d.first, d.second.data(), d.second.size()});
  }

  uint8_t* out = nullptr;
  size_t out_len = 0;
  const int32_t rc = rx2ffi_replace_textures(orig.data(), orig.size(),
                                             overrides.data(), overrides.size(),
                                             &out, &out_len);
  if (rc != kRx2FfiOk || out == nullptr) {
    REXLOG_WARN("[rx2-override] {} -> conversion failed (rc={})", rel, rc);
    rx2ffi_free(out, out_len);
    return nullptr;
  }
  auto data = std::make_shared<std::vector<uint8_t>>(out, out + out_len);
  rx2ffi_free(out, out_len);
  REXLOG_INFO("[rx2-override] {} <- {} loose .dds ({} -> {} bytes)", rel,
              dds.size(), orig.size(), data->size());
  return data;
#else
  (void)rel;
  (void)original;
  return nullptr;
#endif
}

}  // namespace nhllegacy
