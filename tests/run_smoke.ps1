# SPDX-License-Identifier: GPL-3.0-or-later
# Layer smoke test: clean run + session check, then safe mode after two crashes.
#   powershell -File tests/run_smoke.ps1 [-Config Release] [-X86] [-PacingOnly] [-D3D11CallsOnly]
param([string]$Config = "Release", [switch]$X86, [switch]$PacingOnly, [switch]$D3D11CallsOnly)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$native = if ($X86) { "build\win32" } else { "build" }
$exe = Join-Path $root "$native\tests\d3d11_smoke\$Config\d3d11_smoke.exe"
$layer = Join-Path $root "target\release\examples\layer.exe"
$fails = 0
function Check($name, $ok) {
    if ($ok) { "PASS $name" } else { "FAIL $name"; $script:fails++ }
}

function CheckPacing {
    $bin = Split-Path $exe
    $run = Join-Path $root "build\pacing-smoke\$([guid]::NewGuid())"
    New-Item -ItemType Directory -Path $run -Force | Out-Null
    Copy-Item -LiteralPath $exe, "$bin\d3d11.dll", "$bin\load_probe.dll" -Destination $run
    Copy-Item -LiteralPath "$bin\load_probe.dll" -Destination "$run\sl.reflex.dll"
    $previousDataDir = $env:TESSERA_DATA_DIR
    $env:TESSERA_DATA_DIR = Join-Path $run "data"
    try {
        # A lower or equal external cap delegates; a higher one leaves our 60 FPS cap in charge.
        foreach ($ext in @(50, 60, 61)) {
            Set-Content "$run\tessera.ini" -Value "[tessera]`nfps_limit=60`next_fps_limit=$ext" -Encoding ascii
            $out = & "$run\d3d11_smoke.exe" --pacing
            $code = $LASTEXITCODE
            $out | Write-Host
            $ok = $false
            if ($code -eq 0 -and "$out" -match 'median=([0-9.]+) ms, recovery median=([0-9.]+) ms') {
                $median = [double]$Matches[1]
                $recovery = [double]$Matches[2]
                $ok = if ($ext -le 60) { $median -lt 10 } else { $median -ge 16 -and $recovery -ge 16 }
            }
            Check "60 FPS cap with an external cap of $ext" $ok
        }
        Set-Content "$run\tessera.ini" -Value "[tessera]`nfps_limit=60" -Encoding ascii
        & "$run\d3d11_smoke.exe" --pacing driver | Write-Host
        Check "idle SDK, secondary device, Reflex and NVAPI cap transitions" ($LASTEXITCODE -eq 0)
    } finally { $env:TESSERA_DATA_DIR = $previousDataDir }
}

function CheckD3D11Calls {
    $bin = Split-Path $exe
    $run = Join-Path $root "build\d3d11-calls-smoke\$([guid]::NewGuid())"
    New-Item -ItemType Directory -Path $run -Force | Out-Null
    Copy-Item -LiteralPath $exe, "$bin\d3d11.dll", "$bin\load_probe.dll" -Destination $run
    $previousDataDir = $env:TESSERA_DATA_DIR
    try {
        foreach ($enabled in @(0, 1)) {
            $env:TESSERA_DATA_DIR = Join-Path $run "data-$enabled"
            Set-Content "$run\tessera.ini" -Value "[tessera]`nnext=load_probe.dll`nd3d11_call_diagnostics=$enabled" -Encoding ascii
            & "$run\d3d11_smoke.exe" --d3d11-calls | Write-Host
            Check "D3D11 calls forward with diagnostics $enabled" ($LASTEXITCODE -eq 0)
            $session = Get-ChildItem "$env:TESSERA_DATA_DIR\sessions\*.tsess" | Select-Object -First 1
            $csv = & $layer calls $session.FullName
            Check "D3D11 call CSV export" ($LASTEXITCODE -eq 0)
            $calls = @($csv | ConvertFrom-Csv | Where-Object { $_.call -in @("Map", "GetData", "UpdateSubresource", "Flush", "UpdateSubresource1", "Flush1") })
            if ($enabled -eq 0) {
                Check "diagnostics off reports no context calls" ($calls.Count -eq 0)
            } else {
                Check "all six slow context calls logged" (@($calls.call | Sort-Object -Unique).Count -eq 6)
                Check "context call intervals and thread IDs" ($calls.Count -gt 0 -and
                    @($calls | Where-Object { [uint64]$_.start_qpc -ge [uint64]$_.end_qpc -or
                        [int]$_.duration_us -lt 1000 -or [uint32]$_.thread_id -eq 0 }).Count -eq 0)
                Check "Map mode and GetData flags retained" (
                    @($calls | Where-Object { $_.call -eq 'Map' -and $_.argument -eq '1' -and $_.hresult -eq '0x00000000' }).Count -eq 1 -and
                    @($calls | Where-Object { $_.call -eq 'GetData' -and $_.flags -eq '0x00000001' }).Count -gt 0)
            }
        }
    } finally { $env:TESSERA_DATA_DIR = $previousDataDir }
}

