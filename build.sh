#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the Windows binaries from Linux with .toolchain/linux-<arch> (tools/toolchain.sh):
# tessera.exe and the x64 and x86 layer, copied next to the app in
# build/linux-<arch>/cargo/x86_64-pc-windows-msvc/release/. Tests need Windows (build.ps1 -Test, CI).
#   sh build.sh
set -eu
root=$(cd "$(dirname "$0")" && pwd)
case $(uname -m) in
x86_64) host=linux-x64 ;;
aarch64 | arm64) host=linux-arm64 ;;
*) echo "unsupported host: $(uname -m)" >&2; exit 1 ;;
esac
tc=$root/.toolchain/$host
[ -x "$tc/cargo/bin/cargo" ] || { echo "no toolchain in $tc: run sh tools/toolchain.sh --accept-license" >&2; exit 1; }
xw=$tc/xwin
target=x86_64-pc-windows-msvc

export PATH="$tc/cargo/bin:$tc/llvm/bin:$tc/cmake/bin:$PATH"
export RUSTUP_HOME="$tc/rustup" CARGO_HOME="$tc/cargo"
# rust-toolchain.toml names the Windows host toolchain: use its channel on this host.
export RUSTUP_TOOLCHAIN=$(sed -n 's/^channel *= *"\([^"]*\)".*/\1/p' "$root/rust-toolchain.toml" | sed 's/-x86_64-pc-windows-msvc$//')
export CARGO_TARGET_DIR="$root/build/$host/cargo"
export CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_LINKER=lld-link
export CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_RUSTFLAGS="-Lnative=$xw/crt/lib/x86_64 -Lnative=$xw/sdk/lib/um/x86_64 -Lnative=$xw/sdk/lib/ucrt/x86_64"
inc="/X -fms-compatibility-version=19.44 -imsvc $xw/crt/include -imsvc $xw/sdk/include/ucrt -imsvc $xw/sdk/include/um -imsvc $xw/sdk/include/shared"
export CC_x86_64_pc_windows_msvc=clang-cl CXX_x86_64_pc_windows_msvc=clang-cl AR_x86_64_pc_windows_msvc=llvm-lib
export CFLAGS_x86_64_pc_windows_msvc="$inc" CXXFLAGS_x86_64_pc_windows_msvc="$inc"

cd "$root"
cargo build --release -p tessera-app --target $target
out=$CARGO_TARGET_DIR/$target/release
for arch in x64 x86; do
    dir=build/$host/$arch
    if [ ! -f "$dir/CMakeCache.txt" ]; then
        cmake -S . -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM="$tc/ninja/ninja" \
            -DCMAKE_TOOLCHAIN_FILE="$root/cmake/clang-cl.cmake" -DTESSERA_TOOLCHAIN="$tc" -DTESSERA_ARCH=$arch
    fi
    cmake --build "$dir"
done
x64=build/$host/x64; x86=build/$host/x86
cp "$x64/src/dll/tessera_d3d11.dll" "$x64/src/dll/tessera_dxgi.dll" "$x64/src/dll/tessera_vulkan.dll" \
   "$x64/src/warm/tessera_warm.exe" "$x64/src/d3d12_capture/tessera_d3d12_capture.dll" "$out/"
cp "$x86/src/dll/tessera_d3d11.dll" "$out/tessera_d3d11_x86.dll"
cp "$x86/src/dll/tessera_dxgi.dll" "$out/tessera_dxgi_x86.dll"
cp "$x86/src/dll/tessera_vulkan.dll" "$out/tessera_vulkan_x86.dll"
cp "$x86/src/warm/tessera_warm.exe" "$out/tessera_warm_x86.exe"
echo "built: $out/tessera.exe + x64 and x86 D3D11 payloads"
