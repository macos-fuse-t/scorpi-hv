#!/bin/sh
set -eu

script_dir=$(CDPATH= cd "$(dirname "$0")" && pwd)
cd "${script_dir}"

case "$(uname -s)" in
Darwin)
	os=macos
	: "${CC:=clang}"
	export CC
	;;
Linux)
	os=linux
	if [ -z "${CC+x}" ] && command -v clang >/dev/null 2>&1; then
		CC=clang
		export CC
	fi
	;;
*)
	echo "Unsupported OS: $(uname -s)" >&2
	exit 1
	;;
esac

case "$(uname -m)" in
arm64|aarch64)
	arch=arm64
	;;
x86_64|amd64)
	arch=x86_64
	;;
*)
	echo "Unsupported architecture: $(uname -m)" >&2
	exit 1
	;;
esac

builddir_name="build-${os}-${arch}"
builddir="${script_dir}/${builddir_name}"
buildtype="${BUILDTYPE:-debug}"

meson setup --buildtype="${buildtype}" "${builddir}" "$@"
meson compile -C "${builddir}"
