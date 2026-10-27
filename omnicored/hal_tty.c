/* AXI UART16550 register emulation for console.c on top of the kernel 8250 tty.
 *
 * console.c polls LSR/RBR and writes THR exactly as on bare metal. Here LSR
 * reports DATA_READY from a local receive buffer, an error/break when the
 * kernel's line error counters move, and TX always ready (output is buffered
 * and written per line). An RX FIFO reset through FCR discards pending input.
 * MCR loopback feeds THR bytes back to the receiver for the IRQ self-test.
 */
#include <asm/ioctls.h>
#include <asm/termbits.h>
#include <linux/serial.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "hal.h"
#include "xuartns550_l.h"

int ioctl(int fd, unsigned long request, ...);

#define RX_CAP 4096U
#define TX_CAP 8192U
#define RX_REFILL_US 100U

static int fd = -1;
static char tty_path[128];
static unsigned char rx[RX_CAP];
static unsigned rx_head, rx_len;
static unsigned char tx[TX_CAP];
static unsigned tx_len;
static int muted;
static uint64_t last_refill;
static struct serial_icounter_struct counters;
static int error_pending;
static int loopback;
static const char *fpga_id;

static unsigned line_errors(const struct serial_icounter_struct *c)
{
    return (unsigned)(c->overrun + c->frame + c->parity + c->brk + c->buf_overrun);
}

static void check_errors(void)
{
    struct serial_icounter_struct now;
    if (ioctl(fd, TIOCGICOUNT, &now) < 0) return;
    if (line_errors(&now) != line_errors(&counters)) error_pending = 1;
    counters = now;
}

static void refill(int force)
{
    uint64_t now = hal_now_us();
    ssize_t n;
    if (fd < 0 || rx_len == RX_CAP) return;
    if (!force && now - last_refill < RX_REFILL_US) return;
    last_refill = now;
    if (rx_len == 0) rx_head = 0;
    if (rx_head + rx_len < RX_CAP) {
        n = read(fd, rx + rx_head + rx_len, RX_CAP - rx_head - rx_len);
        if (n > 0) rx_len += (unsigned)n;
    }
    check_errors();
}

static int configure(unsigned baud)
{
    struct termios2 t;
    struct serial_struct ss;
    char path[160];
    const char *name = strrchr(tty_path, '/');
    if (ioctl(fd, TCGETS2, &t) < 0) return -errno;
    t.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXOFF);
    t.c_oflag &= ~OPOST;
    t.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS | CBAUD);
    t.c_cflag |= CS8 | CLOCAL | CREAD | BOTHER;
    t.c_ispeed = t.c_ospeed = baud;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (ioctl(fd, TCSETS2, &t) < 0) return -errno;
    /* Small bursts at 3.125 MBd: interrupt on every byte, push to the ldisc at once. */
    if (ioctl(fd, TIOCGSERIAL, &ss) == 0) {
        ss.flags |= ASYNC_LOW_LATENCY;
        (void)ioctl(fd, TIOCSSERIAL, &ss);
    }
    if (name) {
        int f;
        snprintf(path, sizeof(path), "/sys/class/tty%s/rx_trig_bytes", name);
        f = open(path, O_WRONLY);
        if (f >= 0) {
            if (write(f, "1", 1) < 0) {}
            close(f);
        }
    }
    (void)ioctl(fd, TIOCGICOUNT, &counters);
    return 0;
}

int hal_tty_open(const char *path)
{
    snprintf(tty_path, sizeof(tty_path), "%s", path);
    fd = open(tty_path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        hal_log("open %s: %s", tty_path, strerror(errno));
        return -errno;
    }
    return 0;
}

int hal_tty_fd(void) { return fd; }
int hal_tty_rx_pending(void) { return rx_len != 0; }

void hal_tty_mute(int mute)
{
    hal_tty_flush();
    muted = mute;
}

void hal_tty_set_fpga_id(const char *id) { fpga_id = id && *id ? id : NULL; }

