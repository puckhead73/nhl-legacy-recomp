param([int]$WaitSeconds = 45, [string]$RtPath = "rov",
      [string]$Dir = "e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-vk",
      [string]$ShotName = "vk_window_shot.png",
      [switch]$Baseline)  # -Baseline = D3D12 default path (no NHL_VK_BACKEND)
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win32 {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@
$dir = $Dir
Get-Process nhllegacy -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
Remove-Item env:NHL_VK_BACKEND -ErrorAction SilentlyContinue
if (-not $Baseline) { $env:NHL_VK_BACKEND = "1"; $env:NHL_VK_RT_PATH = $RtPath }
$argline = '--game_data_root "H:\Emulators\games\XBOX\NHL Legacy - Vanilla"'
$p = Start-Process -FilePath "$dir\nhllegacy.exe" -ArgumentList $argline -WorkingDirectory $dir -PassThru
Start-Sleep -Seconds $WaitSeconds
if ($p.HasExited) { Write-Output "PROCESS EXITED EARLY"; return }
$p.Refresh()
$h = $p.MainWindowHandle
Write-Output "hwnd=$h title='$($p.MainWindowTitle)'"
[Win32]::ShowWindow($h, 9) | Out-Null      # SW_RESTORE
[Win32]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 700
$r = New-Object Win32+RECT
[Win32]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L; $ht = $r.B - $r.T
Write-Output "window rect = ${w}x${ht} at ($($r.L),$($r.T))"
$shot = "$dir\$ShotName"
$bmp = New-Object System.Drawing.Bitmap $w, $ht
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size($w, $ht)))
$bmp.Save($shot, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Output "SHOT=$shot ($([math]::Round((Get-Item $shot).Length/1KB,1))KB)"
if (-not $p.HasExited) { $p.Kill() }
