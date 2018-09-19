
#include "configMaster.h"
#include "blackbox_io.h"
#include "asyncfatfs.h"

/* How many bytes can we transmit per loop iteration when writing headers? */
static uint8_t blackboxMaxHeaderBytesPerIteration;

/* How many bytes can we write *this* iteration without overflowing transmit buffers or overstressing the OpenLog? */
int32_t blackboxHeaderBudget;

/**
 * Attempt to open the logging device.
 *
 * Returns true if successful.
 */
bool blackboxDeviceOpen(void)
{
	switch (BlackboxConfig()->device) {
		case BLACKBOX_DEVICE_SERIAL:
			break;
		
#ifdef USE_FLASHFS
		case BLACKBOX_DEVICE_FLASH:
			break;
#endif
		
#ifdef USE_SDCARD
		case BLACKBOX_DEVICE_SDCARD:
			if (afatfs_getFilesystemState() == AFATFS_FILESYSTEM_STATE_FATAL || afatfs_getFilesystemState() == AFATFS_FILESYSTEM_STATE_UNKNOWN || afatfs_isFull()) {
				return false;
			}
			
			blackboxMaxHeaderBytesPerIteration = BLACKBOX_TARGET_HEADER_BUDGET_PER_ITERATION;
			
			return true;
			break;
#endif
		
		default:
			return false;
	}
}
