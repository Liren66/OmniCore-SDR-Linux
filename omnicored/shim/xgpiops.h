/* PS GPIO shim over the Linux GPIO character device (zynq_gpio, pin = line offset). */
#ifndef XGPIOPS_H
#define XGPIOPS_H
#include "xil_types.h"
#include "xstatus.h"

typedef struct { u16 DeviceId; u32 BaseAddr; } XGpioPs_Config;
typedef struct { XGpioPs_Config GpioConfig; u32 IsReady; } XGpioPs;

XGpioPs_Config *XGpioPs_LookupConfig(u16 id);
s32 XGpioPs_CfgInitialize(XGpioPs *g, XGpioPs_Config *cfg, u32 base);
void XGpioPs_SetDirectionPin(XGpioPs *g, u32 pin, u32 direction);
void XGpioPs_SetOutputEnablePin(XGpioPs *g, u32 pin, u32 enable);
void XGpioPs_WritePin(XGpioPs *g, u32 pin, u32 data);
u32 XGpioPs_ReadPin(XGpioPs *g, u32 pin);
#endif
