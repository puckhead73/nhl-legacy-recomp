# Texture auto-extraction build step.
#
# Walks the game's already-extracted loose-asset tree (_compiled/rendering, and
# _compiled/fe if present) and writes a MIRRORED .dds tree: multi-texture .rx2
# containers become a folder of <NN>_<name>.dds slot files; single-texture .rx2
# become a flat <stem>.dds. The recomp's loose-override loader
# (LooseTreeDevice::BuildSynth) reads exactly this layout, so any .dds you edit
# becomes a live in-game override once dropped into _loose_tex (see below).
#
# Builds the exporter CLI (nhl-database-studio :: export_textures_cli) on first
# run. Default output is <GameData>\_extracted_textures (a browsable, editable
# copy -- NOT auto-active, so nothing changes in-game until you opt a texture in).
#
# Usage:
#   pwsh _extract_textures.ps1                          # default game data + out
#   pwsh _extract_textures.ps1 -Out "D:\my_tex"         # custom out
#   pwsh _extract_textures.ps1 -Rendering <dir>         # extract a single subtree
#
# To ACTIVATE an edit: copy the edited folder (e.g. rendering\jersey\texlib_0_0_0\)
# from the extracted tree into <GameData>\_loose_tex\<same relative path>, then run
# the game with the loose overlay (NHL_VK_BACKEND / normal launch). BuildSynth
# re-synthesizes the .rx2 from your .dds at load.
param(
  [string]$GameData  = "H:\Emulators\games\XBOX\NHL Legacy - Vanilla",
  [string]$Out       = "",
  [string]$Rendering = "",
  [string]$Fe        = ""
)
$ErrorActionPreference = "Stop"
$studio = "e:\Repositories\nhl-database-studio"
$cli    = "$studio\target\release\export_textures_cli.exe"

if ($Rendering -eq "") { $Rendering = Join-Path $GameData "_compiled\rendering" }
if ($Fe -eq "")        { $Fe        = Join-Path $GameData "_compiled\fe" }
if ($Out -eq "")       { $Out       = Join-Path $GameData "_extracted_textures" }

# 1. Build the exporter CLI if missing.
if (-not (Test-Path $cli)) {
  Write-Host "[extract-tex] building export_textures_cli (first run)..."
  Push-Location $studio
  cargo build -p tdb-gui-api --bin export_textures_cli --release
  Pop-Location
}
if (-not (Test-Path $cli)) { Write-Error "[extract-tex] CLI build failed: $cli"; exit 1 }

# 2. Validate source (the .big must already be extracted to _compiled).
if (-not (Test-Path $Rendering)) {
  Write-Error "[extract-tex] no rendering root at '$Rendering' -- extract the .big to _compiled first."
  exit 1
}

# 3. Run the export.
$cliArgs = @("--rendering", $Rendering, "--out", $Out)
if (Test-Path $Fe) { $cliArgs += @("--fe", $Fe) }
Write-Host "[extract-tex] extracting textures -> $Out"
& $cli @cliArgs
$code = $LASTEXITCODE
if ($code -ne 0) { Write-Error "[extract-tex] exporter failed (exit $code)"; exit $code }

Write-Host ""
Write-Host "[extract-tex] DONE -> $Out"
Write-Host "[extract-tex] To override a texture in-game: copy its folder into"
Write-Host "              $GameData\_loose_tex\<same relative path>, edit the .dds, launch."
