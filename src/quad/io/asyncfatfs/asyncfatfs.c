
#include <stdio.h>				// for printf
#include "sdcard.h"				// including stdint.h, stdbool.h
#include "asyncfatfs.h"
#include "fat_standard.h"

/* 	FAT filesystems are allowed to differ from these parameters, but we choose not to support those
 *	weird filesystems
 */
#define AFATFS_SECTOR_SIZE				512
#define AFATFS_NUM_FATS					2

#define AFATFS_NUM_CACHE_SECTORS		8

#define AFATFS_MAX_OPEN_FILES			3

/* 	Turn the largest free block on the disk into one contiguous file 
 *	for efficient fragment-free allocation
 */
#define AFATFS_USE_FREEFILE

/* Open the cache sector for read access (it will be read from disk) */
#define AFATFS_CACHE_READ				1

/* Open the cache sector for write access (it will be marked dirty) */
#define AFATFS_CACHE_WRITE				2

/* Lock this sector to prevent its state from transitioning (prevent flushes to disk) */
#define AFATFS_CACHE_LOCK				4

/* Discard this sector in preference to other sectors when it is in the In-Sync state */
#define AFATFS_CACHE_DISCARDABLE		8

/* Increase the retain counter of the cache sector to prevent it from being discarded when in the In-Sync state */
#define AFATFS_CACHE_RETAIN				16

typedef enum {
	AFATFS_INITIALISATION_READ_MBR,
	AFATFS_INITIALISATION_READ_VOLUME_ID,
	
#ifdef AFATFS_USE_FREEFILE
	AFATFS_INITIALISATION_FREEFILE_CREATE,
	AFATFS_INITIALISATION_FREEFILE_CREATING,
	AFATFS_INITIALISATION_FREEFILE_FAT_SEARCH,
	AFATFS_INITIALISATION_FREEFILE_UPDATE_FAT,
	AFATFS_INITIALISATION_FREEFILE_SAVE_DIR_ENTRY,
	AFATFS_INITIALISATION_FREEFILE_LAST = AFATFS_INITIALISATION_FREEFILE_SAVE_DIR_ENTRY,
#endif
	
#ifdef AFATFS_USE_INTROSPECTIVE_LOGGING
	AFATFS_INITIALISATION_INTROSPEC_LOG_CREATE,
	AFATFS_INITIALISATION_INTROSPEC_LOG_CREATING,
#endif
	
	AFATFS_INITIALISATION_DONE
}afatfsInitialisationPhase_e;

typedef enum {
	AFATFS_FREE_SPACE_SEARCH_PHASE_FIND_HOLE,
	AFATFS_FREE_SPACE_SEARCH_PHASE_GROW_HOLE
}afatfsFreeSpaceSearchPhase_e;

typedef struct afatfsFreeSpaceSearch_t {
	uint32_t candidateStart;
	uint32_t candidateEnd;
	uint32_t bestGapStart;
	uint32_t bestGapLength;
	afatfsFreeSpaceSearchPhase_e phase;
}afatfsFreeSpaceSearch_t;

typedef struct afatfsFreeSpaceFAT_t {
	uint32_t startCluster;
	uint32_t endCluster;
}afatfsFreeSpaceFAT_t;

typedef enum {
	AFATFS_CACHE_STATE_EMPTY,
	AFATFS_CACHE_STATE_IN_SYNC,
	AFATFS_CACHE_STATE_READING,
	AFATFS_CACHE_STATE_WRITING,
	AFATFS_CACHE_STATE_DIRTY
}afatfsCacheBlockState_e;

