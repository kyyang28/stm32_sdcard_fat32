#ifndef __ASYNCFATFS_H
#define __ASYNCFATFS_H

#include <stdint.h>
#include <stdbool.h>

typedef struct afatfsFile_t *afatfsFilePtr_t;

typedef enum {
	AFATFS_FILESYSTEM_STATE_UNKNOWN,			// 0
	AFATFS_FILESYSTEM_STATE_FATAL,				// 1
	AFATFS_FILESYSTEM_STATE_INITIALISATION,		// 2
	AFATFS_FILESYSTEM_STATE_READY				// 3
}afatfsFilesystemState_e;

typedef struct afatfsDirEntryPointer_t {
	uint32_t sectorNumberPhysical;
	int16_t entryIndex;
}afatfsDirEntryPointer_t;

typedef enum {
	AFATFS_OPERATION_IN_PROGRESS,
	AFATFS_OPERATION_SUCCESS,
	AFATFS_OPERATION_FAILURE
}afatfsOperationStatus_e;

typedef enum {
	AFATFS_ERROR_NONE = 0,
	AFATFS_ERROR_GENERIC = 1,
	AFATFS_ERROR_BAD_MBR = 2,
	AFATFS_ERROR_BAD_FILESYSTEM_HEADER = 3
}afatfsError_e;

typedef void (*afatfsFileCallback_t)(afatfsFilePtr_t file);
typedef void (*afatfsCallback_t)(void);

void afatfs_init(void);
bool afatfs_flush(void);
void afatfs_poll(void);

#endif	// __ASYNCFATFS_H
