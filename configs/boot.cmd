# OmniCore SDR U-Boot script (mkimage -T script -> boot.scr on the FAT partition).
# Loads the PL bitstream first: the Linux device tree describes PL peripherals.
echo "OmniCore SDR: loading bitstream"
if load ${devtype} ${devnum}:${distro_bootpart} 0x10000000 system_top.bit; then
	fpga loadb 0 0x10000000 ${filesize}
else
	echo "ERROR: system_top.bit not found, PL peripherals will be missing"
fi

load ${devtype} ${devnum}:${distro_bootpart} 0x02080000 zImage
load ${devtype} ${devnum}:${distro_bootpart} 0x02000000 devicetree.dtb

# Root starts read-only so systemd-fsck can check (and repair) it after a power
# loss; systemd-remount-fs then remounts it read-write as listed in /etc/fstab.
# CMA (IIO DMA blocks) below highmem (768 MiB): iiod hands the block pages to
# the network stack, and highmem pages would be copied again for the GEM, which
# has no NETIF_F_HIGHDMA.
setenv bootargs "console=ttyPS0,115200n8 earlycon root=/dev/mmcblk0p2 ro fsck.repair=yes rootfstype=ext4 rootwait net.ifnames=0 uio_pdrv_genirq.of_id=generic-uio cma=128M@0-0x30000000"
bootz 0x02080000 - 0x02000000
