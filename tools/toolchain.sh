#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Portable build toolchain for Linux hosts (x64, ARM64) in .toolchain/linux-<arch>/: LLVM
# (clang-cl, lld-link, llvm-ml, llvm-rc, llvm-lib), CMake, Ninja, Rust (rustup) and the MSVC
# CRT + Windows SDK fetched by xwin, to build the Windows binaries. Nothing is installed in the system.
#   sh tools/toolchain.sh --accept-license
# --accept-license: accepts the Microsoft license of the CRT and SDK files xwin downloads
# (https://go.microsoft.com/fwlink/?LinkId=2086102); they are never redistributed.
# Needs curl, tar with xz, sha256sum and unzip (or python3); building also needs the
# system C compiler (cc, e.g. gcc or clang + libc dev files) for the Rust build scripts.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
accept=0
[ "${1:-}" = "--accept-license" ] && accept=1

llvm=23.1.6; cmakeV=4.5.1; ninjaV=1.13.2; xwinV=0.10.0; rustupV=1.29.3
# MSVC CRT and Windows SDK versions xwin fetches (otherwise the newest in the manifest).
crtV=14.44.17.14; sdkV=10.0.26100
# Pinned downloads (SHA-256 from the release digests).
case $(uname -m) in
x86_64)
    host=linux-x64; llvmArch=X64; cmakeArch=x86_64; ninjaZip=ninja-linux.zip
    xwinTriple=x86_64-unknown-linux-musl; rustTriple=x86_64-unknown-linux-gnu
    shaLlvm=bd4d1c434ff69ef06d19e7934c04708da677e5f2eb51f5bcf052ab91818f0094
    shaCmake=83b71181fb62b4fe8f26067a9321448572e7a43bd04ff6c7f54a24c33698f16d
    shaNinja=5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6
    shaXwin=d870eb4b2f390878af6da1ccd3cf321d22fcb72720984853b4be732ae597fc88
    shaRustup=b1c8cb14e30889e7e2999e6b69e62d9e9b9ffc0f4df8180564d306d9680e6c55 ;;
aarch64 | arm64)
    host=linux-arm64; llvmArch=ARM64; cmakeArch=aarch64; ninjaZip=ninja-linux-aarch64.zip
    xwinTriple=aarch64-unknown-linux-musl; rustTriple=aarch64-unknown-linux-gnu
    shaLlvm=e9daa167da28b9e31d30046f05f98e3e7da3647cdbe1c1863e8b223c07ccf291
    shaCmake=3bec208f11ed96c173032eeaf97937c54dd246acefee22acd7794a6b4e70d536
    shaNinja=fd2cacc8050a7f12a16a2e48f9e06fca5c14fc4c2bee2babb67b58be17a607fc
    shaXwin=6d56d28537a86f37aa3d041318898f25ee3100c6b6ec332ad873c28faf37be23
    shaRustup=9f608c142e0e38a78957d0661ba1dfaf0b6fb37184cfaa4b9a0aa57af6184282 ;;
*) echo "unsupported host: $(uname -m)" >&2; exit 1 ;;
esac
tc=$root/.toolchain/$host
dl=$tc/dl
mkdir -p "$dl"

