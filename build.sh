#!/usr/bin/env bash
# Build the OmniCore SDR Linux SD-card image inside Docker.
#
#   ./build.sh                 # everything -> out/omnicore-sdr-sd.img
#   ./build.sh uboot kernel    # only selected stages (uboot kernel dtb omnicored rootfs image)
#
# Environment:
#   FIRMWARE_DIR   OmniCore-SDR-Firmware checkout (default ../OmniCore-SDR-Firmware)
#   APT_MIRROR     Debian mirror host (default mirrors.ustc.edu.cn)
#   JOBS           parallel make jobs (default nproc)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
FIRMWARE_DIR="$(cd "${FIRMWARE_DIR:-$ROOT/../OmniCore-SDR-Firmware}" && pwd)"
APT_MIRROR="${APT_MIRROR:-mirrors.ustc.edu.cn}"
IMAGE=omnicore-linux-builder

docker build --build-arg APT_MIRROR="$APT_MIRROR" -t "$IMAGE" "$ROOT/docker"

tty_flag=()
[ -t 1 ] && tty_flag=(-t)

# The rootfs stage runs ARM programs in a chroot: it needs the host qemu-arm
# binfmt handler and a privileged container (mounting /proc in the chroot).
priv=()
if [ $# -eq 0 ] || [[ " $* " == *" rootfs "* ]]; then
    if [ ! -e /proc/sys/fs/binfmt_misc/qemu-arm ]; then
        echo "Registering qemu-arm binfmt on the host (until reboot)..."
        docker run --rm --privileged -v "$ROOT":/work "$IMAGE" /work/scripts/register-binfmt.sh
    fi
    priv=(--privileged)
fi

exec docker run --rm "${tty_flag[@]}" "${priv[@]}" \
    -v "$ROOT":/work \
    -v "$FIRMWARE_DIR":/firmware:ro \
    -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    -e APT_MIRROR="$APT_MIRROR" \
    -e JOBS="${JOBS:-$(nproc)}" \
    "$IMAGE" /work/scripts/build-all.sh "$@"
