param(
  [string]$RtPath = "fsi",   # fsi = in-game 3D path (the net bug lives here). "rov" => HUD only, no 3D.
  [switch]$Skip,             # drop DXT3+alpha-test+blend draws (the "net-like" signature)
  [switch]$SkipDxt3,         # drop ALL draws binding a DXT3 texture
  [switch]$SkipAlphaTest,    # drop ALL alpha-tested draws
  [switch]$SkipBlend,        # drop ALL blended draws
  [string]$SkipAddr = "",    # drop draws binding the texture at this base addr (hex, e.g. 158E7000)
  [double]$Ref = [double]::NaN, # override the net's alpha-test ref (sweep: 0.95, 1.5)
  [switch]$NoBlend,          # force identity blend on the net draw
  [double]$ForceAt = [double]::NaN) # replace net's alpha-to-coverage with alpha-test @ this ref
# Net-transparency diagnostic on the SDK native Vulkan (FSI/ROV) path.
# Launches the game VISIBLE + interactive with NHL_VK_DRAWLOG on. Drive to a
# gameplay scene with a NET on screen, leave it for a few seconds, then close the
# game (or run the read-back command this prints). The [nhl-vk-draw] line carries
# the net's raw render-state regs (RB_BLENDCONTROL0 / RB_COLORCONTROL / alpha_ref).
$dir = "e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-vk"
Get-Process nhllegacy -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
foreach ($k in @("NHL_BACKEND","NHL_BETA_TAKEOVER","NHL_BETA_LIVE","NHL_HIGHCUT",
                 "NHL_HIGHCUT_PRESENT","NHL_REPLAY_XTR","NHL_SHOT_FRAME","NHL_SHOT_CONTINUOUS",
                 "NHL_VK_NET_SKIP","NHL_VK_NET_REF","NHL_VK_NET_NOBLEND",
                 "NHL_VK_SKIP_DXT3","NHL_VK_SKIP_ALPHATEST","NHL_VK_SKIP_BLEND","NHL_VK_SKIP_ADDR",
                 "NHL_VK_NET_FORCE_AT","NHL_VK_NET_ADDR","NHL_VK_NO_ATOC_FIX","NHL_VK_ATOC_REF")) {
  Remove-Item "env:$k" -ErrorAction SilentlyContinue
}
$env:NHL_VK_BACKEND = "1"
$env:NHL_VK_RT_PATH  = $RtPath   # "fsi" = in-game 3D path (where the net is opaque); "rov" => no 3D
$env:NHL_VK_NO_VSYNC = "1"
$env:NHL_VK_DRAWLOG  = "1"
if ($Skip)          { $env:NHL_VK_NET_SKIP = "1";        Write-Output "EXPERIMENT: skip DXT3+alpha-test+blend draws" }
if ($SkipDxt3)      { $env:NHL_VK_SKIP_DXT3 = "1";       Write-Output "EXPERIMENT: skip ALL DXT3 draws" }
if ($SkipAlphaTest) { $env:NHL_VK_SKIP_ALPHATEST = "1";  Write-Output "EXPERIMENT: skip ALL alpha-tested draws" }
if ($SkipBlend)     { $env:NHL_VK_SKIP_BLEND = "1";      Write-Output "EXPERIMENT: skip ALL blended draws" }
if ($SkipAddr)      { $env:NHL_VK_SKIP_ADDR = $SkipAddr; Write-Output "EXPERIMENT: skip draws binding tex 0x$SkipAddr" }
if ($NoBlend)       { $env:NHL_VK_NET_NOBLEND = "1";     Write-Output "EXPERIMENT: force identity blend on net-like draws" }
if (-not [double]::IsNaN($Ref)) { $env:NHL_VK_NET_REF = "$Ref"; Write-Output "EXPERIMENT: net alpha-test ref = $Ref" }
if (-not [double]::IsNaN($ForceAt)) { $env:NHL_VK_NET_FORCE_AT = "$ForceAt"; Write-Output "EXPERIMENT: replace net alpha-to-coverage with alpha-test @ $ForceAt" }
$argline = '--game_data_root "H:\Emulators\games\XBOX\NHL Legacy - Vanilla"'
$p = Start-Process -FilePath "$dir\nhllegacy.exe" -ArgumentList $argline -WorkingDirectory $dir -PassThru
Write-Output "GAME LAUNCHED (pid $($p.Id)). Drive to gameplay with a NET on screen, then CLOSE the game."
Write-Output "This window will print the net's render-state regs once the game exits..."
# Wait until the user closes the game window, then dump the net_regs lines.
$p.WaitForExit()
Start-Sleep -Milliseconds 600
$log = (Get-ChildItem "$dir\logs\*.log" | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
Write-Output "LOG=$log"
Write-Output "=== [nhl-vk-draw] last window (texture inventory, largest first) ==="
Select-String -Path $log -Pattern "nhl-vk-draw" | Select-Object -Last 30 | ForEach-Object { ($_.Line -split '\] ')[-1] }
