# 第三方与组件许可说明

根目录 [LICENSE](LICENSE) 保存 GNU GPL v3.0 全文。本项目可自主授权、且未另行注明许可的
原创代码、脚本、配置和文档采用 `GPL-3.0-only`，不包含“或任何后续版本”的额外授权。
已有文件级许可声明、以下组件许可和第三方条款优先于该默认声明。

## 仓库中的单独许可组件

- `dts/zynq-omnicore-sdr.dts`：保留文件头的 `GPL-2.0` 声明（GPL v2 only）。
- `dts/u-boot/zynq-omnicore.dts`：保留文件头的 `GPL-2.0+` 声明（GPL v2 或后续版本）。
- `patches/linux/`：针对 ADI Linux 的补丁及本项目对相应内核文件的修改，沿用上游内核的 GPL v2 条款，
  不因根目录 LICENSE 而改为 GPL v3。参见 [ADI Linux 2023_R2 COPYING](https://github.com/analogdevicesinc/linux/blob/2023_R2/COPYING)
  及被修改文件的声明。
- `patches/iiod/`：针对 libiio v0.24 中 iiod 的补丁及本项目对相应文件的修改，沿用被修改文件的上游许可。
  例如 [iiod.c](https://github.com/analogdevicesinc/libiio/blob/v0.24/iiod/iiod.c) 标注 `LGPL-2.1-or-later`。

补丁中的原有代码、上下文及版权声明仍属于相应权利人。以上为特定组件的单独许可范围，
不是对 GNU GPL v3.0 正文的修改或额外限制。

## 构建时引入的第三方材料

`scripts/build-all.sh` 下载上游源码；`omnicored/Makefile` 还会直接编译外部 Firmware 仓库中的
SDK 与驱动源文件。将这些材料参与构建不意味着将其重新授权为本项目默认许可证。

| 组件 | 构建来源 / 位置 | 适用许可 |
|---|---|---|
| ADI Linux | `analogdevicesinc/linux` 的 `2023_R2`，缓存于 `.cache/linux/` | 以上游 `COPYING`、`LICENSES/` 和各文件声明为准；内核总体为 GPL v2 only，并带 Linux 系统调用说明。 |
| U-Boot | `u-boot/u-boot` 的 `v2024.01`，缓存于 `.cache/u-boot/` | 保留上游许可文件与各文件 SPDX 声明；本次不改变其许可。 |
| libiio / iiod | `analogdevicesinc/libiio` 的 `v0.24`，缓存于 `.cache/libiio/` | 保留上游许可文件及各文件声明，包括 iiod 的 LGPL-2.1-or-later。 |
| Debian 12 armhf 软件包 | `scripts/rootfs.sh` 等构建步骤安装的根文件系统 | 各软件包独立授权，具体查看软件包的版权与许可信息，例如 `/usr/share/doc/<package>/copyright`。 |
| Firmware SDK、AD9361 与 CH32H417_IP 驱动 | `FIRMWARE_DIR` 指向的 `OmniCore-SDR-Firmware` | 保留实际构建版本的文件级版权和许可。部分 ADI no-OS 文件包含在 ADI 器件上运行或直接连接 ADI 器件的条件；根目录 GPL 不取消这些限制。 |
| PS 初始化代码与 FPGA 位流 | Firmware 的 `system_wrapper_hw_platform_0/`，包括 `ps7_init_gpl.c/h` 和 `system_wrapper.bit` | 按源文件及构成位流的各 IP 的原有许可处理，不因复制到启动分区而统一变为 GPL v3。 |

`FIRMWARE_LOCK` 记录的是验证过的固件提交；适用条款应按实际使用的提交核对，不能用另一个仓库
后续新增的许可证代替历史版本的文件声明。

## 构建产物

SD 卡镜像是多种组件的集合，各组件保留自身许可。`omnicored` 将本项目源文件与外部 SDK、
驱动编译到同一程序，其分发条件还取决于这些输入文件的许可及兼容性。
本次添加 LICENSE 不表示组合程序、位流、整卡镜像或既有 Release 的全部内容已转换为 GPL-3.0，
也不表示已完成全部依赖的许可兼容性确认。原有版权、许可和免责声明应继续保留。
