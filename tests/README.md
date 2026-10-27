# 板上测试脚本

在 Windows PC 上运行（PowerShell 7），PC 通过 USB3 和网口连到板子。需要：

- OCH 主机工具 `och`（[OmniCore-SDR-Host](https://github.com/Liren66/OmniCore-SDR-Host)）在 PATH 里；
- libiio 0.24 Windows 工具（`iio_attr`、`iio_readdev`、`iio_writedev`）在 PATH 里；
- Python 3（只用标准库）；
- 能免密 SSH 到 `root@192.168.2.1`（`boot_timing.ps1`、`switching.ps1`、`soak.ps1` 会读日志）。

结果默认写到 `tests/results/`。所有测试 TX 衰减保持 89.75 dB；IIO TX 测试只用 AD9361 内部数字回环，不发射。

| 脚本 | 测什么 |
|---|---|
| `switching.ps1` | USB 自检 → libiio 接管耗时 → IIO 采集中发 OCH 命令（抢回）→ USB 推流时连 libiio（应为 EBUSY 16）→ libiio 2/4 通道吞吐 |
| `soak.ps1 -Rounds 10 -BenchSeconds 60` | 两个 60 s USB 全双工 bench，然后 N 轮：libiio 接管 → LO 设置回读 → 8 MiB IIO 采集 → OCH 抢回 → 自检 → 5 s 全双工 bench |
| `iio_integrity.ps1` | 数据完整性：DDS 循环发 4 路 12 bit 计数器 → AD9361 数字回环 → RX 采集多个 DMA 块，逐样点检查（`iio_counter.py`） |
| `iio_integrity.ps1 -Rate 20000000 -Channels 2 -Samples 536870912 -GapsOnly` | 连续性：在给定采样率下长时间采集，统计 DMA 块之间的断档（0 断档 = 以太网跟得上该采样率） |
| `iio_tx_loopback.ps1` | IIO TX：两路不同频率的单音经数字回环，检查 RX1/RX2 上各自的频点（`iio_tx_tone.py`） |
| `iio_throughput.ps1` | libiio RX 吞吐（2 / 4 通道，多次） |
| `boot_timing.ps1` | 软重启后 OCH、ping、SSH、libiio 各自可用的时间，以及 systemd-analyze |

`soak.ps1` 的 bench 判定沿用 OCH 的稳定性规则：推流开头的 RX overflow 必须在 1 s 内恢复。从 libiio 切回 USB 后立刻推流，偶尔会有一轮恢复超过 1 s 被判失败，这是已知的正常现象（见主 README 的“测试结果”）。

脚本之间的约定：两次 libiio 操作的间隔不要超过 `grace_ms`（默认 5 s），否则板子会先切回 USB 模式，下一次操作又要重新接管。