# fetch NAME URL SHA256: downloads once into dl/NAME/, checks the hash, prints the path.
fetch() {
    d=$dl/$1; f=$d/${2##*/}
    if [ ! -f "$f" ]; then
        mkdir -p "$d"; echo "download $1" >&2
        curl -fsSL --retry 3 -o "$f.part" "$2"
        mv "$f.part" "$f"
    fi
    got=$(sha256sum "$f" | cut -d' ' -f1)
    if [ "$got" != "$3" ]; then rm -f "$f"; echo "$1 SHA-256 mismatch: $got" >&2; exit 1; fi
    echo "$f"
}
unzip_to() {
    if command -v unzip >/dev/null; then unzip -qo "$1" -d "$2"
    else python3 -m zipfile -e "$1" "$2"; fi
}

# LLVM: only the tools the build uses (clang-cl, lld-link and llvm-lib are links to clang,
# lld and llvm-ar) and clang's own headers.
if [ ! -x "$tc/llvm/bin/clang-cl" ]; then
    f=$(fetch llvm "https://github.com/llvm/llvm-project/releases/download/llvmorg-$llvm/LLVM-$llvm-Linux-$llvmArch.tar.xz" $shaLlvm)
    top=LLVM-$llvm-Linux-$llvmArch
    mkdir -p "$tc/llvm"
    tar -xJf "$f" -C "$tc/llvm" --strip-components 1 --wildcards \
        "$top/bin/clang" "$top/bin/clang-[0-9]*" "$top/bin/clang-cl" "$top/bin/lld" "$top/bin/lld-link" \
        "$top/bin/llvm-ml" "$top/bin/llvm-rc" "$top/bin/llvm-ar" "$top/bin/llvm-lib" "$top/lib/clang/*/include"
    rm -f "$f"   # 1 GB, the extracted tools are what matters
fi
if [ ! -x "$tc/cmake/bin/cmake" ]; then
    f=$(fetch cmake "https://github.com/Kitware/CMake/releases/download/v$cmakeV/cmake-$cmakeV-linux-$cmakeArch.tar.gz" $shaCmake)
    mkdir -p "$tc/cmake"
    tar -xzf "$f" -C "$tc/cmake" --strip-components 1
fi
if [ ! -x "$tc/ninja/ninja" ]; then
    f=$(fetch ninja "https://github.com/ninja-build/ninja/releases/download/v$ninjaV/$ninjaZip" $shaNinja)
    unzip_to "$f" "$tc/ninja"
    chmod +x "$tc/ninja/ninja"
fi

# MSVC CRT + Windows SDK for x64 and x86 targets (with the case symlinks a Linux build needs).
if [ ! -d "$tc/xwin/sdk/include" ]; then
    if [ $accept = 0 ]; then
        echo "the MSVC CRT and Windows SDK need --accept-license (Microsoft license, see the header)" >&2; exit 1
    fi
    f=$(fetch xwin "https://github.com/Jake-Shadle/xwin/releases/download/$xwinV/xwin-$xwinV-$xwinTriple.tar.gz" $shaXwin)
    tar -xzf "$f" -C "$(dirname "$f")" --strip-components 1
    "$(dirname "$f")/xwin" --accept-license --arch x86_64,x86 --crt-version $crtV --sdk-version $sdkV --cache-dir "$dl/xwin-cache" splat --output "$tc/xwin"
    # sdk/include/<ver> and sdk/lib/<ver> link to their own folder; the loop breaks copies.
    for l in "$tc"/xwin/sdk/include/* "$tc"/xwin/sdk/lib/*; do
        if [ -L "$l" ] && [ "$(readlink -f "$l")" = "$(readlink -f "$(dirname "$l")")" ]; then rm "$l"; fi
    done
    rm -rf "$dl/xwin-cache"
fi

# Rust: rustup and cargo homes inside the folder. rust-toolchain.toml names the Windows host
# toolchain; here its channel runs on the Linux host with the Windows target added.
export RUSTUP_HOME="$tc/rustup" CARGO_HOME="$tc/cargo"
if [ ! -x "$tc/cargo/bin/cargo" ]; then
    f=$(fetch rustup "https://static.rust-lang.org/rustup/archive/$rustupV/$rustTriple/rustup-init" $shaRustup)
    chmod +x "$f"
    "$f" -y --no-modify-path --default-host $rustTriple --default-toolchain none --profile minimal
fi
toml=$root/rust-toolchain.toml
channel=$(sed -n 's/^channel *= *"\([^"]*\)".*/\1/p' "$toml" | sed 's/-x86_64-pc-windows-msvc$//')
components=$(sed -n 's/^components *= *\[\(.*\)\].*/\1/p' "$toml" | tr -d ' "')
"$tc/cargo/bin/rustup" toolchain install "$channel" --no-self-update --profile minimal -c "$components" -t x86_64-pc-windows-msvc
echo "toolchain ready: $tc"
