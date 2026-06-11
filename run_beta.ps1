# Fast beta-takeover replay launcher: sets env, launches, kills as soon as the
# capture PNG is (re)written so we don't wait out the ~200s of post-capture replay.
# Usage: powershell -File run_beta.ps1 -Inject "<NHL_BETA_INJECT>" -Extra @{NAME=VAL}
param(
  [string]$Inject = "",
  [int]$Frame = 30,
  [hashtable]$Extra = @{},
  [int]$MaxWaitSec = 150
)
$dir = "e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-relwithdebinfo"
$png = "$dir\beta_owned_draw.png"
Get-Process nhllegacy -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
$before = if (Test-Path $png) { (Get-Item $png).LastWriteTimeUtc } else { [datetime]::MinValue }

$env:NHL_BACKEND="beta"
$env:NHL_BETA_TAKEOVER="1"
$env:NHL_BETA_CAPTURE_FRAME="$Frame"
$env:NHL_BETA_BIND_DIAG="1"
$env:NHL_REPLAY_XTR="$dir\gpu_trace\scene_03\454109EC_stream.xtr"
$env:NHL_BETA_INJECT=$Inject
# Clear any prior diag/control vars then apply caller overrides
foreach ($k in @("NHL_BETA_CLEAR_DIAG","NHL_BETA_SKIP_DRAW","NHL_BETA_ONLY_DRAW","NHL_BETA_MAX_DRAW","NHL_BETA_TEXONLY")) {
  Remove-Item "env:$k" -ErrorAction SilentlyContinue
}
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
    if ($t -gt $before) { Start-Sleep -Milliseconds 400; $done = $true; break }
  }
}
if (-not $p.HasExited) { $p.Kill() }
if ($done) { Write-Output "PNG written (frame $Frame captured)" } else { Write-Output "NO PNG within ${MaxWaitSec}s" }
$log = Get-ChildItem "$dir\logs\*.log" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
Write-Output "LOG=$($log.Name)"
