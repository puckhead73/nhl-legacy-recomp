// Persisted enhancement settings — a tiny key=value file next to the exe so the
// overlay's choices survive a restart. The supersampling scale is read at launch
// (it's a restart-required cvar) and rewritten whenever the overlay slider moves.
//
// Kept header-only + dependency-light so both the app (OnPreSetup, reads) and the
// overlay TU (writes) can use it.

#pragma once

#include <filesystem>
#include <fstream>
#include <string>

#include <rex/filesystem.h>  // rex::filesystem::GetExecutableFolder

namespace nhl {

inline std::filesystem::path SettingsPath() {
  return rex::filesystem::GetExecutableFolder() / "nhl_enhancements.ini";
}

// Returns the persisted supersampling scale (1..4), or `fallback` if absent/invalid.
inline int LoadSupersampling(int fallback) {
  std::ifstream f(SettingsPath());
  if (!f) return fallback;
  std::string line;
  while (std::getline(f, line)) {
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    if (line.substr(0, eq) == "supersampling") {
      try {
        const int v = std::stoi(line.substr(eq + 1));
        if (v >= 1 && v <= 4) return v;
      } catch (...) {
      }
    }
  }
  return fallback;
}

// Persist the supersampling scale so it applies on the next launch.
inline void SaveSupersampling(int v) {
  if (v < 1) v = 1;
  if (v > 4) v = 4;
  std::ofstream f(SettingsPath(), std::ios::trunc);
  if (f) f << "supersampling=" << v << "\n";
}

}  // namespace nhl
