#!/bin/sh
# Registers Debian's qemu-arm-static (from the builder image) with the host
# kernel's binfmt_misc. The F flag pins the interpreter at registration time,
# so it keeps working inside chroots after this container exits. Not persistent
# across reboots; remove with: echo -1 > /proc/sys/fs/binfmt_misc/qemu-arm
set -e
B=/proc/sys/fs/binfmt_misc
[ -f $B/register ] || mount -t binfmt_misc binfmt_misc $B
[ -e $B/qemu-arm ] && { echo "qemu-arm binfmt already registered"; exit 0; }
cat /usr/lib/binfmt.d/qemu-arm.conf > $B/register
echo "registered: $(head -2 $B/qemu-arm | tr '\n' ' ')"
