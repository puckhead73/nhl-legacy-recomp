// nhl-legacy-builder - install/verify orchestration.

#pragma once

#include <filesystem>

namespace packager {

struct InstallOptions {
  std::filesystem::path input;        // ISO file or extracted game folder
  bool input_is_folder = false;       // chooses FolderSource vs IsoSource
  std::filesystem::path out_dir;      // install destination (install only)
  std::filesystem::path payload_dir;  // port binaries + manifest.toml
  bool verify_only = false;
};

// Runs the full validate -> preflight -> extract -> lay-down flow (or just
// the validation for verify_only). Prints progress/results to stdout and
// errors to stderr. Returns a process exit code (0 = success).
int RunInstall(const InstallOptions& opts);

}  // namespace packager
