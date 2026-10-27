#ifndef XIL_CACHE_H
#define XIL_CACHE_H
#include "xil_types.h"
static inline void Xil_ICacheEnable(void) {}
static inline void Xil_DCacheEnable(void) {}
static inline void Xil_DCacheFlushRange(UINTPTR a, u32 n) { (void)a; (void)n; }
static inline void Xil_DCacheInvalidateRange(UINTPTR a, u32 n) { (void)a; (void)n; }
#endif
