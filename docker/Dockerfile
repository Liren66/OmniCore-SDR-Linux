FROM debian:bookworm

ARG APT_MIRROR=deb.debian.org
RUN sed -i "s|deb.debian.org|${APT_MIRROR}|g" /etc/apt/sources.list.d/debian.sources \
 && apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential gcc-arm-linux-gnueabihf libc6-dev-armhf-cross \
      bc bison flex libssl-dev libgnutls28-dev uuid-dev libncurses-dev \
      python3 python3-dev python3-setuptools python3-pyelftools swig \
      u-boot-tools device-tree-compiler kmod cpio rsync file xz-utils \
      git ca-certificates wget curl \
      debootstrap qemu-user-static debian-archive-keyring \
      dosfstools mtools e2fsprogs fdisk \
 && rm -rf /var/lib/apt/lists/*

ENV ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf-
WORKDIR /work
