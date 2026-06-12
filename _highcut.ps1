param([int]$Seconds = 120, [switch]$D3D12)
# High-cut path C bring-up. Boots the REAL game on its normal (rexglue) path AND stands up an
# additive in-process plume swapchain in its own window (NHL_HIGHCUT_PRESENT). After ~30 guest
# Presents, the plume window should show the C-1 bring-up TRIANGLE (vertex-colored) over an
# animated clear — proving plume rasterizes real geometry, not just clears. The game's own
# window is unaffected (plume is additive until the C-6 takeover milestone).
#   default      = plume Vulkan (required in-process; a 2nd D3D12 device TDRs rexglue)
#   -D3D12       = force plume D3D12 (expected to TDR rexglue; for isolated testing only)
$dir = "e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-relwithdebinfo"
Get-Process nhllegacy -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
foreach ($k in @("NHL_BACKEND","NHL_BETA_TAKEOVER","NHL_BETA_LIVE","NHL_BETA_FLAT","NHL_BETA_DEPTH",
                 "NHL_BETA_LIVE_HOTKEY","NHL_BETA_LIVE_TRACE","NHL_BETA_CLEAR_DIAG","NHL_REPLAY_XTR")) {
  Remove-Item "env:$k" -ErrorAction SilentlyContinue
}
$env:NHL_HIGHCUT_PRESENT = if ($D3D12) { "d3d12" } else { "1" }
Write-Output "NHL_HIGHCUT_PRESENT=$($env:NHL_HIGHCUT_PRESENT)  (plume window is SEPARATE from the game window)"
$argline = '--game_data_root "H:\Emulators\games\XBOX\NHL Legacy - Vanilla"'
$p = Start-Process -FilePath "$dir\nhllegacy.exe" -ArgumentList $argline -WorkingDirectory $dir -PassThru -NoNewWindow -RedirectStandardError "$dir\hc_stderr.log" -RedirectStandardOutput "$dir\hc_stdout.log"
Write-Output "GAME LAUNCHED (pid $($p.Id)). Watch for a 2nd window titled 'NHL high-cut (plume ...)' with a colored triangle."
$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) { Start-Sleep -Seconds 2 }
if (-not $p.HasExited) { $p.Kill() }
Start-Sleep -Milliseconds 600
$log = (Get-ChildItem "$dir\logs\*.log" | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
Write-Output "LOG=$log"
Select-String -Path $log -Pattern "highcut-plume" | Select-Object -Last 12 | ForEach-Object { ($_.Line -split '\] ')[-1] }
