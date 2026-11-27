# SPDX-License-Identifier: GPL-3.0-or-later
# Builds everything with the toolchain in .toolchain\<host> (tools/toolchain.ps1): Rust app and
# the C++ layer (clang-cl, ThinLTO), copied next to the app in x64 and x86 variants, in
# build\<host>\cargo\release\.
#   powershell -File build.ps1 [-Test] [-Quality] [-Vulkan]
param([switch]$Test, [switch]$Quality, [switch]$Vulkan)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
# Environment changed by tools/env.ps1; restored at the end so the calling session is left as it was.
$envNames = "PATH", "RUSTUP_HOME", "CARGO_HOME", "INCLUDE", "LIB", "LIBPATH", "VSINSTALLDIR", "VCINSTALLDIR",
            "CC", "CXX", "AR", "CFLAGS", "CXXFLAGS", "CARGO_TARGET_DIR", "CARGO_ENCODED_RUSTFLAGS",
            "CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_LINKER"
$savedEnv = @{}
foreach ($n in $envNames) { $savedEnv[$n] = [Environment]::GetEnvironmentVariable($n) }

# Configures (once) and builds a native dir (all targets, or $targets); extra arguments go to
# the configure step.
function Build-Native($dir, $arch, $what, $targets) {
    if (-not (Test-Path "$dir\CMakeCache.txt")) {
        & $cmake -S . -B $dir -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_MAKE_PROGRAM=$ninja" `
            "-DCMAKE_TOOLCHAIN_FILE=$root\cmake\clang-cl.cmake" "-DTESSERA_TOOLCHAIN=$tc" "-DTESSERA_ARCH=$arch" @args
        if ($LASTEXITCODE) { throw "$what cmake configure failed" }
    }
    $only = if ($targets) { @("--target") + $targets } else { @() }
    & $cmake --build $dir @only
    if ($LASTEXITCODE) { throw "$what cmake build failed" }
}

Push-Location $root
try {
    . "$root\tools\env.ps1"
    $out = $cargoOut; $x64Dir = "$buildRoot\x64"; $x86Dir = "$buildRoot\x86"
    cargo build --release -p tessera-app
    if ($LASTEXITCODE) { throw "cargo build failed" }
    Build-Native $x64Dir x64 "x64"
    Copy-Item "$x64Dir\src\dll\tessera_d3d11.dll" "$out\tessera_d3d11.dll" -Force
    Copy-Item "$x64Dir\src\dll\tessera_dxgi.dll" "$out\tessera_dxgi.dll" -Force
    Copy-Item "$x64Dir\src\dll\tessera_vulkan.dll" "$out\tessera_vulkan.dll" -Force
    Copy-Item "$x64Dir\src\warm\tessera_warm.exe" "$out\tessera_warm.exe" -Force
    Copy-Item "$x64Dir\src\d3d12_capture\tessera_d3d12_capture.dll" "$out\tessera_d3d12_capture.dll" -Force
    Build-Native $x86Dir x86 "x86"
    Copy-Item "$x86Dir\src\dll\tessera_d3d11.dll" "$out\tessera_d3d11_x86.dll" -Force
    Copy-Item "$x86Dir\src\dll\tessera_dxgi.dll" "$out\tessera_dxgi_x86.dll" -Force
    Copy-Item "$x86Dir\src\dll\tessera_vulkan.dll" "$out\tessera_vulkan_x86.dll" -Force
    Copy-Item "$x86Dir\src\warm\tessera_warm.exe" "$out\tessera_warm_x86.exe" -Force
    if ($Quality) {
        $q = "$buildRoot\quality"
        Build-Native $q x64 "quality" @("tessera_texcomp", "tessera_test_feedback") "-DTESSERA_BUILD_QUALITY=ON"
        $qt = "$q\src\texcomp"
        Copy-Item "$qt\tessera_texcomp.exe", "$qt\DirectXTex-LICENSE.txt", "$qt\FLIP-LICENSE.txt" $out -Force
    }
    if ($Vulkan) {
        Build-Native "$buildRoot\vulkan-x64" x64 "Vulkan" @("tessera_vulkan_telemetry", "tessera_vulkan_replay",
            "tessera_vulkan_device_fixture", "tessera_vulkan_present") "-DTESSERA_BUILD_VULKAN_TELEMETRY=ON"
        Build-Native "$buildRoot\vulkan-x86" x86 "Vulkan x86" @("tessera_vulkan_present") "-DTESSERA_BUILD_VULKAN_TELEMETRY=ON"
        $vk = "$buildRoot\vulkan-x64\src\vulkan_telemetry"
        Copy-Item "$vk\tessera_vulkan_telemetry.dll", "$vk\tessera_vulkan_replay.exe", "$vk\TesseraTelemetry.json" $out -Force
    }
    if ($Test) {
        cargo test -q -p tessera-core
        if ($LASTEXITCODE) { throw "core tests failed" }
        cargo build -q --release -p tessera-core --example layer  # used by the smoke test
        if ($LASTEXITCODE) { throw "layer example build failed" }
        powershell -NoProfile -ExecutionPolicy Bypass -File tests\run_smoke.ps1
        if ($LASTEXITCODE) { throw "smoke test failed" }
        powershell -NoProfile -ExecutionPolicy Bypass -File tests\run_smoke.ps1 -X86
        if ($LASTEXITCODE) { throw "x86 smoke test failed" }
    }
    "built: $out\tessera.exe + x64 and x86 D3D11 payloads"
} finally {
    Pop-Location
    foreach ($n in $envNames) { [Environment]::SetEnvironmentVariable($n, $savedEnv[$n]) }
}