typedef struct afatfsCacheBlockDescriptor_t {
	/* The physical sector index on disk that this cached block corresponds to */
	uint32_t sectorIndex;
	
	/* NOTE: We use an increasing timestamp to identify cache access times */
	
	/* This is the timestamp that this sector was first marked dirty at 
	 * (so we can flush sectors in write-order)
	 */
	uint32_t writeTimestamp;
	
	/* This is the last time the sector was accessed */
	uint32_t accessTimestamp;
	
	/* This is set to non-zero when we expect to write a consecutive series of this many blocks (including this block), 
	 * so we will tell SD-card to pre-erase those blocks.
	 *
	 * This counter only needs to be set on the first block of a consecutive write (though setting it, appropriately
	 * decreased, on the subsequent blocks won't hurt)
	 */
	uint16_t consecutiveEraseBlockCount;
	
	afatfsCacheBlockState_e state;
	
	/* The state of this block must not transition (do not flush to disk, do not discard). This is useful for a sector
	 * which is currently being written to by the application (so flushing it would be a waste of time).
	 *
	 * This is a binary state rather than a counter because we assume that only one party will be responsible for and 
	 * so consider locking a given sector
	 */
	unsigned locked:1;
	
	/*
	 * A counter for how many parties want this sector to be retained in memory (not discarded). If this value is
	 * non-zero, the sector may be flushed to disk if dirty but must remain in the cache. This is useful if we require 
	 * a directory sector to be cached in order to meet our response time requirements.
	 */
	unsigned retainCount:6;
	
	/*
	 * If this block is in the In Sync state, it should be discarded from the cache in preference to other blocks.
	 * This is useful for data that we don't expect to read again, e.g. data written to an append-only file.
	 * This hint is overridden by the locked and retainCount flags.
	 */
	unsigned discardable:1;
}afatfsCacheBlockDescriptor_t;

typedef enum {
	AFATFS_FILE_TYPE_NONE,
	AFATFS_FILE_TYPE_NORMAL,
	AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY,
	AFATFS_FILE_TYPE_DIRECTORY
}afatfsFileType_e;

typedef enum {
	AFATFS_FILE_OPERATION_NONE,
	AFATFS_FILE_OPERATION_CREATE_FILE,
	AFATFS_FILE_OPERATION_SEEK,			// Seek the file's cursorCluster forwards by seekOffset bytes
	AFATFS_FILE_OPERATION_CLOSE,
	AFATFS_FILE_OPERATION_TRUNCATE,
	AFATFS_FILE_OPERATION_UNLINK,
#ifdef AFATFS_USE_FREEFILE
	AFATFS_FILE_OPERATION_APPEND_SUPERCLUSTER,
	AFATFS_FILE_OPERATION_LOCKED,
#endif
	AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER,
	AFATFS_FILE_OPERATION_EXTEND_SUBDIRECTORY
}afatfsFileOperation_e;

typedef struct afatfsCreateFile_t {
	afatfsFileCallback_t callback;

	uint8_t phase;
	uint8_t filename[FAT_FILENAME_LENGTH];
}afatfsCreateFile_t;

typedef struct afatfsSeek_t {
	afatfsFileCallback_t callback;

	uint32_t seekOffset;
}afatfsSeek_t;

typedef enum {
	AFATFS_APPEND_SUPERCLUSTER_PHASE_INIT = 0,
	AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FREEFILE_DIRECTORY,
	AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FAT,
	AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FILE_DIRECTORY
}afatfsAppendSuperclusterPhase_e;

typedef struct afatfsAppendSuperCluster_t {
	uint32_t previousCluster;
	uint32_t fatRewriteStartCluster;
	uint32_t fatRewriteEndCluster;
	afatfsAppendSuperclusterPhase_e phase;
}afatfsAppendSuperCluster_t;

typedef enum {
	AFATFS_APPEND_FREE_CLUSTER_PHASE_INITIAL = 0,
	AFATFS_APPEND_FREE_CLUSTER_PHASE_FIND_FREESPACE,
	AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT1,
	AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT2,
	AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FILE_DIRECTORY,
	AFATFS_APPEND_FREE_CLUSTER_PHASE_COMPLETE,
	AFATFS_APPEND_FREE_CLUSTER_PHASE_FAILURE
}afatfsAppendFreeClusterPhase_e;

typedef struct afatfsAppendFreeCluster_t {
	uint32_t previousCluster;
	uint32_t searchCluster;
	afatfsAppendFreeClusterPhase_e phase;
}afatfsAppendFreeCluster_t;

typedef enum {
	AFATFS_EXTEND_SUBDIRECTORY_PHASE_INITIAL = 0,
	AFATFS_EXTEND_SUBDIRECTORY_PHASE_ADD_FREE_CLUSTER = 0,
	AFATFS_EXTEND_SUBDIRECTORY_PHASE_WRITE_SECTORS,
	AFATFS_EXTEND_SUBDIRECTORY_PHASE_SUCCESS,
	AFATFS_EXTEND_SUBDIRECTORY_PHASE_FAILURE
}afatfsExtendSubdirectoryPhase_e;

typedef struct afatfsExtendSubdirectory_t {
	/* We need to call this as a sub-operation so we have it as our first member to be compatible with its memory layout */
	afatfsAppendFreeCluster_t appendFreeCluster;
	
	afatfsExtendSubdirectoryPhase_e phase;
	
	uint32_t parentDirectoryCluster;
	afatfsFileCallback_t callback;
}afatfsExtendSubdirectory_t;

