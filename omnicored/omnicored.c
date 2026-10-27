/* omnicored: runs the OmniCore no-OS SDK command firmware as a Linux daemon and
 * arbitrates the AD9361 between the USB3 path (OCH via CH32 USART1 -> PL UART)
 * and libiio over Ethernet.
 *
 *  USB mode (default at boot): the SDK owns the AD9361 through spidev, exactly
 *    as the bare-metal application did. iiod is stopped. The main thread runs
 *    SCHED_FIFO on its own CPU, like the bare-metal main loop had a core to itself.
 *  IIO mode: the kernel ad9361/cf_axi_adc/cf_axi_dds drivers own it. Each
 *    libiio connection gets its own "iiod -i" with the socket as stdin/stdout,
 *    so iiod sends samples straight from the DMA blocks to the socket
 *    (zero-copy, see the iiod and IIO buffer-dma patches).
 *
 * OCH has priority. A libiio connection takes the AD9361 only when USB
 * streaming and TX are off and no OCH command arrived for quiet_ms; otherwise it
 * is answered with -EBUSY. Any OCH command while libiio owns the AD9361 (or
 * while a handover is running) is queued, the libiio clients are disconnected,
 * the AD9361 returns to USB mode with cold-boot RF defaults (stored FIR/
 * correction/LO-source targets kept) and the queued commands run in order.
 * After the last libiio client disconnects for grace_ms the AD9361 also returns.
 *
 * Kernel driver bind/unbind runs on a worker thread, so the CH32 link and the
 * VCXO service keep running during a handover.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hal.h"
#include "board_config.h"
#include "board_monitor.h"
#include "command.h"
#include "console.h"
#include "sdr_board.h"
#include "sdr_control.h"
#include "sdr_pps.h"
#include "sdr_runtime.h"
#include "sdr_vcxo.h"
#include "pps_monitor.h"
#include "parameters.h"
#include "CH32H417_IP.h"
#include "xparameters.h"

void ad9361_remove(struct ad9361_rf_phy *phy);

#ifndef OMNICORED_VERSION
#define OMNICORED_VERSION "OmniCore-SDR Linux (development build)"
#endif
#ifndef OMNICORED_FPGA_ID
#define OMNICORED_FPGA_ID ""
#endif

typedef struct {
    char tty[128];
    unsigned spi_hz;
    int iio_enable;
    unsigned listen_port;
    unsigned quiet_ms, grace_ms;
    int och_preempt;          /* OCH command takes the AD9361 back from libiio */
    int rt_priority;          /* SCHED_FIFO priority in USB mode; 0 = normal scheduling */
    int cpu;                  /* CPU of the main thread (and of iiod); -1 = any */
    int irq_cpu;              /* CPU for the CH32/UART/SPI/PPS interrupts; -1 = leave */
    char iiod[256];           /* iiod binary, run as "iiod -i" per libiio connection */
} Config;

static Config cfg = {
    "/dev/ttyS1", 0, 1, 30431, 3000, 5000, 1, 50, 1, 1, "/usr/local/sbin/iiod",
};

enum { MODE_USB, MODE_TO_IIO, MODE_IIO, MODE_TO_USB };
static const char *const mode_name[] = {"usb", "switching", "libiio", "switching"};
static int mode = MODE_USB;
static volatile sig_atomic_t stopping;

/* ---------------------------------------------------------------- config */

