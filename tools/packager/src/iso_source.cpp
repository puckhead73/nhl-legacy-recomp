#include "iso_source.h"

#include <fstream>
#include <functional>

#include <rex/crypto/sha256.h>
#include <rex/filesystem/devices/disc_image_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/memory/mapped_memory.h>

namespace packager {

namespace fsys = rex::filesystem;

namespace {
// Stream chunk for mapped-file -> ofstream copies. Large enough for disc
// throughput, small enough for ~10Hz progress updates on slow drives.
constexpr size_t kCopyChunk = 4 * 1024 * 1024;
}  // namespace

IsoSource::IsoSource() = default;
IsoSource::~IsoSource() = default;

bool IsoSource::Open(const std::filesystem::path& input, Error& err) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(input, ec)) {
    err = {ErrorCode::kInputMissing, input.string()};
    return false;
  }
  iso_path_ = input;
  device_ = std::make_unique<fsys::DiscImageDevice>("\\GAME", input);
  if (!device_->Initialize()) {
    device_.reset();
    err = {ErrorCode::kNotXdvdfs, input.string()};
    return false;
  }
  return true;
}

bool IsoSource::HashDefaultXex(std::string& out_sha256_hex, uint64_t& out_size,
                               Error& err) {
  fsys::Entry* entry = device_->ResolvePath("default.xex");
  if (!entry || (entry->attributes() & fsys::kFileAttributeDirectory)) {
    err = {ErrorCode::kNoDefaultXex};
    return false;
  }
  auto mm = entry->OpenMapped(rex::memory::MappedMemory::Mode::kRead, 0,
                              entry->size());
  if (!mm || !mm->data() || mm->size() != entry->size()) {
    err = {ErrorCode::kTruncated, "failed to map default.xex"};
    return false;
  }
  out_size = mm->size();
  out_sha256_hex = rex::crypto::sha256(std::string_view(
      reinterpret_cast<const char*>(mm->data()), mm->size()));
  return true;
}

void IsoSource::CollectFiles(std::vector<Item>& out) const {
  std::function<void(const fsys::Entry*, const std::string&)> recurse =
      [&](const fsys::Entry* dir, const std::string& prefix) {
        for (const auto& child : dir->children()) {
          const std::string rel =
              prefix.empty() ? child->name() : prefix + "/" + child->name();
          if (child->attributes() & fsys::kFileAttributeDirectory) {
            recurse(child.get(), rel);
          } else {
            out.push_back({child.get(), rel});
          }
        }
      };
  recurse(device_->root(), "");
}

bool IsoSource::List(std::vector<GameFile>& out, Error& err) {
  (void)err;
  std::vector<Item> items;
  CollectFiles(items);
  out.reserve(items.size());
  for (const auto& item : items) {
    out.push_back({item.rel_path, item.entry->size()});
  }
  return true;
}

bool IsoSource::ExtractAll(const std::filesystem::path& dest_dir,
                           const ProgressFn& progress, Error& err) {
  std::vector<Item> items;
  CollectFiles(items);

  const uint64_t total = device_->total_file_size();
  uint64_t done = 0;

  for (const auto& item : items) {
    const std::filesystem::path dst =
        dest_dir / std::filesystem::path(item.rel_path).make_preferred();
    std::error_code ec;
    std::filesystem::create_directories(dst.parent_path(), ec);

    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out) {
      err = {ErrorCode::kOutDirUnwritable, dst.string()};
      return false;
    }

    const size_t size = item.entry->size();
    if (size > 0) {
      auto mm = item.entry->OpenMapped(rex::memory::MappedMemory::Mode::kRead,
                                       0, size);
      if (!mm || !mm->data() || mm->size() != size) {
        err = {ErrorCode::kTruncated, "failed to read " + item.rel_path};
        return false;
      }
      const uint8_t* src = mm->data();
      size_t remaining = size;
      while (remaining > 0) {
        const size_t chunk = remaining < kCopyChunk ? remaining : kCopyChunk;
        out.write(reinterpret_cast<const char*>(src), chunk);
        if (!out) {
          const auto space = std::filesystem::space(dest_dir, ec);
          err = (!ec && space.available < remaining)
                    ? Error{ErrorCode::kDiskFull, dst.string()}
                    : Error{ErrorCode::kExtractFailed, dst.string()};
          return false;
        }
        src += chunk;
        remaining -= chunk;
        done += chunk;
        if (progress) progress(done, total, item.rel_path);
      }
    } else if (progress) {
      progress(done, total, item.rel_path);
    }
  }
  return true;
}

std::string IsoSource::Describe() const {
  const auto& info = device_->disc_info();
  char buf[160];
  snprintf(buf, sizeof(buf),
           "XDVDFS image, game partition at 0x%zX, %llu files, %.2f GB",
           info.game_offset,
           static_cast<unsigned long long>(device_->file_count()),
           device_->total_file_size() / (1024.0 * 1024.0 * 1024.0));
  return buf;
}

}  // namespace packager
