"""Generate the README figures (SVG, light + dark via prefers-color-scheme).

usage: python docs/images/make_figures.py docs/images
Numbers come from the board measurements in the test report (1.1.0 / 1.2.0 full-card runs).
"""
import os
import sys

OUT = sys.argv[1]
FONT = "system-ui, -apple-system, 'Segoe UI', 'PingFang SC', 'Microsoft YaHei', 'Noto Sans CJK SC', sans-serif"

STYLE = """
<style>
  svg { --surface:#fcfcfb; --ink:#0b0b0b; --ink2:#52514e; --muted:#898781; --grid:#e1e0d9; --axis:#c3c2b7;
        --s1:#2a78d6; --s2:#eb6834; --s3:#1baf7a; --good:#0ca30c; --crit:#d03b3b;
        --box:#f3f2ee; --box-line:#c3c2b7; --hl:#e6f0fc; --hl-line:#2a78d6; --warm:#fdeee6; --warm-line:#eb6834; }
  @media (prefers-color-scheme: dark) {
    svg { --surface:#1a1a19; --ink:#f0efec; --ink2:#c3c2b7; --muted:#898781; --grid:#2c2c2a; --axis:#383835;
          --s1:#3987e5; --s2:#d95926; --s3:#199e70; --good:#0ca30c; --crit:#d03b3b;
          --box:#242422; --box-line:#4a4a46; --hl:#1b2a3d; --hl-line:#3987e5; --warm:#3a2419; --warm-line:#d95926; }
  }
  text { font-family: %s; fill: var(--ink); }
  .t { font-size: 17px; font-weight: 600; }
  .st { font-size: 12.5px; fill: var(--ink2); }
  .lb { font-size: 13px; fill: var(--ink2); }
  .sm { font-size: 11.5px; fill: var(--muted); }
  .v { font-size: 12.5px; font-weight: 600; fill: var(--ink); font-variant-numeric: tabular-nums; }
  .tick { font-size: 11px; fill: var(--muted); font-variant-numeric: tabular-nums; }
  .halo { paint-order: stroke; stroke: var(--surface); stroke-width: 5px; stroke-linejoin: round; }
  .box { fill: var(--box); stroke: var(--box-line); stroke-width: 1; }
  .hl { fill: var(--hl); stroke: var(--hl-line); stroke-width: 1.5; }
  .warm { fill: var(--warm); stroke: var(--warm-line); stroke-width: 1.5; }
  .bt { font-size: 13px; font-weight: 600; }
  .bs { font-size: 11.5px; fill: var(--ink2); }
  .wire { stroke: var(--ink2); stroke-width: 1.5; fill: none; }
  .wire-hl { stroke: var(--s1); stroke-width: 2.5; fill: none; }
  .wire-w { stroke: var(--s2); stroke-width: 2.5; fill: none; }
</style>
""" % FONT


def tw(s, size):
    """Rough text width: CJK glyphs are square, Latin about 0.56 em."""
    return sum(size if ord(c) > 0x2E80 else size * 0.56 for c in s)


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