if ($PacingOnly -or $D3D11CallsOnly) {
    if ($PacingOnly) { CheckPacing }
    if ($D3D11CallsOnly) { CheckD3D11Calls }
    if ($fails) { exit 1 }
    exit 0
}

$run = Join-Path $root "build\smoke-data\$([guid]::NewGuid())"
New-Item -ItemType Directory -Path $run -Force | Out-Null
$bin = Split-Path $exe
Copy-Item -LiteralPath $exe, "$bin\d3d11.dll", "$bin\load_probe.dll", "$bin\nvapi_probe.dll" -Destination $run
$exe = Join-Path $run "d3d11_smoke.exe"
$data = Join-Path $run "data"
$env:TESSERA_DATA_DIR = $data

& $exe --exports | Write-Host
Check "proxy exports work by name and ordinal on cold and cached calls" ($LASTEXITCODE -eq 0)

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
$ini = Join-Path (Split-Path $exe) "tessera.ini"
Set-Content -Path $ini -Value "[tessera]`nresidency=1`nfps_limit=60`nvram_budget_mb=2048`nflip_model=1`nmax_frame_latency=1" -Encoding ascii
$env:TESSERA_SMOKE_CAP_MB = "2048"
try {
    & $exe | Write-Host
    Check "limited run" ($LASTEXITCODE -eq 0)
    # M1 before any device: factories through every route are the layer's; a D3D12 device turns the cap off.
    & $exe --imports | Write-Host
    Check "DXGI factories wrapped, capped before the device, left alone for D3D12" ($LASTEXITCODE -eq 0)
    & $exe --verify "$data\sessions" 60 | Write-Host
    Check "limiter holds 60 fps, budget capped, flip model" ($LASTEXITCODE -eq 0)
    Check "flip conversion logged" ((Select-String -Path $log -Pattern "converted to flip model" -Quiet))
    Check "frame latency through the waitable object (P3)" ((Select-String -Path $log -Pattern "latency 1 through the waitable object: ok" -Quiet))
    # S6: two shaders stored once each across both runs (header 24 + 2 index entries of 16).
    $idx = Get-ChildItem "$data\shaders\d3d11_smoke.exe-*.tshi"
    Check "shader store has 2 shaders" ($idx.Count -eq 1 -and $idx[0].Length -eq 56)
    # S5: the warm-up compiles both, then a second run finds nothing left to do.
    $warm = Join-Path $root "$native\src\warm\$Config\tessera_warm.exe"
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

CheckPacing
CheckD3D11Calls

# R1: a released big render target comes back on the next matching create; held ones and initial data never.
Set-Content -Path $ini -Value "[tessera]`nrt_recycle=1" -Encoding ascii
try {
    & $exe --recycle | Write-Host
    Check "released render target reused, held ones and initial data left alone (R1)" ($LASTEXITCODE -eq 0)
} finally { Remove-Item $ini }

# U1 recognizes PE64 Unreal executable layouts.
if (-not $X86) {
    try {
        & $exe --uecvars | Write-Host
        Check "console variables set, checked and restored (U1)" ($LASTEXITCODE -eq 0)
        Check "bad console variable slots logged" ((Select-String -Path $log -Pattern "r.Test.Bad: object at" -Quiet) -and
            (Select-String -Path $log -Pattern "r.Test.ReadOnly: slot" -Quiet) -and (Select-String -Path $log -Pattern "r.Test.Garbage: bad value" -Quiet) -and (Select-String -Path $log -Pattern "r.Test.Float moved" -Quiet))
        & $exe --uecvars stale | Write-Host
        Check "console variables left alone on another build (U1)" ($LASTEXITCODE -eq 0 -and
            (Select-String -Path $log -Pattern "not the scanned build" -Quiet))
    } finally { if (Test-Path $ini) { Remove-Item $ini } }
}

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
& $exe --imports | Write-Host
Check "DXGI imports untouched in safe mode" ($LASTEXITCODE -eq 3)
$after = (Get-ChildItem "$data\sessions\*.tsess").Count
Check "no session in safe mode" ($after -eq $before)
Check "safe mode logged" ((Select-String -Path "$data\logs\d3d11_smoke.exe.log" -Pattern "safe mode" -Quiet))

if ($fails) { "$fails check(s) failed"; exit 1 } else { "all checks passed" }
