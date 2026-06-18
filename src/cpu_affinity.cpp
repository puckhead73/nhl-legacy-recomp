// cpu_affinity.cpp - Intel hybrid (P/E-core) thread placement.
//
// On Intel 12th-gen+ "hybrid" CPUs the OS scheduler can park the recompiled
// game's heavy threads (NHL Sim / Render / JobManager workers, plus the SDK's
// own GPU-Command / VSync threads) on the low-clock Efficiency (E) cores. For a
// CPU/draw-submission-bound recomp that is a large perf cliff. This pins the
// process's DEFAULT CPU set to the Performance (P) cores so every thread — guest
// threads created through the SDK's KeSetAffinityThread mapping AND the SDK's own
// host threads — inherits the P-core preference without per-thread plumbing.
//
// Env-gated by NHL_PIN_PCORES (default ON). Set NHL_PIN_PCORES=0 (or "off"/"false")
// to disable, e.g. to A/B the effect. On non-hybrid CPUs (all AMD, pre-Alder-Lake
// Intel) every core reports the same EfficiencyClass, so this is a no-op.
//
// Windows headers are confined to this TU (the widely-included nhllegacy_app.h
// deliberately avoids <windows.h>); the app calls the plain extern below.

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

void rex_pin_pcores_if_hybrid();

namespace {

bool env_disabled() {
  const char* v = std::getenv("NHL_PIN_PCORES");
  if (!v || !*v) return false;  // unset/empty => enabled (default ON)
  return std::strcmp(v, "0") == 0 || _stricmp(v, "off") == 0 ||
         _stricmp(v, "false") == 0 || _stricmp(v, "no") == 0;
}

}  // namespace

void rex_pin_pcores_if_hybrid() {
  if (env_disabled()) {
    std::fprintf(stderr, "[nhl-affinity] disabled via NHL_PIN_PCORES\n");
    return;
  }

  const HANDLE proc = GetCurrentProcess();

  // Size the CPU-set buffer (first call with a null buffer returns the length).
  ULONG bytes = 0;
  GetSystemCpuSetInformation(nullptr, 0, &bytes, proc, 0);
  if (bytes == 0) return;  // API unavailable / no info — leave scheduling to the OS.

  std::vector<unsigned char> buf(bytes);
  if (!GetSystemCpuSetInformation(
          reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buf.data()), bytes,
          &bytes, proc, 0)) {
    return;
  }

  // Walk the variable-length records, tracking the max EfficiencyClass (P-cores
  // report the highest value) and collecting the CPU-set Ids in that class.
  BYTE max_eff = 0;
  bool multiple_classes = false;
  BYTE first_eff = 0;
  bool first_seen = false;
  std::vector<ULONG> pcore_ids;

  auto walk = [&](auto visitor) {
    ULONG off = 0;
    while (off + sizeof(SYSTEM_CPU_SET_INFORMATION) <= bytes) {
      auto* info = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buf.data() + off);
      if (info->Size == 0) break;
      if (info->Type == CpuSetInformation) visitor(info->CpuSet);
      off += info->Size;
    }
  };

  // Pass 1: find the highest efficiency class and whether the system is hybrid.
  walk([&](const auto& cs) {
    if (!first_seen) { first_eff = cs.EfficiencyClass; first_seen = true; }
    else if (cs.EfficiencyClass != first_eff) { multiple_classes = true; }
    if (cs.EfficiencyClass > max_eff) max_eff = cs.EfficiencyClass;
  });

  if (!first_seen) return;
  if (!multiple_classes) {
    // Single efficiency class (AMD / non-hybrid Intel) — nothing to gain.
    std::fprintf(stderr, "[nhl-affinity] non-hybrid CPU; no P-core pinning\n");
    return;
  }

  // Pass 2: collect the P-core (max EfficiencyClass) CPU-set Ids.
  walk([&](const auto& cs) {
    if (cs.EfficiencyClass == max_eff) pcore_ids.push_back(cs.Id);
  });

  if (pcore_ids.empty()) return;

  if (SetProcessDefaultCpuSets(proc, pcore_ids.data(),
                               static_cast<ULONG>(pcore_ids.size()))) {
    std::fprintf(stderr,
                 "[nhl-affinity] hybrid CPU: pinned process default to %zu "
                 "P-cores (EfficiencyClass %u)\n",
                 pcore_ids.size(), static_cast<unsigned>(max_eff));
  } else {
    std::fprintf(stderr,
                 "[nhl-affinity] SetProcessDefaultCpuSets failed (err %lu)\n",
                 GetLastError());
  }
}
