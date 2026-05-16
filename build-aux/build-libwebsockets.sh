#!/bin/sh
set -eu

srcdir=$1
builddir=$2
liboutput=$3
cfgoutput=$4
host_system=$5
macos_min_version=$6

mkdir -p "$builddir"

cflags=-D_GNU_SOURCE
if [ "$host_system" = "darwin" ]; then
	cflags="$cflags -mmacosx-version-min=$macos_min_version"
fi

cmake -S "$srcdir" -B "$builddir" -G Ninja \
	-DCMAKE_POSITION_INDEPENDENT_CODE=TRUE \
	-DCMAKE_C_FLAGS="$cflags" \
	-DLWS_WITH_SHARED=FALSE \
	-DLWS_WITH_STATIC=TRUE \
	-DLWS_WITH_SSL=FALSE \
	-DLWS_WITHOUT_TESTAPPS=TRUE

cmake --build "$builddir" --target websockets

cp "$builddir/lib/libwebsockets.a" "$liboutput"
cp "$builddir/lws_config.h" "$cfgoutput"
