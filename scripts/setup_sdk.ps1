# Fetch + pin + patch the ReXGlue SDK this port builds against, and fetch the
# vendored third-party deps (plume / XenosRecomp). Idempotent - re-run to repair.
#
# The SDK is public (https://github.com/rexglue/rexglue-sdk, BSD-3). This project
# pins the exact commit the port was developed against and applies the title-
# specific patch (sdk/rexglue-vulkan-nhl-legacy-bd9b519.patch): Vulkan texture/
# translator fixes (signed BC5 normals, cube sanitizer, exp_adjust), the
# NHL_VK_READBACK_MODE perf override, and the prebuilt-FidelityFX import path.
#
# Usage:  pwsh scripts/setup_sdk.ps1   (or powershell -File scripts/setup_sdk.ps1)
#   -SdkDir <path>   where to put the SDK source (default third_party/rexglue-sdk)
param(
    [string]$SdkDir = "",
    [string]$Pin = "bd9b51918faa71f962fa38af6d5f744af9aba2ce"
)
$ErrorActionPreference = "Stop"

# Run git as a yes/no probe. Windows PowerShell 5.1 turns a native command's
# redirected stderr into error records, and with ErrorActionPreference=Stop the
# first line git prints there aborts the script - exactly when the probe's
# answer is "no". Relax the preference for the probe and judge by exit code.
function Test-Git {
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { & git @args 2>&1 | Out-Null } finally { $ErrorActionPreference = $old }
    return ($LASTEXITCODE -eq 0)
}

$repo = Split-Path -Parent $PSScriptRoot
if (-not $SdkDir) { $SdkDir = Join-Path $repo "third_party\rexglue-sdk" }
$patch = Join-Path $repo "sdk\rexglue-vulkan-nhl-legacy-bd9b519.patch"

# 1. Clone / pin the SDK.
if (-not (Test-Path (Join-Path $SdkDir ".git"))) {
    Write-Host "[sdk] cloning rexglue-sdk -> $SdkDir"
    git clone --recursive https://github.com/rexglue/rexglue-sdk $SdkDir
    if ($LASTEXITCODE -ne 0) { throw "git clone failed" }
}
Write-Host "[sdk] pinning $Pin"
# The pin is not reachable from upstream's default branch any more, so a plain
# clone does not contain it - fetch the commit explicitly if it is missing.
if (-not (Test-Git -C $SdkDir cat-file -e "$Pin^{commit}")) {
    git -C $SdkDir fetch origin $Pin
    if ($LASTEXITCODE -ne 0) { throw "git fetch $Pin failed" }
}
git -C $SdkDir checkout $Pin
if ($LASTEXITCODE -ne 0) { throw "git checkout $Pin failed" }
git -C $SdkDir submodule update --init --recursive
if ($LASTEXITCODE -ne 0) { throw "git submodule update failed" }

# 2. Apply the pinned patches (skip each if already applied).
#    - the title patch (Vulkan/texture/readback fixes)
#    - upstream codegen cherry-picks that postdate the pin (e.g. the conditional
#      bcctr tail-recovery fix, upstream 10cf1ad - a conditional bnectr/beqctr
#      used to end the block and silently drop the rest of the function)
#    - the POSIX wait spin fix (Linux-only code; harmless on Windows, applied so
#      both checkouts stay identical)
$patches = @($patch, (Join-Path $repo "sdk\rexglue-codegen-bcctr-tail-fix.patch"),
             (Join-Path $repo "sdk\rexglue-linux-wait-spin-fix.patch"),
             (Join-Path $repo "sdk\rexglue-silent-audio-fallback.patch"),
             (Join-Path $repo "sdk\rexglue-xma-kick-wait-fix.patch"),
             (Join-Path $repo "sdk\rexglue-linux-shm-leak-fix.patch"),
             (Join-Path $repo "sdk\rexglue-renderdoc-attach-check.patch"),
             (Join-Path $repo "sdk\rexglue-linux-fault-recheck-fix.patch"),
             (Join-Path $repo "sdk\rexglue-sdl-guide-mapping-fix.patch"),
             (Join-Path $repo "sdk\rexglue-idle-cpu-fixes.patch"),
             (Join-Path $repo "sdk\rexglue-fidelityfx-headers-only.patch"))
foreach ($p in $patches) {
    $pname = Split-Path -Leaf $p
    if (Test-Git -C $SdkDir apply --check $p) {
        git -C $SdkDir apply $p
        if ($LASTEXITCODE -ne 0) { throw "$pname apply failed" }
        Write-Host "[sdk] $pname applied"
    } else {
        if (Test-Git -C $SdkDir apply --check --reverse $p) {
            Write-Host "[sdk] $pname already applied - OK"
        } else {
            throw "[sdk] $pname does not apply (tree modified?). Reset with: git -C `"$SdkDir`" checkout -- . ; then re-run."
        }
    }
}

# 3. Materialize git symlinks that Windows git checks out as plain text files.
#    libmspack's cabextract/mspack/*.{c,h} are symlinks to ../../libmspack/mspack;
#    without this the SDK build fails compiling "files" that contain only a path
#    (this is also what the original dev tree's unexplained submodule "-dirty"
#    state was). Idempotent: already-materialized files have no 120000 ls-files
#    entry content mismatch worth guarding - we just rewrite from the target.
foreach ($sub in @("thirdparty\libmspack", "thirdparty\o1heap")) {
    $subPath = Join-Path $SdkDir $sub
    $links = git -C $subPath ls-files -s | Where-Object { $_ -match '^120000' } |
             ForEach-Object { ($_ -split "`t")[1] }
    foreach ($rel in $links) {
        $file = Join-Path $subPath $rel
        $content = (Get-Content -Raw $file -ErrorAction SilentlyContinue)
        if ($null -eq $content -or $content.Length -gt 260 -or $content -match "`n.*`n") { continue }  # already materialized
        $target = Join-Path (Split-Path -Parent $file) $content.Trim()
        if (Test-Path $target) {
            Copy-Item -Force $target $file
            Write-Host "[sdk] materialized symlink $sub\$rel"
        }
    }
}

# 4. Vendored high-cut deps (required unconditionally by CMakeLists for now).
& (Join-Path $repo "tools\fetch_thirdparty.ps1")

Write-Host ""
Write-Host "SDK ready at $SdkDir (commit $Pin + title patch)."
Write-Host "Next: scripts\_ffx_sdk_configure.bat  (see BOOTSTRAP.md for the full order)"