static void load_config(const char *path)
{
    char line[512];
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno != ENOENT) hal_log("config %s: %s", path, strerror(errno));
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char *k = line, *v, *end;
        while (*k == ' ' || *k == '\t') ++k;
        if (*k == '#' || *k == '\n' || !*k) continue;
        v = strchr(k, '=');
        if (!v) continue;
        *v++ = '\0';
        for (end = v + strlen(v); end > v && (end[-1] == '\n' || end[-1] == ' '); --end) *(end - 1) = '\0';
        for (end = k + strlen(k); end > k && end[-1] == ' '; --end) *(end - 1) = '\0';
        while (*v == ' ') ++v;
        if (!strcmp(k, "tty")) snprintf(cfg.tty, sizeof(cfg.tty), "%s", v);
        else if (!strcmp(k, "spi_hz")) cfg.spi_hz = (unsigned)strtoul(v, NULL, 0);
        else if (!strcmp(k, "iio_enable")) cfg.iio_enable = atoi(v);
        else if (!strcmp(k, "listen_port")) cfg.listen_port = (unsigned)strtoul(v, NULL, 0);
        else if (!strcmp(k, "quiet_ms")) cfg.quiet_ms = (unsigned)strtoul(v, NULL, 0);
        else if (!strcmp(k, "grace_ms")) cfg.grace_ms = (unsigned)strtoul(v, NULL, 0);
        else if (!strcmp(k, "och_preempt")) cfg.och_preempt = atoi(v);
        else if (!strcmp(k, "rt_priority")) cfg.rt_priority = atoi(v);
        else if (!strcmp(k, "cpu")) cfg.cpu = atoi(v);
        else if (!strcmp(k, "irq_cpu")) cfg.irq_cpu = atoi(v);
        else if (!strcmp(k, "iiod")) snprintf(cfg.iiod, sizeof(cfg.iiod), "%s", v);
        else hal_log("config: unknown key '%s'", k);
    }
    fclose(f);
}

/* ---------------------------------------------------------------- scheduling */

/* USB mode: SCHED_FIFO like the bare-metal main loop. IIO mode: normal
 * scheduling, so iiod (forked from here) shares the CPUs fairly with the network. */
static void set_realtime(int on)
{
    struct sched_param sp;
    if (cfg.rt_priority <= 0) return;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = on ? cfg.rt_priority : 0;
    /* Children (iiod) must not inherit the real-time policy. */
    if (sched_setscheduler(0, (on ? SCHED_FIFO : SCHED_OTHER) | SCHED_RESET_ON_FORK, &sp))
        hal_log("sched_setscheduler(%s): %s", on ? "FIFO" : "OTHER", strerror(errno));
}

static void pin_cpu(int cpu)
{
    cpu_set_t set;
    if (cpu < 0 || cpu >= CPU_SETSIZE) return;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set)) hal_log("CPU %d affinity: %s", cpu, strerror(errno));
}

/* The interrupts the SDK waits on follow the main thread; eth0/SD stay where they are. */
static void pin_irqs(int cpu)
{
    static const char *const names[] = {"ttyS1", "ch32h417", "pps", "e0006000.spi"};
    char line[512], path[64], val[8];
    FILE *f;
    if (cpu < 0) return;
    f = fopen("/proc/interrupts", "r");
    if (!f) return;
    snprintf(val, sizeof(val), "%d", cpu);
    while (fgets(line, sizeof(line), f)) {
        unsigned irq, i;
        size_t n = strcspn(line, "\n");
        if (sscanf(line, " %u:", &irq) != 1) continue;
        line[n] = '\0';
        for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
            size_t len = strlen(names[i]);
            if (n < len || strcmp(line + n - len, names[i]) || line[n - len - 1] != ' ') continue;
            snprintf(path, sizeof(path), "/proc/irq/%u/smp_affinity_list", irq);
            FILE *a = fopen(path, "w");
            if (!a || fputs(val, a) < 0) hal_log("irq %u (%s) -> CPU %d failed", irq, names[i], cpu);
            if (a) fclose(a);
        }
    }
    fclose(f);
}

/* ---------------------------------------------------------------- handover worker */

enum { JOB_TO_IIO = 1, JOB_TO_USB };
static int worker_pipe[2] = {-1, -1};
static pthread_t worker;
static int worker_running;
static int worker_job;
static int worker_rc;
static atomic_int worker_abort;
static uint64_t switch_start_us, retry_at_us;

