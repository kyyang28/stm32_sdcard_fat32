
#include <stdio.h>
#include <string.h>

#include "configMaster.h"
#include "blackbox_io.h"
#include "asyncfatfs.h"
#include "maths.h"

#define LOGFILE_PREFIX						"LOG"
#define LOGFILE_SUFFIX						"BFL"

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

static void blackboxLogFileCreated(afatfsFilePtr_t file)
{
	printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);

	if (file) {
		blackboxSDCard.logFile = file;
		
		blackboxSDCard.largetstLogFileNumber++;
		
		blackboxSDCard.state = BLACKBOX_SDCARD_READY_TO_LOG;
		printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
	} else {
		/* Retry */
		blackboxSDCard.state = BLACKBOX_SDCARD_READY_TO_CREATE_LOG;
		printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
	}
}

/**
 * Function to create the log file.
 */
static void blackboxCreateLogFile(void)
{
//	printf("blackboxSDCard.largetstLogFileNumber: %u, %s, %s, %d\r\n",blackboxSDCard.largetstLogFileNumber, __FILE__, __FUNCTION__, __LINE__);
	uint32_t remainder = blackboxSDCard.largetstLogFileNumber + 1;
	
	char filename[] = LOGFILE_PREFIX "00000." LOGFILE_SUFFIX;
	
//	printf("filename: %s, %s, %s, %d\r\n",filename, __FILE__, __FUNCTION__, __LINE__);		// LOG00000.BFL
	
	/* Assigning the LOG number */
	for (int i = 7; i >= 3; i--) {
		filename[i] = (remainder % 10) + '0';
		remainder /= 10;
	}
	
	blackboxSDCard.state = BLACKBOX_SDCARD_WAITING;

	printf("filename: %s, %s, %s, %d\r\n", filename, __FILE__, __FUNCTION__, __LINE__);		// LOG00001.BFL
	
	afatfs_fopen(filename, "as", blackboxLogFileCreated);
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
				
				/* Create "logs" directory(folder) on the SDCard */
				afatfs_mkdir("logs", blackboxLogDirCreated);
//				afatfs_mkdir("quadLogs", blackboxLogDirCreated);	// just for testing
			}
			break;
		
		case BLACKBOX_SDCARD_WAITING:
			/* Waiting for directory entry to be created */
			break;
		
		case BLACKBOX_SDCARD_ENUMERATE_FILES:		// list all the files we need to create on the SDCard.
			while (afatfs_findNext(blackboxSDCard.logDirectory, &blackboxSDCard.logDirectoryFinder, &directoryEntry) == AFATFS_OPERATION_SUCCESS) {
//				printf("directoryEntry->filename: %s\r\n", directoryEntry->filename);
//				printf("directoryEntry->fileSize: %u\r\n", directoryEntry->fileSize);
//				printf("directoryEntry->attrib: %u\r\n", directoryEntry->attrib);
//				printf("directoryEntry->creationDate: %u\r\n", directoryEntry->creationDate);
//				printf("directoryEntry->creationTime: %u\r\n", directoryEntry->creationTime);
				if (directoryEntry && !fat_isDirectoryEntryTerminator(directoryEntry)) {
					printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
					/* If this is a log file, parse the log number from the filename */
					if (strncmp(directoryEntry->filename, LOGFILE_PREFIX, strlen(LOGFILE_PREFIX)) == 0
						&& strncmp(directoryEntry->filename + 8, LOGFILE_SUFFIX, strlen(LOGFILE_SUFFIX)) == 0) {
						char logSequenceNumberString[6];
							
						memcpy(logSequenceNumberString, directoryEntry->filename + 3, 5);
						logSequenceNumberString[5] = '\0';
						printf("logSequenceNumberString: %s\r\n", logSequenceNumberString);
							
						blackboxSDCard.largetstLogFileNumber = MAX((uint32_t) atoi(logSequenceNumberString), blackboxSDCard.largetstLogFileNumber);
					}
				} else {
					/* We are done checking all the files on the card, now we can create a new log file */
					afatfs_findLast(blackboxSDCard.logDirectory);
					
					blackboxSDCard.state = BLACKBOX_SDCARD_CHANGE_INTO_LOG_DIRECTORY;
//					printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
					goto doMore;
				}
			}
			break;
		
		case BLACKBOX_SDCARD_CHANGE_INTO_LOG_DIRECTORY:
			/* Change into the log directory */
			if (afatfs_chdir(blackboxSDCard.logDirectory)) {
				/* We no longer need our open handle on the log directory */
				afatfs_fclose(blackboxSDCard.logDirectory, NULL);
				blackboxSDCard.logDirectory = NULL;
				
				blackboxSDCard.state = BLACKBOX_SDCARD_READY_TO_CREATE_LOG;
				
				goto doMore;
			}
			break;
		
		case BLACKBOX_SDCARD_READY_TO_CREATE_LOG:
			blackboxCreateLogFile();
			break;
		
		case BLACKBOX_SDCARD_READY_TO_LOG:
			/* Log has been created!! */
			printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
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
