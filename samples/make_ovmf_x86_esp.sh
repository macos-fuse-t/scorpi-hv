#!/bin/sh

set -eu

img=${1:-samples/ovmf_x86_esp.raw}
size=${2:-64M}
tmp=$(mktemp)

cleanup()
{
	rm -f "$tmp"
}
trap cleanup EXIT INT TERM

mkdir -p "$(dirname "$img")"
truncate -s "$size" "$img"
mformat -i "$img" -F ::

printf 'echo Scorpi OVMF virtio ESP ready\r\nmap\r\n' > "$tmp"
mcopy -i "$img" "$tmp" ::startup.nsh

printf 'created %s\n' "$img"
