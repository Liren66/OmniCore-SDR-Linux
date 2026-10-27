/* Linux back end for the BSP shims in shim/: /dev/mem, GPIO cdev, spidev, UIO, XADC sysfs. */
#include "hal.h"
#include "xil_io.h"
#include "xtime_l.h"
#include "xgpiops.h"
#include "xspips.h"
#include "xscugic.h"
#include "xadcps.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static HalConfig config;

void hal_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("omnicored: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

uint64_t hal_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000U;
}

void XTime_GetTime(XTime *t) { *t = hal_now_us(); }

/* ---------------------------------------------------------------- MMIO */

#define MMIO_WINDOW 0x10000U
static int mem_fd = -1;
static struct { uintptr_t base; volatile uint8_t *ptr; } windows[16];

volatile void *hal_mmio(UINTPTR addr)
{
    uintptr_t base = addr & ~(uintptr_t)(MMIO_WINDOW - 1U);
    unsigned i;
    void *p;
    for (i = 0; i < ARRAY_SIZE(windows) && windows[i].ptr; ++i) {
        if (windows[i].base == base) return windows[i].ptr + (addr - base);
    }
    if (i == ARRAY_SIZE(windows) || mem_fd < 0) {
        hal_log("fatal: cannot map register 0x%08lx", (unsigned long)addr);
        abort();
    }
    p = mmap(NULL, MMIO_WINDOW, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, (off_t)base);
    if (p == MAP_FAILED) {
        hal_log("fatal: mmap 0x%08lx: %s", (unsigned long)base, strerror(errno));
        abort();
    }
    windows[i].base = base;
    windows[i].ptr = p;
    return windows[i].ptr + (addr - base);
}

/* ---------------------------------------------------------------- sysfs helpers */

static int write_file(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY), rc = 0;
    size_t n = strlen(text);
    if (fd < 0) return -errno;
    if (write(fd, text, n) != (ssize_t)n) rc = -errno;
    close(fd);
    return rc;
}

static int read_file(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY);
    ssize_t n;
    if (fd < 0) return -errno;
    n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) return -errno;
    buf[n] = '\0';
    return (int)n;
}

/* Finds a device under /sys/bus/<bus>/devices whose OF compatible list contains compat. */
static int find_of_device(const char *bus, const char *compat, char *name, size_t cap)
{
    char path[PATH_MAX], buf[512];
    DIR *d;
    struct dirent *e;
    snprintf(path, sizeof(path), "/sys/bus/%s/devices", bus);
    d = opendir(path);
    if (!d) return -errno;
    while ((e = readdir(d)) != NULL) {
        int n, off;
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "/sys/bus/%s/devices/%s/of_node/compatible", bus, e->d_name);
        n = read_file(path, buf, sizeof(buf));
        for (off = 0; n > 0 && off < n; off += (int)strlen(buf + off) + 1) {
            if (strcmp(buf + off, compat) == 0) {
                snprintf(name, cap, "%s", e->d_name);
                closedir(d);
                return 0;
            }
        }
    }
    closedir(d);
    return -ENOENT;
}

static int bound_driver(const char *bus, const char *dev, char *drv, size_t cap)
{
    char path[PATH_MAX], target[PATH_MAX];
    ssize_t n;
    const char *slash;
    snprintf(path, sizeof(path), "/sys/bus/%s/devices/%s/driver", bus, dev);
    n = readlink(path, target, sizeof(target) - 1);
    if (n < 0) return 0;
    target[n] = '\0';
    slash = strrchr(target, '/');
    snprintf(drv, cap, "%s", slash ? slash + 1 : target);
    return 1;
}

static int sysfs_unbind(const char *bus, const char *dev)
{
    char drv[128], path[PATH_MAX];
    if (!bound_driver(bus, dev, drv, sizeof(drv))) return 0;
    snprintf(path, sizeof(path), "/sys/bus/%s/drivers/%s/unbind", bus, drv);
    return write_file(path, dev);
}

