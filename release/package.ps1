# package.ps1 - dev-side release pipeline for NHL Legacy Recomp.
#
# Builds the port + packager, generates the payload manifest (pinning the
# vanilla default.xex hash from assets/), stages the end-user layout and
# zips it. PowerShell 5.1 compatible.
#
# THE canonical release ships the Vulkan-fsi `win-amd64-vk-ffx` build (see
# docs/current-status.md). That build has NO cmake preset - it's configured
# directly by scripts\_game_ffx_build.bat - so this script packages the prebuilt,
# play-tested dir as-is (vk presets auto-imply -SkipBuild). Build/refresh it
# first with `scripts\_game_ffx_build.bat` (and `scripts\_ffx_sdk_build_install.bat` if the SDK
# source changed), then run this. The nhl-legacy-builder packager target must
# also be present in that tree (build with `cmake --build out\build\win-amd64-vk-ffx
# --target nhl-legacy-builder` under a vcvars64 + Vulkan SDK shell).
#
# Usage:
#   .\release\package.ps1 -Version 0.1.0                       # package prebuilt vk-ffx
#   .\release\package.ps1 -Version 0.1.0 -TestInput "H:\...\NHL Legacy - Vanilla"
#   .\release\package.ps1 -Version 0.1.0 -Preset win-amd64-relwithdebinfo  # legacy D3D12 (builds via preset)

[CmdletBinding()]
param(
    [string]$Version    = "0.1.0",
    [string]$Preset     = "win-amd64-vk-ffx",
    [string]$SdkDir     = "E:\Tools\rexglue-sdk\src\out\install\win-amd64-ffx",
    [string]$SdkVersion = "0.8.1",
    [string]$LlvmBin    = "C:\Program Files\LLVM\bin",
    [switch]$RunCodegen,             # re-run rexglue codegen before building
    [string]$TestInput = "",         # ISO or extracted folder for the post-zip self-check
    [string]$OutDir    = "",
    [string]$StudioDir = "E:\Repositories\nhl-database-studio",  # for the .big/texture extractor
    [switch]$NoExtractor,            # skip bundling the .big extractor (smaller payload)
    [switch]$SkipBuild,              # package a PREBUILT dir (e.g. the Vulkan PGO build) as-is
    [string]$BuildDirOverride = ""   # use this build dir instead of out\build\<Preset>
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $RepoRoot "out\release" }
$BuildDir = Join-Path $RepoRoot "out\build\$Preset"
if ($BuildDirOverride) { $BuildDir = $BuildDirOverride }

# Runtime DLL flavor suffix follows the preset. The shipping Vulkan builds
# (win-amd64-vk-pgo / -opt) are now Release-based -> rexruntime.dll (no suffix);
# the dev build win-amd64-vk-ffx is still RelWithDebInfo -> "rd".
if     ($Preset -like "*vk-pgo" -or $Preset -like "*vk-opt" -or $Preset -like "*release") { $Flavor = "" }
elseif ($Preset -like "*debug")                                                           { $Flavor = "d" }
elseif ($Preset -like "*vk-ffx" -or $Preset -like "*relwithdebinfo")                      { $Flavor = "rd" }
elseif ($Preset -like "*vk*")                                                             { $Flavor = "rd" }
else { throw "Unrecognized preset '$Preset'" }

# Guard against shipping the slow exe: win-amd64-vk-ffx is the unoptimized dev
# build (-O2, no PGO/LTO). The release should be cut from the PGO build.
if ($Preset -like "*vk-ffx") {
    Write-Host "WARNING: '$Preset' is the unoptimized DEV build (-O2, no PGO/LTO). For a real release, build and package -Preset win-amd64-vk-pgo (see DEV-README)." -ForegroundColor Yellow
}

# The Vulkan builds (win-amd64-vk*) have no CMakePresets entry - they are
# configured by scripts\_game_ffx_build.bat. There is nothing for `cmake --preset` to
# build, so a vk preset always packages the prebuilt dir as-is.
if ($Preset -like "*vk*" -and -not $SkipBuild) {
    Write-Host "Note: '$Preset' is configured by scripts\_game_ffx_build.bat (no cmake preset); packaging the prebuilt dir. Rebuild it first if stale." -ForegroundColor Yellow
    $SkipBuild = $true
}

function Step([string]$Name) {
    Write-Host ""
    Write-Host "=== $Name ===" -ForegroundColor Cyan
}