typedef enum {
	AFATFS_TRUNCATE_FILE_INITIAL = 0,
	AFATFS_TRUNCATE_FILE_UPDATE_DIRECTORY = 0,
	AFATFS_TRUNCATE_FILE_ERASE_FAT_CHAIN_NORMAL,
#ifdef AFATFS_USE_FREEFILE
	AFATFS_TRUNCATE_FILE_ERASE_FAT_CHAIN_CONTIGUOUS,
	AFATFS_TRUNCATE_FILE_PREPEND_TO_FREEFILE,
#endif
	AFATFS_TRUNCATE_FILE_SUCCESS
}afatfsTruncateFilePhase_e;

typedef struct afatfsTruncateFile_t {
	uint32_t startCluster;			// first cluster to erase
	uint32_t currentCluster;		// used to mark progress
	uint32_t endCluster;			// Optional, for contiguous files set to 1 past the end cluster of the file, otherwise set to 0
	afatfsFileCallback_t callback;
	afatfsTruncateFilePhase_e phase;
}afatfsTruncateFile_t;

typedef struct afatfsDeleteFile_t {
	afatfsTruncateFile_t truncateFile;
	afatfsCallback_t callback;
}afatfsUnlinkFile_t;

typedef struct afatfsCloseFile_t {
	afatfsCallback_t callback;
}afatfsCloseFile_t;

typedef struct afatfsFileOperation_t {
	afatfsFileOperation_e operation;
	union {
		afatfsCreateFile_t createFile;
		afatfsSeek_t seek;
		afatfsAppendSuperCluster_t appendSupercluster;
		afatfsAppendFreeCluster_t appendFreecluster;
		afatfsExtendSubdirectory_t extendSubdirectory;
		afatfsUnlinkFile_t unlinkFile;
		afatfsTruncateFile_t truncateFile;
		afatfsCloseFile_t closeFile;
	}state;
}afatfsFileOperation_t;

typedef struct afatfsFile_t {
	afatfsFileType_e type;
	
	/* The byte offset of the cursor within the file */
	uint32_t cursorOffset;
	
	/* The file size in bytes as seen by users of the filesystem (the exact length of the file they've written).
	 * 
	 * This is only used by users of the filesystem, not us, so it only needs to be up-to-date for fseek() (to clip
	 * seeks to the EOF), fread(), feof(), and fclose() (which writes the logicalSize to directory).
	 *
	 * It becomes out of date when we fwrite() to extend the length of the file. In this situation, feof() is properly
	 * true, so we don't have to update the logicalSize for fread() or feof() to get the correct result. We 
	 * only need to update it when we seek backwards (so we don't forget the logical EOF position), or fclose().
	 */
	uint32_t logicalSize;
	
	/* The allocated size in bytes based on how many clusters have been assigned to the file. Always
	 * a multiple of the cluster size.
	 *
	 * This is an underestimate for existing files, because we don't bother to check precisely how long the 
	 * chain is at the time the file is opened (it might be longer than needed to contain the logical size), 
	 * 
	 */
	uint32_t physicalSize;
	
	/* 
	 * The cluster that the file pointer is currently within. When seeking to the end of the file, this
	 * will be set to zero.
	 */
	uint32_t cursorCluster;
	
	/*
	 * The cluster before the one the file pointer is inside. This is set to zero when at the start of the file.
	 */
	uint32_t cursorPreviousCluster;
	
	/* A combination of AFATFS_FILE_MODE_* flags */
	uint8_t mode;
	
	/* Combination of AFATFS_FILE_ATTRIBUTE_* flags for directory entry of this file */
	uint8_t attrib;
	
	/*
	 * We hold on to one sector entry in the cache and remember its index here. The cache is invalidated when
	 * we seek across a sector boundary. This allows fwrite() to complete faster because it doesn't need to
	 * check the cache on every call.
	 */
	int8_t writeLockedCacheIndex;
	
	/* Ditto for fread() */
	int8_t readRetainCacheIndex;
	
	/* The position of our directory entry on the disk (so we can update it without consulting a parent
	 * directory file) 
	 */
	afatfsDirEntryPointer_t directoryEntryPos;
	
	/* The first cluster number of the file, or 0 if this file is empty */
	uint32_t firstCluster;
	
	/* State for a queued operation on the file */
	struct afatfsFileOperation_t operation;
}afatfsFile_t;