/* Binds and waits for the probe (it may be deferred) to finish. */
static int sysfs_bind(const char *bus, const char *dev, const char *drv, unsigned timeout_ms)
{
    char path[PATH_MAX], cur[128];
    uint64_t until = hal_now_us() + (uint64_t)timeout_ms * 1000U;
    if (bound_driver(bus, dev, cur, sizeof(cur)) && strcmp(cur, drv) == 0) return 0;
    snprintf(path, sizeof(path), "/sys/bus/%s/drivers/%s/bind", bus, drv);
    (void)write_file(path, dev);
    do {
        if (bound_driver(bus, dev, cur, sizeof(cur)) && strcmp(cur, drv) == 0) return 0;
        usleep(20000);
    } while (hal_now_us() < until);
    hal_log("bind %s to %s timed out", dev, drv);
    return -ETIMEDOUT;
}

/* ---------------------------------------------------------------- GPIO (XGpioPs) */

#define GPIO_PINS 118U
static int gpio_chip = -1;
static struct { int fd; int output; u32 value; } pins[GPIO_PINS];
static XGpioPs_Config gpio_cfg = {0, 0xE000A000U};

static int gpio_open_chip(void)
{
    char path[64];
    unsigned i;
    if (gpio_chip >= 0) return 0;
    for (i = 0; i < 16; ++i) {
        struct gpiochip_info info;
        int fd;
        snprintf(path, sizeof(path), "/dev/gpiochip%u", i);
        fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) == 0 && strcmp(info.label, "zynq_gpio") == 0) {
            gpio_chip = fd;
            for (i = 0; i < GPIO_PINS; ++i) pins[i].fd = -1;
            return 0;
        }
        close(fd);
    }
    hal_log("zynq_gpio chip not found");
    return -ENODEV;
}

static void gpio_release(u32 pin)
{
    if (pins[pin].fd >= 0) close(pins[pin].fd);
    pins[pin].fd = -1;
}

