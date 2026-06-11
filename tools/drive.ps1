# Vision-guided menu driver: boot, then send a scripted key sequence with a
# screenshot after each step. Usage: drive.ps1 "<keys;pauseSec;label>|..."
param([string]$script, [int]$bootWait = 42, [string]$tag = "drive")
$exe = "E:\Repositories\nhl-legacy-recomp\out\build\win-amd64-relwithdebinfo\nhllegacy.exe"
$game = "H:\Emulators\games\XBOX\NHL Legacy - Vanilla"
$out = "C:\Users\puckh\AppData\Local\Temp"
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @'
using System; using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  public struct RECT { public int L, T, R, B; }
}
'@
function Snap($h, $name) {
  $r = New-Object W+RECT; [W]::GetWindowRect($h, [ref]$r) | Out-Null
  $w = $r.R - $r.L; $hh = $r.B - $r.T
  if ($w -lt 400) { Write-Host "  (no window for $name)"; return }
  $bmp = New-Object System.Drawing.Bitmap($w, $hh)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $dc = $g.GetHdc(); [W]::PrintWindow($h, $dc, 2) | Out-Null; $g.ReleaseHdc($dc)
  $bmp.Save("$out\${tag}_${name}.png"); Write-Host "  snap $name"
}
$p = Start-Process -FilePath $exe -ArgumentList "--game_data_root `"$game`"" -PassThru
Start-Sleep -Seconds $bootWait
$p.Refresh(); $h = $p.MainWindowHandle
[W]::SetForegroundWindow($h) | Out-Null
Snap $h "00boot"
$i = 1
foreach ($step in $script.Split("|")) {
  $parts = $step.Split(";")
  $keys = $parts[0]; $pause = [int]$parts[1]; $label = $parts[2]
  if ($keys) { [W]::SetForegroundWindow($h) | Out-Null; Start-Sleep -Milliseconds 300; [System.Windows.Forms.SendKeys]::SendWait($keys) }
  Start-Sleep -Seconds $pause
  $p.Refresh(); $h = $p.MainWindowHandle
  Snap $h ("{0:00}_{1}" -f $i, $label); $i++
}
Stop-Process -Id $p.Id -Force
Write-Host "done"
