#!/usr/bin/env bash
# Runs inside the builder container (see ../build.sh).
# /work = omnicore-linux, /firmware = OmniCore-SDR-Firmware (read-only).
set -euo pipefail

W=/work
FW=/firmware
CACHE=$W/.cache
BUILD=$W/build
OUT=$W/out
JOBS=${JOBS:-$(nproc)}
APT_MIRROR=${APT_MIRROR:-deb.debian.org}

LINUX_REPO=https://github.com/analogdevicesinc/linux.git
LINUX_BRANCH=2023_R2
UBOOT_REPO=https://github.com/u-boot/u-boot.git
UBOOT_TAG=v2024.01
# Source of the iiod that omnicored runs (patches/iiod); same release as
# Debian bookworm's libiio0, whose libiio.so.0 it links against.
LIBIIO_REPO=https://github.com/analogdevicesinc/libiio.git
LIBIIO_TAG=v0.24
DEBIAN_SUITE=bookworm

SDK=$FW/OmniCore-SDR-Firmware.sdk
HW=$SDK/system_wrapper_hw_platform_0

mkdir -p "$CACHE" "$BUILD" "$OUT"

log() { printf '\n\033[1;32m==> %s\033[0m\n' "$*"; }
warn() { printf '\n\033[1;33mWARNING: %s\033[0m\n' "$*" >&2; }

# Traceability: firmware commit (+"-dirty" with local changes to tracked files),
# the bitstream that goes on the card, and the commit this tree was validated
# against (FIRMWARE_LOCK). A different commit only warns; STRICT_FIRMWARE=1 fails.
OMNICORE_VERSION=$(cat "$W/VERSION")
FIRMWARE_REV=$(git -c safe.directory='*' -C "$FW" rev-parse --short HEAD 2>/dev/null || echo unknown)
if [ "$FIRMWARE_REV" != unknown ] && ! git -c safe.directory='*' -C "$FW" diff --quiet HEAD -- 2>/dev/null; then
    FIRMWARE_REV="$FIRMWARE_REV-dirty"
fi
FIRMWARE_LOCK=$(tr -d ' \n' < "$W/FIRMWARE_LOCK" 2>/dev/null || echo unknown)
BITSTREAM_SHA256=$(sha256sum "$HW/system_wrapper.bit" | cut -d' ' -f1)
FPGA_ID="bit-${BITSTREAM_SHA256:0:12}"
BUILD_DATE=$(date -u +%Y-%m-%d)
export OMNICORE_VERSION FIRMWARE_REV FIRMWARE_LOCK BITSTREAM_SHA256 FPGA_ID BUILD_DATE

if [ "${FIRMWARE_REV%-dirty}" != "$FIRMWARE_LOCK" ] || [ "$FIRMWARE_REV" != "${FIRMWARE_REV%-dirty}" ]; then
    msg="firmware is $FIRMWARE_REV, this tree was validated against $FIRMWARE_LOCK (FIRMWARE_LOCK)"
    if [ "${STRICT_FIRMWARE:-0}" = 1 ]; then
        echo "ERROR: $msg" >&2
        exit 1
    fi
    warn "$msg"
fi

fetch_sources() {
    [ -d "$CACHE/linux/.git" ] || git clone --depth 1 -b "$LINUX_BRANCH" "$LINUX_REPO" "$CACHE/linux"
    [ -d "$CACHE/u-boot/.git" ] || git clone --depth 1 -b "$UBOOT_TAG" "$UBOOT_REPO" "$CACHE/u-boot"
    [ -d "$CACHE/libiio/.git" ] || git clone --depth 1 -b "$LIBIIO_TAG" "$LIBIIO_REPO" "$CACHE/libiio"
    # The ADI tree also carries the HDL-matched AD9361/AXI drivers; nothing else to fetch.
}

