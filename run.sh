#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd "$(dirname "$0")" && pwd)
cd "$script_dir"

case "$(uname -s)" in
Darwin)
    os=macos
    ;;
Linux)
    os=linux
    ;;
*)
    echo "Unsupported OS: $(uname -s)" >&2
    exit 1
    ;;
esac

case "$(uname -m)" in
arm64|aarch64)
    arch=arm64
    freebsd_arch=arm64-aarch64
    freebsd_path=arm64/aarch64
    bootrom=./firmware/u-boot.bin
    ;;
x86_64|amd64)
    arch=x86_64
    freebsd_arch=amd64
    freebsd_path=amd64/amd64
    bootrom=./firmware/SCORPI_EFI_X86.fd
    ;;
*)
    echo "Unsupported architecture: $(uname -m)" >&2
    exit 1
    ;;
esac

FREEBSD_VERSION="${FREEBSD_VERSION:-14.4}"
IMAGE_NAME="FreeBSD-${FREEBSD_VERSION}-RELEASE-${freebsd_arch}-bootonly.iso"
IMAGE_URL="https://download.freebsd.org/releases/${freebsd_path}/ISO-IMAGES/${FREEBSD_VERSION}/${IMAGE_NAME}"
builddir="./build-${os}-${arch}"
scorpi_hv="${builddir}/scorpi-hv"

if [ ! -x "$scorpi_hv" ]; then
    echo "Missing executable: $scorpi_hv" >&2
    echo "Run ./build.sh first." >&2
    exit 1
fi

# Check if the image file already exists
if [ -f "$IMAGE_NAME" ]; then
    echo "Image file '$IMAGE_NAME' already exists. No need to download."
else
    echo "Image file not found. Downloading..."
    
    if command -v curl > /dev/null 2>&1; then
        curl -fL -o "$IMAGE_NAME" "$IMAGE_URL"
    else
        echo "curl is not installed. Please install one to proceed."
        exit 1
    fi
    
    echo "Download completed."
fi

CONFIG_FILE="$(mktemp /tmp/scorpi-freebsd.XXXXXX.yaml)"
trap 'rm -f "$CONFIG_FILE"' EXIT

cat > "$CONFIG_FILE" <<EOF
name: vm1
cpu: 4
memory: 2G
console: stdio
bootrom: $bootrom

devices:
  pci:
    - device: hostbridge
      slot: 0

    - device: virtio-net
      slot: 1
      backend: slirp

    - device: virtio-blk
      slot: 2
      path: ./$IMAGE_NAME
      ro: true
EOF

"$scorpi_hv" -f "$CONFIG_FILE"
