#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Independently formatted HFS+ disk image, via hfsprogs and hfsplus tools.
# No OS mount, no host filesystem driver, no source writes by Common V2.
set -euxo pipefail
program=$(realpath "${1:?Common V2 test executable required}")
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
export HOME="$work"
image="$work/hfsplus.img"
truncate -s 128M "$image"
timeout 45 mkfs.hfsplus -v NativeHFSPlus "$image"
head -c 65536 /dev/urandom > "$work/kernel-payload"
head -c 32768 /dev/urandom > "$work/initrd-payload"
printf 'y\n' | timeout 30 hpmount -p0 "$image"
printf 'y\n' | timeout 30 hpcopy -r "$work/kernel-payload" kernel.img
printf 'y\n' | timeout 30 hpcopy -r "$work/initrd-payload" initramfs
timeout 30 hpumount
(cd "$work" && sha256sum hfsplus.img > source.sha256)
"$program" "$image" "$work/kernel-payload" "$work/initrd-payload"
(cd "$work" && sha256sum -c source.sha256)
cp "$image" "$work/corrupt.img"
printf '\000\000' | dd of="$work/corrupt.img" bs=1 seek=1024 conv=notrunc status=none
"$program" "$work/corrupt.img" "$work/kernel-payload" \
  "$work/initrd-payload" --reject
(cd "$work" && sha256sum -c source.sha256)
echo HFSPLUS_COMMON_V2_EXTERNAL_FORMAT_READS_AND_NONMUTATION_PASS
