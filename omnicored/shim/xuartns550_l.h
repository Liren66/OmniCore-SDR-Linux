/* AXI UART16550 register shim: the kernel 8250 driver owns the UART (/dev/ttyS1);
 * register accesses from console.c are emulated on top of the tty (hal_tty.c). */
#ifndef XUARTNS550_L_H
#define XUARTNS550_L_H
#include "xil_types.h"

#define XUN_REG_OFFSET 0x1000U
#define XUN_RBR_OFFSET (XUN_REG_OFFSET + 0x00U)
#define XUN_THR_OFFSET (XUN_REG_OFFSET + 0x00U)
#define XUN_IER_OFFSET (XUN_REG_OFFSET + 0x04U)
#define XUN_IIR_OFFSET (XUN_REG_OFFSET + 0x08U)
#define XUN_FCR_OFFSET (XUN_REG_OFFSET + 0x08U)
#define XUN_LCR_OFFSET (XUN_REG_OFFSET + 0x0CU)
#define XUN_MCR_OFFSET (XUN_REG_OFFSET + 0x10U)
#define XUN_LSR_OFFSET (XUN_REG_OFFSET + 0x14U)
#define XUN_MSR_OFFSET (XUN_REG_OFFSET + 0x18U)

#define XUN_LSR_DATA_READY 0x01U
#define XUN_LSR_OVERRUN_ERROR 0x02U
#define XUN_LSR_PARITY_ERROR 0x04U
#define XUN_LSR_FRAMING_ERROR 0x08U
#define XUN_LSR_BREAK_INT 0x10U
#define XUN_LSR_ERROR_BREAK 0x1EU
#define XUN_LSR_TX_BUFFER_EMPTY 0x20U
#define XUN_LSR_TX_EMPTY 0x40U

#define XUN_FIFO_ENABLE 0x01U
#define XUN_FIFO_RX_RESET 0x02U
#define XUN_FIFO_TX_RESET 0x04U

#define XUN_LCR_8_DATA_BITS 0x03U

#define XUN_IER_RX_DATA 0x01U
#define XUN_IER_TX_EMPTY 0x02U
#define XUN_IER_RX_LINE 0x04U
#define XUN_MCR_LOOP 0x10U
#define XUN_FIFO_SIZE 16U

u32 XUartNs550_ReadReg(UINTPTR base, u32 offset);
void XUartNs550_WriteReg(UINTPTR base, u32 offset, u32 value);
void XUartNs550_SetBaud(UINTPTR base, u32 input_clock_hz, u32 baud);
void XUartNs550_SetLineControlReg(UINTPTR base, u32 value);
#endif
