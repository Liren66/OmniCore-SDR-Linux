# OmniCore SDR Linux

**OmniCore SDR（Zynq-7020 + AD9361 + CH32H417 USB3）的 Linux 系统**：把原来的裸机 SDK 命令固件原样搬到 Linux 用户态运行，USB3/OCH 的使用方式与裸机完全一致，同时多出一条以太网 libiio 通路，可以直接接 IIO Oscilloscope、GNU Radio、pyadi-iio、MATLAB 等 ADI 生态工具。

- 当前版本 **1.2.1**（2026-10-29），对应固件 [OmniCore-SDR-Firmware](https://github.com/Liren66/OmniCore-SDR-Firmware) `c288940`（`SDK_CMD_1.0_20261026_VCXO`），bitstream **不改动**。
- 一条命令（Docker）编出整卡镜像：U-Boot 2024.01 + ADI Linux 2023_R2（6.1）+ Debian 12 armhf。
- 1.2.0 已完成实板刷卡验证；1.2.1 已完成镜像检查和 ARM SSH 服务验证，测试结果见下文和 [docs/TEST_RESULTS.md](docs/TEST_RESULTS.md)。

![系统架构](docs/images/architecture.svg)

## 目录

- [主要功能](#主要功能)
- [特色功能](#特色功能)
- [性能一览](#性能一览)
- [快速开始](#快速开始)（刷卡、启动、网络、**登录方式**）
- [使用](#使用)
- [编译](#编译)
- [配置](#配置)
- [测试结果](#测试结果)
- [注意事项](#注意事项)
- [目录结构](#目录结构)
- [版本历史](#版本历史)

## 主要功能

- **USB3 / OCH 与裸机一致**：omnicored 把 SDK 的 `console_commands/`、`platform_xilinx/`、no-OS AD9361 驱动和 CH32H417_IP 驱动**原样编译**，只替换 Xilinx BSP 头文件（`omnicored/shim/`）：
  `Xil_In32/Out32`→`/dev/mem`，`XSpiPs`→spidev，`XGpioPs`→GPIO 字符设备，`XScuGic`→UIO 中断，`XUartNs550`→8250 tty，`XAdcPs`→内核 xadc。OCH 命令协议、封装、EVT、溢出恢复都与裸机相同，`och` 主机工具无需任何修改。
- **以太网 libiio**：标准 ADI 内核驱动（ad9361 / cf_axi_adc / cf_axi_dds）+ iiod，端口 30431，mDNS 广播 `_iio._tcp`，主机名 `omnicore-sdr.local`。RX、TX（DDS DMA）都可用。
- **同一颗 AD9361 自动仲裁**：上电默认 USB 模式；USB 空闲时 libiio 连接自动接管；任何 OCH 命令都能把 AD9361 收回来。
- **整卡镜像**：SD 卡启动，首次启动自动把 rootfs 扩展到整卡、生成 SSH 主机密钥和固定 MAC。
- **可追溯**：固件提交、bitstream sha256、构建日期写进 `/etc/omnicore-sdr-release`，`system_info?` 报告 `fpga=bit-<sha256 前 12 位>`。

## 特色功能

### 1. OCH 优先的模式切换

![模式切换](docs/images/mode-switch.svg)

- libiio 只能在 USB 空闲（无推流、无 TX、3 s 内无 OCH 命令）时接管，约 2.0 s（其中 AD9361 LVDS 接口校准约 1.4 s）。USB 正忙时 libiio 客户端收到 `Device or resource busy (16)`，USB 推流不受任何影响。
- IIO 模式（或切换途中）收到 **任何 OCH 命令**：命令先排队 → 断开所有 libiio 客户端 → 收回 AD9361（冷启动默认参数，SDK 内存中的 FIR/RX 校正/LO 源目标保留）→ 按顺序执行排队的命令。实测 1.4–2.1 s，在 OCH 默认 5 s 超时之内，OCH 端看起来只是这一条命令慢了一点。
- `owner?` 命令随时可查当前归属：`owner=usb|libiio|switching clients=N iio=enabled och_preempt=1`。
- 驱动绑定/解绑在工作线程里做，切换期间主循环照常服务 CH32 串口和 VCXO。

### 2. libiio 零拷贝（1.2.0）

![数据路径](docs/images/zero-copy.svg)

1.1.0 的数据路径是：iiod 用 `write()` 从**不可缓存**的 DMA 内存逐字节拷进本地 TCP，再由 omnicored 的 splice 代理转发。CPU1 被拷贝占满，吞吐卡在 58 MB/s 左右。1.2.0 把这条路径改成了真正的零拷贝：

- **内核补丁**（`patches/linux/`）：IIO DMA 块改用 `vm_insert_page` 映射给用户态，页可以交给网络栈；在网络栈释放这些页（数据被 TCP 确认）之前，不把块交回 DMA、也不释放，保证重传数据正确。
- **iiod 补丁**（`patches/iiod/`，基于 libiio v0.24）：数据在 DMA 块里时用 `vmsplice()` + `splice()` 直接挂进 socket；`iiod -i` 可以按连接运行；打开缓冲区前先检查设备是否被占用。
- **omnicored**：去掉本地代理，每个 libiio 连接 fork 一个 iiod，连接直接作为它的 stdin/stdout。
- **CMA 放在低端内存**（`boot.cmd` 的 `cma=128M@0-0x30000000`）：GEM 网卡没有 HIGHDMA 能力，高端内存的页会在 GSO 分段时被再拷一遍。

结果：吞吐 +35%，iiod 所在 CPU1 的负载从约 85% 降到约 20%，瓶颈变成网卡中断和千兆链路本身。

### 3. 实时性与启动时间

- USB 模式主循环 `SCHED_FIFO 50`、`mlockall`、绑 CPU1；CH32/UART/SPI/PPS 中断也绑 CPU1，网口和 SD 中断留在 CPU0。IIO 模式降回普通调度。
- omnicored 不等 `basic.target`，journald 起来就启动；U-Boot 不等待；网口提前自协商；关掉了没用的定时任务。重启后 **OCH 16.7 s 可用**（1.0.0 为 24.7 s）。

### 4. 掉电保护

内核以只读挂载根分区，`systemd-fsck`（`fsck.repair=yes`）检查后再按 fstab 挂成可写，`errors=remount-ro`；`/boot`（FAT）平时只读；`/tmp` 和 journald 在内存里。板子随时断电都不会写坏系统。

### 5. 精简内核

只保留板上存在的硬件：去掉 HDMI/DRM、FB、声卡、USB、摄像头、无线、触摸屏，PHY 只留 Realtek。zImage 8.79 → 7.30 MB，模块 154 → 2 个。保留 debugfs（AD9361 的 IIO 调试属性如 `loopback` 在 debugfs 里）。

## 性能一览

| 项目 | 1.2.0 实测 |
|---|---|
| USB3 全双工（OCH bench，60 s） | 2×2 30.72 MSPS 12 bit：184.3 MB/s；2×2 61.44 MSPS 8 bit：245.7 MB/s；0 丢样、0 错误 |
| libiio RX 吞吐（以太网） | 单路 RX（2 通道）72–74 MB/s；双路 RX（4 通道）79.5–81 MB/s |
| **libiio 单路 RX 连续采样率** | **20 MSPS 稳定**（2 GiB 连续 0 断档）；21 MSPS 也能连续但已贴近上限；22 MSPS 跟不上 |
| libiio 双路 RX 连续采样率 | 10 MSPS（0 断档） |
| libiio 接管 / OCH 抢回 | 约 2.0 s / 1.4–2.1 s |
| 重启到可用 | OCH 16.7 s，ping 34.0 s，SSH 34.7 s，libiio 40.7 s |

![吞吐](docs/images/throughput.svg)

![单路采样率](docs/images/samplerate.svg)

> libiio 的样点在 IIO DMA 里每个 I/Q 分量占 16 bit（每个复样点 4 字节），所以以太网上单路最多约 20 MSPS、双路约 10 MSPS。需要满速率（61.44 MSPS、2×2）时请走 USB3。
> 吞吐和 PC 端也有关系（以上为 Windows 11 + Intel I219 千兆网卡实测）。

![CPU 负载](docs/images/cpu-load.svg)

![启动时间](docs/images/boot-timeline.svg)

## 快速开始

### 1. 刷卡

现成镜像在 [Releases](../../releases) 里：`omnicore-sdr-1.2.1-sd.img.xz`（78 MB，解压后 865 MiB，首次启动自动扩展到整卡），≥ 2 GB 的 SD 卡即可。

```bash
sha256sum -c omnicore-sdr-1.2.1-sd.img.xz.sha256
xz -dc omnicore-sdr-1.2.1-sd.img.xz | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress
```

Windows 用 balenaEtcher 或 Rufus 直接选 `.img.xz` 写入即可，不用先解压。自己编译的镜像在 `out/omnicore-sdr-sd.img`。

### 2. 启动

拨码开关 S3 控制 MIO4：拨到 3V3 一侧 = MIO[5:3]=110 = **SD 启动**；另一侧 = QSPI 启动（原裸机固件）。U-Boot 先 `fpga loadb` 加载 `system_top.bit`，再启动 Linux。

启动串口会显示与 OCH 相同的 **OmniCore SDR** ASCII 标识，串口登录提示和 SSH 连接认证前也会显示。SSH 登录后仍保留版本、固件和内核信息。

首次启动会多花约 16 s（扩展 rootfs、生成 SSH 主机密钥和固定 MAC），OCH 不受影响。

### 3. 网络

- dhcpcd 先尝试 DHCP，**5 s 内拿不到地址则使用 `192.168.2.1/24`**（Pluto 习惯）。直连 PC 时把 PC 网卡设成 `192.168.2.x/24`（例如 `192.168.2.11`）。
- 拿到 DHCP 地址时不会再挂 192.168.2.1（避免与路由器冲突）。
- mDNS：`omnicore-sdr.local`。MAC 地址首次启动随机生成后固定在该卡上。

### 4. 登录方式

| 方式 | 说明 |
|---|---|
| SSH | `ssh root@192.168.2.1`（或 `root@omnicore-sdr.local`），端口 22，允许密码登录 |
| 串口控制台 | UART0（MIO14/15，经 FT2232 B 通道，与 JTAG 共用 Type-C），115200 8N1 |
| 账号 | `root` / `root`；`OmniCore-SDR` / `root`（sudo 组） |

> **请尽快修改默认密码**（`passwd`），或者放入公钥后关闭密码登录（`/etc/ssh/sshd_config` 的 `PasswordAuthentication no`）。

版本信息：`cat /etc/omnicore-sdr-release`、`uname -r`（`6.1.70-OmniCore-SDR`）、`omnicored -V`。

## 使用

### OCH（USB3）

与裸机固件完全相同，例如：

```bash
och info
och selftest
och bench --rx --tx --channels 1,2 --sample-rate 30.72M --seconds 10
och get system.info          # 含 fpga=bit-xxxxxxxxxxxx、console_rx=irq
```

### libiio（以太网）

```bash
iio_info -u ip:192.168.2.1
iio_attr -u ip:192.168.2.1 -c ad9361-phy RX_LO frequency 2400000000
iio_attr -u ip:192.168.2.1 -i -c ad9361-phy voltage0 sampling_frequency 20000000
iio_readdev -u ip:192.168.2.1 -b 1048576 -s 20000000 cf-ad9361-lpc voltage0 voltage1 > rx.bin
```

GNU Radio / pyadi-iio / IIO Oscilloscope 的 URI 都填 `ip:192.168.2.1`。

- 第一次连接要等约 2 s 接管 AD9361（libiio 默认超时 5 s，够用）；之后每个新连接约 0.1 s。
- TX 衰减初值为 89.75 dB（静音），发射前需在 libiio 端设置 `hardwaregain`。
- 最后一个客户端断开 5 s 后自动切回 USB 模式；RF 参数恢复为冷启动默认值，OCH 打开设备时会重新下发。
- 不需要 libiio 时可在配置里设 `iio_enable = 0`。

### 查看日志

```bash
journalctl -u omnicored -f     # 每次切换的耗时、RF 初始化耗时和 SPI 传输次数
```

journald 只存在内存里（`Storage=volatile`），重启后不保留。

## 编译

需要 Docker（不需要 Xilinx 工具，不需要在主机上装交叉编译器）。把固件仓库放在旁边：

```text
OminCore-SDR-LINUX/
├── OmniCore-SDR-Firmware/   # git clone https://github.com/Liren66/OmniCore-SDR-Firmware
└── omnicore-linux/          # 本仓库
```

```bash
cd omnicore-linux
./build.sh                       # 全部阶段 -> out/omnicore-sdr-sd.img
./build.sh kernel                # 只编内核
./build.sh rootfs image          # 只重做 rootfs 和镜像
./build.sh clean                 # 删除 build/ 和 out/
```

- 阶段：`uboot kernel dtb omnicored rootfs image`。
- 环境变量：
  - `FIRMWARE_DIR`：默认 `../OmniCore-SDR-Firmware`。
  - `APT_MIRROR`：默认 `mirrors.ustc.edu.cn`。
  - `JOBS`：并行编译数。
  - `STRICT_FIRMWARE=1`：固件提交与 `FIRMWARE_LOCK` 不一致时直接失败（默认只告警）。
- 源码由构建脚本自动拉取到 `.cache/`：ADI Linux `2023_R2`、U-Boot `v2024.01`、libiio `v0.24`。`patches/linux/` 和 `patches/iiod/` 会自动打上，已经打过的补丁会跳过。修改补丁后需要先还原缓存，例如 `git -C .cache/linux checkout -- .`。
- rootfs 阶段需要 qemu-arm binfmt：`build.sh` 会自动注册（直到主机重启）。

产物在 `out/`：

| 文件 | 说明 |
|---|---|
| `omnicore-sdr-sd.img` (+`.sha256`) | 整卡镜像：p1 FAT32 BOOT 256 MiB，p2 ext4 rootfs |
| `boot/` | BOOT 分区文件：`BOOT.bin`（U-Boot SPL + ps7_init）`u-boot.img` `boot.scr` `zImage` `devicetree.dtb` `system_top.bit` |

组件版本：
- U-Boot v2024.01：SPL 直接使用固件仓库的 `ps7_init_gpl.c`，不需要 FSBL。
- ADI Linux 2023_R2（6.1.70），加 `patches/linux`。
- Debian 12 bookworm armhf，libiio 0.24。
- iiod：libiio v0.24 加 `patches/iiod` 交叉编译，装在 `/usr/local/sbin/iiod`。

## 配置

`/etc/omnicore/omnicored.conf`：

| 键 | 默认 | 说明 |
|---|---|---|
| `tty` | `/dev/ttyS1` | PL UART16550（CH32 命令链路，3.125 MBd） |
| `spi_hz` | 2604166 | USB 模式 SPI 时钟；0 = 与裸机相同（651 kHz）。2.6 MHz 与内核驱动一致 |
| `iio_enable` | 1 | 0 = 只做 USB/OCH，不监听 libiio |
| `listen_port` | 30431 | libiio 端口 |
| `iiod` | `/usr/local/sbin/iiod` | 每个连接运行的 iiod；Debian 的 `/usr/sbin/iiod` 也能用，但没有零拷贝 |
| `quiet_ms` | 3000 | OCH 最后一条命令后多久才允许 libiio 接管 |
| `grace_ms` | 5000 | libiio 全部断开后多久切回 USB |
| `och_preempt` | 1 | OCH 命令收回 AD9361；0 = 回 `ERR busy owner=libiio` |
| `rt_priority` | 50 | USB 模式主循环的 SCHED_FIFO 优先级；0 = 普通调度 |
| `cpu` / `irq_cpu` | 1 / 1 | 主循环（及 iiod）所在 CPU / CH32、UART、SPI、PPS 中断所在 CPU；-1 = 不绑定 |

改完 `systemctl restart omnicored`。

## 测试结果

全部在实板上测得（PC：Windows 11，OCH 0.4.8，libiio 0.24），1.2.0 为刷卡后整卡验证。详细数据、测试方法和历次结果见 **[docs/TEST_RESULTS.md](docs/TEST_RESULTS.md)**，测试脚本在 [tests/](tests/)。

| 项目 | 结果 |
|---|---|
| 首次启动 | 通过：15.8 s 完成扩容/密钥/MAC，OCH 在内核时间 8.6 s 可用，无失败单元 |
| 重启到可用 | OCH 16.7 s、ping 34.0 s、SSH 34.7 s、libiio 40.7 s |
| USB 自检（RX PRBS / TX 回环） | 通过 |
| USB 60 s 全双工 | 184.3 MB/s（30.72M 12 bit）/ 245.7 MB/s（61.44M 8 bit），0 丢样、0 错误 |
| libiio 接管 / OCH 抢回 | 2.02–2.06 s / 1.38–2.03 s |
| USB 推流时连 libiio | 拒绝，错误码 16（EBUSY），推流不受影响 |
| libiio 吞吐 | 单路 RX 72–74 MB/s，双路 RX 75–81 MB/s |
| 数据完整性（TX→RX 数字回环计数器，逐样点） | 0 错误 |
| IIO TX 双音回环 | 通过 |
| 10 轮切换长测（接管 → 采集 → OCH 抢回 → 自检 → USB 推流） | 10/10 通过 |
| 两个客户端同时读同一设备 | 后来的收到 EBUSY，先来的不受影响 |
| 健壮性（中途杀客户端、反复建拆缓冲区） | 无内核告警，CMA 全部归还 |

**关于偶尔失败的切换轮次**：从 libiio 切回 USB 后立刻开始推流，开头偶尔会出现几次 RX overflow，固件会自动重同步，通常 1 s 内恢复。测试脚本把“恢复超过 1 s”判为失败，所以长测里偶尔会有一轮失败（历次约 0–2/10）。这是 1.0.0 起就有的现象，与 Linux 侧改动无关（同一内核上用新、旧用户态交替对照，次数相当），不影响后续推流，属于正常现象。

## 注意事项

- 开机到 OCH 可用要十几秒（Linux 启动），裸机约 1 s。期间 `och info` 会超时，等待即可。
- 内核启动时会按设备树先初始化一次 AD9361，随后 omnicored 接管，按 SDK 流程重新复位初始化。
  - 设备树中的 AD9361 参数与 `board_config.c` 一致：LVDS、`rx_data_delay=4`、`tx_fb_clock_delay=7`、`lvds_invert` 0xFF/0x0F、DCXO 8/5920。
- 一个已经打开设备、但超过 `quiet_ms` 没发命令的 OCH 程序，可能被 libiio 接管。它的下一条命令会收回 AD9361，但之前下发的 RF 配置已经变成冷启动默认值。
- 每个 libiio 连接是独立的 iiod 进程，所以两个客户端不能同时读同一个设备的数据流（后来的收到 EBUSY）。同一客户端的上下文连接与 RX、TX 缓冲区连接互不影响。
- 以太网 PHY 为 RTL8211FD（`phy-mode = "rgmii-id"`）；PHYRSTB 接 PS_POR#，无需 GPIO 复位。
- SD 卡经 TXS0108 电平转换、无卡检测：U-Boot 和 Linux 都把 SD 时钟限制在 25 MHz，并禁止 1.8 V 信令。
- QSPI W25Q256（32 MB）在 Linux 下为 `/dev/mtd0`。
- 板子没有 RTC，镜像里也没有 NTP 客户端，系统时间不准；看日志时以单调时间（`journalctl -o short-monotonic`）为准。

## 目录结构

```text
build.sh                   主机入口（docker build + run）
VERSION / FIRMWARE_LOCK    本仓库版本 / 已验证的固件提交
docker/Dockerfile          构建环境（Debian bookworm + arm 交叉工具链 + debootstrap/qemu）
scripts/                   build-all.sh / rootfs.sh / build-iiod.sh / image.sh / version-files.sh
patches/linux/             内核补丁：IIO DMA 块零拷贝
patches/iiod/              iiod 补丁：零拷贝发送、按连接运行、占用检查
dts/                       Linux 设备树；dts/u-boot/ U-Boot 设备树
configs/                   kernel.fragment / uboot.fragment / boot.cmd
omnicored/                 守护进程：omnicored.c（仲裁 + 按连接启动 iiod）hal.c hal_tty.c shim/（BSP 头替换）
rootfs-overlay/            systemd 单元、dhcpcd、avahi、omnicored.conf、首次启动脚本
tests/                     板上测试脚本（PowerShell + Python，在 Windows PC 上运行）
docs/                      测试结果、图片（docs/images/make_figures.py 生成）
```

## 版本历史

| 版本 | 内容 |
|---|---|
| 1.2.1 | 开机、串口登录和 SSH 连接显示与 OCH 相同的 OmniCore SDR ASCII 标识；统一标识源文件 `configs/banner.txt` |
| 1.2.0 | libiio 零拷贝（内核 + iiod 补丁，每个连接一个 iiod，低端内存 CMA），libiio 吞吐 58 → 80 MB/s；单路 RX 可连续 20 MSPS |
| 1.1.0 | OCH 优先抢占；EBUSY 拒绝；USB 模式实时调度和中断绑定；splice 代理；启动提速（OCH 24.7 → 16.6 s）；掉电保护；精简内核；FIRMWARE_LOCK 与 bitstream 追溯 |
| 1.0.0 | 首个版本：SDK 命令固件在 Linux 上运行，USB/OCH 与 libiio 共存 |
