# Restore game-data roots touched by the stick-widen experiment:
# put the original nhlng.db back and remove staged high-index assets.
$roots = @()
$roots += "F:\Games\NHL Legacy\game\_compiled"
$roots += "H:\Emulators\games\XBOX\NHL Legacy - Vanilla\_compiled"

foreach ($van in $roots) {
    if (-not (Test-Path $van)) { continue }
    Write-Output "--- $van ---"
    $db  = Join-Path $van "db\nhlng.db"
    $bak = Join-Path $van "db\nhlng.db.bak_stickexp"
    if (Test-Path $bak) {
        Copy-Item $bak $db -Force
        Remove-Item $bak -Force
        Write-Output "  restored original db"
    } else {
        Write-Output "  no backup found - db left as-is"
    }
    $staged = @()
    $staged += (Join-Path $van "rendering\playerstick\texlib_200.rx2")
    $staged += (Join-Path $van "fe\ion\artassets\createplayer\sticks\stick200.big")
    foreach ($f in $staged) {
        if (Test-Path $f) { Remove-Item $f -Force; Write-Output ("  removed " + (Split-Path $f -Leaf)) }
    }
}
Write-Output "done"
