# SPDX-License-Identifier: GPL-3.0-or-later
# Run after building tessera_texcomp and tessera_test_feedback in build/quality.
param([string]$Config = "Release", [string]$Fxc)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$bin = Join-Path $root "build\quality\src\texcomp\$Config"
$case = Join-Path $root ("build\quality-smoke\" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $case -Force | Out-Null

if (-not $Fxc) {
    $sdk = Join-Path ([Environment]::GetEnvironmentVariable("ProgramFiles(x86)")) "Windows Kits\10\bin"
    $Fxc = Get-ChildItem -LiteralPath $sdk -Directory |
        Sort-Object -Property { try { [version]$_.Name } catch { [version]"0.0" } } -Descending |
        ForEach-Object { Join-Path $_.FullName "x64\fxc.exe" } |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    if (-not $Fxc) { throw "Could not find the Windows SDK fxc.exe" }
}

$compressor = Join-Path $bin "tessera_texcomp.exe"
& $compressor --selftest
if ($LASTEXITCODE) { throw "Texture compression selftest failed" }

# A single-mip sRGB DDS with opaque white pixels.
$dds = New-Object byte[] (148 + 16 * 16 * 4)
$fields = [ordered]@{ 0 = 0x20534444; 4 = 124; 8 = 0x100f; 12 = 16; 16 = 16; 20 = 64
                       76 = 32; 80 = 4; 84 = 0x30315844; 108 = 0x1000; 128 = 29; 132 = 3; 140 = 1 }
foreach ($entry in $fields.GetEnumerator()) {
    [Array]::Copy([BitConverter]::GetBytes([uint32]$entry.Value), 0, $dds, [int]$entry.Key, 4)
}
for ($i = 148; $i -lt $dds.Length; ++$i) { $dds[$i] = 255 }
$inputDds = Join-Path $case "source.dds"
$outputDds = Join-Path $case "compressed.dds"
[IO.File]::WriteAllBytes($inputDds, $dds)
& $compressor $inputDds $outputDds
if ($LASTEXITCODE) { throw "DDS recompression failed" }
$encoded = [IO.File]::ReadAllBytes($outputDds)
if ($encoded.Length -ge $dds.Length -or [BitConverter]::ToUInt32($encoded, 128) -ne 99) {
    throw "Unexpected DDS format or size"
}
$dds[151] = 127
[IO.File]::WriteAllBytes($inputDds, $dds)
& $compressor $inputDds "$case\rejected.dds"
if ($LASTEXITCODE -ne 2 -or (Test-Path -LiteralPath "$case\rejected.dds")) {
    throw "Translucent input should leave no output file"
}
"PASS sRGB DDS output and rejection without output"

$in = Join-Path $case "original.dxbc"
$out = Join-Path $case "instrumented.dxbc"
$existing = Join-Path $case "existing-uav.dxbc"
$shader = Join-Path $PSScriptRoot "quality\sample.hlsl"
& $Fxc /nologo /T ps_5_0 /E main /O3 /Fo $in $shader
if ($LASTEXITCODE) { throw "Test pixel shader failed to compile" }
& $Fxc /nologo /T ps_5_0 /E main /O3 /D EXISTING_UAV=1 /Fo $existing $shader
if ($LASTEXITCODE) { throw "Existing UAV test shader failed to compile" }
Push-Location $root
try {
    & "$env:USERPROFILE\.cargo\bin\cargo.exe" build -q -p tessera-core --example instrument
    if ($LASTEXITCODE) { throw "DXBC instrumenter build failed" }
} finally { Pop-Location }
$instrument = Join-Path $root "target\debug\examples\instrument.exe"
& $instrument $existing "$case\rejected.dxbc"
if ($LASTEXITCODE -ne 2 -or (Test-Path -LiteralPath "$case\rejected.dxbc")) {
    throw "Shader with an existing UAV should leave no output file"
}
& $instrument $in $out
if ($LASTEXITCODE) { throw "DXBC instrumentation failed" }
& "$bin\tessera_test_feedback.exe" $in $out
if ($LASTEXITCODE) { throw "D3D11 mip feedback test failed" }
