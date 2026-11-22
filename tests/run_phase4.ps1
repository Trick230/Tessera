# SPDX-License-Identifier: GPL-3.0-or-later
# Run after building the d3d11_smoke target.
param([string]$Config = "Release")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$bin = Join-Path $root "build\tests\d3d11_smoke\$Config"
$case = Join-Path $root ("build\phase4-smoke\" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $case -Force | Out-Null
Copy-Item -LiteralPath "$bin\d3d11_smoke.exe", "$bin\d3d11.dll" -Destination $case
$exe = Join-Path $case "d3d11_smoke.exe"
$env:TESSERA_DATA_DIR = Join-Path $case "data"
$log = Join-Path $env:TESSERA_DATA_DIR "logs\d3d11_smoke.exe.log"

function Set-Options([string[]]$Keys) {
    Set-Content -LiteralPath "$case\tessera.ini" -Value (@("[tessera]") + $Keys) -Encoding ascii
}

function Test-Log([string]$Pattern) {
    return (Select-String -LiteralPath $log -Pattern $Pattern -Quiet)
}

Set-Options @("present_cpu_sets=1")
& $exe | Write-Host
if ($LASTEXITCODE) { throw "Present CPU sets smoke run failed" }
"PASS Present CPU sets smoke run (assignment depends on CPU topology)"

foreach ($n in 0, 1) {
    Set-Options @("texture_dedup=$n")
    $env:TESSERA_SMOKE_DEDUP = if ($n) { "1" } else { $null }
    & $exe --dedup | Write-Host
    if ($LASTEXITCODE) { throw "Texture dedup test failed with texture_dedup=$n" }
}
"PASS texture dedup"

$env:TESSERA_SMOKE_ASSET = Join-Path $case "stream.asset"
$asset = New-Object byte[] (34 * 65536)
for ($i = 1; $i -lt 34; ++$i) { $asset[$i * 65536] = 0x53 }
[IO.File]::WriteAllBytes($env:TESSERA_SMOKE_ASSET, $asset)
Set-Options @("stream_io=1", "mip_prediction=1")
$env:TESSERA_SMOKE_PREDICT = "1"
& $exe --stream | Write-Host
if ($LASTEXITCODE -or -not (Test-Log "saved learning trace")) { throw "Stream learning test failed" }

Set-Options @("stream_io=2", "mip_prediction=1")
$env:TESSERA_SMOKE_PREDICT = "2"
& $exe --stream | Write-Host
if ($LASTEXITCODE -or -not (Test-Log "2048 KB from the startup list") -or -not (Test-Log "64 KB from a material match")) {
    throw "Startup and material prefetch test failed"
}
(Get-Item -LiteralPath $env:TESSERA_SMOKE_ASSET).LastWriteTimeUtc = [DateTime]::UtcNow.AddHours(-1)
& $exe --stream | Write-Host
if ($LASTEXITCODE -or -not (Test-Log "0 KB from the startup list") -or (Test-Log "from a material match")) {
    throw "Modified file was prefetched"
}
"PASS I/O learning, startup and material prefetch, and cache invalidation"

Set-Options @("mip_residency=1", "mip_force_evict=1", "mip_streaming=1", "mip_prediction=1")
$env:TESSERA_SMOKE_MIP_STREAM = "1"
$env:TESSERA_SMOKE_MIP_PREDICT = "1"
$tiled = & $exe --tiled
$tiled | Write-Host
if ($LASTEXITCODE) { throw "Tier 2 render and device replacement test failed" }
if ("$tiled" -match "skipped") {
    "SKIP mip streaming and prediction (needs tiled resources Tier 2)"
} else {
    if (-not (Test-Log "[1-9]\d* spare pool hits, [1-9]\d* partial upload batches") -or
        -not (Test-Log "[1-9]\d* predictive restores") -or -not (Test-Log "new device takes over")) {
        throw "Mip streaming, prediction or device replacement was not exercised"
    }
    "PASS spare tiles, split uploads, predictive restore and device replacement"
}
