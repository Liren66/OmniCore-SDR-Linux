/* XTime is a CLOCK_MONOTONIC microsecond count on Linux. */
#ifndef XTIME_L_H
#define XTIME_L_H
#include "xil_types.h"
typedef u64 XTime;
#define COUNTS_PER_SECOND 1000000ULL
void XTime_GetTime(XTime *t);
#endif
