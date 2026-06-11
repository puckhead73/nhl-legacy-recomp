# Oracle capture: replay a scene through the BASE (non-takeover) path and grab the
# presented frame via the presenter (NHL_SHOT_FRAME -> live_frame.png). This is the
# ground-truth reference to diff beta output against.
# Usage: . run_oracle.ps1 ; Run-Oracle -Scene scene_02 -Frame 30
param(
  [int]$Frame = 30,
  [string]$Scene = "scene_02",
  [string]$Out = "oracle.png",
  [int]$MaxWaitSec = 240
)
$dir = "e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-relwithdebinfo"
$png = "$dir\live_frame.png"
Get-Process nhllegacy -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
$before = if (Test-Path $png) { (Get-Item $png).LastWriteTimeUtc } else { [datetime]::MinValue }

# Base path: NHL_BACKEND=beta (builds caches) but NO takeover, so the base renders +
# presents. NHL_SHOT_FRAME writes the presented frame to live_frame.png.
$env:NHL_BACKEND="beta"
$env:NHL_SHOT_FRAME="$Frame"
$env:NHL_REPLAY_XTR="$dir\gpu_trace\$Scene\454109EC_stream.xtr"
foreach ($k in @("NHL_BETA_TAKEOVER","NHL_BETA_EDRAM","NHL_BETA_DEPTH","NHL_BETA_BIND_DIAG","NHL_BETA_CAPTURE_FRAME","NHL_SHOT_CONTINUOUS")) {
  Remove-Item "env:$k" -ErrorAction SilentlyContinue
}

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
if ($done -and (Test-Path $png)) { Copy-Item $png "$dir\$Out" -Force; Write-Output "ORACLE written -> $Out" } else { Write-Output "NO oracle within ${MaxWaitSec}s" }
$log = Get-ChildItem "$dir\logs\*.log" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
Write-Output "LOG=$($log.Name)"
