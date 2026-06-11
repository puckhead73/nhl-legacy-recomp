#include "folder_source.h"

#include <algorithm>
#include <cctype>
#include <fstream>

#include <rex/crypto/sha256.h>

namespace packager {

namespace {

constexpr size_t kCopyChunk = 4 * 1024 * 1024;

std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

}  // namespace

bool FolderSource::Open(const std::filesystem::path& input, Error& err) {
  std::error_code ec;
  if (!std::filesystem::is_directory(input, ec)) {
    err = {ErrorCode::kInputMissing, input.string()};
    return false;
  }
  root_ = input;
  for (const auto& de : std::filesystem::directory_iterator(root_, ec)) {
    if (de.is_regular_file() &&
        ToLower(de.path().filename().string()) == "default.xex") {
      xex_path_ = de.path();
      break;
    }
  }
  if (ec || xex_path_.empty()) {
    err = {ErrorCode::kNoDefaultXex, root_.string()};
    return false;
  }
  return true;
}

bool FolderSource::HashDefaultXex(std::string& out_sha256_hex,
                                  uint64_t& out_size, Error& err) {
  std::error_code ec;
  out_size = std::filesystem::file_size(xex_path_, ec);
  if (ec) {
    err = {ErrorCode::kInputMissing, xex_path_.string()};
    return false;
  }
  out_sha256_hex = ToLower(rex::crypto::sha256_file(xex_path_));
  if (out_sha256_hex.size() != 64) {
    err = {ErrorCode::kInputMissing, "failed to hash " + xex_path_.string()};
    return false;
  }
  return true;
}

bool FolderSource::List(std::vector<GameFile>& out, Error& err) {
  std::error_code ec;
  for (auto it = std::filesystem::recursive_directory_iterator(root_, ec);
       !ec && it != std::filesystem::recursive_directory_iterator(); ++it) {
    if (it->is_regular_file(ec) && !ec) {
      out.push_back({it->path().lexically_relative(root_).generic_string(),
                     it->file_size(ec)});
    }
  }
  if (ec) {
    err = {ErrorCode::kInputMissing, ec.message()};
    return false;
  }
  return true;
}

bool FolderSource::ExtractAll(const std::filesystem::path& dest_dir,
                              const ProgressFn& progress, Error& err) {
  std::vector<GameFile> files;
  if (!List(files, err)) return false;

  uint64_t total = 0;
  for (const auto& f : files) total += f.size;
  uint64_t done = 0;

  // Most game trees are a few huge .big archives plus many small files.
  // Small files go through copy_file (the kernel fast path - per-file
  // overhead, not per-chunk); only files large enough to stall the progress
  // line use a chunked manual copy.
  constexpr uint64_t kChunkedCopyThreshold = 256ull * 1024 * 1024;

  std::vector<char> buf;
  std::filesystem::path last_parent;
  for (const auto& f : files) {
    const std::filesystem::path src =
        root_ / std::filesystem::path(f.rel_path).make_preferred();
    const std::filesystem::path dst =
        dest_dir / std::filesystem::path(f.rel_path).make_preferred();
    std::error_code ec;
    if (dst.parent_path() != last_parent) {
      last_parent = dst.parent_path();
      std::filesystem::create_directories(last_parent, ec);
    }

    if (f.size < kChunkedCopyThreshold) {
      std::filesystem::copy_file(
          src, dst, std::filesystem::copy_options::overwrite_existing, ec);
      if (ec) {
        const auto space = std::filesystem::space(dest_dir, ec);
        err = (!ec && space.available < f.size)
                  ? Error{ErrorCode::kDiskFull, dst.string()}
                  : Error{ErrorCode::kExtractFailed, dst.string()};
        return false;
      }
      done += f.size;
      if (progress) progress(done, total, f.rel_path);
      continue;
    }

    if (buf.empty()) buf.resize(kCopyChunk);
    std::ifstream in(src, std::ios::binary);
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!in) {
      err = {ErrorCode::kInputMissing, src.string()};
      return false;
    }
    if (!out) {
      err = {ErrorCode::kOutDirUnwritable, dst.string()};
      return false;
    }
    while (in) {
      in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
      const std::streamsize n = in.gcount();
      if (n <= 0) break;
      out.write(buf.data(), n);
      if (!out) {
        const auto space = std::filesystem::space(dest_dir, ec);
        err = (!ec && space.available < f.size)
                  ? Error{ErrorCode::kDiskFull, dst.string()}
                  : Error{ErrorCode::kExtractFailed, dst.string()};
        return false;
      }
      done += static_cast<uint64_t>(n);
      if (progress) progress(done, total, f.rel_path);
    }
    if (progress) progress(done, total, f.rel_path);
  }
  return true;
}

std::string FolderSource::Describe() const {
  return "extracted game folder: " + root_.string();
}

}  // namespace packager
