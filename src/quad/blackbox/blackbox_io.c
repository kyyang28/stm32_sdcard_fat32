
#include <stdio.h>

#include "configMaster.h"
#include "blackbox_io.h"
#include "asyncfatfs.h"
#include "maths.h"

/* How many bytes can we transmit per loop iteration when writing headers? */
static uint8_t blackboxMaxHeaderBytesPerIteration;

/* How many bytes can we write *this* iteration without overflowing transmit buffers or overstressing the OpenLog? */
int32_t blackboxHeaderBudget;

#ifdef USE_SDCARD
static struct {
	afatfsFilePtr_t logFile;
	afatfsFilePtr_t logDirectory;
	afatfsFinder_t logDirectoryFinder;
	uint32_t largetstLogFileNumber;
	
	enum {
		BLACKBOX_SDCARD_INITIAL,						// 0
		BLACKBOX_SDCARD_WAITING,						// 1
		BLACKBOX_SDCARD_ENUMERATE_FILES,				// 2
		BLACKBOX_SDCARD_CHANGE_INTO_LOG_DIRECTORY,		// 3
		BLACKBOX_SDCARD_READY_TO_CREATE_LOG,			// 4
		BLACKBOX_SDCARD_READY_TO_LOG					// 5
	} state;
} blackboxSDCard;
#endif

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

/**
 * Call once every loop iteration in order to maintain the global blackboxHeaderBudget with the number of bytes we can transmit this iteration.
 */
void blackboxReplenishHeaderBudget(void)
{
	int32_t freeSpace;
	
	switch (BlackboxConfig()->device) {
		case BLACKBOX_DEVICE_SERIAL:
//			freeSpace = serialTxBytesFree(blackboxPort);
			break;
		
#ifdef USE_FLASHFS
		case BLACKBOX_DEVICE_FLASH:
//			freeSpace = flashfsGetWriteBufferFreeSpace();
			break;
#endif
		
#ifdef USE_SDCARD
		case BLACKBOX_DEVICE_SDCARD:
			freeSpace = afatfs_getFreeBufferSpace();
			break;
#endif
		default:
			freeSpace = 0;
	}
	
	/* blackboxMaxHeaderBytesPerIteration = BLACKBOX_TARGET_HEADER_BUDGET_PER_ITERATION (64) */
	blackboxHeaderBudget = MIN(MIN(freeSpace, blackboxHeaderBudget + blackboxMaxHeaderBytesPerIteration), BLACKBOX_MAX_ACCUMULATED_HEADER_BUDGET);
}

#ifdef USE_SDCARD

static void blackboxLogDirCreated(afatfsFilePtr_t directory)
{
//	printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
	if (directory) {
		blackboxSDCard.logDirectory = directory;
		
		afatfs_findFirst(blackboxSDCard.logDirectory, &blackboxSDCard.logDirectoryFinder);
		
		blackboxSDCard.state = BLACKBOX_SDCARD_ENUMERATE_FILES;
	} else {
		/* Retry */
		blackboxSDCard.state = BLACKBOX_SDCARD_INITIAL;
	}
}

/**
 * Begin a new log on the SDCard.
 *
 * Keep calling until the function returns true (open is complete).
 */
static bool blackboxSDCardBeginLog(void)
{
//	printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
	fatDirectoryEntry_t *directoryEntry;
	
	doMore:
	
	switch (blackboxSDCard.state) {
		case BLACKBOX_SDCARD_INITIAL:
//			printf("afatfs_getFilesystemState(): %u, %s, %s, %d\r\n", afatfs_getFilesystemState(), __FILE__, __FUNCTION__, __LINE__);
			if (afatfs_getFilesystemState() == AFATFS_FILESYSTEM_STATE_READY) {
//				printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
				blackboxSDCard.state = BLACKBOX_SDCARD_WAITING;
				
				/* Create "logs" folder in the SDCard */
				afatfs_mkdir("logs", blackboxLogDirCreated);
			}
			break;
		
		case BLACKBOX_SDCARD_WAITING:
			/* Waiting for directory entry to be created */
			break;
		
		case BLACKBOX_SDCARD_ENUMERATE_FILES:		// list all the files we need to create in the SDCard.
			
			break;
		
		case BLACKBOX_SDCARD_CHANGE_INTO_LOG_DIRECTORY:
			
			break;
		
		case BLACKBOX_SDCARD_READY_TO_CREATE_LOG:
			
			break;
		
		case BLACKBOX_SDCARD_READY_TO_LOG:
			/* Log has been created!! */
			return true;
	}
	
	return false;	// Not finished init yet.
}

#endif	// USE_SDCARD

/**
 * Begin a new log (for devices which support separations between the logs of multiple flights).
 *
 * Keep calling until the function returns true (open is complete).
 */
bool blackboxDeviceBeginLog(void)
{
	switch (BlackboxConfig()->device) {
#ifdef USE_SDCARD
		case BLACKBOX_DEVICE_SDCARD:
			return blackboxSDCardBeginLog();
#endif
		default:
			return true;
	}
}
