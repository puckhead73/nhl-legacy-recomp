# Boot smoke test: launch an installed/built nhllegacy.exe (no arguments -
# exercises the exe-relative game/ default), screenshot the window after the
# boot wait, then kill it. Usage: smoke_boot.ps1 -Exe <path> [-BootWait 45]
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [int]$BootWait = 45,
    [string]$Tag = "smoke"
)
$out = $env:TEMP
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public class W2 {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  public struct RECT { public int L, T, R, B; }
}
'@
$p = Start-Process -FilePath $Exe -WorkingDirectory (Split-Path -Parent $Exe) -PassThru
Write-Host "Launched pid $($p.Id); waiting ${BootWait}s for boot..."
Start-Sleep -Seconds $BootWait
$p.Refresh()
if ($p.HasExited) {
    Write-Host "FAIL: process exited during boot (code $($p.ExitCode))"
    exit 1
}
$h = $p.MainWindowHandle
$r = New-Object W2+RECT; [W2]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L; $hh = $r.B - $r.T
if ($w -lt 400) {
    Write-Host "FAIL: no main window after ${BootWait}s"
    Stop-Process -Id $p.Id -Force
    exit 1
}
$bmp = New-Object System.Drawing.Bitmap($w, $hh)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc(); [W2]::PrintWindow($h, $dc, 2) | Out-Null; $g.ReleaseHdc($dc)
$png = Join-Path $out "${Tag}_boot.png"
$bmp.Save($png)
Stop-Process -Id $p.Id -Force
Write-Host "OK: window ${w}x${hh}, screenshot: $png"
exit 0