/* The SDK prints "fpga=unknown" in system_info; the Linux build knows the bitstream. */
static void patch_fpga_id(void)
{
    static const char key[] = " fpga=unknown ";
    const size_t klen = sizeof(key) - 1;
    unsigned char *hit;
    size_t idlen, tail;
    if (!fpga_id || tx_len < 12 || memcmp(tx, "system_info ", 12)) return;
    hit = memmem(tx, tx_len, key, klen);
    if (!hit) return;
    idlen = strlen(fpga_id);
    if (tx_len - klen + 7 + idlen > TX_CAP) return;
    tail = tx_len - (size_t)(hit - tx) - klen;
    memmove(hit + 7 + idlen, hit + klen, tail);
    memcpy(hit, " fpga=", 6);
    memcpy(hit + 6, fpga_id, idlen);
    hit[6 + idlen] = ' ';
    tx_len = (unsigned)(tx_len - klen + 7 + idlen);
}

void hal_tty_flush(void)
{
    unsigned off = 0;
    patch_fpga_id();
    while (fd >= 0 && off < tx_len) {
        ssize_t n = write(fd, tx + off, tx_len - off);
        if (n > 0) {
            off += (unsigned)n;
        } else if (n < 0 && errno == EAGAIN) {
            struct pollfd p = {fd, POLLOUT, 0};
            if (poll(&p, 1, 1000) <= 0) break;
        } else if (n < 0 && errno != EINTR) {
            break;
        }
    }
    tx_len = 0;
}

u32 XUartNs550_ReadReg(UINTPTR base, u32 offset)
{
    (void)base;
    switch (offset) {
    case XUN_LSR_OFFSET: {
        u32 lsr = XUN_LSR_TX_BUFFER_EMPTY | XUN_LSR_TX_EMPTY;
        if (rx_len == 0) refill(0);
        if (error_pending) {
            error_pending = 0;
            return lsr | XUN_LSR_OVERRUN_ERROR;
        }
        return rx_len ? lsr | XUN_LSR_DATA_READY : lsr;
    }
    case XUN_RBR_OFFSET: {
        unsigned char c;
        if (rx_len == 0) refill(1);
        if (rx_len == 0) return 0;
        c = rx[rx_head];
        rx_head = (rx_head + 1U) % RX_CAP;
        --rx_len;
        return c;
    }
    default:
        return 0;
    }
}

void XUartNs550_WriteReg(UINTPTR base, u32 offset, u32 value)
{
    (void)base;
    switch (offset) {
    case XUN_THR_OFFSET:
        if (loopback) {
            /* 16550 internal loopback (console IRQ self-test): the byte goes to
             * the receiver, never to the CH32, and the RX "interrupt" fires. */
            if (rx_len == 0) rx_head = 0;
            if (rx_head + rx_len < RX_CAP) rx[rx_head + rx_len++] = (unsigned char)value;
            hal_irq_tty_raise();
            return;
        }
        if (muted) return;
        tx[tx_len++] = (unsigned char)value;
        if (tx_len == TX_CAP || value == '\n') hal_tty_flush();
        break;
    case XUN_FCR_OFFSET:
        if (value & XUN_FIFO_RX_RESET) {
            if (fd >= 0) (void)ioctl(fd, TCFLSH, TCIFLUSH);
            rx_head = rx_len = 0;
        }
        break;
    case XUN_MCR_OFFSET:
        loopback = (value & XUN_MCR_LOOP) != 0;
        break;
    default: /* IER/LCR: the 8250 driver owns these. */
        break;
    }
}

void XUartNs550_SetBaud(UINTPTR base, u32 input_clock_hz, u32 baud)
{
    int rc;
    (void)base;
    (void)input_clock_hz;
    if (fd < 0) return;
    /* TCSETS2 rewrites LCR/divisor/FCR/IER, which also recovers a reset UART. */
    rc = configure(baud);
    if (rc) hal_log("configure %s at %u baud: %s", tty_path, baud, strerror(-rc));
}

void XUartNs550_SetLineControlReg(UINTPTR base, u32 value) { (void)base; (void)value; }
