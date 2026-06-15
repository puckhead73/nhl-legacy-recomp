param(
  [Parameter(Mandatory=$true)][int]$Draw,   # captured draw index (highcut_frame_<Draw>.bin)
  [int]$Reg = -1,                            # output = registers[Reg]; -1 = no instrument (baseline)
  [string]$Ssa = "",                         # output = this SSA id (e.g. %1265); overrides -Reg
  [string]$Scalar = "",                      # output = a float SSA broadcast to grayscale (e.g. %1377)
  [int]$Cap = -1,                            # capture-at-point: snapshot registers[Cap]...
  [string]$After = "",                       # ...right after this SSA id (e.g. %1386)
  [switch]$Alpha,                            # show the alpha channel as grayscale
  [switch]$Step,                             # render registers 0..11, one shot each (12 launches)
  [switch]$Full,                             # render the WHOLE frame (default isolates -Draw only)
  [int]$PrimaryDepth = -1,                   # override primary guest surface depth_base (e.g. 736)
  [int]$PrimaryPitch = 0,                    # override primary guest surface pitch
  [int]$Seconds = 40,
  [string]$Out = ""                          # shot path (default _probe_d<Draw>_...png)
)
# C-5g PROBE: instrument ONE captured draw's pixel shader (force its fragment output to an intermediate
# register) and dump the isolated result to a PNG via the in-replay readback. One command does:
#   extract PS -> instrument (_instrument_ps.py) -> replay with DEBUG_PS + C5_SHOT -> report PNG.
$ErrorActionPreference = "Stop"
$dir = "e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-relwithdebinfo"
$tools = "e:\Repositories\nhl-legacy-recomp\tools"
$pkt = "$dir\highcut_frame_$Draw.bin"
if (-not (Test-Path $pkt)) { throw "missing packet $pkt (run _c5dump.ps1 first)" }
$asuffix = ""; if ($Alpha) { $asuffix = "_a" }

# 1. extract this draw's PS spirv (writes highcut_frame_<Draw>.ps.spv next to the packet)
python "$tools\_extract_spirv.py" $pkt | Out-Null
$psSpv = "$dir\highcut_frame_$Draw.ps.spv"

