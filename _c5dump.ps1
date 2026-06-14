param([int]$Seconds = 300)
# C-5a PHASE 1 — CAPTURE A FRAME. Boots the game on the beta LIVE path; YOU drive past the intro to
# a STATIC menu, press F10 to arm the takeover, then wait ~5s. With NHL_HIGHCUT_FRAME_CAPTURE every
# owned draw of each frame is dumped to highcut_frame_<N>.bin (overwriting each frame), and at frame
# end the draw count is written to highcut_frame.count. The LAST fully-captured frame before exit
# (a static menu = stable) is the one the C-5 replay loads. Keep the menu static while it captures.
$dir = "e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-relwithdebinfo"
Get-Process nhllegacy -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
foreach ($k in @("NHL_REPLAY_XTR","NHL_CAPTURE_FULL","NHL_CAPTURE_STREAM","NHL_HOTKEY_CAPTURE",
                 "NHL_BETA_CAPTURE_FRAME","NHL_HIGHCUT_PRESENT","NHL_HIGHCUT_XLAT_TEST","NHL_HIGHCUT_C5")) {
  Remove-Item "env:$k" -ErrorAction SilentlyContinue
}
$env:NHL_BACKEND = "beta"; $env:NHL_BETA_TAKEOVER = "1"; $env:NHL_BETA_LIVE = "1"
$env:NHL_BETA_FLAT = "1"; $env:NHL_BETA_DEPTH = "1"
$env:NHL_BETA_LIVE_HOTKEY = "1"            # F10 arms the takeover at the menu
$env:NHL_HIGHCUT_FRAME_CAPTURE = "1"       # dump every owned draw of each frame
# clear stale captures so a shorter frame can't leave a long stale tail
Get-ChildItem "$dir\highcut_frame_*.bin" -ErrorAction SilentlyContinue | Remove-Item -Force
Remove-Item "$dir\highcut_frame.count" -ErrorAction SilentlyContinue
Remove-Item "$dir\highcut_resolves.bin" -ErrorAction SilentlyContinue   # C-5d.3 resolve sidecar
$argline = '--game_data_root "H:\Emulators\games\XBOX\NHL Legacy - Vanilla"'
$p = Start-Process -FilePath "$dir\nhllegacy.exe" -ArgumentList $argline -WorkingDirectory $dir -PassThru -NoNewWindow -RedirectStandardError "$dir\c5d_stderr.log" -RedirectStandardOutput "$dir\c5d_stdout.log"
Write-Output "GAME LAUNCHED (pid $($p.Id)). Drive to a STATIC MENU, press F10, then hold ~5s on that menu."
$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) { Start-Sleep -Seconds 2 }
if (-not $p.HasExited) { $p.Kill() }
Start-Sleep -Milliseconds 600
$log = (Get-ChildItem "$dir\logs\*.log" | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
Write-Output "LOG=$log"
$cnt = if (Test-Path "$dir\highcut_frame.count") { (Get-Content "$dir\highcut_frame.count" | Select-Object -First 1) } else { "(none)" }
$files = (Get-ChildItem "$dir\highcut_frame_*.bin" -ErrorAction SilentlyContinue | Measure-Object).Count
Write-Output "highcut_frame.count = $cnt   (highcut_frame_*.bin files on disk: $files)"
Write-Output "--- C-5d frame delimiting (present counter should ADVANCE under takeover; frame_index_ stays frozen) ---"
Select-String -Path $log -Pattern "\[highcut-C5\] frame boundary" | Select-Object -Last 8 | ForEach-Object { ($_.Line -split '\] ')[-1] }
Write-Output "--- last frame's capture summary ---"
Select-String -Path $log -Pattern "\[highcut-C5\] frame captured" | Select-Object -Last 3 | ForEach-Object { ($_.Line -split '\] ')[-1] }
Select-String -Path $log -Pattern "\[highcut-C5\] dumped highcut_frame" | Select-Object -Last 6 | ForEach-Object { ($_.Line -split '\] ')[-1] }
