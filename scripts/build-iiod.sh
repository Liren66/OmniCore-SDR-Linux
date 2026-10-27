#!/usr/bin/env bash
# Cross-builds iiod from libiio (patches/iiod applied) against the rootfs's
# libiio.so.0. Same configuration as Debian's iiod minus FunctionFS/AIO (USB
# gadget) and avahi, which the per-connection iiod omnicored runs never uses.
#   build-iiod.sh <libiio source> <rootfs> <output dir>
set -euo pipefail

SRC=$1
R=$2
O=$3
L=$R/usr/lib/arm-linux-gnueabihf
mkdir -p "$O"

# iio-config.h as CMake would write it ("../iio-config.h" from iiod/).
sed -e 's/@LIBIIO_VERSION_MAJOR@/0/; s/@LIBIIO_VERSION_MINOR@/24/' \
    -e 's/"@LIBIIO_VERSION_GIT@"/"omnicore"/; s/@LOG_LEVEL@/Info/' \
    -e 's/"@LIBIIO_SCAN_BACKENDS@"/"local,usb,ip"/' \
    -e 's/#cmakedefine01 \(WITH_LOCAL_BACKEND\|WITH_XML_BACKEND\|WITH_NETWORK_BACKEND\|WITH_USB_BACKEND\|WITH_SERIAL_BACKEND\|WITH_IIOD_SERIAL\|WITH_LOCAL_CONFIG\|WITH_LOCAL_MMAP_API\|WITH_HWMON\|WITH_NETWORK_EVENTFD\)$/#define \1 1/' \
    -e 's/#cmakedefine01 \(.*\)$/#define \1 0/' \
    -e 's/#cmakedefine \(HAS_PIPE2\|HAS_STRDUP\|HAS_STRNDUP\|HAS_STRTOK_R\|HAS_STRERROR_R\|HAS_NEWLOCALE\|HAS_PTHREAD_SETNAME_NP\|HAVE_IPV6\)$/#define \1/' \
    -e 's/#cmakedefine \(.*\)$/\/* #undef \1 *\//' \
    "$SRC/iio-config.h.cmakein" > "$SRC/iio-config.h"

flex -o "$O/lexer.c" "$SRC/iiod/lexer.l"
bison -o "$O/parser.c" --defines="$O/parser.h" "$SRC/iiod/parser.y"
arm-linux-gnueabihf-gcc -O2 -std=gnu99 -D_GNU_SOURCE -Wall -Wno-sign-compare \
    -I"$SRC/iiod" -I"$SRC" -I"$O" -o "$O/iiod" \
    "$SRC/iiod/iiod.c" "$SRC/iiod/ops.c" "$SRC/iiod/thread-pool.c" "$SRC/iiod/serial.c" \
    "$O/parser.c" "$O/lexer.c" \
    "$L/libiio.so.0" -lpthread -Wl,-rpath-link,"$L" -Wl,-rpath-link,"$R/lib/arm-linux-gnueabihf"
arm-linux-gnueabihf-strip "$O/iiod"
