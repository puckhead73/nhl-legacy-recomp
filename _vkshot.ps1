param([int]$WaitSeconds = 45, [string]$RtPath = "rov")
# SPIKE Phase A visual+fps probe: boot on Vulkan, poll the presenter window title
# (fps often shown there), grab a screenshot, then kill.
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
$dir = "e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-vk"
Get-Process nhllegacy -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
foreach ($k in @("NHL_BACKEND","NHL_BETA_TAKEOVER","NHL_BETA_LIVE","NHL_HIGHCUT_PRESENT","NHL_HIGHCUT")) {
  Remove-Item "env:$k" -ErrorAction SilentlyContinue
}
$env:NHL_VK_BACKEND = "1"; $env:NHL_VK_RT_PATH = $RtPath
$argline = '--game_data_root "H:\Emulators\games\XBOX\NHL Legacy - Vanilla"'
$p = Start-Process -FilePath "$dir\nhllegacy.exe" -ArgumentList $argline -WorkingDirectory $dir -PassThru
# Poll the window title across the boot so we catch any fps the presenter prints.
$titles = @()
for ($i = 0; $i -lt $WaitSeconds; $i += 3) {
  Start-Sleep -Seconds 3
  if ($p.HasExited) { Write-Output "PROCESS EXITED EARLY at ~${i}s"; break }
  $p.Refresh()
  $t = $p.MainWindowTitle
  if ($t) { $titles += "${i}s: $t" }
}
$shot = "$dir\vk_phaseA_shot.png"
try {
  $b = [System.Windows.Forms.SystemInformation]::VirtualScreen
  $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
  $bmp.Save($shot, [System.Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose(); $bmp.Dispose()
  Write-Output "SHOT=$shot ($([math]::Round((Get-Item $shot).Length/1KB,1))KB)"
} catch { Write-Output "screenshot failed: $_" }
if (-not $p.HasExited) { $p.Kill() }
Write-Output "=== window titles seen (fps if presenter prints it) ==="
if ($titles.Count) { $titles | ForEach-Object { $_ } } else { Write-Output "(no window title captured)" }
