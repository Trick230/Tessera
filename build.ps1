# SPDX-License-Identifier: GPL-3.0-or-later
# Builds everything: Rust app (target\release\tessera.exe) and the C++ layer,
# copied next to the app as tessera_d3d11.dll (the payload the app installs).
#   powershell -File build.ps1 [-Test]
param([switch]$Test)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$cmake = "D:\BuildTools\VS2022\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
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
    if ($Test) {
        cargo test -q -p tessera-core
        if ($LASTEXITCODE) { throw "core tests failed" }
        powershell -NoProfile -ExecutionPolicy Bypass -File tests\run_smoke.ps1
        if ($LASTEXITCODE) { throw "smoke test failed" }
    }
    "built: target\release\tessera.exe + tessera_d3d11.dll"
} finally { Pop-Location }
