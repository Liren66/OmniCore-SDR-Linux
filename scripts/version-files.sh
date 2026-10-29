#!/usr/bin/env bash
# Writes OmniCore-SDR version information into a root filesystem.
#   version-files.sh <rootfs>   (OMNICORE_VERSION, FIRMWARE_REV, BUILD_DATE from build-all.sh)
set -euo pipefail

R=$1
[ -n "$R" ] && [ -d "$R/usr/lib" ] || { echo "version-files.sh: bad rootfs '$R'" >&2; exit 1; }
B=/work/build
W=$(cd "$(dirname "$0")/.." && pwd)
NAME_VER="OmniCore-SDR Linux ${OMNICORE_VERSION}"
KREL=$(ls "$B/modules/lib/modules" | head -1)

cat > "$R/etc/omnicore-sdr-release" <<END_RELEASE
NAME="OmniCore-SDR Linux"
VERSION="${OMNICORE_VERSION}"
BUILD_DATE="${BUILD_DATE}"
FIRMWARE_REPO="https://github.com/Liren66/OmniCore-SDR-Firmware"
FIRMWARE_REV="${FIRMWARE_REV}"
FIRMWARE_LOCK="${FIRMWARE_LOCK}"
BITSTREAM="system_top.bit"
BITSTREAM_SHA256="${BITSTREAM_SHA256}"
FPGA_ID="${FPGA_ID}"
KERNEL="${KREL}"
UBOOT="U-Boot 2024.01-OmniCore-SDR"
BASE="Debian GNU/Linux 12 (bookworm)"
END_RELEASE

# /etc/os-release is a symlink into base-files; replace it with a branded copy
# (ID=debian is kept so apt and tools still recognise the base system).
sed -e "s|^PRETTY_NAME=.*|PRETTY_NAME=\"${NAME_VER} (Debian 12 bookworm)\"|" \
    "$R/usr/lib/os-release" > "$R/etc/os-release.new"
cat >> "$R/etc/os-release.new" <<END_OSREL
VARIANT="OmniCore-SDR Linux"
VARIANT_ID=omnicore-sdr
IMAGE_VERSION="${OMNICORE_VERSION}"
BUILD_ID="${BUILD_DATE}-fw${FIRMWARE_REV}"
END_OSREL
rm -f "$R/etc/os-release"
mv "$R/etc/os-release.new" "$R/etc/os-release"

# agetty interprets backslash escapes in /etc/issue; SSH displays literal text.
sed 's/\\/\\\\/g' "$W/configs/banner.txt" > "$R/etc/issue"
printf '%s \\n \\l (kernel \\r)\n\n' "$NAME_VER" >> "$R/etc/issue"
cat "$W/configs/banner.txt" > "$R/etc/issue.net"
printf '  %s\n\n' "$NAME_VER" >> "$R/etc/issue.net"
cat > "$R/etc/motd" <<END_MOTD

${NAME_VER}  (build ${BUILD_DATE}, firmware ${FIRMWARE_REV}, ${FPGA_ID}, kernel ${KREL})
  USB3/OCH and libiio share the AD9361; 'journalctl -u omnicored' shows the owner.
  Release details: /etc/omnicore-sdr-release

END_MOTD
