param(
  [int]$Seconds = 120,
  # Bisection knobs — pass as params (NOT persistent env vars, which silently hijack later renders).
  [int]$PrimaryPitch = 0, [int]$PrimaryDepth = -1, [int]$MinDraw = -1, [int]$MaxDraw = -1,
  [string]$Clear = "", [switch]$NoSplit, [switch]$Offscreen, [switch]$NoCull, [switch]$FlipFace
)
# C-5a PHASE 2 — REPLAY THE FRAME. Present-only: the game boots normally and an additive plume
# window stands up. The plume thread loads the captured frame (highcut_frame.count ->
# highcut_frame_<N>.bin) and replays EVERY owned draw in order into one flat RT, each with its own
# pipeline + per-draw blend — a (partial) menu composited on plume. NHL_HIGHCUT_C5=1 enables it.
$dir = "e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-relwithdebinfo"
Get-Process nhllegacy -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
foreach ($k in @("NHL_BACKEND","NHL_BETA_TAKEOVER","NHL_BETA_LIVE","NHL_BETA_FLAT","NHL_BETA_DEPTH",
                 "NHL_BETA_LIVE_HOTKEY","NHL_HIGHCUT_FRAME_CAPTURE","NHL_HIGHCUT_XLAT_TEST","NHL_HIGHCUT_C3")) {
  Remove-Item "env:$k" -ErrorAction SilentlyContinue
}
$env:NHL_HIGHCUT_PRESENT = "1"
$env:NHL_HIGHCUT_C5 = "1"
# ALWAYS clear stale bisection overrides first (a leftover NHL_HIGHCUT_C5_PRIMARY_DEPTH=184 silently
# forces the BLACK skinned/prepass surface to the screen — the recurring "all black" trap). Then set
# ONLY what was passed as a param this run, so the default is a clean default.
foreach ($k in @("NHL_HIGHCUT_C5_PRIMARY_PITCH","NHL_HIGHCUT_C5_PRIMARY_DEPTH","NHL_HIGHCUT_C5_CLEAR",
                 "NHL_HIGHCUT_C5_MINDRAW","NHL_HIGHCUT_C5_MAXDRAW","NHL_HIGHCUT_C5_NOSPLIT",
                 "NHL_HIGHCUT_C5_OFFSCREEN","NHL_HIGHCUT_NOCULL","NHL_HIGHCUT_FLIP_FACE")) {
  Remove-Item "env:$k" -ErrorAction SilentlyContinue
}
if ($PrimaryPitch -gt 0) { $env:NHL_HIGHCUT_C5_PRIMARY_PITCH = "$PrimaryPitch" }
if ($PrimaryDepth -ge 0) { $env:NHL_HIGHCUT_C5_PRIMARY_DEPTH = "$PrimaryDepth" }
if ($MinDraw -ge 0)      { $env:NHL_HIGHCUT_C5_MINDRAW = "$MinDraw" }
if ($MaxDraw -ge 0)      { $env:NHL_HIGHCUT_C5_MAXDRAW = "$MaxDraw" }
if ($Clear -ne "")       { $env:NHL_HIGHCUT_C5_CLEAR = $Clear }
if ($NoSplit)            { $env:NHL_HIGHCUT_C5_NOSPLIT = "1" }
if ($Offscreen)          { $env:NHL_HIGHCUT_C5_OFFSCREEN = "1" }
if ($NoCull)             { $env:NHL_HIGHCUT_NOCULL = "1" }
if ($FlipFace)           { $env:NHL_HIGHCUT_FLIP_FACE = "1" }
$act = @("NHL_HIGHCUT_C5_PRIMARY_PITCH","NHL_HIGHCUT_C5_PRIMARY_DEPTH","NHL_HIGHCUT_C5_CLEAR",
         "NHL_HIGHCUT_C5_MINDRAW","NHL_HIGHCUT_C5_MAXDRAW","NHL_HIGHCUT_C5_NOSPLIT",
         "NHL_HIGHCUT_C5_OFFSCREEN","NHL_HIGHCUT_NOCULL","NHL_HIGHCUT_FLIP_FACE") |
       Where-Object { Test-Path "env:$_" } | ForEach-Object { "$_=$((Get-Item "env:$_").Value)" }
if ($act) { Write-Output ("C5 overrides this run: " + ($act -join "  ")) } else { Write-Output "C5 overrides: none (clean default)" }
$env:VK_INSTANCE_LAYERS = "VK_LAYER_KHRONOS_validation"
if (Test-Path "$dir\vk_layer_settings.txt") { $env:VK_LAYER_SETTINGS_PATH = "$dir\vk_layer_settings.txt" }
$argline = '--game_data_root "H:\Emulators\games\XBOX\NHL Legacy - Vanilla"'
$p = Start-Process -FilePath "$dir\nhllegacy.exe" -ArgumentList $argline -WorkingDirectory $dir -PassThru -NoNewWindow -RedirectStandardError "$dir\c5r_stderr.log" -RedirectStandardOutput "$dir\c5r_stdout.log"
Write-Output "GAME LAUNCHED (pid $($p.Id)). Watch 'NHL high-cut (plume Vulkan)' for the composited menu."
$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) { Start-Sleep -Seconds 2 }
if (-not $p.HasExited) { $p.Kill() }
Start-Sleep -Milliseconds 600
$log = (Get-ChildItem "$dir\logs\*.log" | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
Write-Output "LOG=$log"
Write-Output "--- C-5 replay ---"
Select-String -Path $log -Pattern "\[highcut-C5\]" | Select-Object -Last 6 | ForEach-Object { ($_.Line -split '\] ')[-1] }
Write-Output "--- Vulkan validation errors (target: NONE) ---"
$vk = Select-String -Path $log -Pattern "Validation (Error|Warning)|VUID-"
Write-Output ("count: " + $vk.Count)
$vk | Select-Object -First 8 | ForEach-Object { ($_.Line -split '\] ')[-1] }
