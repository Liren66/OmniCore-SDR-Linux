#ifndef XIL_EXCEPTION_H
#define XIL_EXCEPTION_H
#include "xil_types.h"
typedef void (*Xil_ExceptionHandler)(void *data);
#define XIL_EXCEPTION_ID_INT 5U
static inline void Xil_ExceptionRegisterHandler(u32 id, Xil_ExceptionHandler h, void *d)
{ (void)id; (void)h; (void)d; }
static inline void Xil_ExceptionEnable(void) {}
static inline void Xil_ExceptionDisable(void) {}
#endif
