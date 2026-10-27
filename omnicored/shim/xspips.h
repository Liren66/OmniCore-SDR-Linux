/* PS SPI shim over spidev (the AD9361 spi device is bound to spidev in USB mode). */
#ifndef XSPIPS_H
#define XSPIPS_H
#include "xil_types.h"
#include "xstatus.h"

typedef struct { u16 DeviceId; UINTPTR BaseAddress; u32 InputClockHz; } XSpiPs_Config;
typedef struct { XSpiPs_Config Config; u32 IsReady; u32 Options; u8 Prescaler; } XSpiPs;

#define XSPIPS_MASTER_OPTION 0x00000001U
#define XSPIPS_CLK_ACTIVE_LOW_OPTION 0x00000002U
#define XSPIPS_CLK_PHASE_1_OPTION 0x00000004U
#define XSPIPS_DECODE_SSELECT_OPTION 0x00000008U
#define XSPIPS_FORCE_SSELECT_OPTION 0x00000010U
#define XSPIPS_MANUAL_START_OPTION 0x00000020U

#define XSPIPS_CLK_PRESCALE_4 0x01U
#define XSPIPS_CLK_PRESCALE_8 0x02U
#define XSPIPS_CLK_PRESCALE_16 0x03U
#define XSPIPS_CLK_PRESCALE_32 0x04U
#define XSPIPS_CLK_PRESCALE_64 0x05U
#define XSPIPS_CLK_PRESCALE_128 0x06U
#define XSPIPS_CLK_PRESCALE_256 0x07U

XSpiPs_Config *XSpiPs_LookupConfig(u16 id);
s32 XSpiPs_CfgInitialize(XSpiPs *s, XSpiPs_Config *cfg, u32 base);
s32 XSpiPs_SetOptions(XSpiPs *s, u32 options);
s32 XSpiPs_SetClkPrescaler(XSpiPs *s, u8 prescaler);
s32 XSpiPs_SetSlaveSelect(XSpiPs *s, u8 ss);
s32 XSpiPs_PolledTransfer(XSpiPs *s, u8 *send, u8 *recv, u32 count);
#endif
