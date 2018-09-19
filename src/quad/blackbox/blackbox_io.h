#ifndef __BLACKBOX_IO_H
#define __BLACKBOX_IO_H

#include <stdbool.h>
#include <stdint.h>

/**
 * Ideally, each iteration in which we are logging headers would write a similar amount of data to the device as a 
 * regular logging iteration. This way we won't hog the CPU by making a gigantic write.
 */
#define BLACKBOX_TARGET_HEADER_BUDGET_PER_ITERATION					64

extern int32_t blackboxHeaderBudget;

typedef enum BlackboxDevice {
	BLACKBOX_DEVICE_SERIAL = 0,
	
#ifdef USE_FLASHFS
	BLACKBOX_DEVICE_FLASH = 1,
#endif
	
#ifdef USE_SDCARD
	BLACKBOX_DEVICE_SDCARD = 2,
#endif
}BlackboxDevice;

bool blackboxDeviceOpen(void);

#endif	// __BLACKBOX_IO_H