/* Only sysfs here: never SDK state. */
static void *worker_main(void *arg)
{
    int rc;
    char done = 1;
    (void)arg;
    if (worker_job == JOB_TO_IIO) {
        uint64_t t0 = hal_now_us();
        rc = hal_ad9361_kernel_bind();
        if (rc) hal_log("kernel AD9361 drivers did not bind (%d)", rc);
        if (!rc && atomic_load(&worker_abort)) rc = -ECANCELED;
        if (!rc) hal_log("kernel drivers %llu ms", (unsigned long long)((hal_now_us() - t0) / 1000U));
    } else {
        rc = hal_ad9361_kernel_unbind();
    }
    worker_rc = rc;
    if (write(worker_pipe[1], &done, 1) != 1) {}
    return NULL;
}

static int worker_start(int job)
{
    pthread_attr_t a;
    struct sched_param sp;
    cpu_set_t all;
    int rc;
    memset(&sp, 0, sizeof(sp));
    CPU_ZERO(&all);
    for (rc = 0; rc < CPU_SETSIZE; ++rc) CPU_SET(rc, &all);
    pthread_attr_init(&a);
    pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&a, SCHED_OTHER);
    pthread_attr_setschedparam(&a, &sp);
    pthread_attr_setaffinity_np(&a, sizeof(all), &all);
    pthread_attr_setstacksize(&a, 256 * 1024);
    worker_job = job;
    rc = pthread_create(&worker, &a, worker_main, NULL);
    pthread_attr_destroy(&a);
    if (rc) {
        hal_log("worker thread: %s", strerror(rc));
        return -rc;
    }
    worker_running = 1;
    return 0;
}

/* Returns 1 when the running job finished (result in worker_rc). */
static int worker_finished(void)
{
    char b;
    if (!worker_running || read(worker_pipe[0], &b, 1) != 1) return 0;
    pthread_join(worker, NULL);
    worker_running = 0;
    return 1;
}

/* ---------------------------------------------------------------- libiio clients */

#define MAX_CLIENTS 16
#define MAX_PENDING 8
#define MAX_REJECTS 16
#define MAX_DYING 16

/* libiio opens one connection for the context and one per streaming buffer. */
typedef struct {
    int fd;                   /* our copy of the connection, to see the client leave */
    pid_t pid;                /* its "iiod -i" */
} Client;

typedef struct {
    int fd;
    uint64_t until;
} Reject;

static Client clients[MAX_CLIENTS];
static int nclients;
static pid_t dying[MAX_DYING];   /* killed iiods not reaped yet */
static int ndying;
static int pending[MAX_PENDING]; /* accepted while the kernel drivers come up */
static int npending;
static Reject rejects[MAX_REJECTS];
static int nrejects;
static int listen_fd = -1;
static uint64_t last_client_us;

static void reset_close(int fd)
{
    struct linger l = {1, 0};
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
    close(fd);
}

/* libiio's first request (PRINT/ZPRINT/VERSION) reads an integer reply; a
 * negative errno makes the client report it: "Device or resource busy (16)". */
static void reject_client(int fd, int err)
{
    char reply[16];
    int n = snprintf(reply, sizeof(reply), "%d\n", -err);
    if (write(fd, reply, (size_t)n) != n || nrejects == MAX_REJECTS) {
        reset_close(fd);
        return;
    }
    shutdown(fd, SHUT_WR);
    rejects[nrejects].fd = fd;
    rejects[nrejects].until = hal_now_us() + 2000000U;
    ++nrejects;
}

static void rejects_service(const struct pollfd *fds, uint64_t now)
{
    int i;
    char buf[256];
    for (i = nrejects - 1; i >= 0; --i) {
        int done = now >= rejects[i].until;
        if (fds && (fds[i].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(rejects[i].fd, buf, sizeof(buf));
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) done = 1;
        }
        if (done) {
            close(rejects[i].fd);
            rejects[i] = rejects[--nrejects];
        }
    }
}

