/* Register access through /dev/mem (see hal.c). Addresses are physical. */
#ifndef XIL_IO_H
#define XIL_IO_H
#include "xil_types.h"

volatile void *hal_mmio(UINTPTR addr);

static inline u32 Xil_In32(UINTPTR addr) { return *(volatile u32 *)hal_mmio(addr); }
static inline void Xil_Out32(UINTPTR addr, u32 v) { *(volatile u32 *)hal_mmio(addr) = v; }
static inline u16 Xil_In16(UINTPTR addr) { return *(volatile u16 *)hal_mmio(addr); }
static inline void Xil_Out16(UINTPTR addr, u16 v) { *(volatile u16 *)hal_mmio(addr) = v; }
static inline u8 Xil_In8(UINTPTR addr) { return *(volatile u8 *)hal_mmio(addr); }
static inline void Xil_Out8(UINTPTR addr, u8 v) { *(volatile u8 *)hal_mmio(addr) = v; }
#endif
