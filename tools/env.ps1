# SPDX-License-Identifier: GPL-3.0-or-later
# Build environment from .toolchain\<host> (tools/toolchain.ps1), dot-sourced by build.ps1 and
# tests/run_quality.ps1: clang-cl, lld-link, Rust, CMake and Ninja from the folder, nothing from
# Visual Studio or the user's PATH. Changes the process environment (build.ps1 restores it).
# Sets the tools/paths.ps1 variables plus $tc, $cmake and $ninja.
. "$PSScriptRoot\paths.ps1"
$repoRoot = Split-Path $PSScriptRoot -Parent
$tc = Join-Path $repoRoot ".toolchain\$hostName"
if (-not (Test-Path "$tc\cargo\bin\cargo.exe")) { throw "no toolchain in ${tc}: run tools/toolchain.ps1 -AcceptLicense" }
$x = "$tc\xwin"
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
$env:CARGO_TARGET_DIR = Join-Path $repoRoot "$buildRoot\cargo"
$cmake = "$tc\cmake\bin\cmake.exe"
$ninja = "$tc\ninja\ninja.exe"
