# Fetch + pin the vendored third-party dependencies for the high-cut engine (M0c).
# These are MIT-licensed and NOT committed to this repo (~350M of prebuilt binaries/
# headers); this script reproduces them at the exact pinned commits. Re-run to repair.
#
# Usage:  pwsh tools/fetch_thirdparty.ps1   (run from the repo root)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

# name  ->  @(url, path, pinned_commit)
$deps = @(
  @{ name = "plume";        url = "https://github.com/renderbag/plume.git";        path = "third_party/plume";     pin = "4f556be1531698174a597e7e0a215c22d3238a24" },
  @{ name = "XenosRecomp";  url = "https://github.com/hedge-dev/XenosRecomp.git";  path = "tools/xenos_recomp";    pin = "990d03b28a27b50277ee5d8d942e1c5f873869d1" }
)

foreach ($d in $deps) {
  $dest = Join-Path $root $d.path
  if (Test-Path (Join-Path $dest ".git")) {
    Write-Host "[$($d.name)] already present at $($d.path) - skipping (delete to re-fetch)"
    continue
  }
  Write-Host "[$($d.name)] cloning $($d.url) -> $($d.path)"
  git clone --recurse-submodules $d.url $dest
  Write-Host "[$($d.name)] pinning $($d.pin)"
  git -C $dest checkout $d.pin
  git -C $dest submodule update --init --recursive
}
Write-Host "Done. Vendored deps fetched at pinned commits."
