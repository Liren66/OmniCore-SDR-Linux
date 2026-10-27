/* Linux back end for the Xilinx BSP calls made by the OmniCore no-OS SDK. */
#ifndef OMNICORE_HAL_H
#define OMNICORE_HAL_H
#include <stdint.h>
#include <poll.h>

typedef struct {
    const char *tty;     /* AXI UART16550 tty (CH32 USART1 command link) */
    unsigned spi_hz;     /* 0 = derive from the SDK prescaler like XSpiPs */
} HalConfig;

/* Replaces "fpga=unknown" in the SDK's system_info reply (hal_tty.c); NULL keeps it. */
void hal_tty_set_fpga_id(const char *id);

int hal_init(const HalConfig *cfg);
uint64_t hal_now_us(void);
void hal_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* AD9361 ownership.
 *   kernel_unbind: unbind ad9361/cf_axi_adc/cf_axi_dds, bind spidev.
 *   kernel_bind:   unbind spidev, rebind ad9361/cf_axi_adc/cf_axi_dds.
 * Both only touch sysfs and may run on a worker thread (the kernel probe takes
 * a while). open/close own the spidev fd and the reset GPIO line (other lines
 * such as the ADF4002 stay with the SDK); main thread only. take = unbind +
 * open, for startup. */
int hal_ad9361_kernel_unbind(void);
int hal_ad9361_kernel_bind(void);
int hal_ad9361_open(void);
void hal_ad9361_close(unsigned reset_gpio);
int hal_ad9361_take(void);
/* SPI transfers so far, and the SCLK requested from spidev. */
unsigned long hal_spi_transfers(void);
unsigned hal_spi_hz(void);

/* Interrupts. UIO lines (CH32, PPS) and the console UART line, which is backed
 * by the tty fd (the 8250 driver receives by interrupt). Handlers registered
 * through XScuGic_Connect run from hal_irq_dispatch(). quiesce masks the UIO
 * lines without changing the SDK's requested state; resume restores them. The
 * console line is never quiesced: the console is served in both modes. */
int hal_irq_pollfds(struct pollfd *fds, int max);
void hal_irq_dispatch(const struct pollfd *fds, int n);
/* 1 when a handler must run without waiting for poll() (console bytes already
 * pulled into the emulated FIFO). */
int hal_irq_ready(void);
void hal_irq_quiesce(void);
void hal_irq_resume(void);

/* Console tty emulation (hal_tty.c). */
int hal_tty_fd(void);
int hal_tty_rx_pending(void);
void hal_tty_flush(void);
void hal_tty_mute(int mute);
/* Called by the 16550 loopback emulation: run the console UART "ISR" now. */
void hal_irq_tty_raise(void);
#endif
