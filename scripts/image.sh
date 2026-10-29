#!/usr/bin/env bash
# Assemble out/omnicore-sdr-sd.img: p1 FAT32 BOOT (256 MiB), p2 ext4 rootfs.
# Built without loop devices (mtools + mke2fs -d), so no privileges are needed.
set -euo pipefail

W=/work
B=$W/build
OUT=$W/out
FW=/firmware
HW=$FW/OmniCore-SDR-Firmware.sdk/system_wrapper_hw_platform_0
BOOT_MB=256
IMG=$OUT/omnicore-sdr-sd.img

rm -rf "$OUT/boot"
mkdir -p "$OUT/boot"
cp "$B/u-boot/spl/boot.bin" "$OUT/boot/BOOT.bin"
cp "$B/u-boot/u-boot.img" "$OUT/boot/"
bash "$W/scripts/make-boot-script.sh" "$OUT/boot" >/dev/null
cp "$B/linux/arch/arm/boot/zImage" "$B/devicetree.dtb" "$OUT/boot/"
cp "$HW/system_wrapper.bit" "$OUT/boot/system_top.bit"

rm -f "$B/boot.vfat" "$B/rootfs.ext4"
mkfs.vfat -F 32 -n BOOT -C "$B/boot.vfat" $((BOOT_MB * 1024)) >/dev/null
mcopy -i "$B/boot.vfat" "$OUT/boot/"* ::

used_mb=$(du -sm --exclude=.debootstrap-done "$B/rootfs" | cut -f1)
root_mb=$((used_mb + used_mb / 4 + 256))
mke2fs -q -t ext4 -L rootfs -d "$B/rootfs" "$B/rootfs.ext4" "${root_mb}M"

total_mb=$((1 + BOOT_MB + root_mb))
rm -f "$IMG"
truncate -s "${total_mb}M" "$IMG"
sfdisk -q "$IMG" <<EOF
label: dos
start=1MiB, size=${BOOT_MB}MiB, type=c, bootable
start=$((1 + BOOT_MB))MiB, type=83
EOF
dd if="$B/boot.vfat" of="$IMG" bs=1M seek=1 conv=notrunc,sparse status=none
dd if="$B/rootfs.ext4" of="$IMG" bs=1M seek=$((1 + BOOT_MB)) conv=notrunc,sparse status=none
rm -f "$B/boot.vfat" "$B/rootfs.ext4"

(cd "$OUT" && sha256sum "$(basename "$IMG")" > "$(basename "$IMG").sha256")
echo "image: $IMG (${total_mb} MiB; rootfs grows to the card size on first boot)"
