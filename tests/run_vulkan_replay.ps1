# SPDX-License-Identifier: GPL-3.0-or-later
# Check interruption recovery with an existing Tessera Vulkan recording.
param(
    [Parameter(Mandatory)][string]$Recording,
    [string]$Helper = 'build/vulkan-telemetry/src/vulkan_telemetry/Release/tessera_vulkan_replay.exe'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$recordPath = (Resolve-Path -LiteralPath $Recording).Path
$helperPath = (Resolve-Path -LiteralPath $Helper).Path
$run = Join-Path $repo "build/vulkan-replay-recovery/$([guid]::NewGuid())"
New-Item -ItemType Directory -Path $run | Out-Null
$bytes = [IO.File]::ReadAllBytes($recordPath)
if ($bytes.Length -lt 40) { throw 'Recording has no pipeline' }
$first = 40 + [BitConverter]::ToUInt32($bytes, 36)
if ($first -gt $bytes.Length) { throw 'First pipeline is incomplete' }
# A complete first pipeline followed by an interrupted record header.
$interrupted = [byte[]]::new($first + 1)
[Array]::Copy($bytes, $interrupted, $first)
$interrupted[$first] = 1
[IO.File]::WriteAllBytes("$run/interrupted.tvc", $interrupted)
$checkpoint = "$run/checkpoint.bin"
function Replay($name, $inputPath, $outputPath, $seedPath, $expected) {
    $arguments = @('"' + $inputPath + '"', '"' + $outputPath + '"', '--checkpoint', '"' + $seedPath + '"')
    $process = Start-Process -FilePath $helperPath -ArgumentList $arguments -WindowStyle Hidden -Wait -PassThru `
        -RedirectStandardOutput "$run/$name.out" -RedirectStandardError "$run/$name.err"
    if ($process.ExitCode -ne $expected) { throw "$name exited with $($process.ExitCode), expected $expected; see $run" }
}
Replay interrupted "$run/interrupted.tvc" "$run/incomplete.bin" $checkpoint 1
if (Test-Path "$run/incomplete.bin") { throw 'Interrupted replay promoted a prepared cache' }
if (-not (Test-Path $checkpoint)) { throw 'Interrupted replay lost its checkpoint' }
Replay resumed $recordPath "$run/complete.bin" $checkpoint 0
if (-not (Test-Path "$run/complete.bin")) { throw 'Complete replay did not save its cache' }
# A corrupt seed must be ignored, never treated as a prepared result.
[IO.File]::WriteAllBytes("$run/corrupt.bin", [byte[]]@(1, 2, 3))
Replay corrupt $recordPath "$run/from-corrupt.bin" "$run/corrupt.bin" 0
$foreign = [IO.File]::ReadAllBytes($checkpoint)
$foreign[16] = $foreign[16] -bxor 1
[IO.File]::WriteAllBytes("$run/foreign.bin", $foreign)
Replay foreign $recordPath "$run/from-foreign.bin" "$run/foreign.bin" 0
# Even a different spelling of the input path cannot be used as a checkpoint.
$before = (Get-FileHash -LiteralPath $recordPath).Hash
$alias = Join-Path (Split-Path $recordPath -Parent) ('.\' + (Split-Path $recordPath -Leaf))
Replay alias $recordPath "$run/alias.bin" $alias 1
if ((Get-FileHash -LiteralPath $recordPath).Hash -ne $before) { throw 'Checkpoint modified the recording' }
# A pipeline the driver cannot build must not discard the ones that were built.
if ([Text.Encoding]::ASCII.GetString($bytes, 0, 7) -eq "TSRVKC3") {
    $bad = [byte[]]::new($bytes.Length + 12)
    [Array]::Copy($bytes, $bad, $bytes.Length)
    [BitConverter]::GetBytes([uint32]8).CopyTo($bad, $bytes.Length)
    [BitConverter]::GetBytes([uint32]99).CopyTo($bad, $bytes.Length + 4)
    [IO.File]::WriteAllBytes("$run/partial.tvc", $bad)
    Replay partial "$run/partial.tvc" "$run/partial.bin" "$run/partial-checkpoint.bin" 0
    if (-not (Test-Path "$run/partial.bin")) { throw "Partial replay did not save the pipelines it built" }
}
# The device state recorded by the layer is replayed in place of "every available feature".
$fixture = Join-Path (Split-Path $helperPath -Parent) "tessera_vulkan_device_fixture.exe"
if ((Test-Path $fixture) -and [Text.Encoding]::ASCII.GetString($bytes, 0, 7) -eq "TSRVKC3") {
    $env:VK_LAYER_PATH = Split-Path $helperPath -Parent
    $env:TESSERA_VULKAN_PIPELINE_CAPTURE = "$run/device.tvc"
    try { & $fixture } finally { Remove-Item Env:TESSERA_VULKAN_PIPELINE_CAPTURE, Env:VK_LAYER_PATH }
    if ($LASTEXITCODE) { throw "Vulkan device fixture failed ($LASTEXITCODE)" }
    $device = [IO.File]::ReadAllBytes("$run/device.tvc")
    if ($device.Length -lt 44 -or [BitConverter]::ToUInt32($device, 40) -ne 2) { throw "Capture did not start with the device state" }
    if ([Convert]::ToBase64String($device, 12, 24) -ne [Convert]::ToBase64String($bytes, 12, 24)) {
        "SKIP recorded device state: the recording comes from another device or driver"
    } else {
        $spliced = [byte[]]::new($device.Length + $bytes.Length - 36)
        # The recording's header (its API version), then the fixture's device state, then its pipelines.
        [Array]::Copy($bytes, $spliced, 36)
        [Array]::Copy($device, 36, $spliced, 36, $device.Length - 36)
        [Array]::Copy($bytes, 36, $spliced, $device.Length, $bytes.Length - 36)
        [IO.File]::WriteAllBytes("$run/with-device.tvc", $spliced)
        Replay with-device "$run/with-device.tvc" "$run/with-device.bin" "$run/with-device-checkpoint.bin" 0
        if (-not (Select-String -Quiet -LiteralPath "$run/with-device.err" -SimpleMatch -Pattern "recorded Vulkan device state (0 extension")) {
            throw "Replay did not use the recorded device state without the swapchain extension; see $run/with-device.err"
        }
        "PASS recorded Vulkan device state"
    }
}
"PASS Vulkan replay interruption, resume, corrupt/foreign seeds and path alias ($run)"