static int listen_open(unsigned port)
{
    int fd, one = 1, zero = 0;
    struct sockaddr_in6 a6;
    struct sockaddr_in a4;
    fd = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd >= 0) {
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
        memset(&a6, 0, sizeof(a6));
        a6.sin6_family = AF_INET6;
        a6.sin6_addr = in6addr_any;
        a6.sin6_port = htons((uint16_t)port);
        if (bind(fd, (struct sockaddr *)&a6, sizeof(a6)) == 0 && listen(fd, 8) == 0) return fd;
        close(fd);
    }
    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a4, 0, sizeof(a4));
    a4.sin_family = AF_INET;
    a4.sin_addr.s_addr = htonl(INADDR_ANY);
    a4.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&a4, sizeof(a4)) || listen(fd, 8)) {
        hal_log("listen on port %u: %s", port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* The connection becomes stdin/stdout of "iiod -i". The patched iiod sends
 * buffer data with vmsplice()/splice() straight from the DMA blocks, and exits
 * at end of stream. iiod inherits the main thread's CPU (not the real-time
 * policy: SCHED_RESET_ON_FORK), away from the Ethernet softirq on CPU0. */
static int client_add(int c)
{
    pid_t pid;
    int one = 1;
    if (nclients == MAX_CLIENTS) {
        hal_log("cannot serve libiio client (too many clients)");
        reject_client(c, EMFILE);
        return -1;
    }
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    pid = fork();
    if (pid == 0) {
        sigset_t none;
        int fd;
        if (dup2(c, STDIN_FILENO) < 0 || dup2(c, STDOUT_FILENO) < 0) _exit(127);
        for (fd = STDERR_FILENO + 1; fd < 1024; ++fd) close(fd);
        signal(SIGPIPE, SIG_DFL);
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        execl(cfg.iiod, "iiod", "-i", (char *)NULL);
        _exit(127);
    }
    if (pid < 0) {
        hal_log("fork: %s", strerror(errno));
        reject_client(c, EIO);
        return -1;
    }
    clients[nclients].fd = c;
    clients[nclients].pid = pid;
    ++nclients;
    return 0;
}

static void reap_dying(void)
{
    int i;
    for (i = ndying - 1; i >= 0; --i) {
        if (waitpid(dying[i], NULL, WNOHANG) != 0) dying[i] = dying[--ndying];
    }
}

/* Ends a client: kills its iiod (it may still be streaming) and closes the
 * connection, with a reset when the client is being dropped. */
static void client_close(int i, int reset)
{
    Client *cl = &clients[i];
    if (cl->pid > 0) {
        kill(cl->pid, SIGKILL);
        if (waitpid(cl->pid, NULL, WNOHANG) == 0) {
            if (ndying == MAX_DYING) waitpid(dying[--ndying], NULL, 0);
            dying[ndying++] = cl->pid;
        }
    }
    if (reset) reset_close(cl->fd);
    else close(cl->fd);
    clients[i] = clients[--nclients];
    if (nclients == 0) {
        last_client_us = hal_now_us();
        if (mode == MODE_IIO) hal_log("last libiio client disconnected");
    }
}

/* A client is gone when its connection hangs up or its iiod exits. */
static void clients_service(const struct pollfd *fds)
{
    int i;
    for (i = nclients - 1; i >= 0; --i) {
        int status;
        if (waitpid(clients[i].pid, &status, WNOHANG) == clients[i].pid) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 127) hal_log("cannot run %s", cfg.iiod);
            clients[i].pid = 0;
            client_close(i, 0);
        } else if (fds[i].revents & (POLLRDHUP | POLLHUP | POLLERR)) {
            client_close(i, 0);
        }
    }
    reap_dying();
}

