// nhl-legacy-builder - CLI helpers: progress line + parsed options.

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace packager {

// Single-line \r-updated progress: percent, throughput, current file.
// Throttled to ~10 Hz so the console is not the bottleneck.
class ProgressRenderer {
 public:
  void Update(uint64_t done, uint64_t total, std::string_view current);
  void Finish();  // newline-terminates the progress line if one was drawn

 private:
  using Clock = std::chrono::steady_clock;
  Clock::time_point start_{};
  Clock::time_point last_draw_{};
  bool started_ = false;
  bool drew_ = false;
};

enum class Command { kNone, kInstall, kVerify, kHelp, kVersion };

struct Options {
  Command command = Command::kNone;
  std::filesystem::path iso;      // --iso
  std::filesystem::path from;     // --from (pre-extracted folder)
  std::filesystem::path out;      // --out
  std::filesystem::path payload;  // --payload override (default: exe_dir/payload)
  std::string parse_error;        // non-empty -> print + usage, exit 2
};

// Wide argv: the tool uses a wide entry point both for correct Unicode
// paths and because rexruntime's GetExecutablePath requires the process CRT
// to be wide-initialized (_get_wpgmptr fail-fasts under a narrow main).
Options ParseArgs(int argc, wchar_t** argv);
void PrintUsage();

}  // namespace packager
