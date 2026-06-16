param([int]$Seconds = 80, [string]$RtPath = "rov", [switch]$NoVsync = $true)
$dir = "e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-vk"
Get-Process nhllegacy -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
$env:NHL_VK_BACKEND = "1"; $env:NHL_VK_RT_PATH = $RtPath
if ($NoVsync) { $env:NHL_VK_NO_VSYNC = "1" } else { Remove-Item env:NHL_VK_NO_VSYNC -ErrorAction SilentlyContinue }
$argline = '--game_data_root "H:\Emulators\games\XBOX\NHL Legacy - Vanilla"'
$p = Start-Process -FilePath "$dir\nhllegacy.exe" -ArgumentList $argline -WorkingDirectory $dir -PassThru
Start-Sleep -Seconds $Seconds
if (-not $p.HasExited) { $p.Kill() }
Start-Sleep -Milliseconds 600
$log = (Get-ChildItem "$dir\logs\*.log" | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
Write-Output "LOG=$log"
Write-Output "=== [nhl-vk-fps] samples ==="
Select-String -Path $log -Pattern "nhl-vk-fps" | ForEach-Object { ($_.Line -split '\] ')[-1] }
