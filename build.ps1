# SPDX-License-Identifier: GPL-3.0-or-later
# Builds everything: Rust app (target\release\tessera.exe) and the C++ layer,
# copied next to the app as tessera_d3d11.dll (the payload the app installs).
#   powershell -File build.ps1 [-Test] [-Quality]
param([switch]$Test, [switch]$Quality)
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
    if ($Quality) {
        & $cmake -S . -B build/quality -G "Visual Studio 17 2022" -A x64 -DTESSERA_BUILD_QUALITY=ON
        if ($LASTEXITCODE) { throw "quality configure failed" }
        & $cmake --build build/quality --config Release --target tessera_texcomp
        if ($LASTEXITCODE) { throw "quality tool build failed" }
        Copy-Item build\quality\src\texcomp\Release\tessera_texcomp.exe,
                  build\quality\src\texcomp\Release\DirectXTex-LICENSE.txt,
                  build\quality\src\texcomp\Release\FLIP-LICENSE.txt target\release -Force
    }
    if ($Test) {
        cargo test -q -p tessera-core
        if ($LASTEXITCODE) { throw "core tests failed" }
        cargo build -q --release -p tessera-core --example layer  # used by the smoke test
        if ($LASTEXITCODE) { throw "layer example build failed" }
        powershell -NoProfile -ExecutionPolicy Bypass -File tests\run_smoke.ps1
        if ($LASTEXITCODE) { throw "smoke test failed" }
    }
    "built: target\release\tessera.exe + tessera_d3d11.dll"
} finally { Pop-Location }
