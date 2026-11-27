# SPDX-License-Identifier: GPL-3.0-or-later
# Portable build toolchain for Windows hosts (x64, ARM64) in .toolchain\windows-<arch>\:
# LLVM (clang-cl, lld-link, llvm-ml, llvm-rc, llvm-lib), CMake, Ninja, Rust (rustup) and
# the MSVC CRT + Windows SDK fetched by xwin. Nothing is installed in the system.
#   powershell -File tools/toolchain.ps1 -AcceptLicense
# -AcceptLicense: accepts the Microsoft license of the CRT and SDK files xwin downloads
# (https://go.microsoft.com/fwlink/?LinkId=2086102); they are never redistributed.
param([switch]$AcceptLicense)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$root = Split-Path $PSScriptRoot -Parent

$llvm = "23.1.6"; $cmakeV = "4.5.1"; $ninjaV = "1.13.2"; $xwinV = "0.10.0"; $rustupV = "1.29.3"; $xzV = "5.8.4"
# MSVC CRT and Windows SDK versions xwin fetches (otherwise the newest in the manifest).
$crtV = "14.44.17.14"; $sdkV = "10.0.26100"
# PROCESSOR_ARCHITEW6432 is set when an x64 PowerShell runs emulated on ARM64.
$arm = "ARM64" -in $env:PROCESSOR_ARCHITECTURE, $env:PROCESSOR_ARCHITEW6432
$hostName = if ($arm) { "windows-arm64" } else { "windows-x64" }
$triple = if ($arm) { "aarch64-pc-windows-msvc" } else { "x86_64-pc-windows-msvc" }
$tc = Join-Path $root ".toolchain\$hostName"
$dl = Join-Path $tc "dl"

# Pinned downloads: name -> url, SHA-256 (from the release digests). xwin and xz have no
# Windows ARM64 build: the x64 ones run emulated.
$files = @{
    llvm   = @("https://github.com/llvm/llvm-project/releases/download/llvmorg-$llvm/clang+llvm-$llvm-$triple.tar.xz",
               $(if ($arm) { "fa5a25df6a1405fdfb9942f4d5cc73bfc3b8b31cfd43ee2fbd43bd2e3168be01" } else { "16af05919cf36958dfb4ddaa4fc4d9d4ff577f5daeb76bc112ea49cd8ced82b2" }))
    cmake  = @("https://github.com/Kitware/CMake/releases/download/v$cmakeV/cmake-$cmakeV-windows-$(if ($arm) { 'arm64' } else { 'x86_64' }).zip",
               $(if ($arm) { "6782f11382941bb85c406eb3bd32a702222d8956535ae9cec5ee77395be2cc78" } else { "bfe6eadb245dc8213e0d3feea316b520cb2aae92f5667ffd76059ba6b05c430c" }))
    ninja  = @("https://github.com/ninja-build/ninja/releases/download/v$ninjaV/ninja-$(if ($arm) { 'winarm64' } else { 'win' }).zip",
               $(if ($arm) { "e52f0bdef9dfb1003229dbd6508a508c4073fd017247002adc66e5e806cb0391" } else { "07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65" }))
    xwin   = @("https://github.com/Jake-Shadle/xwin/releases/download/$xwinV/xwin-$xwinV-x86_64-pc-windows-msvc.tar.gz",
               "96e83665ef5c1406aabd0be603a8d08e67e604f36fda630b6807ee948a614e26")
    xz     = @("https://github.com/tukaani-project/xz/releases/download/v$xzV/xz-$xzV-windows.zip",
               "f31af7638391ecf286d48bc8555ce6e131691a1c52a9e44303bce57069e9de56")
    rustup = @("https://static.rust-lang.org/rustup/archive/$rustupV/$triple/rustup-init.exe",
               $(if ($arm) { "407e0d72f81d26a01f9d032b8317908cbb2917ba22765474b682b41390792ad9" } else { "38027d51d69a5d05493627a0634dbea52d6cd793c8f072d2373d3f4163eec7db" }))
}

function Get-Pinned($name) {
    $url, $sha = $files[$name]
    $dir = Join-Path $dl $name
    $file = Join-Path $dir (Split-Path $url -Leaf)
    if (-not (Test-Path $file)) {
        New-Item -ItemType Directory -Force $dir | Out-Null
        Write-Host "download $name"
        curl.exe -fsSL --retry 3 -o "$file.part" $url
        if ($LASTEXITCODE) { throw "download failed: $url" }
        Move-Item "$file.part" $file
    }
    $got = (Get-FileHash -Algorithm SHA256 $file).Hash.ToLower()
    if ($got -ne $sha) { Remove-Item -LiteralPath $file; throw "$name SHA-256 mismatch: $got" }
    $file
}

New-Item -ItemType Directory -Force $tc | Out-Null

