#!/bin/sh
# Cross-build using Microsoft WDK headers/libraries and an LLVM COFF toolchain.
set -eu
if [ "$#" -ne 3 ]; then
    echo "usage: $0 <headers-root-with-km-and-shared> <wdk-arm64-lib-directory> <output-directory>" >&2
    exit 2
fi
headers=$1
libraries=$2
output=$3
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$output"
"${CLANG:-clang}" --target=aarch64-pc-windows-msvc -c "$source_dir/timer.c" \
    -o "$output/timer.obj" -D_ARM64_ -DHALEXT -D_KERNEL_MODE \
    -fms-extensions -fms-compatibility -fno-stack-protector -mgeneral-regs-only -O2 \
    -Wall -Werror -Wno-ignored-attributes -Wno-pragma-pack \
    -Wno-microsoft-anon-tag -isystem "$headers/km" \
    -isystem "$headers/shared" -isystem "$headers/km/crt"
"${CLANG:-clang}" --target=aarch64-pc-windows-msvc -c "$source_dir/hypercall.S" \
    -o "$output/hypercall.obj"
"${LLD_LINK:-lld-link}" /dll /machine:arm64 /entry:HalExtensionInit \
    /subsystem:native /nodefaultlib \
    "/out:$output/HalExtScorpiTimer.dll" "$output/timer.obj" \
    "$output/hypercall.obj" "/libpath:$libraries" \
    halextlib.lib bufferoverflowfastfailk.lib ntoskrnl.lib libcntpr.lib
cp "$source_dir/install-test.ps1" "$output/"
