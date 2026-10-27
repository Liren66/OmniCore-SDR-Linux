/* XADC shim: the kernel xilinx-xadc IIO driver owns the XADC; the temperature
 * channel is read from sysfs and returned in the BSP's 16-bit raw format. */
#ifndef XADCPS_H
#define XADCPS_H
#include "xil_types.h"
#include "xstatus.h"

typedef struct { u16 DeviceId; u32 BaseAddress; } XAdcPs_Config;
typedef struct { XAdcPs_Config Config; u32 IsReady; u32 SeqMode; u32 SeqChannels; } XAdcPs;

#define XADCPS_CH_TEMP 0x00U
#define XADCPS_SEQ_CH_CALIB 0x00000001U
#define XADCPS_SEQ_CH_TEMP 0x00000100U
#define XADCPS_SEQ_MODE_SAFE 0U
#define XADCPS_SEQ_MODE_ONEPASS 1U
#define XADCPS_SEQ_MODE_CONTINPASS 2U
#define XADCPS_AVG_0_SAMPLES 0U
#define XADCPS_AVG_16_SAMPLES 1U

#define XAdcPs_RawToTemperature(AdcData) \
    ((((float)(AdcData) / 65536.0f) / 0.00198421639f) - 273.15f)

XAdcPs_Config *XAdcPs_LookupConfig(u16 id);
s32 XAdcPs_CfgInitialize(XAdcPs *x, XAdcPs_Config *cfg, u32 base);
void XAdcPs_SetSequencerMode(XAdcPs *x, u8 mode);
u8 XAdcPs_GetSequencerMode(XAdcPs *x);
void XAdcPs_SetAdcClkDivisor(XAdcPs *x, u8 div);
void XAdcPs_SetAvg(XAdcPs *x, u8 avg);
s32 XAdcPs_SetSeqChEnables(XAdcPs *x, u32 mask);
u32 XAdcPs_GetSeqChEnables(XAdcPs *x);
s32 XAdcPs_SetSeqAvgEnables(XAdcPs *x, u32 mask);
u16 XAdcPs_GetAdcData(XAdcPs *x, u8 channel);
#endif
