// Persisted enhancement settings — a tiny key=value file next to the exe so the
// overlay's choices survive a restart. Restart-required cvars (supersampling,
// fullscreen, monitor) are read at launch in OnPreSetup and rewritten whenever
// the overlay changes them.
//
// Kept header-only + dependency-light so both the app (OnPreSetup, reads) and the
// overlay TU (writes) can use it. Writes are read-modify-write over the whole
// file so unrelated keys are preserved (a blind truncate would drop them).

#pragma once

#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include <rex/filesystem.h>  // rex::filesystem::GetExecutableFolder

namespace nhl {

inline std::filesystem::path SettingsPath() {
  return rex::filesystem::GetExecutableFolder() / "nhl_enhancements.ini";
}

// Read the whole ini into an ordered key->value map (last value wins on dupes).
inline std::map<std::string, std::string> LoadSettings() {
  std::map<std::string, std::string> kv;
  std::ifstream f(SettingsPath());
  if (!f) return kv;
  std::string line;
  while (std::getline(f, line)) {
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    kv[line.substr(0, eq)] = line.substr(eq + 1);
  }
  return kv;
}

// Upsert a single key, preserving every other key already in the file.
inline void SaveSetting(const std::string& key, const std::string& value) {
  auto kv = LoadSettings();
  kv[key] = value;
  std::ofstream f(SettingsPath(), std::ios::trunc);
  if (!f) return;
  for (const auto& [k, v] : kv) f << k << "=" << v << "\n";
}

// Typed helpers. Each clamps/validates and returns `fallback` if absent/invalid.
inline int LoadInt(const std::string& key, int fallback, int lo, int hi) {
  const auto kv = LoadSettings();
  const auto it = kv.find(key);
  if (it == kv.end()) return fallback;
  try {
    const int v = std::stoi(it->second);
    if (v >= lo && v <= hi) return v;
  } catch (...) {
  }
  return fallback;
}

inline bool LoadBool(const std::string& key, bool fallback) {
  const auto kv = LoadSettings();
  const auto it = kv.find(key);
  if (it == kv.end()) return fallback;
  return it->second == "1" || it->second == "true";
}

inline std::string LoadString(const std::string& key, const std::string& fallback) {
  const auto kv = LoadSettings();
  const auto it = kv.find(key);
  return it == kv.end() ? fallback : it->second;
}

inline double LoadDouble(const std::string& key, double fallback) {
  const auto kv = LoadSettings();
  const auto it = kv.find(key);
  if (it == kv.end()) return fallback;
  try {
    return std::stod(it->second);
  } catch (...) {
    return fallback;
  }
}

// --- Named settings ---------------------------------------------------------

// Supersampling scale (internal-resolution multiplier), 1..4.
inline int LoadSupersampling(int fallback) {
  return LoadInt("supersampling", fallback, 1, 4);
}
inline void SaveSupersampling(int v) {
  if (v < 1) v = 1;
  if (v > 4) v = 4;
  SaveSetting("supersampling", std::to_string(v));
}

// Soften shadows: restore bilinear filtering on the guest's depth/shadow maps
// (SDK `shadow_filter_linear` cvar). Hot-reloadable (read per-draw in the texture
// cache), but persisted so the overlay choice survives a relaunch and is applied
// at launch in OnPreSetup. Default off => shadow sampling unchanged.
inline bool LoadShadowFilterLinear(bool fallback) {
  return LoadBool("shadow_filter_linear", fallback);
}
inline void SaveShadowFilterLinear(bool v) {
  SaveSetting("shadow_filter_linear", v ? "1" : "0");
}

// Shadow softness: extra depth-domain blur passes on guest depth/shadow maps
// (SDK `shadow_softness` cvar), 0..4. 0 = off. Hot-reloadable; persisted.
inline int LoadShadowSoftness(int fallback) {
  return LoadInt("shadow_softness", fallback, 0, 4);
}
inline void SaveShadowSoftness(int v) {
  if (v < 0) v = 0;
  if (v > 4) v = 4;
  SaveSetting("shadow_softness", std::to_string(v));
}

// Borderless-fullscreen vs windowed (the SDK's "fullscreen" cvar is borderless).
inline bool LoadFullscreen(bool fallback) { return LoadBool("fullscreen", fallback); }
inline void SaveFullscreen(bool v) { SaveSetting("fullscreen", v ? "1" : "0"); }

// V-Sync (SDK `vsync` cvar). Restart-required; persist so it survives a relaunch.
// Pass the SDK default as `fallback` so an absent key leaves the default intact.
inline bool LoadVsync(bool fallback) { return LoadBool("vsync", fallback); }
inline void SaveVsync(bool v) { SaveSetting("vsync", v ? "1" : "0"); }

// Monitor index: 0 = default (current), 1.. = a specific display.
inline int LoadMonitor(int fallback) { return LoadInt("monitor", fallback, 0, 16); }
inline void SaveMonitor(int v) {
  if (v < 0) v = 0;
  if (v > 16) v = 16;
  SaveSetting("monitor", std::to_string(v));
}

// FidelityFX present-time effect: bilinear | cas | fsr | fsr2 | fsr3. Persisted
// so the restart-required SDK cvar can be re-applied at launch in OnPreSetup.
inline std::string LoadFfxEffect(const std::string& fallback) {
  return LoadString("ffx_effect", fallback);
}
inline void SaveFfxEffect(const std::string& v) { SaveSetting("ffx_effect", v); }

inline double LoadFfxCasSharpness(double fallback) {
  return LoadDouble("ffx_cas_sharpness", fallback);
}
inline void SaveFfxCasSharpness(double v) {
  SaveSetting("ffx_cas_sharpness", std::to_string(v));
}

inline double LoadFfxFsrSharpness(double fallback) {
  return LoadDouble("ffx_fsr_sharpness", fallback);
}
inline void SaveFfxFsrSharpness(double v) {
  SaveSetting("ffx_fsr_sharpness", std::to_string(v));
}

// Color-grade post-process (SDK present_grade_* cvars). Hot-reloadable, but
// persisted so the overlay's look survives a relaunch (re-applied in OnPreSetup).
inline bool LoadGradeEnable(bool fallback) { return LoadBool("grade_enable", fallback); }
inline void SaveGradeEnable(bool v) { SaveSetting("grade_enable", v ? "1" : "0"); }

inline double LoadGradeExposure(double f) { return LoadDouble("grade_exposure", f); }
inline void SaveGradeExposure(double v) { SaveSetting("grade_exposure", std::to_string(v)); }

inline double LoadGradeContrast(double f) { return LoadDouble("grade_contrast", f); }
inline void SaveGradeContrast(double v) { SaveSetting("grade_contrast", std::to_string(v)); }

inline double LoadGradeSaturation(double f) { return LoadDouble("grade_saturation", f); }
inline void SaveGradeSaturation(double v) { SaveSetting("grade_saturation", std::to_string(v)); }

inline double LoadGradeBrightness(double f) { return LoadDouble("grade_brightness", f); }
inline void SaveGradeBrightness(double v) { SaveSetting("grade_brightness", std::to_string(v)); }

inline double LoadGradeTemperature(double f) { return LoadDouble("grade_temperature", f); }
inline void SaveGradeTemperature(double v) { SaveSetting("grade_temperature", std::to_string(v)); }

inline double LoadGradeTint(double f) { return LoadDouble("grade_tint", f); }
inline void SaveGradeTint(double v) { SaveSetting("grade_tint", std::to_string(v)); }

inline double LoadGradeTonemap(double f) { return LoadDouble("grade_tonemap", f); }
inline void SaveGradeTonemap(double v) { SaveSetting("grade_tonemap", std::to_string(v)); }

}  // namespace nhl