function CheckExit([string]$What) {
    if ($LASTEXITCODE -ne 0) { throw "$What failed (exit $LASTEXITCODE)" }
}

# --- Toolchain on PATH ---
if (-not (Get-Command clang++ -ErrorAction SilentlyContinue)) {
    $env:PATH = "$LlvmBin;$env:PATH"
}

# --- 1. Codegen (optional; generated/ is normally current in the repo) ---
if ($RunCodegen) {
    Step "rexglue codegen"
    & (Join-Path $SdkDir "bin\rexglue.exe") codegen (Join-Path $RepoRoot "nhllegacy_manifest.toml")
    CheckExit "codegen"
}

# --- 2. Configure + build the port and the packager ---
# With -SkipBuild we package an already-built dir as-is (e.g. the Vulkan PGO build
# produced by scripts\_build_vk_pgo.bat, which presets/cmake --preset don't cover).
if (-not $SkipBuild) {
    Step "Configure ($Preset)"
    $SdkDirFwd = $SdkDir -replace '\\', '/'
    cmake --preset $Preset "-DCMAKE_PREFIX_PATH=$SdkDirFwd"
    CheckExit "cmake configure"

    Step "Build nhllegacy + nhl-legacy-builder"
    cmake --build --preset $Preset --target nhllegacy
    CheckExit "build nhllegacy"
    cmake --build --preset $Preset --target nhl-legacy-builder
    CheckExit "build nhl-legacy-builder"
} else {
    Step "Skipping build (packaging prebuilt $BuildDir)"
}

