#pragma once
#include <stdint.h>
typedef struct { uint32_t numer, denom; } mach_timebase_info_data_t;
typedef mach_timebase_info_data_t *mach_timebase_info_t;
int mach_timebase_info(mach_timebase_info_t info);
uint64_t mach_absolute_time(void);
/* Like mach_absolute_time but advancing during device sleep (CLOCK_BOOTTIME). */
uint64_t mach_continuous_time(void);