typedef struct afatfs_t {
	fatFilesystemType_e filesystemType;				// FAT type, FAT12, FAT16, FAT32
	afatfsFilesystemState_e filesystemState;		// FAT state, unknown, fatal, initialisation, ready
	afatfsInitialisationPhase_e initPhase;
	
	/* State used during FS initialisation where only one member of the union is used at a time */
#ifdef AFATFS_USE_FREEFILE
	union {
		afatfsFreeSpaceSearch_t freeSpaceSearch;
		afatfsFreeSpaceFAT_t freeSpaceFAT;
	}initState;
#endif
	
	uint8_t cache[AFATFS_SECTOR_SIZE * AFATFS_NUM_CACHE_SECTORS];
	afatfsCacheBlockDescriptor_t cacheDescriptor[AFATFS_NUM_CACHE_SECTORS];
	uint32_t cacheTimer;
	
	int cacheDirtyEntries;	/* The number of cache entries in the AFATFS_CACHE_STATE_DIRTY state */
	bool cacheFlushInProgress;
	
	afatfsFile_t openFiles[AFATFS_MAX_OPEN_FILES];

#ifdef AFATFS_USE_FREEFILE
	afatfsFile_t freeFile;
#endif
	
#ifdef AFATFS_USE_INTROSPECTIVE_LOGGING
	afatfsFile_t introSpecLog;
#endif
	
	afatfsError_e lastError;
	
	bool filesystemFull;
	
	/* The current working directory */
	afatfsFile_t currentDirectory;
	
	/* The physical sector that the first partition on the device begins at */
	uint32_t partitionStartSector;
	
	/* The first sector of the first FAT */
	uint32_t fatStartSector;
	
	/* The size in sectors of a single FAT */
	uint32_t fatSectors;
	
	/* Number of clusters available for storing user data.
	 * Note that clusters are numbered starting from 2, so the index 
	 * of the last cluster on the volume is numClusters + 1!!!
	 */
	uint32_t numClusters;
	uint32_t clusterStartSector;		// The physical sector that the clusters area begins at
	uint32_t sectorsPerCluster;
	
	/* Number of the cluster that we previously allocated (i.e. free->occupied)
	 * Searches for a free cluster will begin after this cluster.
	 */
	uint32_t lastClusterAllocated;
	
	/* Mask to be ANDed with a byte offset within a file to give the offset within the cluster */
	uint32_t byteInClusterMask;
	
	/* Present on FAT32 and set to zero on FAT16 */
	uint32_t rootDirectoryCluster;

	/* Zero on FAT32, for FAT16 the number of sectors that the root directory occupies */
	uint32_t rootDirectorySectors;
}afatfs_t;

static afatfs_t afatfs;

/**
 * Check for conditions that should always be true (and if otherwise mean a bug or a corrupt filesystem).
 *
 * If the condition is false, the filesystem is marked as being in a fatal state.
 *
 * Returns the value of the condition.
 */
static bool afatfs_assert(bool condition)
{
	/* if condition is true, always returns true, otherwise change filesystemState to AFATFS_FILESYSTEM_STATE_FATAL */
	if (!condition) {
		if (afatfs.lastError == AFATFS_ERROR_NONE) {
			afatfs.lastError = AFATFS_ERROR_GENERIC;
		}
		
		afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_FATAL;
	}
	
	return condition;
}

/**
 * Attemp to flush dirty cache pages out to the sdcard, returning true if all flushable data has been flushed.
 */
bool afatfs_flush(void)
{
	return true;
}


/**
 * Find or allocate a cache sector for the given sector index on disk. Returns a block which matches one of these
 * conditions (in descending order of preference):
 *
 * - The requested sector that already exists in the cache
 * - The index of an empty sector
 * - The index of a synced discardable sector
 * - The index of the oldest synced sector
 *
 * Otherwise it returns -1 to signal failure (cache is full!)
 */
static int afatfs_allocateCacheSector(uint32_t sectorIndex)
{
	int allocateIndex;
	int emptyIndex = -1, discardableIndex = -1;
	
	return 0;			// just for now
}

