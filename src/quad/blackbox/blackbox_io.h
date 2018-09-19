#ifndef __BLACKBOX_IO_H
#define __BLACKBOX_IO_H

typedef enum BlackboxDevice {
	BLACKBOX_DEVICE_SERIAL = 0,
	
#ifdef USE_FLASHFS
	BLACKBOX_DEVICE_FLASH = 1,
#endif
	
#ifdef USE_SDCARD
	BLACKBOX_DEVICE_SDCARD = 2,
#endif
}BlackboxDevice;

#endif	// __BLACKBOX_IO_H
