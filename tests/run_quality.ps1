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
foreach ($mode in "EXPLICIT_LOD", "EXPLICIT_LOD_REGISTER") {
    $explicit = Join-Path $case "$mode.dxbc"
    $instrumented = Join-Path $case "$mode-instrumented.dxbc"
    & $Fxc /nologo /T ps_5_0 /E main /O3 /D "$mode=1" /Fo $explicit $shader
    if ($LASTEXITCODE) { throw "Explicit LOD test shader failed to compile" }
    & $instrument $explicit $instrumented
    if ($LASTEXITCODE) { throw "Explicit LOD instrumentation failed" }
    & "$bin\tessera_test_feedback.exe" $explicit $instrumented
    if ($LASTEXITCODE) { throw "Explicit LOD changed the draw output or reported the wrong mip" }
}

# Exercise both normal layer paths in a clean local installation.
Copy-Item -LiteralPath "$bin\tessera_test_feedback.exe" -Destination $case
Copy-Item -LiteralPath "$root\build\src\dll\$Config\tessera_d3d11.dll" -Destination "$case\d3d11.dll"
$env:TESSERA_DATA_DIR = Join-Path $case "data"
$test = Join-Path $case "tessera_test_feedback.exe"
$layerLog = Join-Path $env:TESSERA_DATA_DIR "logs\tessera_test_feedback.exe.log"
function Read-Records([uint16]$Kind) {
    $log = Get-ChildItem -LiteralPath "$env:TESSERA_DATA_DIR\sessions" -Filter "*.tsess" |
        Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    $bytes = [IO.File]::ReadAllBytes($log.FullName)
    for ($p = 568; $p + 40 -le $bytes.Length; $p += 40) {
        if ([BitConverter]::ToUInt16($bytes, $p + 8) -eq $Kind) {
            [PSCustomObject]@{
                Flags = [BitConverter]::ToUInt32($bytes, $p + 12)
                A = [BitConverter]::ToUInt32($bytes, $p + 16)
                B = [BitConverter]::ToUInt32($bytes, $p + 20)
                Bytes = [BitConverter]::ToUInt64($bytes, $p + 32)
            }
        }
    }
}

Set-Content -LiteralPath "$case\tessera.ini" -Value @("[tessera]", "texture_quality=1") -Encoding ascii
foreach ($mode in "--textures", "--textures-hdr") {
    & $test $mode $in
    if ($LASTEXITCODE) { throw "Texture learning test failed in $mode" }
}
& $instrument --game $test
if ($LASTEXITCODE) { throw "Failed to prepare captured shader" }
$raw = Get-ChildItem -LiteralPath "$env:TESSERA_DATA_DIR\quality" -Directory -Filter "*.raw" | Select-Object -First 1
$packed = [IO.Path]::ChangeExtension($raw.FullName, ".packed")
& $compressor --batch $raw.FullName $packed
if ($LASTEXITCODE -or @(Get-ChildItem -LiteralPath $packed -Filter "*.tbc").Count -ne 2) {
    throw "Texture cache did not prepare both formats"
}

Set-Content -LiteralPath "$case\tessera.ini" -Value @("[tessera]", "mip_feedback=1", "texture_quality=2") -Encoding ascii
foreach ($mode in "--textures", "--textures-hdr", "--textures-changed") {
    & $test $mode $in
    if ($LASTEXITCODE) { throw "Texture replay failed in $mode" }
    $replaced = @(Read-Records 2 | Where-Object { $_.Flags -band 0x04000000 })
    $want = [int]($mode -ne "--textures-changed")
    if ($replaced.Count -ne $want -or ($want -and $replaced[0].Bytes -ne 368)) {
        throw "Unexpected texture substitution in $mode"
    }
    $feedback = @(Read-Records 12)
    if ($feedback.Count -ne 1 -or $feedback[0].A -ne 3 -or $feedback[0].B -ne 2 -or !$feedback[0].Bytes) {
        throw "Missing texture feedback in $mode"
    }
    if (!(Select-String -LiteralPath $layerLog -Pattern "mip feedback: variants=[1-9]\d* .* submitted=1 .* samples=1 " -Quiet)) {
        throw "Missing mip feedback diagnostics in $mode"
    }
}
"PASS runtime M6 and M9, including changed texture bypass"
& $test --layer-occupied $in
if ($LASTEXITCODE -or @(Read-Records 12).Count -ne 0) {
    throw "Feedback touched a draw whose UAV slot was occupied"
}
if (!(Select-String -LiteralPath $layerLog -Pattern "output_busy=[1-9]\d* submitted=0 .* samples=0 " -Quiet)) {
    throw "Missing occupied UAV diagnostics"
}
"PASS occupied UAV left alone"

& $test --layer-indirect $in
$feedback = @(Read-Records 12)
if ($LASTEXITCODE -or $feedback.Count -ne 1 -or $feedback[0].A -ne 3 -or $feedback[0].B -ne 2 -or
    !(Select-String -LiteralPath $layerLog -Pattern "bind_frames=[1-9]\d*" -Quiet)) {
    throw "Feedback missed an indirect draw in the next frame of the sample window"
}
"PASS early-cached indirect draw and sampling across frames"
