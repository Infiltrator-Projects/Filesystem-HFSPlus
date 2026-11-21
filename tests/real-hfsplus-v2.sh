#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Independently formatted HFS+ disk image, populated on disposable CI media.
# The HFS+ host writer is used only for fixture creation; actual reads are
# performed through the installed Common V2 .so, never through Linux VFS.
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
sudo mount -t hfsplus -o loop,rw "$image" "$mounted_at"
mounted=1
sudo cp -- "$work/kernel-payload" "$mounted_at/kernel.img"
sudo cp -- "$work/initrd-payload" "$mounted_at/initramfs"
# HFS+ uses UTF-16BE native names. Test a real non-ASCII UTF-8 filename
# in addition to its native private hard-link metadata directory.
unicode_name="$(printf 'caf\303\251.txt')"
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
# Keep the native HFS+ signature but change its on-media revision to the
# HFSX revision. This is an unsupported variant, not an absent filesystem.
cp "$image" "$work/unsupported-version.img"
printf '\\000\\005' | dd of="$work/unsupported-version.img" bs=1 seek=1026 \
  conv=notrunc status=none
"$program" "$work/unsupported-version.img" "$work/kernel-payload" \
  "$work/initrd-payload" --unsupported-version
(cd "$work" && sha256sum -c source.sha256)
echo HFSPLUS_COMMON_V2_EXTERNAL_FORMAT_READS_AND_NONMUTATION_PASS