static int gpio_request(u32 pin, u32 flags)
{
    struct gpiohandle_request req;
    if (pin >= GPIO_PINS || gpio_open_chip()) return -EINVAL;
    gpio_release(pin);
    memset(&req, 0, sizeof(req));
    req.lineoffsets[0] = pin;
    req.lines = 1;
    req.flags = flags;
    req.default_values[0] = (u8)(pins[pin].value != 0);
    snprintf(req.consumer_label, sizeof(req.consumer_label), "omnicored");
    if (ioctl(gpio_chip, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0) {
        hal_log("gpio %u request: %s", pin, strerror(errno));
        return -errno;
    }
    pins[pin].fd = req.fd;
    pins[pin].output = (flags & GPIOHANDLE_REQUEST_OUTPUT) != 0;
    return 0;
}

XGpioPs_Config *XGpioPs_LookupConfig(u16 id) { (void)id; return &gpio_cfg; }

s32 XGpioPs_CfgInitialize(XGpioPs *g, XGpioPs_Config *cfg, u32 base)
{
    g->GpioConfig = *cfg;
    g->GpioConfig.BaseAddr = base;
    g->IsReady = XIL_COMPONENT_IS_READY;
    return gpio_open_chip() ? XST_FAILURE : XST_SUCCESS;
}

void XGpioPs_SetDirectionPin(XGpioPs *g, u32 pin, u32 direction)
{
    (void)g;
    if (pin < GPIO_PINS)
        (void)gpio_request(pin, direction ? GPIOHANDLE_REQUEST_OUTPUT : GPIOHANDLE_REQUEST_INPUT);
}

/* Linux drives the pin when the line is requested as an output. */
void XGpioPs_SetOutputEnablePin(XGpioPs *g, u32 pin, u32 enable) { (void)g; (void)pin; (void)enable; }

void XGpioPs_WritePin(XGpioPs *g, u32 pin, u32 data)
{
    struct gpiohandle_data d;
    (void)g;
    if (pin >= GPIO_PINS) return;
    pins[pin].value = data & 1U;
    if (pins[pin].fd < 0 || !pins[pin].output) return;
    memset(&d, 0, sizeof(d));
    d.values[0] = (u8)pins[pin].value;
    if (ioctl(pins[pin].fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &d) < 0)
        hal_log("gpio %u write: %s", pin, strerror(errno));
}

/* Like DATA_RO this returns the pad level; an unrequested pin keeps its direction. */
u32 XGpioPs_ReadPin(XGpioPs *g, u32 pin)
{
    struct gpiohandle_data d;
    (void)g;
    if (pin >= GPIO_PINS) return 0;
    if (pins[pin].fd < 0 && gpio_request(pin, 0) < 0) return 0;
    if (ioctl(pins[pin].fd, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &d) < 0) return 0;
    return d.values[0];
}

/* ---------------------------------------------------------------- SPI (XSpiPs) */

#define SPI_REF_HZ 166666666U
static int spi_fd = -1;
static char spi_dev[64];
static XSpiPs_Config spi_cfg = {0, 0xE0006000U, SPI_REF_HZ};
static u32 spi_options;
static u8 spi_prescaler = XSPIPS_CLK_PRESCALE_256;
static unsigned long spi_count;

unsigned hal_spi_hz(void);

static int spi_configure(void)
{
    u8 mode = 0, bits = 8;
    u32 hz = hal_spi_hz();
    if (spi_fd < 0) return -ENODEV;
    if (spi_options & XSPIPS_CLK_PHASE_1_OPTION) mode |= SPI_CPHA;
    if (spi_options & XSPIPS_CLK_ACTIVE_LOW_OPTION) mode |= SPI_CPOL;
    if (ioctl(spi_fd, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &hz) < 0) {
        hal_log("spidev configure: %s", strerror(errno));
        return -errno;
    }
    return 0;
}

XSpiPs_Config *XSpiPs_LookupConfig(u16 id) { (void)id; return &spi_cfg; }

s32 XSpiPs_CfgInitialize(XSpiPs *s, XSpiPs_Config *cfg, u32 base)
{
    s->Config = *cfg;
    s->Config.BaseAddress = base;
    s->IsReady = XIL_COMPONENT_IS_READY;
    return XST_SUCCESS;
}

s32 XSpiPs_SetOptions(XSpiPs *s, u32 options)
{
    s->Options = spi_options = options;
    return spi_configure() ? XST_FAILURE : XST_SUCCESS;
}

s32 XSpiPs_SetClkPrescaler(XSpiPs *s, u8 prescaler)
{
    s->Prescaler = spi_prescaler = prescaler;
    return spi_configure() ? XST_FAILURE : XST_SUCCESS;
}

/* Only the AD9361 (SS0) is wired on this board. */
s32 XSpiPs_SetSlaveSelect(XSpiPs *s, u8 ss) { (void)s; return ss == 0 ? XST_SUCCESS : XST_FAILURE; }

s32 XSpiPs_PolledTransfer(XSpiPs *s, u8 *send, u8 *recv, u32 count)
{
    struct spi_ioc_transfer t;
    (void)s;
    if (spi_fd < 0) return XST_FAILURE;
    memset(&t, 0, sizeof(t));
    t.tx_buf = (uintptr_t)send;
    t.rx_buf = (uintptr_t)recv;
    t.len = count;
    t.bits_per_word = 8;
    ++spi_count;
    return ioctl(spi_fd, SPI_IOC_MESSAGE(1), &t) < 0 ? XST_FAILURE : XST_SUCCESS;
}

/* ---------------------------------------------------------------- AD9361 ownership */

#define AD9361_COMPAT "adi,ad9361"
#define ADC_COMPAT "adi,axi-ad9361-6.00.a"
#define DDS_COMPAT "adi,axi-ad9361-dds-6.00.a"

static char adc_dev[96], dds_dev[96];

static int find_devices(void)
{
    if (!spi_dev[0] && find_of_device("spi", AD9361_COMPAT, spi_dev, sizeof(spi_dev))) {
        hal_log("AD9361 spi device not found in device tree");
        return -ENODEV;
    }
    if (!adc_dev[0]) (void)find_of_device("platform", ADC_COMPAT, adc_dev, sizeof(adc_dev));
    if (!dds_dev[0]) (void)find_of_device("platform", DDS_COMPAT, dds_dev, sizeof(dds_dev));
    return 0;
}

/* Thread safe: only sysfs, after find_devices() ran in hal_ad9361_open(). */
int hal_ad9361_kernel_unbind(void)
{
    char path[PATH_MAX];
    int rc;
    if (!spi_dev[0]) return -ENODEV;
    /* Consumers first: the AXI cores reference the AD9361 converter/clocks. */
    if (dds_dev[0]) (void)sysfs_unbind("platform", dds_dev);
    if (adc_dev[0]) (void)sysfs_unbind("platform", adc_dev);
    if ((rc = sysfs_unbind("spi", spi_dev))) {
        hal_log("unbind %s: %s", spi_dev, strerror(-rc));
        return rc;
    }
    snprintf(path, sizeof(path), "/sys/bus/spi/devices/%s/driver_override", spi_dev);
    if ((rc = write_file(path, "spidev"))) return rc;
    if ((rc = sysfs_bind("spi", spi_dev, "spidev", 2000))) return rc;
    snprintf(path, sizeof(path), "/dev/spidev%s", spi_dev + 3); /* "spiB.C" -> spidevB.C */
    for (rc = 0; rc < 100 && access(path, F_OK); ++rc) usleep(10000);
    return access(path, F_OK) ? -ENODEV : 0;
}

/* Thread safe: only sysfs. The spidev fd and the reset line are released first. */
int hal_ad9361_kernel_bind(void)
{
    char path[PATH_MAX];
    int rc;
    if (!spi_dev[0]) return -ENODEV;
    (void)sysfs_unbind("spi", spi_dev);
    snprintf(path, sizeof(path), "/sys/bus/spi/devices/%s/driver_override", spi_dev);
    (void)write_file(path, "\n");
    /* ad9361 probe runs the full DT init and digital interface tune. */
    if ((rc = sysfs_bind("spi", spi_dev, "ad9361", 15000))) return rc;
    if (adc_dev[0] && (rc = sysfs_bind("platform", adc_dev, "cf_axi_adc", 5000))) return rc;
    if (dds_dev[0] && (rc = sysfs_bind("platform", dds_dev, "cf_axi_dds", 5000))) return rc;
    return 0;
}

int hal_ad9361_open(void)
{
    char path[PATH_MAX];
    int rc;
    if (spi_fd >= 0) return 0;
    if ((rc = find_devices())) return rc;
    snprintf(path, sizeof(path), "/dev/spidev%s", spi_dev + 3);
    spi_fd = open(path, O_RDWR | O_CLOEXEC);
    if (spi_fd < 0) {
        hal_log("open %s: %s", path, strerror(errno));
        return -errno;
    }
    return spi_configure();
}

void hal_ad9361_close(unsigned reset_gpio)
{
    if (spi_fd >= 0) close(spi_fd);
    spi_fd = -1;
    /* The kernel driver requests the AD9361 reset line itself. */
    if (reset_gpio < GPIO_PINS) gpio_release(reset_gpio);
}

int hal_ad9361_take(void)
{
    int rc;
    if (spi_fd >= 0) return 0;
    if ((rc = find_devices()) || (rc = hal_ad9361_kernel_unbind())) return rc;
    return hal_ad9361_open();
}

unsigned long hal_spi_transfers(void) { return spi_count; }

unsigned hal_spi_hz(void)
{
    return config.spi_hz ? config.spi_hz : SPI_REF_HZ >> (spi_prescaler + 1U);
}

/* ---------------------------------------------------------------- GIC (UIO) */

static const struct { u32 id; const char *uio; } irq_map[] = {
    {91U, "ch32h417"}, /* IRQ_F2P[15] */
    {61U, "pps"},      /* IRQ_F2P[0], axi_ad9361 gps_pps_irq */
    {90U, NULL},       /* IRQ_F2P[14], UART16550: backed by the tty fd */
};
#define IS_TTY(i) (irq_map[i].uio == NULL)
int hal_tty_fd(void);
int hal_tty_rx_pending(void);
static struct {
    int fd;
    int enabled;  /* state requested by the SDK */
    Xil_InterruptHandler handler;
    void *ref;
} irqs[ARRAY_SIZE(irq_map)];
static int irq_quiesced;
static XScuGic_Config gic_cfg;

static int irq_index(u32 id)
{
    unsigned i;
    for (i = 0; i < ARRAY_SIZE(irq_map); ++i)
        if (irq_map[i].id == id) return (int)i;
    return -1;
}

static void irq_hw(int i, int enable)
{
    int32_t v = enable;
    if (IS_TTY(i)) return;
    if (irqs[i].fd >= 0 && write(irqs[i].fd, &v, sizeof(v)) != sizeof(v))
        hal_log("uio %s irqcontrol: %s", irq_map[i].uio, strerror(errno));
}

static void irq_open(void)
{
    unsigned i, n;
    char path[64], name[64];
    for (i = 0; i < ARRAY_SIZE(irq_map); ++i) irqs[i].fd = -1;
    for (n = 0; n < 16; ++n) {
        snprintf(path, sizeof(path), "/sys/class/uio/uio%u/name", n);
        if (read_file(path, name, sizeof(name)) <= 0) continue;
        name[strcspn(name, "\n")] = '\0';
        for (i = 0; i < ARRAY_SIZE(irq_map); ++i) {
            if (!IS_TTY(i) && strcmp(name, irq_map[i].uio) == 0) {
                snprintf(path, sizeof(path), "/dev/uio%u", n);
                irqs[i].fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
                irq_hw((int)i, 0);
            }
        }
    }
    for (i = 0; i < ARRAY_SIZE(irq_map); ++i)
        if (irqs[i].fd < 0 && !IS_TTY(i)) hal_log("uio device '%s' missing; its interrupt is unavailable", irq_map[i].uio);
}

XScuGic_Config *XScuGic_LookupConfig(u16 id) { (void)id; return &gic_cfg; }
s32 XScuGic_CfgInitialize(XScuGic *gic, XScuGic_Config *cfg, u32 base)
{
    (void)base;
    gic->Config = cfg;
    gic->IsReady = XIL_COMPONENT_IS_READY;
    return XST_SUCCESS;
}

s32 XScuGic_Connect(XScuGic *gic, u32 id, Xil_InterruptHandler h, void *ref)
{
    int i = irq_index(id);
    uint32_t count;
    (void)gic;
    if (i >= 0 && IS_TTY(i)) irqs[i].fd = hal_tty_fd();
    if (i < 0 || irqs[i].fd < 0) return XST_FAILURE;
    if (!IS_TTY(i))
        while (read(irqs[i].fd, &count, sizeof(count)) == sizeof(count)) {}
    irqs[i].handler = h;
    irqs[i].ref = ref;
    return XST_SUCCESS;
}

void XScuGic_Enable(XScuGic *gic, u32 id)
{
    int i = irq_index(id);
    (void)gic;
    if (i < 0) return;
    irqs[i].enabled = 1;
    if (!irq_quiesced) irq_hw(i, 1);
}

void XScuGic_Disable(XScuGic *gic, u32 id)
{
    int i = irq_index(id);
    (void)gic;
    if (i < 0) return;
    irqs[i].enabled = 0;
    irq_hw(i, 0);
}

/* Trigger type and priority come from the device tree. */
void XScuGic_SetPriorityTriggerType(XScuGic *gic, u32 id, u8 prio, u8 trigger)
{ (void)gic; (void)id; (void)prio; (void)trigger; }
void XScuGic_InterruptHandler(void *gic) { (void)gic; }

int hal_irq_pollfds(struct pollfd *fds, int max)
{
    int n = 0;
    unsigned i;
    for (i = 0; i < ARRAY_SIZE(irqs) && n < max; ++i) {
        if (irqs[i].fd < 0 || (irq_quiesced && !IS_TTY(i))) continue;
        if (IS_TTY(i) && !(irqs[i].enabled && irqs[i].handler)) continue;
        fds[n].fd = irqs[i].fd;
        fds[n].events = POLLIN;
        fds[n].revents = 0;
        ++n;
    }
    return n;
}

/* uio_pdrv_genirq masks the line when it fires, like the SDK ISR does with the GIC. */
void hal_irq_dispatch(const struct pollfd *fds, int n)
{
    int k;
    unsigned i;
    uint32_t count;
    for (k = 0; k < n; ++k) {
        if (!(fds[k].revents & POLLIN)) continue;
        for (i = 0; i < ARRAY_SIZE(irqs); ++i) {
            if (irqs[i].fd != fds[k].fd) continue;
            if (IS_TTY(i)) break; /* run below, once */
            if (read(irqs[i].fd, &count, sizeof(count)) != sizeof(count)) break;
            if (irqs[i].handler && irqs[i].enabled) irqs[i].handler(irqs[i].ref);
            /* A GIC line stays enabled unless the ISR disabled it. */
            if (irqs[i].enabled && !irq_quiesced) irq_hw((int)i, 1);
        }
    }
    /* Console UART: level semantics. Runs while the tty has data or bytes are
     * already in the emulated FIFO; leftovers fire again on the next pass. */
    for (i = 0; i < ARRAY_SIZE(irqs); ++i) {
        int fire = 0;
        if (!IS_TTY(i) || !irqs[i].handler || !irqs[i].enabled) continue;
        for (k = 0; k < n; ++k)
            if (fds[k].fd == irqs[i].fd && (fds[k].revents & POLLIN)) fire = 1;
        if (fire || hal_tty_rx_pending()) irqs[i].handler(irqs[i].ref);
    }
}

int hal_irq_ready(void)
{
    unsigned i;
    for (i = 0; i < ARRAY_SIZE(irqs); ++i)
        if (IS_TTY(i) && irqs[i].handler && irqs[i].enabled && hal_tty_rx_pending()) return 1;
    return 0;
}

void hal_irq_quiesce(void)
{
    unsigned i;
    irq_quiesced = 1;
    for (i = 0; i < ARRAY_SIZE(irqs); ++i) irq_hw((int)i, 0);
}

void hal_irq_resume(void)
{
    unsigned i;
    uint32_t count;
    irq_quiesced = 0;
    for (i = 0; i < ARRAY_SIZE(irqs); ++i) {
        if (irqs[i].fd < 0 || IS_TTY(i)) continue;
        while (read(irqs[i].fd, &count, sizeof(count)) == sizeof(count)) {}
        if (irqs[i].enabled) irq_hw((int)i, 1);
    }
}

void hal_irq_tty_raise(void)
{
    unsigned i;
    for (i = 0; i < ARRAY_SIZE(irqs); ++i)
        if (IS_TTY(i) && irqs[i].handler && irqs[i].enabled) irqs[i].handler(irqs[i].ref);
}

/* ---------------------------------------------------------------- XADC (sysfs) */

static char xadc_temp_raw[PATH_MAX];
static XAdcPs_Config xadc_cfg = {0, 0xF8007100U};

static int xadc_find(void)
{
    char path[PATH_MAX], name[32];
    unsigned n;
    for (n = 0; n < 16; ++n) {
        snprintf(path, sizeof(path), "/sys/bus/iio/devices/iio:device%u/name", n);
        if (read_file(path, name, sizeof(name)) <= 0) continue;
        if (strncmp(name, "xadc", 4) == 0) {
            snprintf(xadc_temp_raw, sizeof(xadc_temp_raw),
                     "/sys/bus/iio/devices/iio:device%u/in_temp0_raw", n);
            return 0;
        }
    }
    return -ENODEV;
}

XAdcPs_Config *XAdcPs_LookupConfig(u16 id) { (void)id; return &xadc_cfg; }
s32 XAdcPs_CfgInitialize(XAdcPs *x, XAdcPs_Config *cfg, u32 base)
{
    x->Config = *cfg;
    x->Config.BaseAddress = base;
    x->IsReady = XIL_COMPONENT_IS_READY;
    if (xadc_find()) {
        hal_log("xadc iio device not found; board temperature unavailable");
        return XST_FAILURE;
    }
    return XST_SUCCESS;
}
void XAdcPs_SetSequencerMode(XAdcPs *x, u8 mode) { x->SeqMode = mode; }
u8 XAdcPs_GetSequencerMode(XAdcPs *x) { return (u8)x->SeqMode; }
void XAdcPs_SetAdcClkDivisor(XAdcPs *x, u8 div) { (void)x; (void)div; }
void XAdcPs_SetAvg(XAdcPs *x, u8 avg) { (void)x; (void)avg; }
s32 XAdcPs_SetSeqChEnables(XAdcPs *x, u32 mask) { x->SeqChannels = mask; return XST_SUCCESS; }
u32 XAdcPs_GetSeqChEnables(XAdcPs *x) { return x->SeqChannels; }
s32 XAdcPs_SetSeqAvgEnables(XAdcPs *x, u32 mask) { (void)x; (void)mask; return XST_SUCCESS; }

/* The driver reports the 12-bit code; the BSP format is left aligned in 16 bits. */
u16 XAdcPs_GetAdcData(XAdcPs *x, u8 channel)
{
    char buf[32];
    long raw;
    (void)x;
    if (channel != XADCPS_CH_TEMP || !xadc_temp_raw[0]) return 0;
    if (read_file(xadc_temp_raw, buf, sizeof(buf)) <= 0) return 0;
    raw = strtol(buf, NULL, 10);
    if (raw <= 0 || raw > 0xFFF) return 0;
    return (u16)(raw << 4);
}

/* ---------------------------------------------------------------- init */

int hal_tty_open(const char *path);

int hal_init(const HalConfig *cfg)
{
    config = *cfg;
    mem_fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (mem_fd < 0) {
        hal_log("open /dev/mem: %s", strerror(errno));
        return -errno;
    }
    if (gpio_open_chip()) return -ENODEV;
    irq_open();
    return hal_tty_open(config.tty);
}
