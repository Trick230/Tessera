# SPDX-License-Identifier: GPL-3.0-or-later
# Layer smoke test: clean run + session check, then safe mode after two crashes.
#   powershell -File tests/run_smoke.ps1 [-Config Release]
param([string]$Config = "Release")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\tests\d3d11_smoke\$Config\d3d11_smoke.exe"
$layer = Join-Path $root "target\release\examples\layer.exe"
$data = Join-Path $root "build\smoke-data"
if (Test-Path $data) { Remove-Item -Recurse -Force $data }
$env:TESSERA_DATA_DIR = $data
$fails = 0
function Check($name, $ok) {
    if ($ok) { "PASS $name" } else { "FAIL $name"; $script:fails++ }
}

# M2 is off by default; the session check expects its priority flags.
$ini = Join-Path (Split-Path $exe) "tessera.ini"
Set-Content -Path $ini -Value "[tessera]`nresidency=1" -Encoding ascii
try {
    & $exe | Write-Host
    Check "clean run" ($LASTEXITCODE -eq 0)
} finally { Remove-Item $ini }
& $exe --verify "$data\sessions" | Write-Host
Check "session contents" ($LASTEXITCODE -eq 0)
$counter = @(Get-ChildItem "$data\safemode\d3d11_smoke.exe-*.count")
Check "one counter keyed by path" ($counter.Count -eq 1)
$counter = $counter[0].FullName
Check "counter cleared on clean exit" ((Get-Content $counter) -eq "0")

$log = "$data\logs\d3d11_smoke.exe.log"
Check "process tweaks logged" ((Select-String -Path $log -Pattern "process:" -Quiet))
Check "above-normal priority (P5)" ((Select-String -Path $log -Pattern "above-normal priority ok" -Quiet))

# NVAPI resolved by the exe and by a game DLL loaded later (4: no NVAPI here).
& $exe --nvapi | Write-Host
$nv = $LASTEXITCODE
if ($nv -ne 4) { Check "NVAPI wrapped for the exe and a game DLL" ($nv -eq 0) }

# Frame limiter (P1), budget cap (M1), flip model (P2) and frame latency (P3) from tessera.ini next to the layer.
# ext_fps_limit: another tool limits to 50 below our 60, so ours runs just under 50.
$ini = Join-Path (Split-Path $exe) "tessera.ini"
Set-Content -Path $ini -Value "[tessera]`nresidency=1`nfps_limit=60`next_fps_limit=50`nvram_budget_mb=2048`nflip_model=1`nmax_frame_latency=1" -Encoding ascii
$env:TESSERA_SMOKE_CAP_MB = "2048"
try {
    & $exe | Write-Host
    Check "limited run" ($LASTEXITCODE -eq 0)
    & $exe --verify "$data\sessions" 50 | Write-Host
    Check "limiter holds just under the other tool's 50 fps, budget capped, flip model" ($LASTEXITCODE -eq 0)
    Check "flip conversion logged" ((Select-String -Path $log -Pattern "converted to flip model" -Quiet))
    Check "frame latency through the waitable object (P3)" ((Select-String -Path $log -Pattern "latency 1 through the waitable object: ok" -Quiet))
    # S6: two shaders stored once each across both runs (header 24 + 2 index entries of 16).
    $idx = Get-ChildItem "$data\shaders\d3d11_smoke.exe-*.tshi"
    Check "shader store has 2 shaders" ($idx.Count -eq 1 -and $idx[0].Length -eq 56)
    # S5: the warm-up compiles both, then a second run finds nothing left to do.
    $warm = Join-Path $root "build\src\warm\$Config\tessera_warm.exe"
    $base = $idx[0].FullName -replace '\.tshi$', ''
    $w1 = & $warm $base
    Check "warm-up compiles 2 shaders" ($LASTEXITCODE -eq 0 -and "$w1" -match "compiled now 2, failed now 0")
    $w2 = & $warm $base
    Check "warm-up resumes with nothing left" ($LASTEXITCODE -eq 0 -and "$w2" -match "compiled now 0, failed now 0, skipped now 0, total ok 2")
    # The app path: copy under the game's exe name, run, read the log back.
    $w3 = & $layer warm $exe $warm
    Check "app warm-up reads the log" ($LASTEXITCODE -eq 0 -and "$w3" -match "warm log: ok 2 failed 0")
    Check "warm copy has the game's exe name" (@(Get-ChildItem "$data\warm\*\d3d11_smoke.exe").Count -eq 1)
} finally { Remove-Item $ini; Remove-Item Env:TESSERA_SMOKE_CAP_MB }

# M3-M5: managed textures lose their top mip while unbound and get it back before any read.
Set-Content -Path $ini -Value "[tessera]`nmip_residency=1`nmip_force_evict=1" -Encoding ascii
try {
    $t = & $exe --tiled
    $t | Write-Host
    Check "mip residency run" ($LASTEXITCODE -eq 0)
    if ("$t" -notmatch "skipped") {
        Check "residency on logged" ((Select-String -Path $log -Pattern "residency: on" -Quiet))
        $alive = Select-String -Path $log -Pattern "the managed one is still alive" | Select-Object -First 1
        $over = Select-String -Path $log -Pattern "new device takes over" | Select-Object -First 1
        Check "a second device waits while the first lives" ($alive -and (-not $over -or $alive.LineNumber -lt $over.LineNumber))
        Check "a replacing device takes over once the first is gone" ($over -and
            @(Select-String -Path $log -Pattern "residency: on").Count -ge 2)
    }
} finally { Remove-Item $ini }

& $exe --crash | Out-Null
& $exe --crash | Out-Null
Check "counter after two crashes" ((Get-Content $counter) -eq "2")
# The app computes the same path key (crates/core/src/session.rs).
Check "app reads the same counter" ((& $layer crashes $exe) -eq "2")
$before = (Get-ChildItem "$data\sessions\*.tsess").Count
& $exe | Write-Host
Check "game still runs in safe mode" ($LASTEXITCODE -eq 0)
& $exe --nvapi | Write-Host
if ($nv -ne 4) { Check "imports untouched in safe mode" ($LASTEXITCODE -eq 3) }
$after = (Get-ChildItem "$data\sessions\*.tsess").Count
Check "no session in safe mode" ($after -eq $before)
Check "safe mode logged" ((Select-String -Path "$data\logs\d3d11_smoke.exe.log" -Pattern "safe mode" -Quiet))

if ($fails) { "$fails check(s) failed"; exit 1 } else { "all checks passed" }