/* Before the kernel drivers go away, no iiod may still hold the IIO devices. */
static void drop_clients(void)
{
    uint64_t until;
    while (nclients) client_close(nclients - 1, 1);
    until = hal_now_us() + 1000000U;
    while (ndying && hal_now_us() < until) {
        reap_dying();
        if (ndying) usleep(1000);
    }
    if (ndying) hal_log("%d iiod process(es) did not exit", ndying);
}

static void reject_pending(int err)
{
    while (npending) reject_client(pending[--npending], err);
}

/* ---------------------------------------------------------------- SDK */

#define QUEUE_LINES 16
static uint64_t last_cmd_us;
static char line_buf[CONSOLE_LINE_CAPACITY];
static char queued[QUEUE_LINES][CONSOLE_LINE_CAPACITY];
static int nqueued;

static void rf_boot_timed(const char *why)
{
    uint64_t t0 = hal_now_us();
    unsigned long s0 = hal_spi_transfers();
    int rc = sdr_rf_boot();
    if (rc < 0) hal_log("RF initialization failed (%s); OCH can retry with rf_reinit!", why);
    hal_log("RF init (%s): %llu ms, %lu SPI transfers at %u Hz", why,
            (unsigned long long)((hal_now_us() - t0) / 1000U), hal_spi_transfers() - s0, hal_spi_hz());
}

static void sdk_boot(void)
{
    /* Same sequence as the bare-metal main.c. */
    if (console_init(CONSOLE_BAUD_RATE) != 0) hal_log("console UART initialization failed");
    if (board_monitor_init() < 0) {
        console_print("Board RGB/fan or Zynq temperature initialization failed; inspect board status.\n");
    }
    board_peripherals_init();
    {
        uint64_t t0 = hal_now_us();
        unsigned long s0 = hal_spi_transfers();
        if (sdr_rf_boot() < 0) console_print("RF initialization failed; use rf_reinit! to retry.\n");
        hal_log("RF init (boot): %llu ms, %lu SPI transfers at %u Hz",
                (unsigned long long)((hal_now_us() - t0) / 1000U), hal_spi_transfers() - s0, hal_spi_hz());
    }
    if (command_init() != 0) console_print("CH32 driver/interrupt initialization failed.\n");
    sdr_board_initializing(0);
    console_print("SDR ready protocol=1.0 TX=off USB=off; use help?\n");
    hal_tty_flush();
}

static int usb_idle(uint64_t now)
{
    return !sdr_state.tx_enabled && !sdr_state.usb_rx && !sdr_state.usb_tx &&
           now - last_cmd_us >= (uint64_t)cfg.quiet_ms * 1000U;
}

/* Parses "@<id> <body>" like command_execute; returns the id or 0. */
static unsigned long envelope_id(const char *line, const char **body)
{
    unsigned long id = 0;
    while (*line == ' ' || *line == '\t') ++line;
    *body = line;
    if (*line != '@') return 0;
    for (++line; *line >= '0' && *line <= '9'; ++line) id = id * 10U + (unsigned long)(*line - '0');
    while (*line == ' ' || *line == '\t') ++line;
    *body = line;
    return id;
}

static int is_owner_query(const char *body)
{
    return strncmp(body, "owner?", 6) == 0 && (body[6] == '\0' || body[6] == '\r' || body[6] == '\n');
}

static void reply_owner_or_busy(const char *line, int busy)
{
    const char *body;
    unsigned long id = envelope_id(line, &body);
    if (id) console_print("RSP id=%lu begin\n", id);
    if (busy) console_print("ERR busy owner=%s clients=%d (USB control resumes after libiio disconnects)\n",
                            mode_name[mode], nclients);
    else console_print("owner=%s clients=%d iio=%s och_preempt=%d\n", mode_name[mode], nclients,
                       cfg.iio_enable ? "enabled" : "disabled", cfg.och_preempt);
    if (id) console_print("RSP id=%lu end result=%d\n", id, busy ? -1 : 0);
}

static void start_to_usb(const char *why);

