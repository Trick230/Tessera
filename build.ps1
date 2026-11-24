# SPDX-License-Identifier: GPL-3.0-or-later
# Builds everything: Rust app (target\release\tessera.exe) and the C++ layer,
# copied next to the app in x64 and x86 variants.
#   powershell -File build.ps1 [-Test] [-Quality] [-Vulkan]
param([switch]$Test, [switch]$Quality, [switch]$Vulkan)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$cmake = "D:\BuildTools\VS2022\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path $cmake)) { $cmake = "cmake" }  # any CMake on PATH
$env:PATH = "$env:USERPROFILE\.cargo\bin;$env:PATH"

Push-Location $root
try {
    cargo build --release -p tessera-app
    if ($LASTEXITCODE) { throw "cargo build failed" }
    if (-not (Test-Path build\CMakeCache.txt)) {
        & $cmake -S . -B build -G "Visual Studio 17 2022" -A x64
        if ($LASTEXITCODE) { throw "cmake configure failed" }
    }
    & $cmake --build build --config Release
    if ($LASTEXITCODE) { throw "cmake build failed" }
    Copy-Item build\src\dll\Release\tessera_d3d11.dll target\release\tessera_d3d11.dll -Force
    Copy-Item build\src\warm\Release\tessera_warm.exe target\release\tessera_warm.exe -Force
    Copy-Item build\src\d3d12_capture\Release\tessera_d3d12_capture.dll target\release\tessera_d3d12_capture.dll -Force
    if (-not (Test-Path build\win32\CMakeCache.txt)) {
        & $cmake -S . -B build/win32 -G "Visual Studio 17 2022" -A Win32
        if ($LASTEXITCODE) { throw "Win32 cmake configure failed" }
    }
    & $cmake --build build/win32 --config Release
    if ($LASTEXITCODE) { throw "Win32 cmake build failed" }
    Copy-Item build\win32\src\dll\Release\tessera_d3d11.dll target\release\tessera_d3d11_x86.dll -Force
    Copy-Item build\win32\src\warm\Release\tessera_warm.exe target\release\tessera_warm_x86.exe -Force
    if ($Quality) {
        & $cmake -S . -B build/quality -G "Visual Studio 17 2022" -A x64 -DTESSERA_BUILD_QUALITY=ON
        if ($LASTEXITCODE) { throw "quality configure failed" }
        & $cmake --build build/quality --config Release --target tessera_texcomp
        if ($LASTEXITCODE) { throw "quality tool build failed" }
        Copy-Item build\quality\src\texcomp\Release\tessera_texcomp.exe,
                  build\quality\src\texcomp\Release\DirectXTex-LICENSE.txt,
                  build\quality\src\texcomp\Release\FLIP-LICENSE.txt target\release -Force
    }
    if ($Vulkan) {
        & $cmake -S . -B build/vulkan-telemetry -G "Visual Studio 17 2022" -A x64 -DTESSERA_BUILD_VULKAN_TELEMETRY=ON
        if ($LASTEXITCODE) { throw "Vulkan configure failed" }
        & $cmake --build build/vulkan-telemetry --config Release --target tessera_vulkan_telemetry tessera_vulkan_replay
        if ($LASTEXITCODE) { throw "Vulkan tools build failed" }
        $vk = "build/vulkan-telemetry/src/vulkan_telemetry/Release"
        Copy-Item "$vk/tessera_vulkan_telemetry.dll","$vk/tessera_vulkan_replay.exe",
                  "$vk/TesseraTelemetry.json" target/release -Force
    }
    if ($Test) {
        cargo test -q -p tessera-core
        if ($LASTEXITCODE) { throw "core tests failed" }
        cargo build -q --release -p tessera-core --example layer  # used by the smoke test
        if ($LASTEXITCODE) { throw "layer example build failed" }
        powershell -NoProfile -ExecutionPolicy Bypass -File tests\run_smoke.ps1
        if ($LASTEXITCODE) { throw "smoke test failed" }
        powershell -NoProfile -ExecutionPolicy Bypass -File tests\run_smoke.ps1 -X86
        if ($LASTEXITCODE) { throw "Win32 smoke test failed" }
    }
    "built: target\release\tessera.exe + x64 and x86 D3D11 payloads"
} finally { Pop-Location }
