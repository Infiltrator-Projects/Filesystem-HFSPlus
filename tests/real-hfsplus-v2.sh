#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Independently formatted HFS+ disk image, via hfsprogs and hfsplus tools.
# No OS mount, no host filesystem driver, no source writes by Common V2.
set -euxo pipefail
program=$(realpath "${1:?Common V2 test executable required}")
work=$(mktemp -d)
mounted=0
mounted_at="$work/fixture-mounted"
cleanup() {
  if (( mounted )); then sudo umount "$mounted_at" || true; fi
  rm -rf -- "$work"
}
trap cleanup EXIT
mkdir -p "$mounted_at"
export HOME="$work"
image="$work/hfsplus.img"
truncate -s 128M "$image"
timeout 45 mkfs.hfsplus -v NativeHFSPlus "$image"
head -c 65536 /dev/urandom > "$work/kernel-payload"
head -c 32768 /dev/urandom > "$work/initrd-payload"
# Fixture creation only: a fresh regular-file image is populated by an
# independent Linux HFS+ writer on the disposable CI runner, never by the
# production filesystem module or the actual target machine.
sudo mount -t hfsplus -o loop,rw "$image" "$mounted_at"
mounted=1
sudo cp -- "$work/kernel-payload" "$mounted_at/kernel.img"
sudo cp -- "$work/initrd-payload" "$mounted_at/initramfs"
# HFS+ stores names as UTF-16BE and may normalize Unicode to decomposed
# form. The V2 directory emitter must return valid UTF-8, not refuse it.
unicode_name=
sync
sudo umount "$mounted_at"
mounted=0
(cd "$work" && sha256sum hfsplus.img > source.sha256)
"$program" "$image" "$work/kernel-payload" "$work/initrd-payload"
(cd "$work" && sha256sum -c source.sha256)
cp "$image" "$work/corrupt.img"
printf '\000\000' | dd of="$work/corrupt.img" bs=1 seek=1024 conv=notrunc status=none
"$program" "$work/corrupt.img" "$work/kernel-payload" \
  "$work/initrd-payload" --reject
(cd "$work" && sha256sum -c source.sha256)
echo HFSPLUS_COMMON_V2_EXTERNAL_FORMAT_READS_AND_NONMUTATION_PASS
caf\u00e9.txt'
sudo cp -- "$work/kernel-payload" "$mounted_at/$unicode_name"
sync
sudo umount "$mounted_at"
mounted=0
(cd "$work" && sha256sum hfsplus.img > source.sha256)
"$program" "$image" "$work/kernel-payload" "$work/initrd-payload"
(cd "$work" && sha256sum -c source.sha256)
cp "$image" "$work/corrupt.img"
printf '\000\000' | dd of="$work/corrupt.img" bs=1 seek=1024 conv=notrunc status=none
"$program" "$work/corrupt.img" "$work/kernel-payload" \
  "$work/initrd-payload" --reject
(cd "$work" && sha256sum -c source.sha256)
echo HFSPLUS_COMMON_V2_EXTERNAL_FORMAT_READS_AND_NONMUTATION_PASS