/* owner? works in all modes. In USB mode everything else is the SDK's. Otherwise
 * an OCH command is queued and takes the AD9361 back (och_preempt=1) or is
 * refused as busy (och_preempt=0). CH32 "vcxo_ack" lines are bridge traffic, not
 * host activity: they are consumed silently in all modes. */
static void handle_line(const char *line)
{
    const char *body;
    unsigned long id = envelope_id(line, &body);
    if (!id && sdr_vcxo_execute(body)) return;
    if (is_owner_query(body)) {
        reply_owner_or_busy(line, 0);
        return;
    }
    if (mode == MODE_USB) {
        last_cmd_us = hal_now_us();
        command_execute(line);
        return;
    }
    if (*body == '\0' || *body == '\r' || *body == '\n') return;
    if (!cfg.och_preempt || nqueued == QUEUE_LINES) {
        reply_owner_or_busy(line, 1);
        return;
    }
    snprintf(queued[nqueued++], CONSOLE_LINE_CAPACITY, "%s", line);
    last_cmd_us = hal_now_us();
    if (mode == MODE_IIO) start_to_usb("OCH command");
    else if (mode == MODE_TO_IIO && !atomic_load(&worker_abort)) {
        hal_log("OCH command during handover to libiio: cancelling it");
        atomic_store(&worker_abort, 1);
    }
}

static void poll_console(void)
{
    int i;
    for (i = 0; i < 8; ++i) {
        int r = console_poll_command(line_buf, sizeof(line_buf));
        if (r == CONSOLE_PENDING) break;
        if (r < 0) {
            console_print("Invalid/overlong line discarded (maximum 127 characters).\n");
            continue;
        }
        handle_line(line_buf);
    }
    hal_tty_flush();
}

static void run_queued(void)
{
    int i, n = nqueued;
    nqueued = 0;
    for (i = 0; i < n; ++i) {
        last_cmd_us = hal_now_us();
        command_execute(queued[i]);
        hal_tty_flush();
    }
}

static void refuse_queued(void)
{
    int i, n = nqueued;
    nqueued = 0;
    for (i = 0; i < n; ++i) reply_owner_or_busy(queued[i], 1);
    hal_tty_flush();
}

/* ---------------------------------------------------------------- mode switching */

static void stop_usb_side(void)
{
    u32 ctrl0;
    (void)sdr_stop_all();
    /* As in sdr_rf_boot: drop EN/RUN, keep the applied IQ format bit. With
     * TX_EN clear the CH32 IP passes the ADI DMA samples to the DAC. */
    ctrl0 = Xil_In32(XPAR_CH32H417_IP_0_S00_AXI_BASEADDR + CH32H417_IP_CTRL0);
    Xil_Out32(XPAR_CH32H417_IP_0_S00_AXI_BASEADDR + CH32H417_IP_CTRL0,
              ctrl0 & ~(CH32H417_IP_RX_EN | CH32H417_IP_TX_EN | CH32H417_IP_TX_RUN));
    pps_monitor_nominal(0);
}

static void start_to_iio(void)
{
    hal_log("USB idle: handing the AD9361 to libiio");
    switch_start_us = hal_now_us();
    sdr_board_initializing(1);
    hal_tty_mute(1);
    stop_usb_side();
    hal_tty_mute(0);
    hal_irq_quiesce();
    hal_ad9361_close(GPIO_RESET_PIN);
    set_realtime(0);
    atomic_store(&worker_abort, 0);
    mode = MODE_TO_IIO;
    if (worker_start(JOB_TO_IIO)) {
        worker_rc = -EAGAIN;
        start_to_usb("worker failed");
    }
}

static void start_to_usb(const char *why)
{
    hal_log("%s: returning the AD9361 to USB mode", why);
    switch_start_us = hal_now_us();
    drop_clients();
    reject_pending(EBUSY);
    sdr_board_initializing(1);
    mode = MODE_TO_USB;
    retry_at_us = 0;
    if (worker_start(JOB_TO_USB)) retry_at_us = hal_now_us() + 1000000U;
}