/**
 * Get a cache entry for the given sector and store a pointer to the cached memory in *buffer.
 *
 * physicalSectorIndex - The index of the sector in the SD card to cache
 * sectorflags         - A union of AFATFS_CACHE_* constants that says which operations the sector will be cached for.
 * buffer              - A pointer to the 512-byte memory buffer for the sector will be stored here upon success
 * eraseCount          - For write operations, set to a non-zero number to hint that we plan to write that many sectors
 *                       consecutively (including this sector)
 *
 * Returns:
 *     AFATFS_OPERATION_SUCCESS     - On success
 *     AFATFS_OPERATION_IN_PROGRESS - Card is busy, call again later
 *     AFATFS_OPERATION_FAILURE     - When the filesystem encounters a fatal error
 */
static afatfsOperationStatus_e afatfs_cacheSector(uint32_t physicalSectorIndex, uint8_t **buffer, uint8_t sectorFlags, uint32_t eraseCount)
{
	/* We never write to the MBR, so any attempt to write there is an asyncfatfs bug */
	/* sectorFlags = AFATFS_CACHE_READ & AFATFS_CACHE_DISCARDABLE
	 * sectorFlags & AFATFS_CACHE_WRITE) == 0 is true;
	 * physicalSectorIndex = 0, physicalSectorIndex != 0 is false
	 */
	if (!afatfs_assert(sectorFlags & AFATFS_CACHE_WRITE) == 0 || physicalSectorIndex != 0) {
		return AFATFS_OPERATION_FAILURE;
	}

	/* Allocate cache sector */
	int cacheSectorIndex = afatfs_allocateCacheSector(physicalSectorIndex);
	
	/* Error checking */
	if (cacheSectorIndex == -1) {
		/* We don't have enough free cache to service this request right now, try again later */
		return AFATFS_OPERATION_IN_PROGRESS;
	}
	
	
	
	return AFATFS_OPERATION_SUCCESS;
}

static void afatfs_initContinue(void)
{
#ifdef AFATFS_USE_FREEFILE
	afatfsOperationStatus_e status;
#endif
	
	uint8_t *sector;
	
	doMore:
	
	switch (afatfs.initPhase) {
		case AFATFS_INITIALISATION_READ_MBR:
			if (afatfs_cacheSector(0, &sector, AFATFS_CACHE_READ | AFATFS_CACHE_DISCARDABLE, 0) == AFATFS_OPERATION_SUCCESS) {
				printf("%s, %d\r\n", __FUNCTION__, __LINE__);
			}
			break;
		
		case AFATFS_INITIALISATION_READ_VOLUME_ID:
			break;
		
#ifdef AFATFS_USE_FREEFILE
		case AFATFS_INITIALISATION_FREEFILE_CREATE:
			break;
		
		case AFATFS_INITIALISATION_FREEFILE_CREATING:
			break;
		
		case AFATFS_INITIALISATION_FREEFILE_FAT_SEARCH:
			break;
		
		case AFATFS_INITIALISATION_FREEFILE_UPDATE_FAT:
			break;
		
		case AFATFS_INITIALISATION_FREEFILE_SAVE_DIR_ENTRY:
			break;
#endif
		
#ifdef AFATFS_USE_INTROSPECTIVE_LOGGING
		case AFATFS_INITIALISATION_INTROSPEC_LOG_CREATE:
			break;
		
		case AFATFS_INITIALISATION_INTROSPEC_LOG_CREATING:
			break;
#endif
		
		case AFATFS_INITIALISATION_DONE:
			afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_READY;
			break;
	}
}

/**
 * Check files for pending operations and execute them
 */
static void afatfs_fileOperationsPoll(void)
{
	
}

/**
 * Check to see if there are any pending operations on the filesystem and perform a little work (without waiting on the
 * sdcard). You must call this periodically.
 */
void afatfs_poll(void)
{
	/* Only attempt to continue FS operations if the card is present & ready, otherwise we would just be wasting time */
	if (sdcard_poll()) {
		afatfs_flush();			// TODO: need to be implemented
		
		switch (afatfs.filesystemState) {
			case AFATFS_FILESYSTEM_STATE_INITIALISATION:
				afatfs_initContinue();
				break;
			
			case AFATFS_FILESYSTEM_STATE_READY:
				afatfs_fileOperationsPoll();
				break;
			
			default:
				;
		}
	}
}

/* Initialisation of AFATFS */
void afatfs_init(void)
{
	afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_INITIALISATION;
	afatfs.initPhase = AFATFS_INITIALISATION_READ_MBR;
	afatfs.lastClusterAllocated = FAT_SMALLEST_LEGAL_CLUSTER_NUMBER;
	
#ifdef AFATFS_USE_INTROSPECTIVE_LOGGING
	sdcard_setProfilerCallback(afatfs_sdcardProfilerCallback);
#endif
}
