param([string]$RtPath = "rov", [switch]$Baseline,
      [string]$Dir = "e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-vk",
      [string]$Tag = "vk")
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W32 {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RC r);
  [StructLayout(LayoutKind.Sequential)] public struct RC { public int L, T, R, B; }
}
"@
function Shot($p, $path) {
  $p.Refresh(); $h = $p.MainWindowHandle
  [W32]::ShowWindow($h, 9) | Out-Null; [W32]::SetForegroundWindow($h) | Out-Null
  Start-Sleep -Milliseconds 600
  $r = New-Object W32+RC; [W32]::GetWindowRect($h, [ref]$r) | Out-Null
  $w = $r.R - $r.L; $ht = $r.B - $r.T
  if ($w -le 0 -or $ht -le 0) { Write-Output "  bad rect"; return }
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size($w, $ht)))
  $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
  Write-Output "  SHOT $path ($([math]::Round((Get-Item $path).Length/1KB,0))KB)"
}
$dir = $Dir
Get-Process nhllegacy -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
Remove-Item env:NHL_VK_BACKEND -ErrorAction SilentlyContinue
if (-not $Baseline) { $env:NHL_VK_BACKEND = "1"; $env:NHL_VK_RT_PATH = $RtPath }
$argline = '--game_data_root "H:\Emulators\games\XBOX\NHL Legacy - Vanilla"'
$p = Start-Process -FilePath "$dir\nhllegacy.exe" -ArgumentList $argline -WorkingDirectory $dir -PassThru
foreach ($t in @(40, 70, 100, 130)) {
  while (((Get-Date) - $p.StartTime).TotalSeconds -lt $t) { Start-Sleep -Milliseconds 500; if ($p.HasExited) { break } }
  if ($p.HasExited) { Write-Output "EXITED before ${t}s"; break }
  Write-Output "t=${t}s title='$($p.MainWindowTitle)'"
  Shot $p "$dir\${Tag}_t${t}.png"
}
if (-not $p.HasExited) { $p.Kill() }