# Applies patches/<tree>/*.patch to a cached source tree; already applied ones are skipped.
apply_patches() {
    local src=$1 dir=$2 p
    for p in "$dir"/*.patch; do
        [ -e "$p" ] || continue
        if git -c safe.directory='*' -C "$src" apply --reverse --check "$p" 2>/dev/null; then
            continue
        fi
        echo "applying $(basename "$p")"
        git -c safe.directory='*' -C "$src" apply "$p" || {
            echo "ERROR: $p does not apply to $src; a changed patch needs a clean tree:" >&2
            echo "       git -C ${src#$W/} checkout -- ." >&2
            exit 1
        }
    done
}

stage_uboot() {
    log "U-Boot $UBOOT_TAG (SPL + ps7_init_gpl.c from system_wrapper_hw_platform_0)"
    local src=$CACHE/u-boot o=$BUILD/u-boot
    local bdir=$src/board/xilinx/zynq/zynq-omnicore
    mkdir -p "$bdir" "$o"
    cp "$HW/ps7_init_gpl.c" "$HW/ps7_init_gpl.h" "$bdir/"
    cp "$W/dts/u-boot/zynq-omnicore.dts" "$src/arch/arm/dts/"
    grep -q 'zynq-omnicore.dtb' "$src/arch/arm/dts/Makefile" ||
        sed -i 's/^dtb-\$(CONFIG_ARCH_ZYNQ) += \\/dtb-$(CONFIG_ARCH_ZYNQ) += zynq-omnicore.dtb \\/' \
            "$src/arch/arm/dts/Makefile"
    make -C "$src" O="$o" xilinx_zynq_virt_defconfig
    "$src/scripts/kconfig/merge_config.sh" -m -O "$o" "$o/.config" "$W/configs/uboot.fragment"
    make -C "$src" O="$o" olddefconfig
    make -C "$src" O="$o" -j"$JOBS" DEVICE_TREE=zynq-omnicore
    bash "$W/scripts/make-boot-script.sh" "$o"
}

stage_kernel() {
    log "Linux ($LINUX_BRANCH)"
    local src=$CACHE/linux o=$BUILD/linux
    mkdir -p "$o"
    apply_patches "$src" "$W/patches/linux"
    make -C "$src" O="$o" zynq_xcomm_adv7511_defconfig
    "$src/scripts/kconfig/merge_config.sh" -m -O "$o" "$o/.config" "$W/configs/kernel.fragment"
    make -C "$src" O="$o" olddefconfig
    make -C "$src" O="$o" -j"$JOBS" zImage modules
    rm -rf "$BUILD/modules"
    make -C "$src" O="$o" INSTALL_MOD_PATH="$BUILD/modules" INSTALL_MOD_STRIP=1 modules_install
}

stage_dtb() {
    log "Device tree"
    local src=$CACHE/linux o=$BUILD/linux
    mkdir -p "$o"
    cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
        -I "$src/arch/arm/boot/dts" -I "$src/include" \
        "$W/dts/zynq-omnicore-sdr.dts" -o "$BUILD/zynq-omnicore-sdr.dts.pp"
    dtc -@ -I dts -O dtb -W no-unit_address_vs_reg -W no-simple_bus_reg \
        -o "$BUILD/devicetree.dtb" "$BUILD/zynq-omnicore-sdr.dts.pp"
}

stage_omnicored() {
    log "omnicored (no-OS SDK on Linux)"
    make -C "$W/omnicored" SDK_SRC="$SDK/OmniCore-SDR-no-OS/src" \
        CH32_DRV="$SDK/OmniCore-SDR-no-OS_bsp/ps7_cortexa9_0/libsrc/CH32H417_IP_v1_0/src" \
        CC=arm-linux-gnueabihf-gcc O="$BUILD/omnicored" -j"$JOBS" \
        VERSION_STRING="OmniCore-SDR Linux $OMNICORE_VERSION (firmware $FIRMWARE_REV, $FPGA_ID, $BUILD_DATE)" \
        FPGA_ID="$FPGA_ID"
}

stage_rootfs() {
    log "Debian $DEBIAN_SUITE armhf rootfs"
    apply_patches "$CACHE/libiio" "$W/patches/iiod"
    "$W/scripts/rootfs.sh" "$BUILD/rootfs" "$DEBIAN_SUITE" "http://$APT_MIRROR/debian"
}

stage_image() {
    log "SD card image"
    "$W/scripts/image.sh"
}

stage_clean() {
    log "Removing build/ and out/ (root-owned files from the container)"
    rm -rf "$BUILD" "$OUT"
    mkdir -p "$BUILD" "$OUT"
}

fix_owner() {
    if [ -n "${HOST_UID:-}" ]; then
        chown -R "$HOST_UID:${HOST_GID:-$HOST_UID}" "$OUT" 2>/dev/null || true
    fi
}
trap fix_owner EXIT

stages=("$@")
[ ${#stages[@]} -eq 0 ] && stages=(uboot kernel dtb omnicored rootfs image)

fetch_sources
for s in "${stages[@]}"; do
    "stage_$s"
done
log "done: ${stages[*]}"
