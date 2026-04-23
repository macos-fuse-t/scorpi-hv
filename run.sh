#!/bin/bash

IMAGE_URL="https://download.freebsd.org/releases/ISO-IMAGES/14.2/FreeBSD-14.2-RELEASE-arm64-aarch64-bootonly.iso"
IMAGE_NAME="FreeBSD-14.2-RELEASE-arm64-aarch64-bootonly.iso"

# Check if the image file already exists
if [ -f "$IMAGE_NAME" ]; then
    echo "Image file '$IMAGE_NAME' already exists. No need to download."
else
    echo "Image file not found. Downloading..."
    
    if command -v curl &> /dev/null; then
        curl -o "$IMAGE_NAME" "$IMAGE_URL"
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
bootrom: ./firmware/u-boot.bin

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

./builddir/scorpi -f "$CONFIG_FILE"
