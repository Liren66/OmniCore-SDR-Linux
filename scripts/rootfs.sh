#!/usr/bin/env bash
# Debian armhf root filesystem: debootstrap first stage natively, second stage
# and package configuration in a chroot through the host's qemu-arm binfmt
# (scripts/register-binfmt.sh). Needs a privileged container to mount /proc.
#   rootfs.sh <dir> <suite> <mirror>
set -euo pipefail

R=$1
SUITE=$2
MIRROR=$3
W=/work
BUILD=$W/build

PACKAGES=(
    systemd systemd-sysv udev dbus kmod iproute2 iputils-ping ifupdown
    openssh-server sudo nano less procps psmisc ethtool net-tools
    dhcpcd-base avahi-daemon libnss-mdns
    libiio0 libiio-utils iiod
    ca-certificates cloud-guest-utils e2fsprogs dosfstools
    locales tzdata
)

in_target() {
    chroot "$R" /usr/bin/env -i HOME=/root PATH=/usr/sbin:/usr/bin:/sbin:/bin LANG=C.UTF-8 \
        DEBIAN_FRONTEND=noninteractive "$@"
}

mounted=()
mount_target() {
    [ -L "$R/proc" ] && rm -f "$R/proc" && mkdir "$R/proc"
    mount -t proc proc "$R/proc" && mounted+=("$R/proc")
    mount -t sysfs sysfs "$R/sys" && mounted+=("$R/sys")
    mount --bind /dev "$R/dev" && mounted+=("$R/dev")
    mount -t devpts devpts "$R/dev/pts" && mounted+=("$R/dev/pts")
}
umount_target() {
    local i
    for ((i = ${#mounted[@]} - 1; i >= 0; i--)); do umount -l "${mounted[i]}" || true; done
    mounted=()
}
trap umount_target EXIT

[ -f /proc/sys/fs/binfmt_misc/register ] || mount -t binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc
if [ ! -e /proc/sys/fs/binfmt_misc/qemu-arm ]; then
    echo "qemu-arm binfmt is not registered on the host (run scripts/register-binfmt.sh)" >&2
    exit 1
fi

if [ ! -f "$R/.debootstrap-done" ]; then
    rm -rf "$R"
    mkdir -p "$R"
    include=$(IFS=,; echo "${PACKAGES[*]}")
    debootstrap --arch=armhf --foreign --variant=minbase --include="$include" \
        "$SUITE" "$R" "$MIRROR"
    in_target /debootstrap/debootstrap --second-stage
    touch "$R/.debootstrap-done"
fi

cat > "$R/etc/apt/sources.list" <<EOF
deb $MIRROR $SUITE main contrib non-free non-free-firmware
deb $MIRROR $SUITE-updates main contrib non-free non-free-firmware
deb ${MIRROR%/debian}/debian-security $SUITE-security main contrib non-free non-free-firmware
EOF

# Kernel modules, omnicored and configuration overlay.
rm -rf "$R/lib/modules"
cp -a "$BUILD/modules/lib/modules" "$R/lib/"
install -D -m 0755 "$BUILD/omnicored/omnicored" "$R/usr/sbin/omnicored"
# The iiod omnicored runs per libiio connection (zero-copy sends, patches/iiod);
# Debian's /usr/sbin/iiod stays installed but unused.
"$W/scripts/build-iiod.sh" "$W/.cache/libiio" "$R" "$BUILD/iiod"
install -D -m 0755 "$BUILD/iiod/iiod" "$R/usr/local/sbin/iiod"
# Up to 1.1: iiod as a localhost service behind omnicored's proxy (gone from the overlay).
rm -f "$R/etc/systemd/system/omnicore-iiod.service"
cp -a "$W/rootfs-overlay/." "$R/"
chmod 0755 "$R/usr/local/sbin/"* 2>/dev/null || true

echo omnicore-sdr > "$R/etc/hostname"
"$W/scripts/version-files.sh" "$R"
cat > "$R/etc/hosts" <<EOF
127.0.0.1	localhost
127.0.1.1	omnicore-sdr
::1		localhost ip6-localhost ip6-loopback
EOF
# Power-loss hardening: the kernel mounts / read-only (boot.cmd) so systemd-fsck
# checks it, then it is remounted rw here. /boot (FAT, only U-Boot reads it) stays
# read-only: "mount -o remount,rw /boot" before updating boot files. /tmp in RAM.
cat > "$R/etc/fstab" <<EOF
/dev/mmcblk0p2	/	ext4	defaults,noatime,errors=remount-ro	0 1
/dev/mmcblk0p1	/boot	vfat	ro,noatime	0 2
tmpfs	/tmp	tmpfs	nosuid,nodev,size=64M	0 0
EOF
mkdir -p "$R/boot"
# journald keeps logs in RAM (rootfs-overlay journald.conf.d); no persistent journal.
rm -rf "$R/var/log/journal"

mount_target
in_target sh -c '
    set -e
    echo "en_US.UTF-8 UTF-8" > /etc/locale.gen
    locale-gen >/dev/null
    echo "root:root" | chpasswd
    if id analog >/dev/null 2>&1; then userdel -r analog 2>/dev/null || userdel analog; fi
    id OmniCore-SDR >/dev/null 2>&1 || useradd -m -s /bin/bash -G sudo,dialout OmniCore-SDR ||
        useradd --badname -m -s /bin/bash -G sudo,dialout OmniCore-SDR
    echo "OmniCore-SDR:root" | chpasswd
    sed -i -e "s/^#\?Port .*/Port 22/" -e "s/^#\?PermitRootLogin.*/PermitRootLogin yes/" \
        -e "s/^#\?PasswordAuthentication.*/PasswordAuthentication yes/" /etc/ssh/sshd_config
    grep -q "^Port 22" /etc/ssh/sshd_config || echo "Port 22" >> /etc/ssh/sshd_config
    # omnicored owns the libiio port and runs an iiod per connection.
    systemctl disable iiod.service >/dev/null 2>&1 || true
    systemctl mask iiod.service
    systemctl enable omnicore-eth-up.service
    systemctl enable omnicored.service dhcpcd.service omnicore-firstboot.service \
        systemd-timesyncd.service ssh.service avahi-daemon.service 2>/dev/null || \
    systemctl enable omnicored.service dhcpcd.service omnicore-firstboot.service ssh.service avahi-daemon.service
    systemctl set-default multi-user.target
    systemctl enable serial-getty@ttyPS0.service
    # Boot time and SD wear: no unattended apt/dpkg/e2scrub jobs (the board has
    # no RTC, so every "missed" timer fired at each boot), and ifupdown is not
    # needed next to dhcpcd (systemd brings up lo).
    systemctl disable apt-daily.timer apt-daily-upgrade.timer dpkg-db-backup.timer \
        e2scrub_all.timer e2scrub_reap.service networking.service 2>/dev/null || true
    apt-get clean
    rm -rf /var/lib/apt/lists/*
'
umount_target
# Host keys are generated per board on first boot.
rm -f "$R"/etc/ssh/ssh_host_*
