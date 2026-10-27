/* GIC shim: each SDK interrupt id is backed by a UIO device (hal.c).
 * Enable/Disable mask the UIO interrupt; handlers run from the main loop. */
#ifndef XSCUGIC_H
#define XSCUGIC_H
#include "xil_types.h"
#include "xstatus.h"
#include "xil_exception.h"

typedef void (*Xil_InterruptHandler)(void *data);
typedef struct { u16 DeviceId; u32 CpuBaseAddress; u32 DistBaseAddress; } XScuGic_Config;
typedef struct { XScuGic_Config *Config; u32 IsReady; } XScuGic;

#define XSCUGIC_PENDING_CLR_OFFSET 0x00000280U

XScuGic_Config *XScuGic_LookupConfig(u16 id);
s32 XScuGic_CfgInitialize(XScuGic *gic, XScuGic_Config *cfg, u32 base);
s32 XScuGic_Connect(XScuGic *gic, u32 id, Xil_InterruptHandler h, void *ref);
void XScuGic_Enable(XScuGic *gic, u32 id);
void XScuGic_Disable(XScuGic *gic, u32 id);
void XScuGic_SetPriorityTriggerType(XScuGic *gic, u32 id, u8 prio, u8 trigger);
void XScuGic_InterruptHandler(void *gic);
/* Pending state is owned by the kernel GIC driver. */
static inline void XScuGic_DistWriteReg(XScuGic *gic, u32 off, u32 v) { (void)gic; (void)off; (void)v; }
#endif
