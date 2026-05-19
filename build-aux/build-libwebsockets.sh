#!/bin/sh
set -eu

srcdir=$1
builddir=$2
liboutput=$3
cfgoutput=$4
host_system=$5
macos_min_version=$6

if [ ! -f "$srcdir/CMakeLists.txt" ]; then
	subprojects_dir=$(dirname "$srcdir")
	project_root=$(dirname "$subprojects_dir")

	if [ ! -f "$project_root/subprojects/libwebsockets.wrap" ]; then
		echo "libwebsockets source is missing and no Meson wrap was found at $project_root/subprojects/libwebsockets.wrap" >&2
		exit 1
	fi

	if ! meson subprojects download --sourcedir "$project_root" libwebsockets; then
		if [ ! -f "$srcdir/CMakeLists.txt" ]; then
			exit 1
		fi
	fi
fi

if [ ! -f "$srcdir/CMakeLists.txt" ]; then
	echo "libwebsockets source directory is missing: $srcdir" >&2
	exit 1
fi

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
