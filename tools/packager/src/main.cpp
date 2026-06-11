// nhl-legacy-builder - entry point.
//
// With arguments: plain CLI (see cli.cpp for usage).
// With no arguments (e.g. double-clicked from Explorer): interactive mode -
// prompts for the input dump and install location, then runs install.
//
// NOTE: this is a wide entry point (wmain) on purpose. rexruntime's
// filesystem helpers call _get_wpgmptr, which fail-fasts (0xC0000409 in
// _invoke_watson) unless the process CRT was wide-initialized. The game host
// uses wWinMain for the same reason. Wide argv also keeps non-ASCII ISO
// paths intact.

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include "cli.h"
#include "installer.h"

#ifndef PACKAGER_VERSION
#define PACKAGER_VERSION "0.0.0-dev"
#endif

namespace {

namespace fs = std::filesystem;

// True when this process owns its console (launched from Explorer rather
// than an existing terminal) - in that case pause before the window closes.
bool OwnsConsole() {
  DWORD pids[2];
  return GetConsoleProcessList(pids, 2) <= 1;
}

void PauseForExit() {
  fprintf(stdout, "\nPress Enter to exit.");
  fflush(stdout);
  std::wstring line;
  std::getline(std::wcin, line);
}

std::wstring Prompt(const char* text) {
  fprintf(stdout, "%s", text);
  fflush(stdout);
  std::wstring line;
  std::getline(std::wcin, line);
  // Trim whitespace and surrounding quotes (Explorer "Copy as path" adds them).
  const auto first = line.find_first_not_of(L" \t\"");
  const auto last = line.find_last_not_of(L" \t\"");
  if (first == std::wstring::npos) return L"";
  return line.substr(first, last - first + 1);
}

int RunInteractive(const fs::path& payload_dir) {
  fprintf(stdout,
          "NHL Legacy Recomp Builder v%s\n"
          "----------------------------------------\n"
          "Turns your own NHL Legacy disc dump into a playable PC port.\n"
          "You need either a raw .iso of the disc or an already-extracted\n"
          "game folder (containing default.xex).\n\n",
          PACKAGER_VERSION);

  const std::wstring input =
      Prompt("Path to your NHL Legacy .iso or extracted folder: ");
  if (input.empty()) {
    fprintf(stderr, "No input given - nothing to do.\n");
    return 2;
  }

  std::wstring out = Prompt("Install location [NHL Legacy]: ");
  if (out.empty()) out = L"NHL Legacy";

  packager::InstallOptions opts;
  opts.input = fs::path(input);
  opts.input_is_folder = fs::is_directory(opts.input);
  opts.out_dir = fs::path(out);
  opts.payload_dir = payload_dir;
  fprintf(stdout, "\n");
  return packager::RunInstall(opts);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  // Mirror the host app's pre-init (cvar/logging state the runtime DLL
  // expects). The packager parses its own flags, so only argv[0] is passed.
  char prog[] = "nhl-legacy-builder";
  char* fake_argv[] = {prog, nullptr};
  rex::cvar::Init(1, fake_argv);
  rex::cvar::ApplyEnvironment();
  rex::InitLoggingEarly();

  const fs::path default_payload =
      rex::filesystem::GetExecutableFolder() / "payload";

  if (argc <= 1) {
    const int rc = RunInteractive(default_payload);
    if (OwnsConsole()) PauseForExit();
    return rc;
  }

  packager::Options opts = packager::ParseArgs(argc, argv);
  if (opts.command == packager::Command::kHelp) {
    packager::PrintUsage();
    return 0;
  }
  if (opts.command == packager::Command::kVersion) {
    fprintf(stdout, "nhl-legacy-builder %s\n", PACKAGER_VERSION);
    return 0;
  }
  if (!opts.parse_error.empty()) {
    fprintf(stderr, "ERROR: %s\n\n", opts.parse_error.c_str());
    packager::PrintUsage();
    return 2;
  }

  packager::InstallOptions install;
  install.input = opts.iso.empty() ? opts.from : opts.iso;
  install.input_is_folder = !opts.from.empty();
  install.out_dir = opts.out;
  install.payload_dir = opts.payload.empty() ? default_payload : opts.payload;
  install.verify_only = opts.command == packager::Command::kVerify;
  return packager::RunInstall(install);
}
