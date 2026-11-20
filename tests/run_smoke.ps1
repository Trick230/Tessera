# SPDX-License-Identifier: GPL-3.0-or-later
# Layer smoke test: clean run + session check, then safe mode after two crashes.
#   powershell -File tests/run_smoke.ps1 [-Config Release]
param([string]$Config = "Release")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\tests\d3d11_smoke\$Config\d3d11_smoke.exe"
$data = Join-Path $root "build\smoke-data"
if (Test-Path $data) { Remove-Item -Recurse -Force $data }
$env:TESSERA_DATA_DIR = $data
$fails = 0
function Check($name, $ok) {
    if ($ok) { "PASS $name" } else { "FAIL $name"; $script:fails++ }
}

& $exe | Write-Host
Check "clean run" ($LASTEXITCODE -eq 0)
& $exe --verify "$data\sessions" | Write-Host
Check "session contents" ($LASTEXITCODE -eq 0)
$counter = @(Get-ChildItem "$data\safemode\d3d11_smoke.exe-*.count")
Check "one counter keyed by path" ($counter.Count -eq 1)
$counter = $counter[0].FullName
Check "counter cleared on clean exit" ((Get-Content $counter) -eq "0")

$log = "$data\logs\d3d11_smoke.exe.log"
Check "process tweaks logged" ((Select-String -Path $log -Pattern "process:" -Quiet))

# Frame limiter (P1) from tessera.ini next to the layer.
$ini = Join-Path (Split-Path $exe) "tessera.ini"
Set-Content -Path $ini -Value "[tessera]`nfps_limit=60" -Encoding ascii
try {
    & $exe | Write-Host
    Check "limited run" ($LASTEXITCODE -eq 0)
    & $exe --verify "$data\sessions" 60 | Write-Host
    Check "limiter holds 60 fps" ($LASTEXITCODE -eq 0)
} finally { Remove-Item $ini }

& $exe --crash | Out-Null
& $exe --crash | Out-Null
Check "counter after two crashes" ((Get-Content $counter) -eq "2")
# The app computes the same path key (crates/core/src/session.rs).
$layer = Join-Path $root "target\release\examples\layer.exe"
Check "app reads the same counter" ((& $layer crashes $exe) -eq "2")
$before = (Get-ChildItem "$data\sessions\*.tsess").Count
& $exe | Write-Host
Check "game still runs in safe mode" ($LASTEXITCODE -eq 0)
$after = (Get-ChildItem "$data\sessions\*.tsess").Count
Check "no session in safe mode" ($after -eq $before)
Check "safe mode logged" ((Select-String -Path "$data\logs\d3d11_smoke.exe.log" -Pattern "safe mode" -Quiet))

if ($fails) { "$fails check(s) failed"; exit 1 } else { "all checks passed" }