static void finish_to_iio(void)
{
    uint64_t ms = (hal_now_us() - switch_start_us) / 1000U;
    /* An OCH command may arrive after the worker's last abort check. */
    if (!worker_rc && (atomic_load(&worker_abort) || nqueued)) worker_rc = -ECANCELED;
    if (worker_rc) {
        hal_log("handover to libiio %s after %llu ms", worker_rc == -ECANCELED ? "cancelled" : "failed",
                (unsigned long long)ms);
        reject_pending(worker_rc == -ECANCELED ? EBUSY : EIO);
        start_to_usb(worker_rc == -ECANCELED ? "OCH command" : "handover failed");
        return;
    }
    sdr_board_initializing(0);
    mode = MODE_IIO;
    last_client_us = hal_now_us();
    hal_log("libiio owns the AD9361 (handover %llu ms)", (unsigned long long)ms);
    while (npending) {
        int c = pending[--npending];
        if (client_add(c) == 0) hal_log("libiio client connected (%d active)", nclients);
    }
}

static void finish_to_usb(void)
{
    uint64_t t_worker = hal_now_us();
    if (worker_rc || hal_ad9361_open()) {
        hal_log("cannot reclaim the AD9361 (%d); retrying in 1 s", worker_rc);
        refuse_queued();
        retry_at_us = hal_now_us() + 1000000U;
        return;
    }
    hal_irq_resume();
    hal_tty_mute(1);
    ad9361_remove(ad9361_phy);
    ad9361_phy = NULL;
    /* The GPIO lines were released to the kernel driver: request the reset
     * line again (AD9361 held in reset) and reapply the SPI mode. */
    board_peripherals_init();
    set_realtime(1);
    rf_boot_timed("return to USB");
    sdr_pps_refresh();
    hal_tty_mute(0);
    sdr_board_initializing(0);
    mode = MODE_USB;
    last_cmd_us = hal_now_us();
    hal_log("USB (OCH) owns the AD9361 (handover %llu ms, drivers %llu ms)",
            (unsigned long long)((hal_now_us() - switch_start_us) / 1000U),
            (unsigned long long)((t_worker - switch_start_us) / 1000U));
    run_queued();
}

static void accept_clients(void)
{
    for (;;) {
        int c = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (c < 0) return;
        switch (mode) {
        case MODE_USB:
            if (!usb_idle(hal_now_us())) {
                hal_log("libiio connection refused: USB/OCH session active");
                reject_client(c, EBUSY);
                break;
            }
            start_to_iio();
            /* fall through */
        case MODE_TO_IIO:
            if (mode != MODE_TO_IIO || atomic_load(&worker_abort) || npending == MAX_PENDING) reject_client(c, EBUSY);
            else pending[npending++] = c;
            break;
        case MODE_IIO:
            if (client_add(c) == 0) hal_log("libiio client connected (%d active)", nclients);
            break;
        default:
            reject_client(c, EBUSY);
            break;
        }
    }
}

/* ---------------------------------------------------------------- main */

static void on_signal(int sig)
{
    (void)sig;
    stopping = 1;
}