$PortExe     = Join-Path $BuildDir "nhllegacy.exe"
$BuilderExe  = Join-Path $BuildDir "tools\packager\nhl-legacy-builder.exe"
$RuntimeDll  = "rexruntime$Flavor.dll"
# TracyClient is only present when the SDK was built with REXGLUE_ENABLE_TRACY=ON.
# The canonical Release SDK build turns it OFF (no profiling instrumentation in
# the shipped runtime), so this DLL may not exist — every copy below is guarded.
$TracyDll    = "TracyClient$Flavor.dll"
if (-not (Test-Path $PortExe)) {
    throw "Port exe missing: $PortExe. Build it with scripts\_game_ffx_build.bat first."
}
if (-not (Test-Path $BuilderExe)) {
    throw "Packager missing: $BuilderExe. Build it under a vcvars64 + Vulkan SDK shell:`n  cmake --build `"$BuildDir`" --target nhl-legacy-builder"
}

# --- 3. Hashes + payload manifest ---
Step "Generate payload manifest"
$XexPath = Join-Path $RepoRoot "assets\default.xex"
$XexHash = (Get-FileHash -Algorithm SHA256 $XexPath).Hash.ToLower()
$XexSize = (Get-Item $XexPath).Length
$ExeHash = (Get-FileHash -Algorithm SHA256 $PortExe).Hash.ToLower()

$ManifestText = @"
# NHL Legacy Recomp payload manifest - generated by release/package.ps1.
# Pins the exact game build this release's port was recompiled from.

[tool]
version      = "$Version"
sdk_version  = "$SdkVersion"
port_build   = "$Preset"
runtime_dll  = "$RuntimeDll"

[xex]
title        = "NHL Legacy (vanilla)"
sha256       = "$XexHash"
size_bytes   = $XexSize

[port]
exe          = "nhllegacy.exe"
exe_sha256   = "$ExeHash"
"@

# --- 4. Stage the release layout ---
Step "Stage release layout"
$StageName = "nhl-legacy-recomp-$Version"
$Stage     = Join-Path $OutDir $StageName
$Payload   = Join-Path $Stage "payload"
if (Test-Path $Stage) { Remove-Item -Recurse -Force $Stage }
New-Item -ItemType Directory -Force $Payload | Out-Null

# Resolve the FidelityFX runtime DLL(s). rexruntime on the FFX build hard-imports
# amd_fidelityfx_vk.dll, so BOTH the port and the builder fail to load without it
# (0xC0000135 STATUS_DLL_NOT_FOUND). The build scripts don't always stage it next
# to the exe, so fall back to the SDK install's bin/ when it's not in $BuildDir.
$FfxDlls = @(Get-ChildItem $BuildDir -Filter "amd_fidelityfx*.dll" -ErrorAction SilentlyContinue)
if (-not $FfxDlls) {
    $SdkBin = Join-Path $SdkDir "bin"
    $FfxDlls = @(Get-ChildItem $SdkBin -Filter "amd_fidelityfx*.dll" -ErrorAction SilentlyContinue)
}
if (-not $FfxDlls) {
    throw "amd_fidelityfx*.dll not found in '$BuildDir' or '$SdkDir\bin' - the FFX build needs it. Stage it beside the exe or fix -SdkDir."
}

# Top level: builder CLI + its runtime DLLs (incl. FFX) + docs.
Copy-Item $BuilderExe $Stage
Copy-Item (Join-Path $BuildDir "tools\packager\$RuntimeDll") $Stage
$BuilderTracy = Join-Path $BuildDir "tools\packager\$TracyDll"
if (Test-Path $BuilderTracy) { Copy-Item $BuilderTracy $Stage }
$FfxDlls | ForEach-Object { Copy-Item $_.FullName $Stage }
Copy-Item (Join-Path $PSScriptRoot "README.payload.md") (Join-Path $Stage "README.txt")
Copy-Item (Join-Path $PSScriptRoot "THIRD-PARTY-NOTICES.txt") $Stage

# payload/: the prebuilt port + its runtime DLLs (incl. FFX) + manifest.
Copy-Item $PortExe $Payload
Copy-Item (Join-Path $BuildDir $RuntimeDll) $Payload
$PayloadTracy = Join-Path $BuildDir $TracyDll
if (Test-Path $PayloadTracy) { Copy-Item $PayloadTracy $Payload }
$FfxDlls | ForEach-Object { Copy-Item $_.FullName $Payload }
Set-Content -Path (Join-Path $Payload "manifest.toml") -Value $ManifestText -Encoding ascii

# payload/extractor/: the QuickBMS-based .big unpacker the installer runs after
# extracting the disc (game/ -> game/_compiled), so end users get the loose-file
# modding tree. The .big are EA's EB\x00\x03 format, which only QuickBMS +
# fightnight.bms decodes; extract_big_cli wraps it (built from nhl-database-studio).
if (-not $NoExtractor) {
    Step "Bundle .big extractor"
    $StudioRes = Join-Path $StudioDir "app\src-tauri\resources\extractor"
    $BigCli    = Join-Path $StudioDir "target\release\extract_big_cli.exe"
    if (-not (Test-Path $BigCli)) {
        Write-Host "Building extract_big_cli (nhl-database-studio) ..."
        Push-Location $StudioDir
        cargo build -p tdb-gui-api --bin extract_big_cli --release
        Pop-Location
    }
    $missing = @($BigCli, (Join-Path $StudioRes "quickbms.exe"), (Join-Path $StudioRes "fightnight.bms")) |
        Where-Object { -not (Test-Path $_) }
    if ($missing) {
        throw "extractor bundle missing: $($missing -join ', '). Pass -NoExtractor to skip, or fix -StudioDir."
    }
    $ExtractorOut = Join-Path $Payload "extractor"
    New-Item -ItemType Directory -Force $ExtractorOut | Out-Null
    Copy-Item $BigCli $ExtractorOut
    Copy-Item (Join-Path $StudioRes "quickbms.exe") $ExtractorOut
    Copy-Item (Join-Path $StudioRes "fightnight.bms") $ExtractorOut
    Write-Host "Bundled extractor -> payload\extractor\ (extract_big_cli, quickbms, fightnight.bms)"
}

# --- 5. Zip ---
Step "Zip"
$ZipPath = Join-Path $OutDir "$StageName.zip"
if (Test-Path $ZipPath) { Remove-Item -Force $ZipPath }
Compress-Archive -Path $Stage -DestinationPath $ZipPath
Write-Host "Release zip: $ZipPath ($([math]::Round((Get-Item $ZipPath).Length / 1MB, 1)) MB)"

# --- 6. Self-check (optional) ---
if ($TestInput) {
    Step "Self-check: verify against $TestInput"
    $CheckDir = Join-Path $env:TEMP "nhl-legacy-release-check"
    if (Test-Path $CheckDir) { Remove-Item -Recurse -Force $CheckDir }
    Expand-Archive -Path $ZipPath -DestinationPath $CheckDir
    $CheckedBuilder = Join-Path $CheckDir "$StageName\nhl-legacy-builder.exe"
    if (Test-Path -PathType Container $TestInput) {
        & $CheckedBuilder verify --from $TestInput
    } else {
        & $CheckedBuilder verify --iso $TestInput
    }
    CheckExit "release self-check"
    Write-Host "Self-check passed." -ForegroundColor Green
}

Write-Host ""
Write-Host "Done: $ZipPath" -ForegroundColor Green
