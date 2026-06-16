param([int]$Seconds = 240, [int]$Every = 20)
# FILMSTRIP driver (restart §5.2) — runs the LIVE co-run exactly like _live.ps1, but also dumps the
# plume swapchain to out\...\live_shots\f<frame>.png every $Every guest frames, so a whole run becomes a
# sequence of readable PNGs (one per ~screen). Drive through boot/menu/CAP/loading/gameplay after F10;
# afterwards inspect live_shots\ to SEE what plume renders screen-by-screen. PARITY-debugging prerequisite.
#
# USE: launch, press F10 at a live scene to arm the takeover, then NAVIGATE the game through the screens
# you want to inspect. The filmstrip captures whatever the plume window shows.
$dir = "e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-relwithdebinfo"
$shots = "$dir\live_shots"
Get-Process nhllegacy -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
if (Test-Path $shots) { Remove-Item "$shots\*" -Force -ErrorAction SilentlyContinue } else { New-Item -ItemType Directory -Path $shots | Out-Null }
Get-ChildItem env: | Where-Object Name -like 'NHL_*' | ForEach-Object { Remove-Item "env:$($_.Name)" }
# Producer (beta takeover, builds packets) :
$env:NHL_BACKEND = "beta"; $env:NHL_BETA_TAKEOVER = "1"; $env:NHL_BETA_LIVE = "1"
$env:NHL_BETA_FLAT = "1"; $env:NHL_BETA_DEPTH = "1"
$env:NHL_BETA_LIVE_HOTKEY = "1"            # F10 arms the takeover
$env:NHL_HIGHCUT_FRAME_CAPTURE = "1"       # required: drives the per-draw packet build + frame boundary
# Consumer (plume present window, renders) + the live bridge:
$env:NHL_HIGHCUT_PRESENT = "1"             # stand up the plume Vulkan window
$env:NHL_HIGHCUT_C5 = "1"                  # enable the C-5 renderable-draw path in plume
$env:NHL_HIGHCUT_LIVE_FEED = "1"           # C-6: rebuild from the in-memory bridge each committed frame
# Filmstrip:
$env:NHL_HIGHCUT_FILMSTRIP = $shots
$env:NHL_HIGHCUT_FILMSTRIP_EVERY = "$Every"
# Profiler: logs [highcut-perf] live FPS + per-section producer cost (translate/untile/packet) every 60
# committed frames, so the freeze fix is measured on REAL gameplay.
$env:NHL_HIGHCUT_PROFILE = "1"
$argline = '--game_data_root "H:\Emulators\games\XBOX\NHL Legacy - Vanilla"'
$p = Start-Process -FilePath "$dir\nhllegacy.exe" -ArgumentList $argline -WorkingDirectory $dir -PassThru -NoNewWindow -RedirectStandardError "$dir\live_stderr.log" -RedirectStandardOutput "$dir\live_stdout.log"
Write-Output "GAME LAUNCHED (pid $($p.Id)). Press F10 at a live scene, then navigate the screens you want to inspect."
Write-Output "FILMSTRIP -> $shots  (every $Every frames)"
$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) { Start-Sleep -Seconds 2 }
if (-not $p.HasExited) { $p.Kill() }
Start-Sleep -Milliseconds 600
$log = (Get-ChildItem "$dir\logs\*.log" | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
Write-Output "LOG=$log"
$n = (Get-ChildItem "$shots\*.png" -ErrorAction SilentlyContinue | Measure-Object).Count
Write-Output "FILMSTRIP FRAMES WRITTEN: $n  (in $shots)"
Write-Output "--- takeover + live-feed signals ---"
Select-String -Path $log -Pattern 'LIVE takeover ACTIVE|highcut-C5-LIVE|loaded \d+ renderable' | Select-Object -Last 4 | ForEach-Object { ($_.Line -split '\] ')[-1] }
Write-Output "--- live FPS + producer cost (real gameplay) ---"
Select-String -Path $log -Pattern 'highcut-perf' | Select-Object -Last 12 | ForEach-Object { ($_.Line -split '\] ')[-1] }
Write-Output "--- crashes / device-removed (target none) ---"
Select-String -Path $log -Pattern 'device removed|0x887A|TDR|abort|assert|exception' | Select-Object -Last 5 | ForEach-Object { ($_.Line -split '\] ')[-1] }
