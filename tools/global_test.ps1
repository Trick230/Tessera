# SPDX-License-Identifier: GPL-3.0-or-later
# Global test of the D3D11 layer on a real game: does it help, and does every
# module work? Interleaved runs without/with the layer (off, on, off, on, ...).
# In each run the player loads the same save and plays the same route while
# PresentMon records; the "on" runs also keep the layer's log and session
# summary (shaders, VRAM, working set, mip residency) and list what each
# module reported. Needs an elevated shell (PresentMon uses ETW) and a fresh
# build (build.ps1: target\release\tessera_d3d11.dll and the layer/session
# examples).
#   powershell -File tools\global_test.ps1 -AppId 1462040 -LaunchArgs "-dx11" `
#       -GameDir "D:\Steam\steamapps\common\FINAL FANTASY VII REMAKE\End\Binaries\Win64" `
#       -ExeName ff7remake_.exe -PresentMon build\bench\pm\PresentMon-2.6.0-x64.exe
# -Auto records at whatever the game shows after -Settle seconds (no player).
# The "on" runs use the game's tessera.ini with mip_residency=1 and fps_limit
# set to -FpsLimit (0 by default, so the limiter does not bias the
# comparison). Leaves the layer and tessera.ini as it found them.
# Output: <OutDir>\run<N>_<off|on>.csv/.log/.session.txt, global.log, summary.txt.
param(
    [Parameter(Mandatory)][string]$AppId,
    [string]$LaunchArgs = "",
    [Parameter(Mandatory)][string]$GameDir,
    [Parameter(Mandatory)][string]$ExeName,
    [Parameter(Mandatory)][string]$PresentMon,
    [int]$Runs = 4,
    [int]$Record = 300,
    [int]$Settle = 60,
    [int]$FpsLimit = 0,
    [switch]$Auto,
    [string]$Steam = "C:\Program Files (x86)\Steam\steam.exe",
    [string]$OutDir = (Join-Path (Split-Path $PSScriptRoot -Parent) "build\global")
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$layer = Join-Path $root "target\release\examples\layer.exe"
$sessionExe = Join-Path $root "target\release\examples\session.exe"
$payload = Join-Path $root "target\release\tessera_d3d11.dll"
$PresentMon = (Resolve-Path $PresentMon).Path
$procName = [IO.Path]::GetFileNameWithoutExtension($ExeName)
$data = Join-Path $env:LOCALAPPDATA "Tessera"
$ini = Join-Path $GameDir "tessera.ini"
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { throw "run this from an elevated shell (PresentMon needs ETW)" }
foreach ($f in $layer, $sessionExe, $payload, $PresentMon) { if (-not (Test-Path $f)) { throw "missing $f (run build.ps1)" } }
if (Get-Process $procName -ErrorAction SilentlyContinue) { throw "$ExeName is already running" }
New-Item -ItemType Directory -Force $OutDir | Out-Null
Get-ChildItem $OutDir -Filter "run*" -ErrorAction SilentlyContinue | Remove-Item
$log = Join-Path $OutDir "global.log"
function Log($m) { $l = "{0:HH:mm:ss} {1}" -f (Get-Date), $m; $l; Add-Content -Path $log -Value $l -Encoding utf8 }
Set-Content -Path $log -Value "" -Encoding utf8
Add-Type -Name W -Namespace Tessera -MemberDefinition `
    '[DllImport("user32.dll")] public static extern bool PostMessage(System.IntPtr h, uint m, System.IntPtr w, System.IntPtr l);'

# tessera.ini for the "on" runs: the game's own settings, plus these.
$iniBackup = $null
if (Test-Path $ini) { $iniBackup = Get-Content $ini -Raw }
function Write-TestIni {
    $want = [ordered]@{ mip_residency = "1"; fps_limit = "$FpsLimit" }
    $lines = if ($iniBackup) { @($iniBackup -split "`r?`n" | Where-Object { $_ -ne "" }) } else { @("[tessera]") }
    foreach ($k in $want.Keys) {
        $i = [array]::FindIndex([string[]]$lines, [Predicate[string]] { param($l) $l -match "^\s*$k\s*=" })
        if ($i -ge 0) { $lines[$i] = "$k=$($want[$k])" } else { $lines += "$k=$($want[$k])" }
    }
    Set-Content -Path $ini -Value $lines -Encoding ascii
}

$initial = (& $layer status $GameDir) -join ""
Log "layer initially: $initial; on runs: mip_residency=1 fps_limit=$FpsLimit, the rest from tessera.ini"
try {
    for ($i = 1; $i -le $Runs; $i++) {
        $on = ($i % 2) -eq 0
        $mode = if ($on) { "on" } else { "off" }
        if ($on) {
            & $layer install $GameDir $payload | Out-Null
            if (-not $LASTEXITCODE) { Write-TestIni }
        } else { & $layer uninstall $GameDir | Out-Null }
        if ($LASTEXITCODE) { throw "layer $mode failed" }
        $start = Get-Date
        Start-Process $Steam -ArgumentList "-applaunch $AppId $LaunchArgs"
        $p = $null
        for ($t = 0; $t -lt 180 -and -not $p; $t++) { Start-Sleep 1; $p = Get-Process $procName -ErrorAction SilentlyContinue | Select-Object -First 1 }
        if (-not $p) { throw "run ${i}: $ExeName did not start" }
        Log "run $i ($mode): pid $($p.Id)"
        if ($Auto) { Start-Sleep $Settle }
        else {
            [console]::Beep(880, 300)
            Read-Host "run $i of $Runs ($mode): load the save, stand at the start of the route, then press Enter here and play it for $Record s"
        }
        $csv = Join-Path $OutDir "run${i}_$mode.csv"
        $pmlog = Join-Path $OutDir "run${i}_$mode.pm.txt"
        & $PresentMon --process_id $p.Id --output_file $csv --timed $Record --terminate_after_timed `
            --no_console_stats --stop_existing_session --session_name TesseraGlobal *> $pmlog
        $pmExit = $LASTEXITCODE
        if (Test-Path $csv) { Log "run ${i}: recorded $csv" } else { Log "run ${i}: NO CSV (PresentMon exit $pmExit, see $pmlog)" }
        # A clean exit keeps the session file whole and the safe-mode counter at zero.
        if ($Auto) {
            $p.Refresh()
            [Tessera.W]::PostMessage($p.MainWindowHandle, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        } else {
            [console]::Beep(660, 300); [console]::Beep(660, 300)
            Write-Host "run ${i}: recorded. Quit the game from its menu (waiting up to 5 min)."
        }
        $wait = if ($Auto) { 60000 } else { 300000 }
        if (-not $p.WaitForExit($wait)) { Log "run ${i}: the game did not exit, killing (unclean: counts toward safe mode)"; $p.Kill(); $p.WaitForExit() }
        if ($on) {
            $glog = Join-Path $data "logs\$ExeName.log"
            if (Test-Path $glog) { Copy-Item $glog (Join-Path $OutDir "run${i}_on.log") }
            $sess = Get-ChildItem (Join-Path $data "sessions") -Filter "$ExeName*.tsess" -ErrorAction SilentlyContinue |
                Where-Object { $_.LastWriteTime -ge $start } | Sort-Object LastWriteTime | Select-Object -Last 1
            if ($sess) { & $sessionExe $sess.FullName | Set-Content (Join-Path $OutDir "run${i}_on.session.txt") -Encoding utf8 }
            else { Log "run ${i}: no session file written (layer not loaded?)" }
        }
        Start-Sleep 10  # let Steam and the launcher settle
    }
} catch {
    Log "error: $_"
} finally {
    if ($initial -match "NotInstalled") { & $layer uninstall $GameDir | Out-Null } else { & $layer install $GameDir $payload | Out-Null }
    if ($null -ne $iniBackup) { Set-Content -Path $ini -Value $iniBackup -NoNewline -Encoding ascii }
    elseif (Test-Path $ini) { Remove-Item $ini }
    Log "layer restored: $((& $layer status $GameDir) -join ''), tessera.ini restored"
}