# LLVM: only the tools the build uses and clang's own headers.
if (-not (Test-Path "$tc\llvm\bin\clang-cl.exe")) {
    $f = Get-Pinned llvm
    $top = "clang+llvm-$llvm-$triple"
    $out = Join-Path $tc "llvm"
    New-Item -ItemType Directory -Force $out | Out-Null
    $members = "bin/clang-cl.exe", "bin/lld-link.exe", "bin/llvm-ml.exe", "bin/llvm-rc.exe", "bin/llvm-lib.exe", "lib/clang/*/include"
    # Windows tar.exe reads .xz only where it was built with liblzma (not on Windows Server
    # 2022, where it waits forever on an external xz): decompress with the pinned xz instead.
    $xzDir = Join-Path $dl "xz-bin"
    Expand-Archive (Get-Pinned xz) $xzDir -Force
    $names = ($members | ForEach-Object { "`"$top/$_`"" }) -join " "
    # cmd pipes the bytes untouched (a PowerShell 5.1 pipe would re-encode them); /s strips
    # only the outer quote pair.
    cmd /s /c "`"`"$xzDir\bin_x86-64\xz.exe`" -dc `"$f`" | `"$env:SystemRoot\System32\tar.exe`" -xf - -C `"$out`" --strip-components 1 $names`""
    if ($LASTEXITCODE) { throw "LLVM extract failed" }
    Remove-Item -LiteralPath $f   # 900 MB, the extracted tools are what matters
}
if (-not (Test-Path "$tc\cmake\bin\cmake.exe")) {
    $f = Get-Pinned cmake
    $tmp = Join-Path $tc "cmake.tmp"
    Expand-Archive $f $tmp -Force
    Move-Item (Get-ChildItem $tmp -Directory | Select-Object -First 1).FullName "$tc\cmake"
    Remove-Item $tmp -Recurse -Force
}
if (-not (Test-Path "$tc\ninja\ninja.exe")) {
    Expand-Archive (Get-Pinned ninja) "$tc\ninja" -Force
}

# MSVC CRT + Windows SDK for x64 and x86 targets.
if (-not (Test-Path "$tc\xwin\sdk\include")) {
    if (-not $AcceptLicense) { throw "the MSVC CRT and Windows SDK need -AcceptLicense (Microsoft license, see the header)" }
    $f = Get-Pinned xwin
    & "$env:SystemRoot\System32\tar.exe" -xf $f -C (Split-Path $f) --strip-components 1
    if ($LASTEXITCODE) { throw "xwin extract failed" }
    $xwin = Join-Path (Split-Path $f) "xwin.exe"
    & $xwin --accept-license --arch x86_64,x86 --crt-version $crtV --sdk-version $sdkV --cache-dir "$dl\xwin-cache" splat --disable-symlinks --output "$tc\xwin"
    if ($LASTEXITCODE) { throw "xwin splat failed" }
    # xwin links sdk\include\<ver> and sdk\lib\<ver> to their own folder (winsysroot layout);
    # the loop breaks copying the folder to another PC. Delete only the links.
    Get-ChildItem "$tc\xwin\sdk\include", "$tc\xwin\sdk\lib" -Force |
        Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint } |
        ForEach-Object { [IO.Directory]::Delete($_.FullName) }
    Remove-Item "$dl\xwin-cache" -Recurse -Force
}

# Rust: rustup and cargo homes inside the folder; rust-toolchain.toml picks the toolchain.
$env:RUSTUP_HOME = "$tc\rustup"; $env:CARGO_HOME = "$tc\cargo"
if (-not (Test-Path "$tc\cargo\bin\cargo.exe")) {
    & (Get-Pinned rustup) -y --no-modify-path --default-host $triple --default-toolchain none --profile minimal
    if ($LASTEXITCODE) { throw "rustup install failed" }
}
# The channel and components rust-toolchain.toml pins (rustup refuses a bare install of a
# channel that names its host).
$toml = Get-Content -Raw (Join-Path $root "rust-toolchain.toml")
$channel = [regex]::Match($toml, 'channel\s*=\s*"([^"]+)"').Groups[1].Value
$components = [regex]::Matches([regex]::Match($toml, 'components\s*=\s*\[([^\]]*)\]').Groups[1].Value, '"([^"]+)"') |
    ForEach-Object { $_.Groups[1].Value }
# On ARM64 the pinned x64 toolchain runs emulated: rustup needs --force-non-host for it.
$nonHost = if ($arm) { @("--force-non-host") } else { @() }
& "$tc\cargo\bin\rustup.exe" toolchain install $channel --no-self-update --profile minimal -c ($components -join ",") @nonHost
if ($LASTEXITCODE) { throw "rust toolchain install failed" }
"toolchain ready: $tc"
