param([int]$Seconds = 50, [string]$RtPath = "rov")
# SPIKE Phase A: plain recompiled game on the SDK's native Vulkan backend.
# NO NHL_BACKEND / NHL_HIGHCUT_* — just NHL_VK_BACKEND.
$dir = "e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-vk"
Get-Process nhllegacy -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
# Clear any stale render/capture env from prior runs.
foreach ($k in @("NHL_BACKEND","NHL_BETA_TAKEOVER","NHL_BETA_LIVE","NHL_BETA_FLAT","NHL_BETA_DEPTH",
                 "NHL_BETA_LIVE_TRACE","NHL_SHOT_FRAME","NHL_SHOT_CONTINUOUS","NHL_REPLAY_XTR",
                 "NHL_HIGHCUT_PRESENT","NHL_HIGHCUT","NHL_HIGHCUT_PROFILE")) {
  Remove-Item "env:$k" -ErrorAction SilentlyContinue
}
$env:NHL_VK_BACKEND = "1"
$env:NHL_VK_RT_PATH = $RtPath
$argline = '--game_data_root "H:\Emulators\games\XBOX\NHL Legacy - Vanilla"'
$p = Start-Process -FilePath "$dir\nhllegacy.exe" -ArgumentList $argline -WorkingDirectory $dir -PassThru `
       -RedirectStandardError "$dir\vk_stderr.log" -RedirectStandardOutput "$dir\vk_stdout.log"
$start = Get-Date
Start-Sleep -Seconds $Seconds
$alive = -not $p.HasExited
if ($alive) { $p.Kill() }
Start-Sleep -Milliseconds 800
Write-Output ("process stayed alive {0}s: {1} (exit at {2}s)" -f $Seconds, $alive, [int]((Get-Date)-$start).TotalSeconds)
$log = (Get-ChildItem "$dir\logs\*.log" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1)
if (-not $log) { Write-Output "NO LOG FOUND in $dir\logs"; Get-Content "$dir\vk_stderr.log" -Tail 20 -ErrorAction SilentlyContinue; return }
Write-Output "LOG=$($log.FullName)"
Write-Output "=== backend / vulkan / present init ==="
Select-String -Path $log.FullName -Pattern "vulkan|Vulkan|graphics system|nhl-vk-spike|render_target_path|presenter|swapchain|VkDevice|GPU|physical device" | Select-Object -First 25 | ForEach-Object { $_.Line }
Write-Output "=== errors / fatals / asserts ==="
Select-String -Path $log.FullName -Pattern "error|fatal|assert|exception|crash|abort|sev=0|failed" | Select-Object -First 20 | ForEach-Object { $_.Line }
Write-Output "=== stderr tail ==="
Get-Content "$dir\vk_stderr.log" -Tail 12 -ErrorAction SilentlyContinue