# Frame times per run, then off against on (means over the runs).
function Pct($s, $p) { $s[[math]::Max(0, [math]::Ceiling($p / 100 * $s.Count) - 1)] }
$rows = Get-ChildItem (Join-Path $OutDir "run*_*.csv") | Sort-Object { [int]($_.BaseName -replace '\D', '') } | ForEach-Object {
    $r = @(Import-Csv $_.FullName)
    if ($r.Count -eq 0) { return }
    $col = if ($r[0].PSObject.Properties.Name -contains "MsBetweenPresents") { "MsBetweenPresents" } else { "FrameTime" }
    $v = @($r | ForEach-Object { [double]$_.$col } | Where-Object { $_ -gt 0 } | Sort-Object)
    if ($v.Count -eq 0) { return }
    $avg = ($v | Measure-Object -Average).Average
    $p50 = Pct $v 50
    $secs = ($v | Measure-Object -Sum).Sum / 1000
    [pscustomobject]@{
        Run = $_.BaseName; Mode = ($_.BaseName -split "_")[1]; Frames = $v.Count; Fps = 1000 / $avg
        P50 = $p50; P99 = (Pct $v 99); P999 = (Pct $v 99.9); Max = $v[-1]
        # Stutters a player notices: over twice the median, and over 50 ms; per minute.
        Hitches = 60 * @($v | Where-Object { $_ -gt 2 * $p50 }).Count / $secs
        Over50 = 60 * @($v | Where-Object { $_ -gt 50 }).Count / $secs
    }
}
$out = @()
$out += "Frame times (ms; hitches = frames over 2x the median, per minute)"
foreach ($r in $rows) {
    $out += "{0,-8} n={1,6}  fps {2,6:N1}  p50 {3,6:N2}  p99 {4,6:N2}  p99.9 {5,6:N2}  max {6,7:N1}  hitches/min {7,6:N1}  >50ms/min {8,5:N1}" -f `
        $r.Run, $r.Frames, $r.Fps, $r.P50, $r.P99, $r.P999, $r.Max, $r.Hitches, $r.Over50
}
foreach ($m in "off", "on") {
    $g = @($rows | Where-Object Mode -eq $m)
    if ($g.Count -eq 0) { continue }
    $mean = { param($k) ($g | Measure-Object -Property $k -Average).Average }
    $out += "{0,-8} runs={1}  fps {2,6:N1}  p50 {3,6:N2}  p99 {4,6:N2}  p99.9 {5,6:N2}  hitches/min {6,6:N1}  >50ms/min {7,5:N1}" -f `
        "mean $m", $g.Count, (& $mean Fps), (& $mean P50), (& $mean P99), (& $mean P999), (& $mean Hitches), (& $mean Over50)
}
if (-not $rows) { $out += "no PresentMon CSVs: nothing measured (see run*.pm.txt)" }

# What each module said in the "on" runs.
$pattern = "config:|residency:|workset:|memory:|process:|flip:|limiter|pacing|shaders:|nvapi|safe mode|FAILED|failed|disabled|overwritten|clean exit"
foreach ($f in Get-ChildItem (Join-Path $OutDir "run*_on.log") -ErrorAction SilentlyContinue) {
    $out += ""
    $out += "$($f.BaseName): module messages"
    $out += Select-String -Path $f.FullName -Pattern $pattern | ForEach-Object { "  " + $_.Line } | Select-Object -Unique
}
foreach ($f in Get-ChildItem (Join-Path $OutDir "run*_on.session.txt") -ErrorAction SilentlyContinue) {
    $out += ""
    $out += "$($f.BaseName -replace '\.session$', ''): session summary"
    $out += Get-Content $f.FullName | ForEach-Object { "  " + $_ }
}
$out | Set-Content (Join-Path $OutDir "summary.txt") -Encoding utf8
$out
Log "done: $(Join-Path $OutDir 'summary.txt')"
