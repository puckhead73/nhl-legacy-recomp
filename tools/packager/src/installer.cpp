#include "installer.h"

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "cli.h"
#include "errors.h"
#include "folder_source.h"
#include "game_source.h"
#include "iso_source.h"
#include "manifest.h"

namespace packager {

namespace fs = std::filesystem;

namespace {

// Headroom on top of the exact byte total so the extract does not land the
// destination volume at 0 bytes free.
constexpr uint64_t kDiskSlack = 256ull * 1024 * 1024;

int Fail(const Error& err) {
  fprintf(stderr, "\nERROR: %s\n", Describe(err).c_str());
  return 1;
}

// Payload entries that are documentation/metadata, not part of the install.
bool IsPayloadMetadata(const fs::path& name) {
  const std::string n = name.filename().string();
  if (n == "manifest.toml") return true;
  if (n.size() >= 3 && n.compare(n.size() - 3, 3, ".md") == 0) return true;
  if (n.rfind("README", 0) == 0) return true;
  return false;
}

uint64_t DirSize(const fs::path& dir) {
  uint64_t total = 0;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(dir, ec);
       !ec && it != fs::recursive_directory_iterator(); ++it) {
    if (it->is_regular_file(ec) && !ec) total += it->file_size(ec);
  }
  return total;
}

bool IsInsideOf(const fs::path& inner, const fs::path& outer) {
  std::error_code ec;
  const fs::path a = fs::weakly_canonical(inner, ec);
  const fs::path b = fs::weakly_canonical(outer, ec);
  if (ec) return false;
  auto bi = b.begin();
  for (auto ai = a.begin(); bi != b.end(); ++ai, ++bi) {
    if (ai == a.end() || *ai != *bi) return false;
  }
  return true;
}

}  // namespace

int RunInstall(const InstallOptions& opts) {
  // --- Load the payload manifest (pins the supported game build) ---
  PayloadManifest manifest;
  Error err;
  if (!LoadPayloadManifest(opts.payload_dir / "manifest.toml", manifest,
                           err)) {
    return Fail(err);
  }

  // --- Open and validate the input ---
  std::unique_ptr<GameSource> source;
  if (opts.input_is_folder) {
    source = std::make_unique<FolderSource>();
  } else {
    source = std::make_unique<IsoSource>();
  }
  fprintf(stdout, "Opening %s ...\n", opts.input.string().c_str());
  if (!source->Open(opts.input, err)) return Fail(err);
  fprintf(stdout, "  Detected: %s\n", source->Describe().c_str());

  fprintf(stdout, "Checking default.xex against the supported build ...\n");
  std::string xex_hash;
  uint64_t xex_size = 0;
  if (!source->HashDefaultXex(xex_hash, xex_size, err)) return Fail(err);

  if (manifest.xex_size_bytes && xex_size != manifest.xex_size_bytes) {
    err = {ErrorCode::kHashMismatch,
           "default.xex size " + std::to_string(xex_size) +
               " differs from the supported build's " +
               std::to_string(manifest.xex_size_bytes)};
    return Fail(err);
  }
  if (xex_hash != manifest.xex_sha256) {
    err = {ErrorCode::kHashMismatch,
           "expected SHA-256 " + manifest.xex_sha256 + ", got " + xex_hash};
    return Fail(err);
  }
  fprintf(stdout, "  OK: default.xex matches %s (SHA-256 %s)\n",
          manifest.xex_title.empty() ? "the supported build"
                                     : manifest.xex_title.c_str(),
          xex_hash.c_str());

  if (opts.verify_only) {
    fprintf(stdout,
            "\nVerification passed. This dump is supported - run 'install' "
            "to build the port.\n");
    return 0;
  }

  // --- Preflight the destination ---
  const fs::path game_dir = opts.out_dir / "game";
  if (opts.input_is_folder && IsInsideOf(game_dir, opts.input)) {
    err = {ErrorCode::kOutDirUnwritable,
           "the install destination is inside the input folder"};
    return Fail(err);
  }

  std::error_code ec;
  fs::create_directories(game_dir, ec);
  if (ec) {
    err = {ErrorCode::kOutDirUnwritable, opts.out_dir.string()};
    return Fail(err);
  }
  {
    // Writability probe before committing to a multi-GB extract.
    const fs::path probe = opts.out_dir / ".write_probe";
    std::ofstream f(probe, std::ios::binary | std::ios::trunc);
    if (!f) {
      err = {ErrorCode::kOutDirUnwritable, opts.out_dir.string()};
      return Fail(err);
    }
    f.close();
    fs::remove(probe, ec);
  }

  std::vector<GameFile> files;
  if (!source->List(files, err)) return Fail(err);
  uint64_t total_bytes = 0;
  for (const auto& f : files) total_bytes += f.size;
  const uint64_t payload_bytes = DirSize(opts.payload_dir);

  const auto space = fs::space(opts.out_dir, ec);
  if (!ec && space.available < total_bytes + payload_bytes + kDiskSlack) {
    err = {ErrorCode::kDiskFull,
           "need ~" +
               std::to_string(
                   (total_bytes + payload_bytes + kDiskSlack) >> 30) +
               " GB free at " + opts.out_dir.string() + ", only " +
               std::to_string(space.available >> 30) + " GB available"};
    return Fail(err);
  }

  // Marker so an aborted run is detectable; removed on success.
  const fs::path marker = opts.out_dir / ".nhl-legacy-install.incomplete";
  std::ofstream(marker, std::ios::binary | std::ios::trunc).flush();

  // --- Extract game files ---
  fprintf(stdout, "Extracting %zu game files (%.2f GB) to %s ...\n",
          files.size(), total_bytes / (1024.0 * 1024.0 * 1024.0),
          game_dir.string().c_str());
  ProgressRenderer progress;
  const bool extracted = source->ExtractAll(
      game_dir,
      [&](uint64_t done, uint64_t total, std::string_view current) {
        progress.Update(done, total, current);
      },
      err);
  progress.Finish();
  if (!extracted) return Fail(err);

  // --- Lay down the prebuilt port binaries ---
  fprintf(stdout, "Installing the port binaries ...\n");
  for (const auto& de : fs::directory_iterator(opts.payload_dir, ec)) {
    if (!de.is_regular_file() || IsPayloadMetadata(de.path())) continue;
    fs::copy_file(de.path(), opts.out_dir / de.path().filename(),
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
      err = {ErrorCode::kExtractFailed,
             "copying " + de.path().filename().string() + ": " +
                 ec.message()};
      return Fail(err);
    }
  }

  const fs::path exe = opts.out_dir / manifest.port_exe;
  if (!fs::is_regular_file(exe, ec)) {
    err = {ErrorCode::kBadPayload,
           manifest.port_exe + " missing from the payload"};
    return Fail(err);
  }

  fs::remove(marker, ec);

  fprintf(stdout,
          "\nDone! Your NHL Legacy port is ready:\n"
          "  %s\n"
          "Launch it directly - no arguments needed.\n",
          exe.string().c_str());
  return 0;
}

}  // namespace packager
