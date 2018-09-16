#ifndef __ASYNCFATFS_H
#define __ASYNCFATFS_H

#include <stdint.h>
#include <stdbool.h>

#include "fat_standard.h"

typedef struct afatfsFile_t *afatfsFilePtr_t;

typedef enum {
	AFATFS_FILESYSTEM_STATE_UNKNOWN,			// 0
	AFATFS_FILESYSTEM_STATE_FATAL,				// 1
	AFATFS_FILESYSTEM_STATE_INITIALIZATION,		// 2
	AFATFS_FILESYSTEM_STATE_READY				// 3
}afatfsFilesystemState_e;

typedef struct afatfsDirEntryPointer_t {
	uint32_t sectorNumberPhysical;
	int16_t entryIndex;
}afatfsDirEntryPointer_t;

typedef afatfsDirEntryPointer_t afatfsFinder_t;

typedef enum {
	AFATFS_OPERATION_IN_PROGRESS,		// 0
	AFATFS_OPERATION_SUCCESS,			// 1
	AFATFS_OPERATION_FAILURE			// 2
}afatfsOperationStatus_e;

typedef enum {
	AFATFS_SEEK_SET,					// 0
	AFATFS_SEEK_CUR,					// 1
	AFATFS_SEEK_END						// 2
}afatfsSeek_e;

typedef enum {
	AFATFS_ERROR_NONE = 0,
	AFATFS_ERROR_GENERIC = 1,
	AFATFS_ERROR_BAD_MBR = 2,
	AFATFS_ERROR_BAD_FILESYSTEM_HEADER = 3
}afatfsError_e;

typedef void (*afatfsFileCallback_t)(afatfsFilePtr_t file);
typedef void (*afatfsCallback_t)(void);

bool afatfs_chdir(afatfsFilePtr_t dirHandle);

void afatfs_findFirst(afatfsFilePtr_t directory, afatfsFinder_t *finder);
afatfsOperationStatus_e afatfs_findNext(afatfsFilePtr_t directory, afatfsFinder_t *finder, fatDirectoryEntry_t **dirEntry);
void afatfs_findLast(afatfsFilePtr_t directory);

void afatfs_init(void);
bool afatfs_flush(void);
void afatfs_poll(void);

#endif	// __ASYNCFATFS_H
