# Phase-5 EDRAM multi-pass takeover replay launcher (scene_04 gameplay).
# Renders the owned draw through the RT cache (NHL_BETA_EDRAM) so resolves run and the
# composite samples them. Kills as soon as beta_owned_draw.png is (re)written.
# Usage: powershell -File run_edram.ps1 [-Frame 30] [-Scene scene_04] [-Extra @{NAME=VAL}]
param(
  [int]$Frame = 30,
  [string]$Scene = "scene_04",
  [hashtable]$Extra = @{},
  [int]$MaxWaitSec = 240,
  [int]$Edram = 1,
  [int]$Depth = 0
)
$dir = "e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-relwithdebinfo"
$png = "$dir\beta_owned_draw.png"
Get-Process nhllegacy -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
$before = if (Test-Path $png) { (Get-Item $png).LastWriteTimeUtc } else { [datetime]::MinValue }

$env:NHL_BACKEND="beta"
$env:NHL_BETA_TAKEOVER="1"
$env:NHL_BETA_CAPTURE_FRAME="$Frame"
$env:NHL_REPLAY_XTR="$dir\gpu_trace\$Scene\454109EC_stream.xtr"
foreach ($k in @("NHL_BETA_BIND_DIAG","NHL_BETA_SKIP_DRAW","NHL_BETA_ONLY_DRAW","NHL_BETA_MAX_DRAW","NHL_BETA_TEXONLY","NHL_BETA_INJECT","NHL_BETA_DEPTH","NHL_BETA_EDRAM")) {
  Remove-Item "env:$k" -ErrorAction SilentlyContinue
}
if ($Edram -ne 0) { $env:NHL_BETA_EDRAM="1" }
if ($Depth -ne 0) { $env:NHL_BETA_DEPTH="1" }
foreach ($k in $Extra.Keys) { Set-Item "env:$k" $Extra[$k] }

$argline = '--game_data_root "H:\Emulators\games\XBOX\NHL Legacy - Vanilla"'
$p = Start-Process -FilePath "$dir\nhllegacy.exe" -ArgumentList $argline -WorkingDirectory $dir -PassThru -NoNewWindow -RedirectStandardError "$dir\run_stderr.log" -RedirectStandardOutput "$dir\run_stdout.log"
$deadline = (Get-Date).AddSeconds($MaxWaitSec)
$done = $false
while ((Get-Date) -lt $deadline) {
  Start-Sleep -Milliseconds 800
  if ($p.HasExited) { $done = $true; break }
  if (Test-Path $png) {
    $t = (Get-Item $png).LastWriteTimeUtc
    if ($t -gt $before) { Start-Sleep -Milliseconds 600; $done = $true; break }
  }
}
if (-not $p.HasExited) { $p.Kill() }
if ($done) { Write-Output "PNG written (frame $Frame captured)" } else { Write-Output "NO PNG within ${MaxWaitSec}s" }
$log = Get-ChildItem "$dir\logs\*.log" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
Write-Output "LOG=$($log.Name)"