int main(int argc, char **argv)
{
    const char *conf = "/etc/omnicore/omnicored.conf";
    HalConfig hc;
    int opt;

    while ((opt = getopt(argc, argv, "c:V")) != -1) {
        if (opt == 'c') conf = optarg;
        else if (opt == 'V') {
            printf("omnicored %s\n", OMNICORED_VERSION);
            if (OMNICORED_FPGA_ID[0]) printf("bitstream %s\n", OMNICORED_FPGA_ID);
            return 0;
        } else {
            fprintf(stderr, "usage: %s [-c config] [-V]\n", argv[0]);
            return 2;
        }
    }
    load_config(conf);
    hal_log("%s", OMNICORED_VERSION);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (cfg.rt_priority > 0 && mlockall(MCL_CURRENT | MCL_FUTURE)) hal_log("mlockall: %s", strerror(errno));
    pin_cpu(cfg.cpu);
    set_realtime(1);
    if (pipe2(worker_pipe, O_NONBLOCK | O_CLOEXEC)) {
        hal_log("pipe: %s", strerror(errno));
        return 1;
    }

    hc.tty = cfg.tty;
    hc.spi_hz = cfg.spi_hz;
    if (hal_init(&hc)) return 1;
    /* After hal_init: the 8250 driver requests the ttyS1 interrupt on open. */
    pin_irqs(cfg.irq_cpu);
    hal_tty_set_fpga_id(OMNICORED_FPGA_ID);
    if (hal_ad9361_take()) hal_log("AD9361 SPI unavailable; RF commands will fail");
    sdk_boot();
    last_cmd_us = hal_now_us();

    if (cfg.iio_enable) listen_fd = listen_open(cfg.listen_port);
    hal_log("ready: USB (OCH) owns the AD9361; libiio on port %u %s; OCH preempts libiio: %s; "
            "SCHED_FIFO %d on CPU %d", cfg.listen_port, listen_fd >= 0 ? "(on demand)" : "disabled",
            cfg.och_preempt ? "yes" : "no", cfg.rt_priority, cfg.cpu);

    while (!stopping) {
        struct pollfd fds[3 + MAX_CLIENTS + MAX_REJECTS + 4];
        int n = 0, nirq = 0, base_clients, base_rej, i;
        uint64_t now;

        fds[n].fd = hal_tty_fd(); fds[n].events = POLLIN; fds[n].revents = 0; ++n;
        fds[n].fd = worker_pipe[0]; fds[n].events = POLLIN; fds[n].revents = 0; ++n;
        fds[n].fd = listen_fd; fds[n].events = POLLIN; fds[n].revents = 0; ++n;
        base_clients = n;
        for (i = 0; i < nclients; ++i) {
            /* Only the hang-up: the data is iiod's to read. */
            fds[n].fd = clients[i].fd; fds[n].events = POLLRDHUP; fds[n++].revents = 0;
        }
        base_rej = n;
        for (i = 0; i < nrejects; ++i) {
            fds[n].fd = rejects[i].fd; fds[n].events = POLLIN; fds[n++].revents = 0;
        }
        nirq = hal_irq_pollfds(fds + n, 4);

        poll(fds, (nfds_t)(n + nirq), hal_irq_ready() ? 0 : mode == MODE_USB ? 2 : 20);

        hal_irq_dispatch(fds + n, nirq);
        clients_service(fds + base_clients);
        rejects_service(fds + base_rej, hal_now_us());
        if (listen_fd >= 0 && (fds[2].revents & POLLIN)) accept_clients();

        if (mode == MODE_USB) command_service();
        else board_monitor_service();
        /* As in main.c: between commands, never inside an RSP envelope. The
         * ADF4002/CH32 PA4 tuning is independent of who owns the AD9361. */
        sdr_vcxo_service();
        poll_console();

        if (worker_finished()) {
            if (mode == MODE_TO_IIO) finish_to_iio();
            else if (mode == MODE_TO_USB) finish_to_usb();
        }
        now = hal_now_us();
        if (mode == MODE_TO_USB && !worker_running && retry_at_us && now >= retry_at_us) {
            retry_at_us = 0;
            if (worker_start(JOB_TO_USB)) retry_at_us = now + 1000000U;
        }
        if (mode == MODE_IIO && nclients == 0 && now - last_client_us >= (uint64_t)cfg.grace_ms * 1000U)
            start_to_usb("no libiio clients");
    }

    hal_log("stopping");
    drop_clients();
    if (worker_running) pthread_join(worker, NULL);
    if (mode == MODE_USB) {
        hal_tty_mute(1);
        (void)sdr_stop_all();
    }
    hal_tty_flush();
    return 0;
}
