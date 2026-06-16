param([string]$Label = "scene", [int]$Seconds = 45)
# C-5 BROADER-SCENE AUDIT. Render the CURRENT capture (whatever _c5dump last produced) headless to a
# PNG and run the automated renderer checks. Run after each _c5dump of a different scene to confirm
# equipment + shadows + HUD hold across views:  .\_audit.ps1 -Label openice
$root = "e:\Repositories\nhl-legacy-recomp"
$dir  = "$root\out\build\win-amd64-relwithdebinfo"
$png  = "$dir\_audit_$Label.png"
Get-ChildItem env: | Where-Object Name -like 'NHL_*' | ForEach-Object { Remove-Item "env:$($_.Name)" }
$env:NHL_HIGHCUT_C5_SHOT = $png
Remove-Item $png -ErrorAction SilentlyContinue
Write-Output "=== AUDIT '$Label' : rendering current capture headless ==="
& "$root\_c5render.ps1" -Seconds $Seconds 2>&1 | Select-Object -Last 3
$log = (Get-ChildItem "$dir\logs\*.log" | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
Write-Output "--- draws / primary surface ---"
Select-String -Path $log -Pattern 'loaded \d+ renderable|PRIMARY surface =' | Select-Object -Last 2 | ForEach-Object { ($_.Line -split '\] ')[-1] }
Write-Output "--- composition: depth host-copy + HUD overlay ---"
Select-String -Path $log -Pattern 'composite mode|HUD overlay' | Select-Object -Last 2 | ForEach-Object { ($_.Line -split '\] ')[-1] }
$vk = @(Select-String -Path $log -Pattern 'Validation Error|Validation Warning|VUID-').Count
Write-Output "--- Vulkan validation messages, target 0: $vk ---"
Write-Output "--- stub census, target none ---"
python "$root\tools\_stub_census.py" $dir 2>&1 | Select-Object -First 4
Write-Output "--- PNG anomaly scan ---"
if (Test-Path $png) { python "$root\tools\_audit_png.py" $png } else { Write-Output "  NO PNG written" }
Write-Output "VIEW: $png"
