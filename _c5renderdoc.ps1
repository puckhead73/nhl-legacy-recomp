param(
  # Default: isolate ONE primary-surface SKINNED draw so the captured plume frame is a single draw
  # (trivial to inspect). 369-380 are primary (depth=736) skinned draws; 375 = 708 verts.
  [int]$Draw = 375,
  [switch]$FullFrame,        # render the whole frame instead of one draw
  [int]$Seconds = 600,       # how long to leave the game open for you to F12 (then it auto-closes)
  [string]$RenderDoc = "E:\Personal Projects\NHL Modding Studio\Tools\Graphic Editors\RenderDoc_1.44_64\RenderDoc_1.44_64"
)
# C-5d.3 RENDERDOC CAPTURE - run the C-5 plume replay with RenderDoc's VULKAN layer enabled (via env,
# NO admin / system registration needed), so the skinned VS can be inspected live. This hooks ONLY the
# plume Vulkan instance - the game's D3D12 (rexglue) is untouched. The 'NHL high-cut (plume Vulkan)'
# window shows a RenderDoc overlay; press F12 there to capture one frame.
$dir = "e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-relwithdebinfo"
$rdjson = Join-Path $RenderDoc "renderdoc.json"
if (-not (Test-Path $rdjson))  { Write-Error "renderdoc.json not found at: $rdjson  (pass -RenderDoc <folder>)"; exit 1 }
if (-not (Test-Path "$dir\highcut_frame.count")) { Write-Error "no captured frame ($dir\highcut_frame.count) - run _c5dump.ps1 first"; exit 1 }

Get-Process nhllegacy,qrenderdoc -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400

# Clean stale C-5 overrides AND the validation layer (RenderDoc replaces the layer chain).
foreach ($k in @("NHL_HIGHCUT_C5_PRIMARY_PITCH","NHL_HIGHCUT_C5_PRIMARY_DEPTH","NHL_HIGHCUT_C5_CLEAR",
                 "NHL_HIGHCUT_C5_MINDRAW","NHL_HIGHCUT_C5_MAXDRAW","NHL_HIGHCUT_C5_NOSPLIT",
                 "NHL_HIGHCUT_C5_OFFSCREEN","NHL_HIGHCUT_NOCULL","NHL_HIGHCUT_FLIP_FACE",
                 "VK_LAYER_SETTINGS_PATH")) {
  Remove-Item "env:$k" -ErrorAction SilentlyContinue
}
$env:NHL_HIGHCUT_PRESENT = "1"
$env:NHL_HIGHCUT_C5 = "1"
if (-not $FullFrame) {
  $env:NHL_HIGHCUT_C5_MINDRAW = "$Draw"
  $env:NHL_HIGHCUT_C5_MAXDRAW = "$($Draw + 1)"
  Write-Output "Isolating SKINNED draw #$Draw (primary surface) -> the plume frame is just this one draw."
} else {
  Write-Output "Rendering the FULL frame -> find the skinned draw in the Event Browser (it binds a set2 RGBA32F bone texture)."
}

# Enable the RenderDoc Vulkan capture layer for THIS launch only (no system registration / no admin).
$env:VK_LAYER_PATH = $RenderDoc                         # where renderdoc.json + renderdoc.dll live
$env:VK_INSTANCE_LAYERS = "VK_LAYER_RENDERDOC_Capture"  # name from renderdoc.json
$env:ENABLE_VULKAN_RENDERDOC_CAPTURE = "1"              # the layer's enable_environment

$argline = '--game_data_root "H:\Emulators\games\XBOX\NHL Legacy - Vanilla"'
$p = Start-Process -FilePath "$dir\nhllegacy.exe" -ArgumentList $argline -WorkingDirectory $dir -PassThru `
       -RedirectStandardError "$dir\c5rdc_stderr.log" -RedirectStandardOutput "$dir\c5rdc_stdout.log"
Write-Output ""
Write-Output "GAME LAUNCHED (pid $($p.Id)) with the RenderDoc Vulkan layer."
Write-Output "STEPS:"
Write-Output "  1. Wait for 'NHL high-cut (plume Vulkan)' to appear (it shows a RenderDoc overlay: 'F12 to capture')."
Write-Output "  2. CLICK that window to focus it, then press F12 to capture one frame."
Write-Output "  3. Captures save to:  $env:TEMP\RenderDoc\   (open in $RenderDoc\qrenderdoc.exe)"
Write-Output "  4. Close the game window when done (or it auto-closes in $Seconds s)."
Write-Output "INSPECT the skinned-VS explosion in qrenderdoc:"
Write-Output "  - Select the draw -> Mesh Viewer -> compare VS In vs VS Out positions (Out exploded? by how much?)."
Write-Output "  - Pipeline State -> VS -> Resources: set2 RGBA32F bone texture (matrices look right?), set0 vertex SSBO."
Write-Output "  - Right-click a vertex -> Debug Vertex -> step the VS: watch the bone-matrix sample, the WEIGHTS, gl_Position."
Write-Output ""

$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) { Start-Sleep -Seconds 2 }
if (-not $p.HasExited) { $p.Kill() }
Start-Sleep -Milliseconds 600

$rdcDir = Join-Path $env:TEMP "RenderDoc"
$rdc = Get-ChildItem "$rdcDir\nhllegacy_*.rdc","$rdcDir\*.rdc" -ErrorAction SilentlyContinue |
       Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($rdc) { Write-Output "CAPTURED: $($rdc.FullName)`n  open in: $RenderDoc\qrenderdoc.exe" }
else { Write-Output "No .rdc found in $rdcDir - did the plume window get focus + F12? Re-run and try again." }