function Invoke-Probe([string]$shot, [string]$instrSpv) {
  Get-Process nhllegacy -ErrorAction SilentlyContinue | Stop-Process -Force
  Start-Sleep -Milliseconds 300
  foreach ($k in @("NHL_BACKEND","NHL_BETA_TAKEOVER","NHL_BETA_LIVE","NHL_BETA_FLAT","NHL_BETA_DEPTH",
                   "NHL_HIGHCUT_FRAME_CAPTURE","NHL_HIGHCUT_C5_PRIMARY_PITCH","NHL_HIGHCUT_C5_PRIMARY_DEPTH",
                   "NHL_HIGHCUT_C5_CLEAR","NHL_HIGHCUT_C5_NOSPLIT","NHL_HIGHCUT_C5_OFFSCREEN",
                   "NHL_HIGHCUT_NOCULL","NHL_HIGHCUT_FLIP_FACE","NHL_HIGHCUT_DEBUG_PS","NHL_HIGHCUT_DEBUG_PS_DRAW",
                   "NHL_HIGHCUT_C5_SHOT","NHL_HIGHCUT_C5_MINDRAW","NHL_HIGHCUT_C5_MAXDRAW")) {
    Remove-Item "env:$k" -ErrorAction SilentlyContinue
  }
  $env:NHL_HIGHCUT_PRESENT = "1"; $env:NHL_HIGHCUT_C5 = "1"
  if (-not $Full) { $env:NHL_HIGHCUT_C5_MINDRAW = "$Draw"; $env:NHL_HIGHCUT_C5_MAXDRAW = "$($Draw + 1)" }
  if ($PrimaryDepth -ge 0) { $env:NHL_HIGHCUT_C5_PRIMARY_DEPTH = "$PrimaryDepth" }
  if ($PrimaryPitch -gt 0) { $env:NHL_HIGHCUT_C5_PRIMARY_PITCH = "$PrimaryPitch" }
  if ($instrSpv -ne "") { $env:NHL_HIGHCUT_DEBUG_PS = $instrSpv; $env:NHL_HIGHCUT_DEBUG_PS_DRAW = "$Draw" }
  $env:NHL_HIGHCUT_C5_SHOT = $shot
  if (Test-Path $shot) { Remove-Item $shot -Force }
  $argline = '--game_data_root "H:\Emulators\games\XBOX\NHL Legacy - Vanilla"'
  $p = Start-Process -FilePath "$dir\nhllegacy.exe" -ArgumentList $argline -WorkingDirectory $dir -PassThru `
         -NoNewWindow -RedirectStandardError "$dir\c5p_stderr.log" -RedirectStandardOutput "$dir\c5p_stdout.log"
  $deadline = (Get-Date).AddSeconds($Seconds)
  while ((Get-Date) -lt $deadline -and -not $p.HasExited -and -not (Test-Path $shot)) { Start-Sleep -Milliseconds 500 }
  Start-Sleep -Milliseconds 800
  if (-not $p.HasExited) { $p.Kill() }
  Start-Sleep -Milliseconds 400
  if (Test-Path $shot) { Write-Output "SHOT: $shot" } else { Write-Output "NO SHOT (see $dir\c5p_stderr.log)" }
}

function Get-InstrArgs([string]$src, [string]$dst, [string]$mode, [string]$val) {
  $a = @($src, $dst)
  if ($mode -eq "reg") { $a += @("--reg", $val) }
  elseif ($mode -eq "ssa") { $a += @("--ssa", $val) }
  elseif ($mode -eq "step") { $a += @("--step") }
  if ($Alpha) { $a += @("--alpha") }
  return $a
}

if ($Step) {
  $prefix = "$dir\_probe_d$Draw"
  $iargs = Get-InstrArgs $psSpv $prefix "step" ""
  python "$tools\_instrument_ps.py" @iargs | Out-Null
  for ($k = 0; $k -lt 12; $k++) {
    $spv  = "$prefix" + "_reg$k" + $asuffix + ".spv"
    $shot = "$prefix" + "_reg$k" + $asuffix + ".png"
    Write-Output "--- reg $k ---"
    Invoke-Probe $shot $spv
  }
  Write-Output "Done. Shots: $prefix`_reg*$asuffix.png"
  return
}

$instr = ""
if ($Scalar -ne "") {
  $tag = ($Scalar -replace '[^0-9]','')
  $instr = "$dir\_probe_d$Draw" + "_scal$tag.spv"
  $iargs = @($psSpv, $instr, "--scalar", $Scalar)
  python "$tools\_instrument_ps.py" @iargs | Out-Null
  if ($Out -eq "") { $Out = "$dir\_probe_d$Draw" + "_scal$tag.png" }
  Invoke-Probe $Out $instr
  return
}
if ($Cap -ge 0 -and $After -ne "") {
  $tag = ($After -replace '[^0-9]','')
  $instr = "$dir\_probe_d$Draw" + "_cap$Cap" + "a$tag.spv"
  $iargs = @($psSpv, $instr, "--cap", "$Cap", "--after", $After)
  if ($Alpha) { $iargs += "--alpha" }
  python "$tools\_instrument_ps.py" @iargs | Out-Null
  if ($Out -eq "") { $Out = "$dir\_probe_d$Draw" + "_cap$Cap" + "a$tag" + $asuffix + ".png" }
  Invoke-Probe $Out $instr
  return
}
if ($Ssa -ne "") {
  $instr = "$dir\_probe_d$Draw" + "_ssa.spv"
  $iargs = Get-InstrArgs $psSpv $instr "ssa" $Ssa
  python "$tools\_instrument_ps.py" @iargs | Out-Null
  if ($Out -eq "") { $Out = "$dir\_probe_d$Draw" + "_ssa" + $asuffix + ".png" }
} elseif ($Reg -ge 0) {
  $instr = "$dir\_probe_d$Draw" + "_reg$Reg.spv"
  $iargs = Get-InstrArgs $psSpv $instr "reg" "$Reg"
  python "$tools\_instrument_ps.py" @iargs | Out-Null
  if ($Out -eq "") { $Out = "$dir\_probe_d$Draw" + "_reg$Reg" + $asuffix + ".png" }
} else {
  if ($Out -eq "") { $Out = "$dir\_probe_d$Draw" + "_base.png" }
}
Invoke-Probe $Out $instr
