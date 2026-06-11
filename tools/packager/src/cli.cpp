#include "cli.h"

#include <cstdio>
#include <string>

namespace packager {

void ProgressRenderer::Update(uint64_t done, uint64_t total,
                              std::string_view current) {
  const auto now = Clock::now();
  if (!started_) {
    start_ = now;
    started_ = true;
  }
  // Throttle redraws, but always draw the 100% state.
  if (drew_ && done < total &&
      now - last_draw_ < std::chrono::milliseconds(100)) {
    return;
  }
  last_draw_ = now;
  drew_ = true;

  const double pct = total ? 100.0 * static_cast<double>(done) /
                                 static_cast<double>(total)
                           : 100.0;
  const double secs =
      std::chrono::duration<double>(now - start_).count();
  const double mbps =
      secs > 0.01 ? (static_cast<double>(done) / (1024.0 * 1024.0)) / secs
                  : 0.0;

  // Keep the line under a typical 120-col console; truncate long names.
  std::string name(current);
  constexpr size_t kMaxName = 60;
  if (name.size() > kMaxName) {
    name = "..." + name.substr(name.size() - (kMaxName - 3));
  }
  fprintf(stdout, "\r  %5.1f%%  %7.1f MB/s  %-*s", pct, mbps,
          static_cast<int>(kMaxName), name.c_str());
  fflush(stdout);
}

void ProgressRenderer::Finish() {
  if (drew_) {
    fprintf(stdout, "\n");
    fflush(stdout);
  }
}

void PrintUsage() {
  fprintf(stdout,
          "NHL Legacy Recomp Builder - turns your NHL Legacy disc dump into "
          "a playable PC port.\n"
          "\n"
          "Usage:\n"
          "  nhl-legacy-builder install --iso <path-to.iso> --out <dir>\n"
          "  nhl-legacy-builder install --from <extracted-folder> --out <dir>\n"
          "  nhl-legacy-builder verify  --iso <path-to.iso>\n"
          "  nhl-legacy-builder verify  --from <extracted-folder>\n"
          "\n"
          "Commands:\n"
          "  install   Validate the dump, extract game files and assemble a\n"
          "            ready-to-run install at <dir>.\n"
          "  verify    Validate the dump only (no extraction). Reports the\n"
          "            detected disc layout and whether default.xex matches\n"
          "            the supported game build.\n"
          "\n"
          "Options:\n"
          "  --iso <path>      Path to a raw Xbox 360 disc image (.iso).\n"
          "  --from <dir>      Path to an already-extracted game folder\n"
          "                    (must contain default.xex).\n"
          "  --out <dir>       Install destination (created if missing).\n"
          "  --payload <dir>   Override the payload directory (default: the\n"
          "                    'payload' folder next to this executable).\n"
          "  --help            Show this help.\n"
          "  --version         Show version information.\n"
          "\n"
          "Run with no arguments for interactive mode.\n");
}

namespace {

// Lossy wide->narrow for error messages only; paths stay wide end-to-end.
std::string Narrow(std::wstring_view w) {
  std::string out;
  out.reserve(w.size());
  for (wchar_t c : w) out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
  return out;
}

}  // namespace

Options ParseArgs(int argc, wchar_t** argv) {
  Options opts;
  auto need_value = [&](int& i, const char* flag) -> const wchar_t* {
    if (i + 1 >= argc) {
      opts.parse_error = std::string(flag) + " requires a value";
      return nullptr;
    }
    return argv[++i];
  };

  for (int i = 1; i < argc; ++i) {
    const std::wstring_view arg = argv[i];
    if (arg == L"install") {
      opts.command = Command::kInstall;
    } else if (arg == L"verify") {
      opts.command = Command::kVerify;
    } else if (arg == L"--help" || arg == L"-h" || arg == L"/?") {
      opts.command = Command::kHelp;
      return opts;
    } else if (arg == L"--version") {
      opts.command = Command::kVersion;
      return opts;
    } else if (arg == L"--iso") {
      if (const wchar_t* v = need_value(i, "--iso")) opts.iso = v;
    } else if (arg == L"--from") {
      if (const wchar_t* v = need_value(i, "--from")) opts.from = v;
    } else if (arg == L"--out") {
      if (const wchar_t* v = need_value(i, "--out")) opts.out = v;
    } else if (arg == L"--payload") {
      if (const wchar_t* v = need_value(i, "--payload")) opts.payload = v;
    } else {
      opts.parse_error = "unknown argument: " + Narrow(arg);
    }
    if (!opts.parse_error.empty()) return opts;
  }

  if (opts.command == Command::kNone) {
    opts.parse_error = "no command given (expected 'install' or 'verify')";
    return opts;
  }
  if (!opts.iso.empty() && !opts.from.empty()) {
    opts.parse_error = "--iso and --from are mutually exclusive";
    return opts;
  }
  if (opts.iso.empty() && opts.from.empty()) {
    opts.parse_error = "an input is required: --iso <path> or --from <dir>";
    return opts;
  }
  if (opts.command == Command::kInstall && opts.out.empty()) {
    opts.parse_error = "install requires --out <dir>";
    return opts;
  }
  return opts;
}

}  // namespace packager