class Svg:
    def __init__(self, w, h, title, desc):
        self.w, self.h = w, h
        self.parts = [
            '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d" role="img">' % (w, h, w, h),
            "<title>%s</title><desc>%s</desc>" % (esc(title), esc(desc)),
            STYLE,
            '<defs><marker id="ah" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">'
            '<path d="M0,0 L10,5 L0,10 z" style="fill:var(--ink2)"/></marker>'
            '<marker id="ahb" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" markerHeight="6" orient="auto-start-reverse">'
            '<path d="M0,0 L10,5 L0,10 z" style="fill:var(--s1)"/></marker>'
            '<marker id="ahw" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" markerHeight="6" orient="auto-start-reverse">'
            '<path d="M0,0 L10,5 L0,10 z" style="fill:var(--s2)"/></marker></defs>',
            '<rect width="100%" height="100%" rx="10" style="fill:var(--surface)"/>',
        ]

    def add(self, s):
        self.parts.append(s)

    def text(self, x, y, s, cls="lb", anchor="start", extra=""):
        if cls in ("v", "sm"):
            cls += " halo"
        self.add('<text x="%.1f" y="%.1f" class="%s" text-anchor="%s" %s>%s</text>' % (x, y, cls, anchor, extra, esc(s)))

    def save(self, name):
        self.parts.append("</svg>")
        with open(os.path.join(OUT, name), "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(self.parts) + "\n")


def hbar(svg, x0, y, w, h, color):
    """Horizontal bar anchored at x0, 4px rounded data end only."""
    r = min(4, w / 2, h / 2)
    if w <= 0:
        return
    svg.add('<path d="M%.1f,%.1f H%.1f a%.1f,%.1f 0 0 1 %.1f,%.1f V%.1f a%.1f,%.1f 0 0 1 -%.1f,%.1f H%.1f Z" style="fill:%s"/>'
            % (x0, y, x0 + w - r, r, r, r, r, y + h - r, r, r, r, r, x0, color))


def x_axis(svg, x0, x1, ytop, ybot, vmax, step, fmt="%g", unit=""):
    for v in range(0, int(vmax) + 1, step):
        x = x0 + (x1 - x0) * v / vmax
        svg.add('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" style="stroke:var(--grid);stroke-width:1"/>' % (x, ytop, x, ybot))
        svg.text(x, ybot + 16, (fmt % v) + (unit if v == vmax else ""), "tick", "middle")
    svg.add('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" style="stroke:var(--axis);stroke-width:1"/>' % (x0, ytop, x0, ybot))


def legend(svg, x, y, items):
    for label, color in items:
        svg.add('<rect x="%.1f" y="%.1f" width="12" height="12" rx="3" style="fill:%s"/>' % (x, y - 10, color))
        svg.text(x + 18, y, label, "lb")
        x += 18 + tw(label, 13) + 22


# ---------------------------------------------------------------- charts

def throughput():
    s = Svg(760, 310, "libiio RX 吞吐：1.1.0 vs 1.2.0",
            "Grouped bars. 1.1.0 (splice proxy): single RX 56.7 MB/s, dual RX 59.4 MB/s. "
            "1.2.0 (zero-copy): single RX 72.2 MB/s, dual RX 79.5 MB/s. Gigabit Ethernet payload limit about 117 MB/s.")
    s.text(24, 34, "libiio 以太网 RX 吞吐（板子 → PC，iio_readdev，-b 1M）", "t")
    s.text(24, 54, "1.2.0 零拷贝比 1.1.0 代理方案快 27–34%；数据为刷卡后整卡验证的实测值", "st")
    legend(s, 24, 84, [("单路 RX（I/Q，2 个通道）", "var(--s1)"), ("双路 RX（I/Q×2，4 个通道）", "var(--s2)")])
    x0, x1, vmax = 190, 700, 120
    ytop, ybot = 112, 262
    x_axis(s, x0, x1, ytop, ybot, vmax, 20, unit=" MB/s")
    xl = x0 + (x1 - x0) * 117 / vmax
    s.add('<line x1="%.1f" y1="%d" x2="%.1f" y2="%d" style="stroke:var(--ink2);stroke-width:1.5;stroke-dasharray:4 4"/>' % (xl, ytop, xl, ybot))
    s.text(xl, ytop - 6, "千兆以太网上限 ≈117", "sm", "middle")
    groups = [("1.1.0", "splice 代理", 56.7, 59.4), ("1.2.0", "零拷贝", 72.2, 79.5)]
    bh, gap = 22, 2
    for i, (ver, how, a, b) in enumerate(groups):
        gy = 126 + i * 70
        s.text(x0 - 12, gy + 18, ver, "bt", "end")
        s.text(x0 - 12, gy + 36, how, "bs", "end")
        for j, (v, col) in enumerate(((a, "var(--s1)"), (b, "var(--s2)"))):
            y = gy + j * (bh + gap)
            w = (x1 - x0) * v / vmax
            hbar(s, x0, y, w, bh, col)
            s.text(x0 + w + 6, y + 16, "%.1f" % v, "v")
    s.save("throughput.svg")


def samplerate():
    rows = [(15.36, 60.4, 0), (17.5, 68.7, 0), (18.5, 72.6, 0), (20.0, 79.2, 0), (21.0, 82.1, 0), (22.0, 82.5, 158)]
    s = Svg(760, 372, "单路 RX 连续采集：各采样率能否跟上",
            "Bars show the data rate each sample rate needs (4 bytes per complex sample). "
            "15.36, 17.5, 18.5, 20 and 21 MSPS: 0 gaps over 1 GiB. 22 MSPS: 158 gaps. Measured ceiling about 82 MB/s.")
    s.text(24, 34, "单路 RX（I/Q）能连续跑多快", "t")
    s.text(24, 54, "每个采样率连续抓 1 GiB（约 13 s，20 MSPS 另跑 2 GiB），检查 DMA 块之间有没有断档；实测吞吐上限约 82 MB/s", "st")
    x0, x1, vmax = 120, 540, 100
    ytop, ybot = 84, 330
    x_axis(s, x0, x1, ytop, ybot, vmax, 20, unit=" MB/s")
    s.text(x1 + 14, ytop - 4, "所需数据率", "sm")

    bh = 26
    for i, (rate, got, gaps) in enumerate(rows):
        y = 96 + i * 38
        need = rate * 4
        w = (x1 - x0) * need / vmax
        hbar(s, x0, y, w, bh, "var(--s1)")
        s.text(x0 - 12, y + 18, ("%.2f" % rate).rstrip("0").rstrip(".") + " MSPS", "bt", "end")
        s.text(x1 + 14, y + 18, "%.1f MB/s" % need, "v")
        ok = gaps == 0
        cx = 650
        if ok:
            s.add('<circle cx="%d" cy="%.1f" r="8" style="fill:var(--good)"/>' % (cx, y + 13))
            s.add('<path d="M%d,%.1f l3,3 l6,-6" style="stroke:#fff;stroke-width:2;fill:none"/>' % (cx - 4.5, y + 13))
            s.text(cx + 14, y + 18, "连续，0 断档", "lb")
        else:
            s.add('<circle cx="%d" cy="%.1f" r="8" style="fill:var(--crit)"/>' % (cx, y + 13))
            s.add('<path d="M%d,%.1f l7,7 M%d,%.1f l-7,7" style="stroke:#fff;stroke-width:2"/>' % (cx - 3.5, y + 9.5, cx + 3.5, y + 9.5))
            s.text(cx + 14, y + 18, "%d 次断档" % gaps, "lb")
    s.save("samplerate.svg")


def cpu():
    s = Svg(760, 280, "4 通道 libiio 读流时的 CPU 负载",
            "Grouped bars. Copy path (1.1.0 way, 63 MB/s): CPU1 (iiod) 84 percent, CPU0 (Ethernet softirq) 42 percent. "
            "Zero-copy (1.2.0, 85 MB/s): CPU1 18 percent, CPU0 74 percent.")
    s.text(24, 34, "读流时两个核的负载（iiod 绑在 CPU1）", "t")
    s.text(24, 54, "拷贝方式 63 MB/s 时 CPU1 已经跑满；零拷贝 85 MB/s 时 CPU1 只剩 18%，瓶颈转到网卡中断和链路", "st")
    legend(s, 24, 84, [("拷贝（write，1.1.0 方式）", "var(--s1)"), ("零拷贝（vmsplice+splice，1.2.0）", "var(--s2)")])
    x0, x1, vmax = 190, 700, 100
    ytop, ybot = 102, 238
    x_axis(s, x0, x1, ytop, ybot, vmax, 20, unit=" %")
    groups = [("CPU1", "iiod 搬数据", 84, 18), ("CPU0", "网卡中断 softirq", 42, 74)]
    bh, gap = 22, 2
    for i, (cpuname, what, a, b) in enumerate(groups):
        gy = 112 + i * 64
        s.text(x0 - 12, gy + 18, cpuname, "bt", "end")
        s.text(x0 - 12, gy + 36, what, "bs", "end")
        for j, (v, col) in enumerate(((a, "var(--s1)"), (b, "var(--s2)"))):
            y = gy + j * (bh + gap)
            w = (x1 - x0) * v / vmax
            hbar(s, x0, y, w, bh, col)
            s.text(x0 + w + 6, y + 16, "%d%%" % v, "v")
    s.save("cpu-load.svg")


def boot():
    s = Svg(760, 210, "软重启后各服务可用时间（1.2.0）",
            "Timeline from the reboot command: OCH ready 16.7 s, ping 34.0 s, SSH 34.7 s, libiio 40.7 s.")
    s.text(24, 34, "重启到可用（从发出 reboot 计时，1.2.0 整卡实测）", "t")
    s.text(24, 54, "OCH（USB3）不等网络，16.7 s 就能用；以太网要等 DHCP 超时后回落到 192.168.2.1", "st")
    x0, x1, vmax = 40, 720, 45
    ya = 150
    for v in range(0, 46, 5):
        x = x0 + (x1 - x0) * v / vmax
        s.add('<line x1="%.1f" y1="%d" x2="%.1f" y2="%d" style="stroke:var(--grid);stroke-width:1"/>' % (x, 80, x, ya))
        s.text(x, ya + 18, "%d%s" % (v, " s" if v == 45 else ""), "tick", "middle")
    s.add('<line x1="%d" y1="%d" x2="%d" y2="%d" style="stroke:var(--axis);stroke-width:1.5"/>' % (x0, ya, x1, ya))
    # (seconds, label, stem level, label side)
    marks = [(16.7, "OCH 可用", 0, 1), (34.0, "ping 通", 1, -1), (34.7, "SSH", 1, 1), (40.7, "libiio", 0, 1)]
    for v, name, lift, side in marks:
        x = x0 + (x1 - x0) * v / vmax
        top = 110 - lift * 22
        s.add('<line x1="%.1f" y1="%d" x2="%.1f" y2="%d" style="stroke:var(--s1);stroke-width:2"/>' % (x, top, x, ya))
        s.add('<circle cx="%.1f" cy="%d" r="5" style="fill:var(--s1);stroke:var(--surface);stroke-width:2"/>' % (x, ya))
        s.text(x + 6 * side, top + 4, "%s %.1f s" % (name, v), "v", "start" if side > 0 else "end")
    s.text(x0, 192, "首次启动另加约 16 s：扩展 rootfs 到整卡、生成 SSH 主机密钥和固定 MAC（OCH 不受影响，内核时间 8.6 s 可用）", "sm")
    s.save("boot-timeline.svg")


# ---------------------------------------------------------------- diagrams

def box(s, x, y, w, h, title, sub=(), cls="box"):
    s.add('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="8" class="%s"/>' % (x, y, w, h, cls))
    s.text(x + w / 2, y + 22, title, "bt", "middle")
    for i, line in enumerate(sub):
        s.text(x + w / 2, y + 40 + i * 16, line, "bs", "middle")


def arrow(s, pts, cls="wire", marker="ah", label=None, lx=None, ly=None, anchor="middle"):
    d = "M" + " L".join("%.1f,%.1f" % p for p in pts)
    s.add('<path d="%s" class="%s" marker-end="url(#%s)"/>' % (d, cls, marker))
    if label:
        s.text(lx, ly, label, "sm", anchor)


def architecture():
    s = Svg(900, 470, "OmniCore SDR Linux 架构",
            "OCH host connects over USB3 to the CH32H417 bridge; commands reach omnicored through the PL UART, samples flow "
            "through the CH32H417 IP. libiio clients connect over Ethernet to omnicored, which forks one patched iiod per "
            "connection; iiod streams AD9361 DMA blocks to the socket without copies. omnicored arbitrates the AD9361.")
    s.text(24, 34, "系统架构", "t")
    s.text(24, 54, "同一块 AD9361：USB 模式归 OCH（SDK 原样运行在用户态），IIO 模式归内核驱动 + libiio；omnicored 负责仲裁，OCH 优先", "st")
    # hosts
    box(s, 24, 86, 170, 70, "OCH 主机", ("och CLI / SDK 命令协议",))
    box(s, 24, 318, 170, 70, "libiio 客户端", ("iio_readdev / GNU Radio …",))
    # zynq outline
    s.add('<rect x="236" y="74" width="640" height="370" rx="12" style="fill:none;stroke:var(--box-line);stroke-dasharray:5 4"/>')
    s.text(252, 96, "Zynq-7020（PS：Linux 6.1 + Debian 12）  /  PL：原厂 bitstream，不改动", "sm")
    box(s, 260, 110, 170, 70, "CH32H417", ("USB3 桥（板上）",))
    box(s, 470, 110, 200, 84, "omnicored", ("裸机 SDK 命令代码", "spidev · /dev/mem · UIO"), "hl")
    box(s, 700, 110, 156, 70, "AD9361", ("RF 收发器",))
    box(s, 260, 230, 170, 70, "CH32H417 IP（PL）", ("USB 样点通路",))
    box(s, 700, 230, 156, 70, "AXI AD9361 接口", ("ADC/DAC 数据 + DMA",))
    box(s, 470, 318, 200, 84, "iiod -i（每个连接一个）", ("vmsplice + splice", "DMA 块零拷贝发往 socket"), "warm")
    box(s, 700, 340, 156, 70, "内核驱动", ("ad9361 · IIO buffer-dma",))
    # wires
    arrow(s, [(194, 121), (258, 121)], label="USB3", lx=226, ly=114)
    arrow(s, [(430, 132), (468, 132)], label="UART", lx=449, ly=125)
    arrow(s, [(670, 140), (698, 140)])
    s.text(684, 160, "SPI", "sm", "middle")
    arrow(s, [(345, 180), (345, 228)], label="样点", lx=352, ly=208, anchor="start")
    arrow(s, [(778, 180), (778, 228)])
    arrow(s, [(698, 265), (432, 265)], label="USB 模式数据", lx=500, ly=258)
    arrow(s, [(194, 353), (468, 353)], label="以太网 :30431", lx=330, ly=346)
    arrow(s, [(610, 194), (610, 316)], label="accept → fork", lx=618, ly=300, anchor="start")
    arrow(s, [(778, 300), (778, 338)])
    arrow(s, [(698, 372), (672, 372)], cls="wire-w", marker="ahw")
    s.text(24, 430, "USB 模式：SCHED_FIFO 50 跑在 CPU1，CH32/UART/SPI/PPS 中断也在 CPU1", "sm")
    s.text(24, 450, "IIO 模式：每个 libiio 连接 = 一个 iiod 进程，同样在 CPU1；网卡中断在 CPU0", "sm")
    s.save("architecture.svg")


def zerocopy():
    s = Svg(900, 400, "libiio 数据路径：1.1.0 代理 vs 1.2.0 零拷贝",
            "Before: DMA block (uncached) is copied by iiod write() into a loopback TCP socket, then omnicored splices it "
            "into the Ethernet socket. After: iiod vmsplices the DMA block pages into a pipe and splices them into the client "
            "socket; the GEM NIC reads the same pages; the kernel keeps the block from the DMA until the pages are released.")
    s.text(24, 34, "数据路径对比", "t")
    s.text(24, 54, "1.2.0 去掉了本地代理，DMA 块的物理页直接挂进 TCP 报文，CPU 不再逐字节读不可缓存内存", "st")
    # before
    s.text(24, 90, "1.1.0", "bt")
    s.text(24, 108, "约 58 MB/s", "bs")
    xs = [100, 270, 440, 610]
    names = [("DMA 块", ("不可缓存内存",)), ("iiod write()", ("CPU 拷贝 ①",)), ("回环 TCP", ("omnicored splice",)),
             ("网卡 GEM", ("每包 cache 清理",))]
    for x, (t, sub) in zip(xs, names):
        box(s, x, 74, 150, 62, t, sub, "warm" if t == "iiod write()" else "box")
    for a, b in zip(xs, xs[1:]):
        arrow(s, [(a + 150, 105), (b - 2, 105)])
    box(s, 790, 74, 90, 62, "PC", ("",))
    arrow(s, [(760, 105), (788, 105)])
    # after
    s.text(24, 204, "1.2.0", "bt")
    s.text(24, 222, "约 80 MB/s", "bs")
    names2 = [("DMA 块", ("vm_insert_page 映射",)), ("vmsplice", ("页引用进管道",)), ("splice → socket", ("skb 直接引用这些页",)),
              ("网卡 GEM", ("DMA 读同一批页",))]
    for x, (t, sub) in zip(xs, names2):
        box(s, x, 188, 150, 62, t, sub, "hl")
    for a, b in zip(xs, xs[1:]):
        arrow(s, [(a + 150, 219), (b - 2, 219)], cls="wire-hl", marker="ahb")
    box(s, 790, 188, 90, 62, "PC", ("",))
    arrow(s, [(760, 219), (788, 219)], cls="wire-hl", marker="ahb")
    # guard loop
    s.add('<path d="M685,252 C685,300 175,300 175,254" class="wire" style="stroke-dasharray:5 4" marker-end="url(#ah)"/>')
    s.text(430, 322, "TCP 确认、网络栈释放页引用之后，内核才把块交回 DMA / 释放（deferred 队列）", "sm", "middle")
    s.text(24, 362, "关键点：CMA 放在低端内存（cma=128M@0-0x30000000），否则 GEM 没有 HIGHDMA 能力，", "sm")
    s.text(24, 380, "GSO 分段时会把高端内存的页再拷一遍；数据必须直达客户端 socket，经回环代理会被 TCP 合并拷贝。", "sm")
    s.save("zero-copy.svg")


def switching():
    s = Svg(900, 330, "AD9361 归属切换（OCH 优先）",
            "USB mode is the default. A libiio connection while USB is idle switches to libiio mode in about 2.0 s; "
            "otherwise the client gets EBUSY. Any OCH command in libiio mode preempts: clients are dropped and the AD9361 "
            "returns to USB mode in 1.4 to 2.1 s, then the queued command runs. 5 s after the last client leaves it returns too.")
    s.text(24, 34, "模式切换：OCH 优先于 libiio", "t")
    s.text(24, 54, "排队的 OCH 命令在切回 USB 后按顺序执行，整个过程在 OCH 默认 5 s 超时之内", "st")
    box(s, 60, 120, 220, 96, "USB 模式（默认）", ("SDK 经 spidev 控制 AD9361", "主循环 SCHED_FIFO 50 · CPU1"), "hl")
    box(s, 620, 120, 220, 96, "libiio 模式", ("内核 ad9361 / cf_axi 驱动", "每个连接一个零拷贝 iiod"), "warm")
    arrow(s, [(282, 140), (618, 140)])
    s.text(450, 120, "libiio 连接 + USB 空闲（无推流/TX，3 s 内无 OCH 命令）", "lb", "middle")
    s.text(450, 157, "约 2.0 s（含 AD9361 LVDS 接口校准约 1.4 s）", "sm", "middle")
    arrow(s, [(618, 196), (282, 196)])
    s.text(450, 190, "任何 OCH 命令：先排队，断开 libiio，收回 AD9361", "lb", "middle")
    s.text(450, 214, "1.4–2.1 s 后执行排队的命令", "sm", "middle")
    s.text(450, 250, "最后一个 libiio 客户端断开 5 s 后也会切回 USB 模式", "sm", "middle")
    s.add('<path d="M120,218 C110,292 230,292 220,220" class="wire" marker-end="url(#ah)"/>')
    s.text(170, 300, "USB 正忙时来的 libiio 连接：回 EBUSY(16)", "sm", "middle")
    s.add('<path d="M680,218 C670,292 790,292 780,220" class="wire" marker-end="url(#ah)"/>')
    s.text(730, 300, "同一设备已在读流：新客户端 EBUSY，原流不受影响", "sm", "middle")
    s.save("mode-switch.svg")


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for f in (throughput, samplerate, cpu, boot, architecture, zerocopy, switching):
        f()
    print("written:", sorted(os.listdir(OUT)))
