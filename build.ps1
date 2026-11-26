# SPDX-License-Identifier: GPL-3.0-or-later
# Builds everything: Rust app (target\release\tessera.exe) and the C++ layer,
# copied next to the app in x64 and x86 variants.
#   powershell -File build.ps1 [-Test] [-Quality] [-Vulkan] [-Portable]
# -Portable: clang-cl, Rust, CMake and Ninja from .toolchain\<host> (tools/toolchain.ps1), no
# Visual Studio; output in build\<host>\ (x64, x86, cargo\release\tessera.exe).
param([switch]$Test, [switch]$Quality, [switch]$Vulkan, [switch]$Portable)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
# Environment changed below; restored at the end so the calling session is left as it was.
$envNames = "PATH", "RUSTUP_HOME", "CARGO_HOME", "INCLUDE", "LIB", "LIBPATH", "VSINSTALLDIR", "VCINSTALLDIR",
            "CC", "CXX", "AR", "CFLAGS", "CXXFLAGS", "CARGO_TARGET_DIR", "CARGO_ENCODED_RUSTFLAGS",
            "CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_LINKER"
$savedEnv = @{}
foreach ($n in $envNames) { $savedEnv[$n] = [Environment]::GetEnvironmentVariable($n) }

if ($Portable) {
    if ($Vulkan) { throw "-Vulkan needs the Vulkan SDK: not in -Portable builds" }
    # PROCESSOR_ARCHITEW6432 is set when an x64 PowerShell runs emulated on ARM64.
    $arm = "ARM64" -in $env:PROCESSOR_ARCHITECTURE, $env:PROCESSOR_ARCHITEW6432
    $hostName = if ($arm) { "windows-arm64" } else { "windows-x64" }
    $tc = Join-Path $root ".toolchain\$hostName"
    if (-not (Test-Path "$tc\cargo\bin\cargo.exe")) { throw "no toolchain in ${tc}: run tools/toolchain.ps1 -AcceptLicense" }
    $x = "$tc\xwin"
    # Only the folder's tools: nothing from Visual Studio or the user's PATH.
    $env:PATH = "$tc\cargo\bin;$tc\llvm\bin;$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\WindowsPowerShell\v1.0"
    $env:RUSTUP_HOME = "$tc\rustup"; $env:CARGO_HOME = "$tc\cargo"
    $incDirs = "crt\include", "sdk\include\ucrt", "sdk\include\um", "sdk\include\shared", "sdk\include\winrt" | ForEach-Object { "$x\$_" }
    $libDirs = "crt\lib\x86_64", "sdk\lib\um\x86_64", "sdk\lib\ucrt\x86_64" | ForEach-Object { "$x\$_" }
    $env:INCLUDE = $incDirs -join ";"; $env:LIB = $libDirs -join ";"
    Remove-Item Env:LIBPATH, Env:VSINSTALLDIR, Env:VCINSTALLDIR -ErrorAction SilentlyContinue
    # rustc and cc look up Visual Studio themselves and hand its LIB/INCLUDE to the tools:
    # /lldignoreenv and /X keep lld-link and clang-cl on the xwin folders only.
    $env:CC = "clang-cl"; $env:CXX = "clang-cl"; $env:AR = "llvm-lib"
    $env:CFLAGS = (@("/X") + ($incDirs | ForEach-Object { "-imsvc", $_ })) -join " "   # cc splits on spaces
    $env:CXXFLAGS = $env:CFLAGS
    $env:CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_LINKER = "lld-link"
    $env:CARGO_ENCODED_RUSTFLAGS = (@("-Clink-arg=/lldignoreenv") + ($libDirs | ForEach-Object { "-Lnative=$_" })) -join [char]0x1f
    $cargoDir = "build\$hostName\cargo"
    $env:CARGO_TARGET_DIR = Join-Path $root $cargoDir
    $cmake = "$tc\cmake\bin\cmake.exe"
    $x64Dir = "build\$hostName\x64"; $x86Dir = "build\$hostName\x86"; $cfg = ""
} else {
    $cmake = "D:\BuildTools\VS2022\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (-not (Test-Path $cmake)) { $cmake = "cmake" }  # any CMake on PATH
    $env:PATH = "$env:USERPROFILE\.cargo\bin;$env:PATH"
    $cargoDir = "target"
    $x64Dir = "build"; $x86Dir = "build\win32"; $cfg = "Release"
}
$out = "$cargoDir\release"
$sub = if ($cfg) { "$cfg\" } else { "" }

# Configures (once) and builds a native dir (all targets, or $target); extra arguments go to
# the configure step.
function Build-Native($dir, $arch, $what, $target) {
    if (-not (Test-Path "$dir\CMakeCache.txt")) {
        if ($Portable) {
            & $cmake -S . -B $dir -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_MAKE_PROGRAM=$tc\ninja\ninja.exe" `
                "-DCMAKE_TOOLCHAIN_FILE=$root\cmake\clang-cl.cmake" "-DTESSERA_TOOLCHAIN=$tc" "-DTESSERA_ARCH=$arch" @args
        } else {
            & $cmake -S . -B $dir -G "Visual Studio 17 2022" -A $(if ($arch -eq "x86") { "Win32" } else { "x64" }) @args
        }
        if ($LASTEXITCODE) { throw "$what cmake configure failed" }
    }
    $only = if ($target) { @("--target", $target) } else { @() }
    & $cmake --build $dir --config Release @only
    if ($LASTEXITCODE) { throw "$what cmake build failed" }
}

Push-Location $root
try {
    cargo build --release -p tessera-app
    if ($LASTEXITCODE) { throw "cargo build failed" }
    Build-Native $x64Dir x64 "x64"
    Copy-Item "$x64Dir\src\dll\${sub}tessera_d3d11.dll" "$out\tessera_d3d11.dll" -Force
    Copy-Item "$x64Dir\src\dll\${sub}tessera_dxgi.dll" "$out\tessera_dxgi.dll" -Force
    Copy-Item "$x64Dir\src\dll\${sub}tessera_vulkan.dll" "$out\tessera_vulkan.dll" -Force
    Copy-Item "$x64Dir\src\warm\${sub}tessera_warm.exe" "$out\tessera_warm.exe" -Force
    Copy-Item "$x64Dir\src\d3d12_capture\${sub}tessera_d3d12_capture.dll" "$out\tessera_d3d12_capture.dll" -Force
    Build-Native $x86Dir x86 "Win32"
    Copy-Item "$x86Dir\src\dll\${sub}tessera_d3d11.dll" "$out\tessera_d3d11_x86.dll" -Force
    Copy-Item "$x86Dir\src\dll\${sub}tessera_dxgi.dll" "$out\tessera_dxgi_x86.dll" -Force
    Copy-Item "$x86Dir\src\dll\${sub}tessera_vulkan.dll" "$out\tessera_vulkan_x86.dll" -Force
    Copy-Item "$x86Dir\src\warm\${sub}tessera_warm.exe" "$out\tessera_warm_x86.exe" -Force
    if ($Quality) {
        $q = if ($Portable) { "build\$hostName\quality" } else { "build\quality" }
        Build-Native $q x64 "quality" tessera_texcomp "-DTESSERA_BUILD_QUALITY=ON"
        $qt = "$q\src\texcomp\$sub"
        Copy-Item "${qt}tessera_texcomp.exe", "${qt}DirectXTex-LICENSE.txt", "${qt}FLIP-LICENSE.txt" $out -Force
    }
    if ($Vulkan) {
        & $cmake -S . -B build/vulkan-telemetry -G "Visual Studio 17 2022" -A x64 -DTESSERA_BUILD_VULKAN_TELEMETRY=ON
        if ($LASTEXITCODE) { throw "Vulkan configure failed" }
        & $cmake --build build/vulkan-telemetry --config Release --target tessera_vulkan_telemetry tessera_vulkan_replay tessera_vulkan_device_fixture
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
        # PowerShell 5.1 drops empty arguments to native commands: pass the quotes themselves.
        $cfgArg = if ($cfg) { @() } else { @("-Config", '""') }
        powershell -NoProfile -ExecutionPolicy Bypass -File tests\run_smoke.ps1 -Build $x64Dir -Cargo $cargoDir @cfgArg
        if ($LASTEXITCODE) { throw "smoke test failed" }
        powershell -NoProfile -ExecutionPolicy Bypass -File tests\run_smoke.ps1 -X86 -Build $x86Dir -Cargo $cargoDir @cfgArg
        if ($LASTEXITCODE) { throw "Win32 smoke test failed" }
    }
    "built: $out\tessera.exe + x64 and x86 D3D11 payloads"
} finally {
    Pop-Location
    foreach ($n in $envNames) { [Environment]::SetEnvironmentVariable($n, $savedEnv[$n]) }
}
