# SPDX-License-Identifier: GPL-3.0-or-later
# Layer overhead on a real Steam game: interleaved runs without/with the layer
# (off, on, off, on, ...), each recorded with PresentMon, then a summary of
# frame times. Needs an elevated shell (PresentMon uses ETW) and
# target\release\examples\layer.exe + target\release\tessera_d3d11.dll.
#   powershell -File tools\measure_overhead.ps1 -AppId 1462040 -LaunchArgs "-dx11" `
#       -GameDir "D:\Steam\...\Win64" -ExeName ff7remake_.exe -PresentMon <path>
# Leaves the layer as it found it. Output: <OutDir>\run<N>_<off|on>.csv, measure.log, summary.txt.
param(
    [Parameter(Mandatory)][string]$AppId,
    [string]$LaunchArgs = "",
    [Parameter(Mandatory)][string]$GameDir,
    [Parameter(Mandatory)][string]$ExeName,
    [Parameter(Mandatory)][string]$PresentMon,
    [int]$Runs = 4,
    [int]$Settle = 60,
    [int]$Record = 60,
    [string]$Steam = "C:\Program Files (x86)\Steam\steam.exe",
    [string]$OutDir = (Join-Path (Split-Path $PSScriptRoot -Parent) "build\bench")
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$layer = Join-Path $root "target\release\examples\layer.exe"
$payload = Join-Path $root "target\release\tessera_d3d11.dll"
$procName = [IO.Path]::GetFileNameWithoutExtension($ExeName)
New-Item -ItemType Directory -Force $OutDir | Out-Null
$log = Join-Path $OutDir "measure.log"
function Log($m) { $l = "{0:HH:mm:ss} {1}" -f (Get-Date), $m; $l; Add-Content -Path $log -Value $l -Encoding utf8 }
Set-Content -Path $log -Value "" -Encoding utf8
Add-Type -Name W -Namespace Tessera -MemberDefinition `
    '[DllImport("user32.dll")] public static extern bool PostMessage(System.IntPtr h, uint m, System.IntPtr w, System.IntPtr l);'

$initial = (& $layer status $GameDir) -join ""
Log "layer initially: $initial"
try {
    for ($i = 1; $i -le $Runs; $i++) {
        $on = ($i % 2) -eq 0
        $mode = if ($on) { "on" } else { "off" }
        if ($on) { & $layer install $GameDir $payload | Out-Null } else { & $layer uninstall $GameDir | Out-Null }
        if ($LASTEXITCODE) { throw "layer $mode failed" }
        Start-Process $Steam -ArgumentList "-applaunch $AppId $LaunchArgs"
        $p = $null
        for ($t = 0; $t -lt 180 -and -not $p; $t++) { Start-Sleep 1; $p = Get-Process $procName -ErrorAction SilentlyContinue | Select-Object -First 1 }
        if (-not $p) { throw "run ${i}: $ExeName did not start" }
        Log "run $i ($mode): pid $($p.Id), settling $Settle s"
        Start-Sleep $Settle
        $csv = Join-Path $OutDir "run${i}_$mode.csv"
        $pmlog = Join-Path $OutDir "run${i}_$mode.pm.txt"
        & $PresentMon --process_id $p.Id --output_file $csv --timed $Record --terminate_after_timed `
            --no_console_stats --stop_existing_session --session_name TesseraMeasure *> $pmlog
        $pmExit = $LASTEXITCODE
        if (Test-Path $csv) { Log "run ${i}: recorded $csv" }
        else { Log "run ${i}: NO CSV (PresentMon exit $pmExit, see $pmlog)" }
        $p.Refresh()
        [Tessera.W]::PostMessage($p.MainWindowHandle, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        if (-not $p.WaitForExit(60000)) { Log "run ${i}: no exit after WM_CLOSE, killing"; $p.Kill(); $p.WaitForExit() }
        Start-Sleep 10  # let Steam and the launcher settle
    }
} catch {
    Log "error: $_"
} finally {
    if ($initial -match "NotInstalled") { & $layer uninstall $GameDir | Out-Null } else { & $layer install $GameDir $payload | Out-Null }
    Log "layer restored: $((& $layer status $GameDir) -join '')"
}

function Pct($s, $p) { $s[[math]::Max(0, [math]::Ceiling($p / 100 * $s.Count) - 1)] }
$summary = Get-ChildItem (Join-Path $OutDir "run*_*.csv") | Sort-Object Name | ForEach-Object {
    $rows = @(Import-Csv $_.FullName)
    if ($rows.Count -eq 0) { "{0,-10} no frames" -f $_.BaseName; return }
    $col = if ($rows[0].PSObject.Properties.Name -contains "MsBetweenPresents") { "MsBetweenPresents" } else { "FrameTime" }
    $v = $rows | ForEach-Object { [double]$_.$col } | Where-Object { $_ -gt 0 } | Sort-Object
    $avg = ($v | Measure-Object -Average).Average
    "{0,-10} n={1,5}  avg {2,6:N3}  p50 {3,6:N3}  p99 {4,6:N3}  p99.9 {5,6:N3}  max {6,7:N2}  fps {7,6:N1}" -f `
        $_.BaseName, $v.Count, $avg, (Pct $v 50), (Pct $v 99), (Pct $v 99.9), $v[-1], (1000 / $avg)
}
if (-not $summary) { $summary = "no PresentMon CSVs: nothing measured (see run*.pm.txt)" }
$summary | Set-Content (Join-Path $OutDir "summary.txt") -Encoding utf8
$summary
Log "done"
