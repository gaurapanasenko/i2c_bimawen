#!/bin/bash
set -euo pipefail
arch=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$arch/.." && pwd)
ver=$(sed -n "s/^pkgver=//p" "$arch/PKGBUILD" | head -1)
out="${1:-$arch}"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cp "$arch/dkms.conf" "$arch/i2c_bimawen.conf" "$tmp/"
cp "$root"/{LICENSE,Makefile,hook.c,ddc.c,ddc.h,backlight.c,backlight.h,retry.c,retry.h} "$tmp/"
tar czvf "$out/i2c_bimawen-${ver}.tar.gz" -C "$tmp" .
echo "Wrote $out/i2c_bimawen-${ver}.tar.gz"