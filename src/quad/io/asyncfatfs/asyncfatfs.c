
#include <stdio.h>				// for printf
#include <string.h>				// memcpy

#include "sdcard.h"				// including stdint.h, stdbool.h
#include "asyncfatfs.h"			// including fat_standard.h
//#include "fat_standard.h"
#include "maths.h"				// MIN, MAX, etc

/* 	FAT filesystems are allowed to differ from these parameters, but we choose not to support those
 *	weird filesystems
 */
#define AFATFS_SECTOR_SIZE							512
#define AFATFS_NUM_FATS								2

#define AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR			(AFATFS_SECTOR_SIZE / sizeof(uint32_t))		// 512 / 4 = 128
#define AFATFS_FAT16_FAT_ENTRIES_PER_SECTOR			(AFATFS_SECTOR_SIZE / sizeof(uint16_t))		// 512 / 2 = 256

#define AFATFS_FILES_PER_DIRECTORY_SECTOR			(AFATFS_SECTOR_SIZE / sizeof(fatDirectoryEntry_t))	// 512 / 32 = 16

#define AFATFS_NUM_CACHE_SECTORS					8

#define AFATFS_MAX_OPEN_FILES						3

/* In MBR (Master Boot Record), the partition table starts at offset (in Hexademical) 0x1BE which is 446 (in Integer)
 *
 * Each partition table is 16 bytes in length
 *
 * Master Boot Record / Extended Partition Boot Record
 * (offset)
 * 0x0000 to 0x01BD - First 446 bytes (boot loader code)
 * 0x01BE to 0x01CD - Partition entry 1 (16 bytes)
 * 0x01CE to 0x01DD - Partition entry 2 (16 bytes)
 * 0x01DE to 0x01ED - Partition entry 3 (16 bytes)
 * 0x01EE to 0x01FD - Partition entry 4 (16 bytes)
 * 0x01FE to 0x01FF - Boot signature (55 AA) (2 bytes)
 *
 * In total,
 * 	boot loader code (446 bytes) + entry 1 (16 bytes) + entry 2 (16 bytes) + entry 3 (16 bytes) + entry 4 (16 bytes) + Boot signature (2 bytes) = 512 bytes
 */
#define AFATFS_PARTITION_TABLE_START_OFFSET			446		// 446 = 0x1BE

/*
 * How many blocks will we write in a row before we bother using the SDcard's multiple block write method?
 * If this define is omitted, this disables multi-block write.
 */
#define AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT		4

/* 	Turn the largest free block on the disk into one contiguous file 
 *	for efficient fragment-free allocation
 */
#define AFATFS_USE_FREEFILE

/* When allocating a freefile, leave this many clusters un-allocated for regular files to use */
#define AFATFS_FREEFILE_LEAVE_CLUSTERS 				100

/* Filename in 8.3 format */
#define AFATFS_FREESPACE_FILENAME					"FREESPAC.E"

#define AFATFS_INTROSPEC_LOG_FILENAME				"ASYNCFAT.LOG"

#define AFATFS_DEFAULT_FILE_DATE 					FAT_MAKE_DATE(2015, 12, 01)
#define AFATFS_DEFAULT_FILE_TIME 					FAT_MAKE_TIME(00, 00, 00)

/* +-------------------------------------------------------------------------------------------+ */
/* +                                         FILE MODE                                         + */
/* +-------------------------------------------------------------------------------------------+ */

/* Read from the file */
#define AFATFS_FILE_MODE_READ						1

/* Write to the file */
#define AFATFS_FILE_MODE_WRITE						2

/* Append to the file, may not be combined with the write flag */
#define AFATFS_FILE_MODE_APPEND						4

/* File will occupy a series of superclusters (only valid for creating new files) */
#define AFATFS_FILE_MODE_CONTIGUOUS					8

/* File should be created if it doesn't exist */
#define AFATFS_FILE_MODE_CREATE						16

/* The file's directory entry should be locked in cache so we can read it with no latency */
#define AFATFS_FILE_MODE_RETAIN_DIRECTORY			32

/* +-------------------------------------------------------------------------------------------+ */
/* +                                         FILE MODE                                         + */
/* +-------------------------------------------------------------------------------------------+ */


/* Open the cache sector for read access (it will be read from disk) */
#define AFATFS_CACHE_READ							1

/* Open the cache sector for write access (it will be marked dirty) */
#define AFATFS_CACHE_WRITE							2

/* Lock this sector to prevent its state from transitioning (prevent flushes to disk) */
#define AFATFS_CACHE_LOCK							4

/* Discard this sector in preference to other sectors when it is in the In-Sync state */
#define AFATFS_CACHE_DISCARDABLE					8

/* Increase the retain counter of the cache sector to prevent it from being discarded when in the In-Sync state */
#define AFATFS_CACHE_RETAIN							16

typedef enum {
	AFATFS_INITIALISATION_READ_MBR,															// 0
	AFATFS_INITIALISATION_READ_VOLUME_ID,													// 1
	
#ifdef AFATFS_USE_FREEFILE
	AFATFS_INITIALISATION_FREEFILE_CREATE,													// 2
	AFATFS_INITIALISATION_FREEFILE_CREATING,												// 3
	AFATFS_INITIALISATION_FREEFILE_FAT_SEARCH,												// 4
	AFATFS_INITIALISATION_FREEFILE_UPDATE_FAT,												// 5
	AFATFS_INITIALISATION_FREEFILE_SAVE_DIR_ENTRY,											// 6
	AFATFS_INITIALISATION_FREEFILE_LAST = AFATFS_INITIALISATION_FREEFILE_SAVE_DIR_ENTRY,	// 7
#endif
	
#ifdef AFATFS_USE_INTROSPECTIVE_LOGGING
	AFATFS_INITIALISATION_INTROSPEC_LOG_CREATE,												// 2 if AFATFS_USE_FREEFILE is not defined, 8 otherwise
	AFATFS_INITIALISATION_INTROSPEC_LOG_CREATING,											// 3 if AFATFS_USE_FREEFILE is not defined, 9 otherwise
#endif
	
	AFATFS_INITIALISATION_DONE																// 2 if neither AFATFS_USE_FREEFILE nor AFATFS_USE_INTROSPECTIVE_LOGGING is defined
																							// 8 if only AFATFS_USE_FREEFILE is defined
																							// 10 if both AFATFS_USE_FREEFILE and AFATFS_USE_INTROSPECTIVE_LOGGING are defined
}afatfsInitialisationPhase_e;

typedef enum {
	AFATFS_FREE_SPACE_SEARCH_PHASE_FIND_HOLE,		// 0
	AFATFS_FREE_SPACE_SEARCH_PHASE_GROW_HOLE		// 1
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
	AFATFS_CACHE_STATE_EMPTY,			// 0
	AFATFS_CACHE_STATE_IN_SYNC,			// 1
	AFATFS_CACHE_STATE_READING,			// 2
	AFATFS_CACHE_STATE_WRITING,			// 3
	AFATFS_CACHE_STATE_DIRTY			// 4
}afatfsCacheBlockState_e;

enum {
	AFATFS_CREATEFILE_PHASE_INITIAL = 0,		// 0
	AFATFS_CREATEFILE_PHASE_FIND_FILE,			// 1
	AFATFS_CREATEFILE_PHASE_CREATE_NEW_FILE,	// 2
	AFATFS_CREATEFILE_PHASE_SUCCESS,			// 3
	AFATFS_CREATEFILE_PHASE_FAILURE				// 4
};

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
	AFATFS_FILE_TYPE_NONE,						// 0
	AFATFS_FILE_TYPE_NORMAL,					// 1
	AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY,		// 2
	AFATFS_FILE_TYPE_DIRECTORY					// 3
}afatfsFileType_e;

typedef enum {
	AFATFS_FILE_OPERATION_NONE,					// 0
	AFATFS_FILE_OPERATION_CREATE_FILE,			// 1
	AFATFS_FILE_OPERATION_SEEK,					// 2, seek the file's cursorCluster forwards by seekOffset bytes
	AFATFS_FILE_OPERATION_CLOSE,				// 3
	AFATFS_FILE_OPERATION_TRUNCATE,				// 4
	AFATFS_FILE_OPERATION_UNLINK,				// 5
#ifdef AFATFS_USE_FREEFILE
	AFATFS_FILE_OPERATION_APPEND_SUPERCLUSTER,	// 6
	AFATFS_FILE_OPERATION_LOCKED,				// 7
#endif
	AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER,	// 8
	AFATFS_FILE_OPERATION_EXTEND_SUBDIRECTORY	// 9
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
	AFATFS_APPEND_SUPERCLUSTER_PHASE_INIT = 0,						// 0
	AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FREEFILE_DIRECTORY,		// 1
	AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FAT,					// 2
	AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FILE_DIRECTORY			// 3
}afatfsAppendSuperclusterPhase_e;

typedef struct afatfsAppendSuperCluster_t {
	uint32_t previousCluster;
	uint32_t fatRewriteStartCluster;
	uint32_t fatRewriteEndCluster;
	afatfsAppendSuperclusterPhase_e phase;
}afatfsAppendSuperCluster_t;

typedef enum {
	AFATFS_APPEND_FREE_CLUSTER_PHASE_INITIAL = 0,					// 0
	AFATFS_APPEND_FREE_CLUSTER_PHASE_FIND_FREESPACE,				// 1
	AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT1,					// 2
	AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT2,					// 3
	AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FILE_DIRECTORY,			// 4
	AFATFS_APPEND_FREE_CLUSTER_PHASE_COMPLETE,						// 5
	AFATFS_APPEND_FREE_CLUSTER_PHASE_FAILURE						// 6
}afatfsAppendFreeClusterPhase_e;

typedef struct afatfsAppendFreeCluster_t {
	uint32_t previousCluster;
	uint32_t searchCluster;
	afatfsAppendFreeClusterPhase_e phase;
}afatfsAppendFreeCluster_t;

typedef enum {
	AFATFS_EXTEND_SUBDIRECTORY_PHASE_INITIAL = 0,					// 0
	AFATFS_EXTEND_SUBDIRECTORY_PHASE_ADD_FREE_CLUSTER = 0,			// 0
	AFATFS_EXTEND_SUBDIRECTORY_PHASE_WRITE_SECTORS,					// 1
	AFATFS_EXTEND_SUBDIRECTORY_PHASE_SUCCESS,						// 2
	AFATFS_EXTEND_SUBDIRECTORY_PHASE_FAILURE						// 3
}afatfsExtendSubdirectoryPhase_e;

typedef enum {
	AFATFS_SAVE_DIRECTORY_NORMAL,
	AFATFS_SAVE_DIRECTORY_FOR_CLOSE,
	AFATFS_SAVE_DIRECTORY_DELETED
}afatfsSaveDirectoryEntryMode_e;

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

typedef enum {
    AFATFS_FAT_PATTERN_UNTERMINATED_CHAIN,	// 0
    AFATFS_FAT_PATTERN_TERMINATED_CHAIN,	// 1
    AFATFS_FAT_PATTERN_FREE					// 2
} afatfsFATPattern_e;

typedef enum {
	CLUSTER_SEARCH_FREE_AT_BEGINNING_OF_FAT_SECTOR,
	CLUSTER_SEARCH_FREE,
	CLUSTER_SEARCH_OCCUPIED
}afatfsClusterSearchCondition_e;

typedef enum {
	AFATFS_FIND_CLUSTER_IN_PROGRESS,
	AFATFS_FIND_CLUSTER_FOUND,
	AFATFS_FIND_CLUSTER_FATAL,
	AFATFS_FIND_CLUSTER_NOT_FOUND
}afatfsFindClusterStatus_e;

typedef struct afatfsAppendSupercluster_t {
    uint32_t previousCluster;
    uint32_t fatRewriteStartCluster;
    uint32_t fatRewriteEndCluster;
    afatfsAppendSuperclusterPhase_e phase;
} afatfsAppendSupercluster_t;

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
		afatfsAppendSupercluster_t appendSupercluster;
		afatfsAppendFreeCluster_t appendFreeCluster;
		afatfsExtendSubdirectory_t extendSubdirectory;
		afatfsUnlinkFile_t unlinkFile;
		afatfsTruncateFile_t truncateFile;
		afatfsCloseFile_t closeFile;
	}state;
}afatfsFileOperation_t;

typedef union afatfsFATSector_t {
	uint8_t *bytes;
	uint16_t *fat16;
	uint32_t *fat32;
}afatfsFATSector_t;

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

/* +------------------------------------------------------------------------------------------------------------------------------- */
/* +------------------------------------------------------------------------------------------------------------------------------- */
/* +--------------------------------------------------------- FUNCTIONS ----------------------------------------------------------+ */
/* +------------------------------------------------------------------------------------------------------------------------------- */
/* +------------------------------------------------------------------------------------------------------------------------------- */

/**
 * Check if the input parameter is power of two or not.
 *
 * For example:
 *		Case x = 2:
 * 	    	~x = -3 => ~x + 1 = -2
 *			(x & (~x + 1)) = 2 & (-2) = 2
 *			(x & (~x + 1)) == x => 2 == 2 which is TRUE in this case
 * 
 *		Case x = 3:
 * 	    	~x = -4 => ~x + 1 = -4 + 1 = -3
 *			(x & (~x + 1)) = 3 & (-3) = 1
 *			(x & (~x + 1)) == x => 1 == 3 which is FALSE in this case
 */
static bool isPowerOfTwo(unsigned int x)
{
	return ((x != 0) && ((x & (~x + 1)) == x));
}

static uint32_t roundUpTo(uint32_t value, uint32_t rounding)
{
	uint32_t remainder = value % rounding;
	
	if (remainder > 0) {
		value += rounding - remainder;
	}
	
	return value;
}

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
		printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
	}
	
	return condition;
}

/**
 * Get the buffer memory for the cache entry of the given index.
 */
static uint8_t *afatfs_cacheSectorGetMemory(int cacheEntryIndex)
{
	return afatfs.cache + cacheEntryIndex * AFATFS_SECTOR_SIZE;		// AFATFS_SECTOR_SIZE = 512
}

/**
 * The number of FAT table entries that fit within one AFATFS sector size.
 *
 * Note that this is the same as the number of clusters in an AFATFS supercluster.
 *
 * Returns:
 * 		AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR = 512 / 4 = 128
 * 		AFATFS_FAT16_FAT_ENTRIES_PER_SECTOR = 512 / 2 = 256
 */
static uint32_t afatfs_fatEntriesPerSector(void)
{
	return afatfs.filesystemType == FAT_FILESYSTEM_TYPE_FAT32 ? AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR : AFATFS_FAT16_FAT_ENTRIES_PER_SECTOR;
}

/**
 * Called by the SD card driver when one of our write operations completes
 */
static void afatfs_sdcardWriteComplete(sdcardBlockOperation_e operation, uint32_t sectorIndex, uint8_t *buffer, uint32_t callbackData)
{
	(void) operation;
	(void) callbackData;
	
	afatfs.cacheFlushInProgress = false;
	
	for (int i = 0; i < AFATFS_NUM_CACHE_SECTORS; i++) {
		/**
		 * Keep in mind that someone may have marked the sector as dirty after writing had already begun. In this case we must leave
		 * it marked as dirty because those modifications may have been made too late to make it to the disk!
		 */
		if (afatfs.cacheDescriptor[i].sectorIndex == sectorIndex
			&& afatfs.cacheDescriptor[i].state == AFATFS_CACHE_STATE_WRITING
		) {
			if (buffer == NULL) {
				/* Write failed, remark the sector as dirty */
				afatfs.cacheDescriptor[i].state = AFATFS_CACHE_STATE_DIRTY;
				afatfs.cacheDirtyEntries++;
			} else {
				afatfs_assert(afatfs_cacheSectorGetMemory(i) == buffer);
				
				afatfs.cacheDescriptor[i].state = AFATFS_CACHE_STATE_IN_SYNC;
			}
			break;
		}
	}
}

/* Attempt to flush the dirty cache entry with the given index to the SD card */
static void afatfs_cacheFlushSector(int cacheIndex)
{
	afatfsCacheBlockDescriptor_t *cacheDescriptor = &afatfs.cacheDescriptor[cacheIndex];
	
#ifdef AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT
	if (cacheDescriptor->consecutiveEraseBlockCount) {
		sdcard_beginWriteBlocks(cacheDescriptor->sectorIndex, cacheDescriptor->consecutiveEraseBlockCount);
	}
#endif
	
	switch (sdcard_writeBlock(cacheDescriptor->sectorIndex, afatfs_cacheSectorGetMemory(cacheIndex), afatfs_sdcardWriteComplete, 0)) {
		case SDCARD_OPERATION_IN_PROGRESS:
			/* The card will call us back later when the buffer transmission finishes */
			afatfs.cacheDirtyEntries--;
			cacheDescriptor->state = AFATFS_CACHE_STATE_WRITING;
			afatfs.cacheFlushInProgress = true;
			break;
		
		case SDCARD_OPERATION_SUCCESS:
			/* Buffer is already transmitted */
			afatfs.cacheDirtyEntries--;
			cacheDescriptor->state = AFATFS_CACHE_STATE_IN_SYNC;
			break;
		
		case SDCARD_OPERATION_BUSY:
		case SDCARD_OPERATION_FAILURE:
		default:
			;
	}
}

/**
 * Attemp to flush dirty cache pages out to the sdcard, returning true if all flushable data has been flushed.
 */
bool afatfs_flush(void)
{
	if (afatfs.cacheDirtyEntries > 0) {
		/* Flush the oldest flushable sector */
		uint32_t earliestSectorTime = 0xFFFFFFFF;
		int earliestSectorIndex = -1;
		
		for (int i = 0; i < AFATFS_NUM_CACHE_SECTORS; i++) {
			if (afatfs.cacheDescriptor[i].state == AFATFS_CACHE_STATE_DIRTY && !afatfs.cacheDescriptor[i].locked
				&& (earliestSectorIndex == -1 || afatfs.cacheDescriptor[i].writeTimestamp < earliestSectorTime)
			) {
				earliestSectorIndex = i;
				earliestSectorTime = afatfs.cacheDescriptor[i].writeTimestamp;
			}
		}
		
		if (earliestSectorIndex > -1) {
			afatfs_cacheFlushSector(earliestSectorIndex);
			
			/* That flush will take time to complete so we may as well tell caller to come back later */
			return false;
		}
	}
	
	return true;
}

/* Initialise the cache sector of the FAT FS */
static void afatfs_cacheSectorInit(afatfsCacheBlockDescriptor_t *descriptor, uint32_t sectorIndex, bool locked)
{
	descriptor->sectorIndex = sectorIndex;
	
	descriptor->accessTimestamp = descriptor->writeTimestamp = ++afatfs.cacheTimer;
	
	descriptor->consecutiveEraseBlockCount = 0;
	
	descriptor->state = AFATFS_CACHE_STATE_EMPTY;
	
	descriptor->locked = locked;
	
	descriptor->retainCount = 0;
	
	descriptor->discardable = 0;
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
	
	uint32_t oldestSyncedSectorLastUse = 0xFFFFFFFF;
	int oldestSyncedSectorIndex = -1;
	
//	printf("afatfs.numClusters: %u\r\n", afatfs.numClusters);		// afatfs.numClusters = 0

	if (
		!afatfs_assert(
			afatfs.numClusters == 0	// we are unable to check sector bounds during startup since we haven't read volume label yet.
			|| sectorIndex < afatfs.clusterStartSector + afatfs.numClusters * afatfs.sectorsPerCluster
		)
	) {
//		printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
		return -1;
	}
	
	/* AFATFS_NUM_CACHE_SECTORS = 8 */
	for (int i = 0; i < AFATFS_NUM_CACHE_SECTORS; i++) {
		/* sectorIndex = physicalSectorIndex */
		if (afatfs.cacheDescriptor[i].sectorIndex == sectorIndex) {
			/*
			 * If the sector is actually empty then do a complete re-init of it just like the standard
			 * empty case. (Sectors marked as empty should be treated as if they don't have a block index assigned)
			 */
			if (afatfs.cacheDescriptor[i].state == AFATFS_CACHE_STATE_EMPTY) {
				emptyIndex = i;
				break;
			}
			
			/* Bump the last access time */
			afatfs.cacheDescriptor[i].accessTimestamp = ++afatfs.cacheTimer;
			return i;
		}
		
		switch (afatfs.cacheDescriptor[i].state) {
			case AFATFS_CACHE_STATE_EMPTY:
				emptyIndex = i;
				break;
			
			case AFATFS_CACHE_STATE_IN_SYNC:
				/* is this a synced sector that we could evict from the cache? */
				if (!afatfs.cacheDescriptor[i].locked && afatfs.cacheDescriptor[i].retainCount == 0) {
					/* discardable is TRUE */
					if (afatfs.cacheDescriptor[i].discardable) {
						discardableIndex = i;
					} else if (afatfs.cacheDescriptor[i].accessTimestamp < oldestSyncedSectorLastUse) {
						/* oldestSyncedSectorLastUse = 0xFFFFFFFF
						 * 
						 * This is older than last block we decided to evict, so evict this one in preference
						 */
						oldestSyncedSectorLastUse = afatfs.cacheDescriptor[i].accessTimestamp;
						oldestSyncedSectorIndex = i;
					}
				}
				break;
			
			default:
				;
		}
	}
	
//	printf("emptyIndex: %d\r\n", emptyIndex);								// emptyIndex = 0
//	printf("discardableIndex: %d\r\n", discardableIndex);					// discardableIndex = -1
//	printf("oldestSyncedSectorIndex: %d\r\n", oldestSyncedSectorIndex);		// oldestSyncedSectorIndex = -1
	
	if (emptyIndex > -1) {
		allocateIndex = emptyIndex;
	} else if (discardableIndex > -1) {
		allocateIndex = discardableIndex;
	} else if (oldestSyncedSectorIndex > -1) {
		allocateIndex = oldestSyncedSectorIndex;
	} else {
		allocateIndex = -1;
	}
	
	if (allocateIndex > -1) {
		/* locked parameter = false */
		afatfs_cacheSectorInit(&afatfs.cacheDescriptor[allocateIndex], sectorIndex, false);
	}
	
	return allocateIndex;
}

/**
 * Called by the SD card driver when one of our read operations completes
 */
static void afatfs_sdcardReadComplete(sdcardBlockOperation_e operation, uint32_t sectorIndex, uint8_t *buffer, uint32_t callbackData)
{
	(void) operation;
	(void) callbackData;
	
	for (int i = 0; i < AFATFS_NUM_CACHE_SECTORS; i++) {
		if (afatfs.cacheDescriptor[i].state != AFATFS_CACHE_STATE_EMPTY
			&& afatfs.cacheDescriptor[i].sectorIndex == sectorIndex) {
			if (buffer == NULL) {
				/* Read failed, mark the sector as empty and whoever asked for it will ask for it again later to retry */
				afatfs.cacheDescriptor[i].state = AFATFS_CACHE_STATE_EMPTY;
				printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
			} else {
//				printf("afatfs_cacheSectorGetMemory(i) == buffer: %d, %s, %s, %d\r\n", afatfs_cacheSectorGetMemory(i) == buffer, __FILE__, __FUNCTION__, __LINE__);
//				printf("afatfs.cacheDescriptor[i].state == AFATFS_CACHE_STATE_READING: %d, %s, %s, %d\r\n", afatfs.cacheDescriptor[i].state == AFATFS_CACHE_STATE_READING, __FILE__, __FUNCTION__, __LINE__);
				afatfs_assert(afatfs_cacheSectorGetMemory(i) == buffer && afatfs.cacheDescriptor[i].state == AFATFS_CACHE_STATE_READING);
				afatfs.cacheDescriptor[i].state = AFATFS_CACHE_STATE_IN_SYNC;
				printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
			}
			
			break;
		}
	}
}

static void afatfs_cacheSectorMarkDirty(afatfsCacheBlockDescriptor_t *descriptor)
{
	if (descriptor->state != AFATFS_CACHE_STATE_DIRTY) {
		descriptor->writeTimestamp = ++afatfs.cacheTimer;
		descriptor->state = AFATFS_CACHE_STATE_DIRTY;
		afatfs.cacheDirtyEntries++;
	}
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
	if (!afatfs_assert((sectorFlags & AFATFS_CACHE_WRITE) == 0 || physicalSectorIndex != 0)) {
//		printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
		return AFATFS_OPERATION_FAILURE;
	}
	
	/* Allocate cache sector */
	int cacheSectorIndex = afatfs_allocateCacheSector(physicalSectorIndex);
	
//	printf("cacheSectorIndex: %d\r\n", cacheSectorIndex);		// cacheSectorIndex = 0
	
	/* Error checking */
	if (cacheSectorIndex == -1) {
		/* We don't have enough free cache to service this request right now, try again later */
//		printf("%s, %d\r\n", __FUNCTION__, __LINE__);
		return AFATFS_OPERATION_IN_PROGRESS;
	}
	
//	printf("afatfs.cacheDescriptor[cacheSectorIndex].state: %d, %s, %s, %d\r\n", afatfs.cacheDescriptor[cacheSectorIndex].state, __FILE__, __FUNCTION__, __LINE__);
	switch (afatfs.cacheDescriptor[cacheSectorIndex].state) {
		case AFATFS_CACHE_STATE_READING:
//			printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
			return AFATFS_OPERATION_IN_PROGRESS;
			break;
		
		case AFATFS_CACHE_STATE_EMPTY:
//			printf("sectorFlags & AFATFS_CACHE_READ: %u, %s, %d\r\n", sectorFlags & AFATFS_CACHE_READ, __FUNCTION__, __LINE__);
			if ((sectorFlags & AFATFS_CACHE_READ) != 0) {
//				printf("%s, %d\r\n", __FUNCTION__, __LINE__);
				if (sdcard_readBlock(physicalSectorIndex, afatfs_cacheSectorGetMemory(cacheSectorIndex), afatfs_sdcardReadComplete, 0)) {
					afatfs.cacheDescriptor[cacheSectorIndex].state = AFATFS_CACHE_STATE_READING;
//					printf("afatfs.cacheDescriptor[cacheSectorIndex].state: %d, %s, %s, %d\r\n", afatfs.cacheDescriptor[cacheSectorIndex].state, __FILE__, __FUNCTION__, __LINE__);
				}
				return AFATFS_OPERATION_IN_PROGRESS;
			}
			
			/* We only get to decide these fields if we're the first ones to cache the sector */
			afatfs.cacheDescriptor[cacheSectorIndex].discardable = (sectorFlags & AFATFS_CACHE_DISCARDABLE) != 0 ? 1 : 0;
			
#ifdef AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT
			/* Don't bother pre-erasing for small block sequences */
			if (eraseCount < AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT) {
				eraseCount = 0;
			} else {
				eraseCount = MIN(eraseCount, UINT16_MAX);	// If caller ask for a longer chain of sectors we silently truncate that here
			}
			
			afatfs.cacheDescriptor[cacheSectorIndex].consecutiveEraseBlockCount = eraseCount;
#endif
			
			/* Fall through */
		
		case AFATFS_CACHE_STATE_WRITING:
		case AFATFS_CACHE_STATE_IN_SYNC:
			if ((sectorFlags & AFATFS_CACHE_WRITE) != 0) {
				afatfs_cacheSectorMarkDirty(&afatfs.cacheDescriptor[cacheSectorIndex]);
			}
			/* Fall through */
		
		case AFATFS_CACHE_STATE_DIRTY:
			if ((sectorFlags & AFATFS_CACHE_LOCK) != 0) {
				afatfs.cacheDescriptor[cacheSectorIndex].locked = 1;
			}
			
			if ((sectorFlags & AFATFS_CACHE_RETAIN) != 0) {
				afatfs.cacheDescriptor[cacheSectorIndex].retainCount++;
			}
			
//			printf("cacheSectorIndex: %u, %s, %d\r\n", cacheSectorIndex, __FUNCTION__, __LINE__);	// cacheSectorIndex = 7
			*buffer = afatfs_cacheSectorGetMemory(cacheSectorIndex);
//			printf("buffer: 0x%x, %s, %d\r\n", (unsigned int)*buffer, __FUNCTION__, __LINE__);	// cacheSectorIndex = 7
						
			return AFATFS_OPERATION_SUCCESS;
			break;
		
		default:
			/* Cache block in unknown state, should never happen */
			afatfs_assert(false);
//			printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
			return AFATFS_OPERATION_FAILURE;
	}	
}

/**
 * Parse the details out of the given MBR sector (512 bytes long).
 * Return:
 *		true: if a compatible filesystem was found
 *		false: otherwise
 */
static bool afatfs_parseMBR(const uint8_t *sector)
{
//	printf("sector[%d]: %u, %d\r\n", AFATFS_SECTOR_SIZE - 2, sector[AFATFS_SECTOR_SIZE - 2], __LINE__);
//	printf("sector[%d]: %u, %d\r\n", AFATFS_SECTOR_SIZE - 1, sector[AFATFS_SECTOR_SIZE - 1], __LINE__);
	
	/* Check MBR signature */
	if (sector[AFATFS_SECTOR_SIZE - 2] != 0x55 || sector[AFATFS_SECTOR_SIZE - 1] != 0xAA) {
		return false;
	}
	
	/* AFATFS_PARTITION_TABLE_START_OFFSET = 446
	 * partition entry starts at offset 446
	 */
	mbrPartitionEntry_t *partition = (mbrPartitionEntry_t *)(sector + AFATFS_PARTITION_TABLE_START_OFFSET);
	
	/* For partitions, each one is 16 bytes length */
	for (int i = 0; i < 4; i++) {
//		printf("partition[%d].lbaBegin: %u, %s, %d\r\n", i, partition[i].lbaBegin, __FUNCTION__, __LINE__);
//		printf("partition[%d].type: 0x%x, %s, %d\r\n\r\n", i, partition[i].type, __FUNCTION__, __LINE__);
		/* partition[i].lbaBegin = 8192
		 * partition[%d].type = 12
		 *
		 * MBR_PARTITION_TYPE_FAT32 = 0x0B (11)
		 * MBR_PARTITION_TYPE_FAT32_LBA = 0x0C (12)
		 * MBR_PARTITION_TYPE_FAT16 = 0x06
		 * MBR_PARTITION_TYPE_FAT16_LBA = 0x0E (14)
		 */
		if (
			partition[i].lbaBegin > 0
			&& (
				partition[i].type == MBR_PARTITION_TYPE_FAT32
				|| partition[i].type == MBR_PARTITION_TYPE_FAT32_LBA
				|| partition[i].type == MBR_PARTITION_TYPE_FAT16
				|| partition[i].type == MBR_PARTITION_TYPE_FAT16_LBA
			)
		) {
			afatfs.partitionStartSector = partition[i].lbaBegin;
//			printf("afatfs.partitionStartSector: %u, %s, %d\r\n", afatfs.partitionStartSector, __FUNCTION__, __LINE__);
			return true;
		}
	}
	
//	printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
	return false;
}

static bool afatfs_parseVolumeID(const uint8_t *sector)
{
	/* local pointer variable volume points to the read sector region */
	fatVolumeID_t *volume = (fatVolumeID_t *)sector;
	
	/* Initialise afatfs.fileSystemType to FAT_FILESYSTEM_TYPE_INVALID (0) */
	afatfs.filesystemType = FAT_FILESYSTEM_TYPE_INVALID;
	
	/*
	 * AFATFS_SECTOR_SIZE = 512
	 * AFATFS_NUM_FATS = 2
	 * FAT_VOLUME_ID_SIGNATURE_1 = 0x55
	 * FAT_VOLUME_ID_SIGNATURE_2 = 0xAA
	 */
	if (volume->bytesPerSector != AFATFS_SECTOR_SIZE || volume->numFATs != AFATFS_NUM_FATS
		|| sector[510] != FAT_VOLUME_ID_SIGNATURE_1 || sector[511] != FAT_VOLUME_ID_SIGNATURE_2) {
		return false;
	}
	
//	printf("afatfs.partitionStartSector: %u, %s, %d\r\n", afatfs.partitionStartSector, __FUNCTION__, __LINE__);
//	printf("volume->reservedSectorCount: %u, %s, %d\r\n", volume->reservedSectorCount, __FUNCTION__, __LINE__);
	/* Assign the afatfs.fatStartSector
	 *
	 * afatfs.partitionStartSector = 8192
	 * volume->reservedSectorCount = 598
	 */
	afatfs.fatStartSector = afatfs.partitionStartSector + volume->reservedSectorCount;		// afatfs.fatStartSector = 8790
	
//	printf("afatfs.fatStartSector: %u, %s, %d\r\n", afatfs.fatStartSector, __FUNCTION__, __LINE__);
	
	/* Assign the volume->sectorsPerCluster to afatfs.sectorPerCluster */
	afatfs.sectorsPerCluster = volume->sectorsPerCluster;
//	printf("volume->sectorsPerCluster: %u, %s, %d\r\n", volume->sectorsPerCluster, __FUNCTION__, __LINE__);
//	printf("afatfs.sectorsPerCluster: %u, %s, %d\r\n", afatfs.sectorsPerCluster, __FUNCTION__, __LINE__);
	/* afatfs.sectorsPerCluster should be 64 */
	if (afatfs.sectorsPerCluster < 1 || afatfs.sectorsPerCluster > 128 || !isPowerOfTwo(afatfs.sectorsPerCluster)) {
		return false;
	}
	
	/* Assign afatfs.byteInClusterMask which should be AFATFS_SECTOR_SIZE * afatfs.sectorsPerCluster - 1 = 512 (bytes) * 64 - 1 = 32767 bytes */
	afatfs.byteInClusterMask = AFATFS_SECTOR_SIZE * afatfs.sectorsPerCluster - 1;		// afatfs.byteInClusterMask = 32767
	
//	printf("afatfs.byteInClusterMask: %u, %s, %d\r\n", afatfs.byteInClusterMask, __FUNCTION__, __LINE__);
	
	/* Assign the size in sectors of a single FAT, which should be 3797 sectors in FAT32 according to 16G Sandisk microSD card */
	afatfs.fatSectors = volume->FATSize16 != 0 ? volume->FATSize16 : volume->fatDescriptor.fat32.FATSize32;	// afatfs.fatSectors = 3797

//	printf("afatfs.fatSectors: %u, %s, %d\r\n", afatfs.fatSectors, __FUNCTION__, __LINE__);
	
	/** Assign the number of sectors that the root directory occupies
	 *  Always zero on FAT32 since rootEntryCount is always zero on FAT32 (this is non-zero on FAT16)
	 */
	afatfs.rootDirectorySectors = ((volume->rootEntryCount * FAT_DIRECTORY_ENTRY_SIZE) + (volume->bytesPerSector - 1)) / volume->bytesPerSector;

//	printf("afatfs.rootDirectorySectors: %u, %s, %d\r\n", afatfs.rootDirectorySectors, __FUNCTION__, __LINE__);
	
	/* totalSectors = volume->totalSector32 = 31108096 */
	uint32_t totalSectors = volume->totalSectors16 != 0 ? volume->totalSectors16 : volume->totalSectors32;

//	printf("totalSectors: %u, %s, %d\r\n", totalSectors, __FUNCTION__, __LINE__);

	/* The count of sectors in the data region of the volume
	 *
	 * dataSectors = 31108096 - (598 + 2 * 3797) + 0 = 31108096 - (598 + 2 * 3797) + 0 = 31108096 - 8192 = 31099904
	 */
	uint32_t dataSectors = totalSectors - (volume->reservedSectorCount + (AFATFS_NUM_FATS * afatfs.fatSectors) + afatfs.rootDirectorySectors);	

//	printf("dataSectors: %u, %s, %d\r\n", dataSectors, __FUNCTION__, __LINE__);

	/* The count of clusters
	 *
	 * afatfs.numClusters = 31099904 / 64 = 485936
	 */
	afatfs.numClusters = dataSectors / volume->sectorsPerCluster;

//	printf("afatfs.numClusters: %u, %s, %d\r\n", afatfs.numClusters, __FUNCTION__, __LINE__);

	/* Determine the FAT type
	 *
	 * FAT12_MAX_CLUSTERS = 4084
	 * FAT16_MAX_CLUSTERS = 65524
	 *
	 * WARNING: MUST USE <= sign here to avoid off-by-one error that mentioned in <fatgen103.pdf>
	 */
	if (afatfs.numClusters <= FAT12_MAX_CLUSTERS) {
		afatfs.filesystemType = FAT_FILESYSTEM_TYPE_FAT12;
		return false;		// Since FAT is not a supported filesystem
	} else if (afatfs.numClusters <= FAT16_MAX_CLUSTERS) {
		afatfs.filesystemType = FAT_FILESYSTEM_TYPE_FAT16;
	} else {
		afatfs.filesystemType = FAT_FILESYSTEM_TYPE_FAT32;
	}
	
	/* rootCluster is the cluster number of the first cluster of the root directory, usually 2 but not required to be 2
	 * 
	 * volume->fatDescriptor.fat32.rootCluster is 2, which is read by WinHex tool
	 */
	if (afatfs.filesystemType == FAT_FILESYSTEM_TYPE_FAT32) {
		afatfs.rootDirectoryCluster = volume->fatDescriptor.fat32.rootCluster;
//		printf("afatfs.rootDirectoryCluster: %u, %s, %d\r\n", afatfs.rootDirectoryCluster, __FUNCTION__, __LINE__);
	} else {
		afatfs.rootDirectoryCluster = 0;	// FAT16 does not store the root directory in clusters
	}
	
	/* Determine the end of FATs
	 *
	 * endOfFATs = 8790 + 2 * 3797 = 16384
	 */
//	printf("afatfs.fatStartSector: %u, %s, %s, %d\r\n", afatfs.fatStartSector, __FILE__, __FUNCTION__, __LINE__);
	uint32_t endOfFATs = afatfs.fatStartSector + AFATFS_NUM_FATS * afatfs.fatSectors;	// AFATFS_NUM_FATS = 2, afatfs.fatSectors = 3797

//	printf("endOfFATs: %u, %s, %d\r\n", endOfFATs, __FUNCTION__, __LINE__);

	/* Determine the start sector of the cluster
	 *
	 * afatfs.clusterStartSector = 16384 + 0 = 16384
	 */
	afatfs.clusterStartSector = endOfFATs + afatfs.rootDirectorySectors;	// afatfs.rootDirectorySectors should be 0 on FAT32 system

//	printf("afatfs.clusterStartSector: %u, %s, %d\r\n", afatfs.clusterStartSector, __FUNCTION__, __LINE__);
	
	return true;
}

/**
 * Determine if the file operation is busy or not
 * 
 * Returns:
 *   	true: if the file operation is busy, otherwise false.
 */
static bool afatfs_fileIsBusy(afatfsFilePtr_t file)
{
	return file->operation.operation != AFATFS_FILE_OPERATION_NONE;
}

/**
 * Reset the in-memory data for the given handle back to the zeroed initial state
 */
static void afatfs_initFileHandle(afatfsFilePtr_t file)
{
	memset(file, 0, sizeof(*file));
	file->writeLockedCacheIndex = -1;
	file->readRetainCacheIndex = -1;
}

/**
 * Bring the logical filesize up to date with the current cursor positionf
 */
static void afatfs_fileUpdateFileSize(afatfsFile_t *file)
{
	file->logicalSize = MAX(file->logicalSize, file->cursorOffset);
}

static void afatfs_fileUnlockCacheSector(afatfsFilePtr_t file)
{
	if (file->writeLockedCacheIndex != -1) {
		printf("%s, %d\r\n", __FUNCTION__, __LINE__);
		afatfs.cacheDescriptor[file->writeLockedCacheIndex].locked = 0;
		file->writeLockedCacheIndex = -1;
	}
	
	if (file->readRetainCacheIndex != -1) {
		printf("%s, %d\r\n", __FUNCTION__, __LINE__);
		afatfs.cacheDescriptor[file->readRetainCacheIndex].retainCount = MAX((int)afatfs.cacheDescriptor[file->readRetainCacheIndex].retainCount - 1, 0);
		file->readRetainCacheIndex = -1;
	}
}

/**
 * Size of a FAT cluster in bytes
 */
static uint32_t afatfs_clusterSize(void)
{
	return afatfs.sectorsPerCluster * AFATFS_SECTOR_SIZE;		// clusterSize = 64 * 512 = 32768
}

/**
 * Given a byte offset within a file, return the byte offset of that position within the cluster it belongs to.
 */
static uint32_t afatfs_byteIndexInCluster(uint32_t byteOffset)
{
	/* afatfs.byteInClusterMask = 32767 */
	return afatfs.byteInClusterMask & byteOffset;
}

/**
 * Get the position of the FAT entry for the cluster with the given number.
 */
static void afatfs_getFATPositionForCluster(uint32_t cluster, uint32_t *fatSectorIndex, uint32_t *fatSectorEntryIndex)
{
	if (afatfs.filesystemType == FAT_FILESYSTEM_TYPE_FAT16) {
		/* Handle the FAT16 */
		uint32_t entriesPerFATSector = AFATFS_SECTOR_SIZE / sizeof(uint16_t);	// 512 / 2 = 256
		
		*fatSectorIndex = cluster / entriesPerFATSector;
		*fatSectorEntryIndex = cluster & (entriesPerFATSector - 1);
	} else {
		/* Handle the FAT32 */
		uint32_t entriesPerFATSector = AFATFS_SECTOR_SIZE / sizeof(uint32_t);	// 512 / 4 = 128
		
		*fatSectorIndex = fat32_decodeClusterNumber(cluster) / entriesPerFATSector;
		*fatSectorEntryIndex = cluster & (entriesPerFATSector - 1);
	}
}

/**
 * Get the physical sector number that corresponds to the FAT sector of the given fatSectorIndex within the given
 * FAT (fatIndex may be 0 or 1).
 * (0, 0) gives the first sector of the first FAT.
 */
static uint32_t afatfs_fatSectorToPhysical(int fatIndex, uint32_t fatSectorIndex)
{
	/* According to Sandisk MicroSD 16GB
	 * afatfs.fatStartSector = 8790
	 * afatfs.fatSectors = 3797
	 */
	return afatfs.fatStartSector + (fatIndex ? afatfs.fatSectors : 0) + fatSectorIndex;
}

/**
 * Look up the FAT to find out which cluster follows the one with the given number and store it into *nextCluster.
 * 
 * Use fat_isFreeSpace() and fat_isEndOfChainMarker() on nextCluster to distinguish those special values from regular
 * cluster numbers.
 *
 * Note that if you're trying to find the next cluster of a file, you should be calling afatfs_fileGetNextCluster() 
 * instead, as that one supports contiguous freefile-based files (which needn't consult the FAT).
 *
 * Returns:
 *		AFATFS_OPERATION_IN_PROGRESS	- FS is busy right now, call again later
 * 		AFATFS_OPERATION_SUCCESS		- *nextCluster is set to the next cluster number
 */
static afatfsOperationStatus_e afatfs_FATGetNextCluster(int fatIndex, uint32_t cluster, uint32_t *nextCluster)
{
	uint32_t fatSectorIndex, fatSectorEntryIndex;
	afatfsFATSector_t sector;
	
	afatfs_getFATPositionForCluster(cluster, &fatSectorIndex, &fatSectorEntryIndex);
	
	afatfsOperationStatus_e result = afatfs_cacheSector(afatfs_fatSectorToPhysical(fatIndex, fatSectorIndex), &sector.bytes, AFATFS_CACHE_READ, 0);
	
	if (result == AFATFS_OPERATION_SUCCESS) {
		if (afatfs.filesystemType == FAT_FILESYSTEM_TYPE_FAT16) {
			*nextCluster = sector.fat16[fatSectorEntryIndex];
		} else {
			/* fat32_decodeClusterNumber() function basically get rid of the highest 4 bits and leave the lowest 24 bits of sector.fat32[fatSectorEntryIndex] */
			*nextCluster = fat32_decodeClusterNumber(sector.fat32[fatSectorEntryIndex]);
		}
	}
	
	return result;
}

/**
 * Get the cluster that follows the currentCluster in the FAT chain for the given file.
 *
 * Returns:
 *		AFATFS_OPERATION_IN_PROGRESS	- FS is busy right now, call again later
 *		AFATFS_OPERATION_SUCCESS		- *nextCluster is set to the next cluster number
 */
static afatfsOperationStatus_e afatfs_fileGetNextCluster(afatfsFilePtr_t file, uint32_t currentCluster, uint32_t *nextCluster)
{
#ifndef AFATFS_USE_FREEFILE
	(void) file;
#else
	if ((file->mode & AFATFS_FILE_MODE_CONTIGUOUS) != 0) {
		uint32_t freeFileStart = afatfs.freeFile.firstCluster;
		
		afatfs_assert(currentCluster + 1 <= freeFileStart);
		
		/* Would the next cluster lie outside the allocated file? (i.e. beyond the end of the file into the start of the freefile) */
		if (currentCluster + 1 == freeFileStart) {
			*nextCluster = 0;
		} else {
			*nextCluster = currentCluster + 1;
		}
		
		return AFATFS_OPERATION_SUCCESS;
	} else
#endif
	{
		return afatfs_FATGetNextCluster(0, currentCluster, nextCluster);
	}
}



static bool afatfs_FATIsEndOfChainMarker(uint32_t clusterNumber)
{
	if (afatfs.filesystemType == FAT_FILESYSTEM_TYPE_FAT32) {
		return fat32_isEndOfChainMarker(clusterNumber);
	} else {
		return fat16_isEndOfChainMarker(clusterNumber);
	}
}

/**
 * Returns true if the file's cursor is sitting beyond the end of the last allocated cluster (i.e. the logical fileSize
 * is not checked).
 */
static bool afatfs_isEndOfAllocatedFile(afatfsFilePtr_t file)
{
	if (file->type == AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY) {
		return file->cursorOffset >= AFATFS_SECTOR_SIZE * afatfs.rootDirectorySectors;
	} else {
		return file->cursorCluster == 0 || afatfs_FATIsEndOfChainMarker(file->cursorCluster);
	}
}

/**
 * Attempt to seek the file pointer by the offset, relative to the current position.
 *
 * Returns true if the seek was complete, or false if you should try again later by calling this routine again (the 
 * cursor is not moved and no seek operation is queued on the file for you).
 *
 * You can only seek forwards by the size of a cluster or less, or backwards to stay within the same cluster. Otherwise
 * false will always be returned (calling this routine again will never make progress on the seek).
 *
 * This amount of seek is special because we have to wait on at most one read operation, so it's easy to make the seek
 * atomic.
 */
static bool afatfs_fseekAtomic(afatfsFilePtr_t file, int32_t offset)
{
	/* AFATFS_SECTOR_SIZE = 512 */
	
	/* Seeks within a sector */
	uint32_t newSectorOffset = offset + file->cursorOffset % AFATFS_SECTOR_SIZE;
	
//	printf("newSectorOffset: %u, %d\r\n", newSectorOffset, __LINE__);
	
	/* newSectorOffset is non-negative and smaller than AFATFS_SECTOR_SIZE, we're staying within the same sector */
	if (newSectorOffset < AFATFS_SECTOR_SIZE) {
		file->cursorOffset += offset;
//		printf("offset: %u, %d\r\n", offset, __LINE__);
//		printf("file->cursorOffset: %u, %d\r\n", file->cursorOffset, __LINE__);
		return true;
	}
	
	/* We're seeking outside the sector so unlock it if we were holding it */
	afatfs_fileUnlockCacheSector(file);
	
	/* FAT16 root directories are made up of contiguous sectors rather than clusters */
	if (file->type == AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY) {
		file->cursorOffset += offset;
		return true;
	}
	
	uint32_t clusterSizeBytes = afatfs_clusterSize();	// cluster size in bytes
	uint32_t offsetInCluster = afatfs_byteIndexInCluster(file->cursorOffset);
	uint32_t newOffsetInCluster = offsetInCluster + offset;
	
	afatfsOperationStatus_e status;
	
	if (offset > (int32_t) clusterSizeBytes || offset < -(int32_t) offsetInCluster) {
		return false;
	}
	
	/* Are we seeking outside the cluster? If so we'll need to find out the next cluster number. */
	if (newOffsetInCluster >= clusterSizeBytes) {
		uint32_t nextCluster;
		
		status = afatfs_fileGetNextCluster(file, file->cursorCluster, &nextCluster);
		if (status == AFATFS_OPERATION_SUCCESS) {
			/* Seek to the beginning of the next cluster */
			uint32_t bytesToSeek = clusterSizeBytes - offsetInCluster;
			
			file->cursorPreviousCluster = file->cursorCluster;
			file->cursorCluster = nextCluster;
			file->cursorOffset += bytesToSeek;
			
			offset -= bytesToSeek;
		} else {
			/* Try again later */
			return false;
		}
	}
	
	/* If we didn't already hit the end of the file, add any remaining offset needed inside the cluster */
	if (!afatfs_isEndOfAllocatedFile(file)) {
		file->cursorOffset += offset;
	}
	
	return true;
}

/**
 * Seek the file pointer forwards by offset bytes.
 * Calls the callback when the seek if complete.
 * 
 * Will happily seek beyond the logical end of the file.
 *
 * Returns:
 * 		AFATFS_OPERATION_SUCCESS		- The seek was completed immediately
 *		AFATFS_OPERATION_IN_PROGRESS	- The seek was queued and will complete later
 *		AFATFS_OPERATION_FAILURE		- The seek could not be queued because the file was busy with another operation, try again later
 */
static afatfsOperationStatus_e afatfs_fseekInternal(afatfsFilePtr_t file, uint32_t offset, afatfsFileCallback_t callback)
{
	/* See if we can seek without queuing an operation */
	if (afatfs_fseekAtomic(file, offset)) {
//		printf("%s, %d\r\n", __FUNCTION__, __LINE__);
		if (callback) {
//			printf("%s, %d\r\n", __FUNCTION__, __LINE__);
			callback(file);
		}
		
		return AFATFS_OPERATION_SUCCESS;
	} else {
		/* Our operation must queue */
		if (afatfs_fileIsBusy(file)) {
			return AFATFS_OPERATION_FAILURE;
		}
		
		/* Link the pointer opState to file->operation.state.seek */
		afatfsSeek_t *opState = &file->operation.state.seek;
		
		file->operation.operation = AFATFS_FILE_OPERATION_SEEK;
		opState->callback = callback;
		opState->seekOffset = offset;
		
		return AFATFS_OPERATION_IN_PROGRESS;
	}
}

/**
 * Attempt to seek the file cursor from the given point (`whence`) by the given offset, just like C's fseek
 * 
 * AFATFS_SEEK_SET with offset 0 will always return AFATFS_OPERATION_SUCCESS
 *
 * Returns:
 * 		AFATFS_OPERATION_SUCCESS			- The seek was completed immediately
 * 		AFATFS_OPERATION_IN_PROGRESS		- The seek was queued
 * 		AFATFS_OPERATION_FAILURE			- The seek was completed immediately
 */
afatfsOperationStatus_e afatfs_fseek(afatfsFilePtr_t file, int32_t offset, afatfsSeek_e whence)
{
//	printf("file->logicalSize: %u\r\n", file->logicalSize);		// file->logicalSize = 0
//	printf("file->cursorOffset: %u\r\n", file->cursorOffset);	// file->cursorOffset = 0
	
	/* We need an up-to-date logical filesize so we can clamp seeks to the EOF */
	afatfs_fileUpdateFileSize(file);
	
//	printf("file->logicalSize: %u\r\n", file->logicalSize);		// file->logicalSize = 0
	
	switch (whence) {
		case AFATFS_SEEK_CUR:
			printf("%s, %d\r\n", __FUNCTION__, __LINE__);
			if (offset >= 0) {
				/* Only forward seeks are supported by this routine */
				return afatfs_fseekInternal(file, MIN(file->cursorOffset + offset, file->logicalSize), NULL);
			}
			
			/* Convert a backwards relative to seek into a SEEK_SET.
			 * TODO: considerable room for improvement if within the same cluster
			 */
			offset += file->cursorOffset;
			break;
		
		case AFATFS_SEEK_END:
			printf("%s, %d\r\n", __FUNCTION__, __LINE__);
			/* Are we already at the right position? */
			if (file->logicalSize + offset == file->cursorOffset) {
				return AFATFS_OPERATION_SUCCESS;
			}
			
			/* Convert into a SEEK_SET */
			offset += file->logicalSize;
			break;
		
		case AFATFS_SEEK_SET:
//			printf("%s, %d\r\n", __FUNCTION__, __LINE__);
			;
			/* Fall through */
	}
	
//	printf("file->writeLockedCacheIndex: %d\r\n", file->writeLockedCacheIndex);
//	printf("file->readRetainCacheIndex: %d\r\n", file->readRetainCacheIndex);

	/* Now we have a SEEK_SET with a positive offset. Begin by seeking to the start of the file */
	afatfs_fileUnlockCacheSector(file);
	
	printf("file->firstCluster: %d\r\n", file->firstCluster);
	file->cursorPreviousCluster = 0;
	file->cursorCluster = file->firstCluster;
	file->cursorOffset = 0;
	
	/* Then seek forwards by the offset */
	return afatfs_fseekInternal(file, MIN((uint32_t) offset, file->logicalSize), NULL);
}

/**
 * Change the working directory to the directory with the given handle (use fopen).
 *
 * Pass NULL for `directory` in order to change to the root directory.
 *
 * Returns true on success, false if you should call again later to retry.
 * After changing into a directory, you handle to that directory may be closed by fclose().
 */
bool afatfs_chdir(afatfsFilePtr_t directory)
{
	printf();
	if (afatfs_fileIsBusy(&afatfs.currentDirectory)) {
		return false;
	}
	
	if (directory) {
		/* Directory is NOT NULL */
		if (afatfs_fileIsBusy(directory)) {
			return false;		// file operation is busy
		}
		
		/* file operation is NOT busy */
		memcpy(&afatfs.currentDirectory, directory, sizeof(*directory));
		return true;
	} else {
		/* Directory is NULL */
		/* Initialise file handle
		 *
		 * Clear the afatfs.currentDirectory to zero
		 * Set afatfs.currentDirectory.writeLockedCacheIndex to -1
		 * Set afatfs.currentDirectory.readRetainCacheIndex to -1
		 */
		afatfs_initFileHandle(&afatfs.currentDirectory);
		
//		printf("afatfs.currentDirectory.writeLockedCacheIndex: %d\r\n", afatfs.currentDirectory.writeLockedCacheIndex); // writeLockedCacheIndex = -1
//		printf("afatfs.currentDirectory.readRetainCacheIndex: %d\r\n", afatfs.currentDirectory.readRetainCacheIndex); // readRetainCacheIndex = -1
		
		/* Configure the current directory mode */
		afatfs.currentDirectory.mode = AFATFS_FILE_MODE_READ | AFATFS_FILE_MODE_WRITE;

//		printf("afatfs.currentDirectory.mode: %u\r\n", afatfs.currentDirectory.mode); // mode = 3
		
		/* Configure the current directory type */
		if (afatfs.filesystemType == FAT_FILESYSTEM_TYPE_FAT16) {
			afatfs.currentDirectory.type = AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY;
		} else {
			afatfs.currentDirectory.type = AFATFS_FILE_TYPE_DIRECTORY;		// FAT32 type
		}
		
//		printf("afatfs.currentDirectory.type: %u\r\n", afatfs.currentDirectory.type);	// afatfs.currentDirectory.type = 3 (FAT32)
		
		/* Configure the first cluster of the current directory */
		afatfs.currentDirectory.firstCluster = afatfs.rootDirectoryCluster;		// afatfs.rootDirectoryCluster = 2
		
//		printf("afatfs.currentDirectory.firstCluster: %u\r\n", afatfs.currentDirectory.firstCluster);	// firstCluster = 2
		
		/* Configure the attribute of the current directory */
		afatfs.currentDirectory.attrib = FAT_FILE_ATTRIBUTE_DIRECTORY;
		
//		printf("afatfs.currentDirectory.attrib: 0x%x\r\n", afatfs.currentDirectory.attrib);	// attrib = 0x10 (ATTR_DIRECTORY)
		
		/* Configure the sectorNumberPhysical of the directory entry position to ZERO since the root directory don't have a directory entry to represent themselves */
		afatfs.currentDirectory.directoryEntryPos.sectorNumberPhysical = 0;

//		printf("afatfs.currentDirectory.directoryEntryPos.sectorNumberPhysical: %u\r\n", afatfs.currentDirectory.directoryEntryPos.sectorNumberPhysical);
		
		/* Call afatfs_fseek() to seek the current directory */
		afatfs_fseek(&afatfs.currentDirectory, 0, AFATFS_SEEK_SET);		// update file->logicalSize
	}
}

/**
 * Initialise the finder so that the first call with the directory to findNext() will return the first file in the directory
 */
void afatfs_findFirst(afatfsFilePtr_t directory, afatfsFinder_t *finder)
{
	afatfs_fseek(directory, 0, AFATFS_SEEK_SET);
	finder->entryIndex = -1;
}

/**
 * Given a byte offset within a file, return the index of the sector within the cluster it belongs to.
 */
static uint32_t afatfs_sectorIndexInCluster(uint32_t byteOffset)
{
	return afatfs_byteIndexInCluster(byteOffset) / AFATFS_SECTOR_SIZE;
}

static uint32_t afatfs_fileClusterToPhysical(uint32_t clusterNumber, uint32_t sectorIndex)
{
//	printf("afatfs.clusterStartSector: %u\r\n", afatfs.clusterStartSector);
//	printf("afatfs.sectorsPerCluster: %u\r\n", afatfs.sectorsPerCluster);
//	printf("sectorIndex: %u\r\n", sectorIndex);
	return afatfs.clusterStartSector + (clusterNumber - 2) * afatfs.sectorsPerCluster + sectorIndex;
}

static uint32_t afatfs_fileGetCursorPhysicalSector(afatfsFilePtr_t file)
{
//	printf("file->type: %u\r\n", file->type);			// file->type = 3 (AFATFS_FILE_TYPE_DIRECTORY)
	if (file->type == AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY) {
		return afatfs.fatStartSector + AFATFS_NUM_FATS * afatfs.fatSectors + file->cursorOffset / AFATFS_SECTOR_SIZE;
	} else {
		/* FAT32 */
//		printf("file->cursorOffset: %u\r\n", file->cursorOffset);			// file->cursorOffset = 0
		uint32_t cursorSectorInCluster = afatfs_sectorIndexInCluster(file->cursorOffset);
//		printf("cursorSectorInCluster: %u\r\n", cursorSectorInCluster);		// cursorSectorInCluster = 0
//		printf("file->cursorCluster: %u\r\n", file->cursorCluster);		// file->cursorCluster = 0
		return afatfs_fileClusterToPhysical(file->cursorCluster, cursorSectorInCluster);
	}
}

static int afatfs_getCacheDescriptorIndexForBuffer(uint8_t *memory)
{
	int index = (memory - afatfs.cache) / AFATFS_SECTOR_SIZE;
	
	if (afatfs_assert(index >= 0 && index < AFATFS_NUM_CACHE_SECTORS)) {
		return index;
	} else {
		return -1;
	}
}

/**
 * Take a look on the sector at the current file cursor position.
 *
 * Returns a pointer to the sector buffer if successful, or NULL if at the end of file (check afatfs_isEndOfAllocatedFile())
 * or the sector has not yet been read in from disk.
 */
static uint8_t* afatfs_fileRetainCursorSectorForRead(afatfsFilePtr_t file)
{
	uint8_t *result;
	
	uint32_t physicalSector = afatfs_fileGetCursorPhysicalSector(file);
	
//	printf("physicalSector: %u\r\n", physicalSector);			// physicalSector = 16384 - 2 * 64 = 16256
	
	/**
	 * If we've already got a locked sector then we can assume that was the same one that's the cursor (because this 
	 * cache is invalidated when crossing a sector boundary)
	 */
	if (file->readRetainCacheIndex != -1) {
		if (!afatfs_assert(physicalSector == afatfs.cacheDescriptor[file->readRetainCacheIndex].sectorIndex)) {
			return NULL;
		}
		
		result = afatfs_cacheSectorGetMemory(file->readRetainCacheIndex);
	} else {
		if (afatfs_isEndOfAllocatedFile(file)) {
			return NULL;
		}
		
		/* We never read the root sector using files */
		afatfs_assert(physicalSector > 0);
		
		afatfsOperationStatus_e status = afatfs_cacheSector(
			physicalSector,
			&result,
			AFATFS_CACHE_READ | AFATFS_CACHE_RETAIN,
			0
		);
		
		if (status != AFATFS_OPERATION_SUCCESS) {
			/* Sector not ready for read */
//			printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
			return NULL;
		}
		
		file->readRetainCacheIndex = afatfs_getCacheDescriptorIndexForBuffer(result);
	}
	
	return result;
}

/**
 * Attempt to advance the directory pointer `finder` to the next entry in the directory.
 * 
 * Returns:
 *		AFATFS_OPERATION_SUCCESS		- A pointer to the next directory entry has been loaded into *dirEntry.
 *										  If the directory was exhausted then *dirEntry will be set to NULL.
 *		AFATFS_OPERATION_IN_PROGRESS	- The disk is busy. The pointer is not advanced, call again later to retry
 */
afatfsOperationStatus_e afatfs_findNext(afatfsFilePtr_t directory, afatfsFinder_t *finder, fatDirectoryEntry_t **dirEntry)
{
	uint8_t *sector;
	
//	printf("AFATFS_FILES_PER_DIRECTORY_SECTOR: %u\r\n", AFATFS_FILES_PER_DIRECTORY_SECTOR);	// AFATFS_FILES_PER_DIRECTORY_SECTOR = 16
//	printf("finder->entryIndex: %d\r\n", finder->entryIndex);
	
	if (finder->entryIndex == AFATFS_FILES_PER_DIRECTORY_SECTOR - 1) {
		if (afatfs_fseekAtomic(directory, AFATFS_SECTOR_SIZE)) {
			finder->entryIndex = -1;
			/* Fall through to read the first entry of that new sector */
		} else {
			return AFATFS_OPERATION_IN_PROGRESS;
		}
	}
	
	sector = afatfs_fileRetainCursorSectorForRead(directory);
//	printf("sector: 0x%x, %s, %s, %d\r\n", (uint32_t)sector, __FILE__, __FUNCTION__, __LINE__);
	
//	printf("sector: 0x%x\r\n", (uint32_t)sector);
	
	if (sector) {
		finder->entryIndex++;
		
		*dirEntry = (fatDirectoryEntry_t *)sector + finder->entryIndex;
		
		finder->sectorNumberPhysical = afatfs_fileGetCursorPhysicalSector(directory);
		
		printf("%s, %d\r\n", __FUNCTION__, __LINE__);
		
		return AFATFS_OPERATION_SUCCESS;
	} else {
		if (afatfs_isEndOfAllocatedFile(directory)) {
			*dirEntry = NULL;
			printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
			return AFATFS_OPERATION_SUCCESS;
		}
		
		return AFATFS_OPERATION_IN_PROGRESS;
	}
}

/**
 * Release resources associated with a find operation. Calling this more than once is harmless.
 */
void afatfs_findLast(afatfsFilePtr_t directory)
{
	afatfs_fileUnlockCacheSector(directory);
}

/**
 * Load details from the given FAT directory entry into the file.
 */
static void afatfs_fileLoadDirectoryEntry(afatfsFile_t *file, fatDirectoryEntry_t *entry)
{
	printf("entry->firstClusterHigh: %u\r\n", entry->firstClusterHigh);
	printf("entry->firstClusterLow: %u\r\n", entry->firstClusterLow);
	file->firstCluster = (uint32_t) (entry->firstClusterHigh << 16) | entry->firstClusterLow;
	printf("file->firstCluster: %u\r\n", file->firstCluster);
	file->logicalSize = entry->fileSize;
	printf("file->logicalSize: %u\r\n", file->logicalSize);
	file->physicalSize = roundUpTo(entry->fileSize, afatfs_clusterSize());
	printf("file->physicalSize: %u\r\n", file->physicalSize);
	file->attrib = entry->attrib;
	printf("file->attrib: %u\r\n", file->attrib);
}

static afatfsCacheBlockDescriptor_t *afatfs_getCacheDescriptorForBuffer(uint8_t *memory)
{
	return afatfs.cacheDescriptor + afatfs_getCacheDescriptorIndexForBuffer(memory);
}

static void afatfs_appendRegularFreeClusterInitOperationState(afatfsAppendFreeCluster_t *state, uint32_t previousCluster)
{
	state->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_INITIAL;		// AFATFS_APPEND_FREE_CLUSTER_PHASE_INITIAL = 0
	state->previousCluster = previousCluster;
	state->searchCluster = afatfs.lastClusterAllocated;
}

/**
 * Sector here is the sector index within the cluster.
 */
static void afatfs_fileGetCursorClusterAndSector(afatfsFilePtr_t file, uint32_t *cluster, uint16_t *sector)
{
	*cluster = file->cursorCluster;
	
	if (file->type == AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY) {
		*sector = file->cursorOffset / AFATFS_SECTOR_SIZE;
	} else {
		/* FAT32 */
		*sector = afatfs_sectorIndexInCluster(file->cursorOffset);
	}
}

/**
 * Write the directory entry for the file into its `directoryEntryPos` position in its containing directory.
 *
 * mode:
 *     AFATFS_SAVE_DIRECTORY_NORMAL    - Store the file's physical size, not the logical size, in the directory entry
 *     AFATFS_SAVE_DIRECTORY_FOR_CLOSE - We're done extending the file so we can write the logical size now.
 *     AFATFS_SAVE_DIRECTORY_DELETED   - Mark the directory entry as deleted
 *
 * Returns:
 *     AFATFS_OPERATION_SUCCESS - The directory entry has been stored into the directory sector in cache.
 *     AFATFS_OPERATION_IN_PROGRESS - Cache is too busy, retry later
 *     AFATFS_OPERATION_FAILURE - If the filesystem enters the fatal state
 */
static afatfsOperationStatus_e afatfs_saveDirectoryEntry(afatfsFilePtr_t file, afatfsSaveDirectoryEntryMode_e mode)
{
    uint8_t *sector;
    afatfsOperationStatus_e result;

    if (file->directoryEntryPos.sectorNumberPhysical == 0) {
        return AFATFS_OPERATION_SUCCESS; // Root directories don't have a directory entry
    }

    result = afatfs_cacheSector(file->directoryEntryPos.sectorNumberPhysical, &sector, AFATFS_CACHE_READ | AFATFS_CACHE_WRITE, 0);

#ifdef AFATFS_DEBUG_VERBOSE
    fprintf(stderr, "Saving directory entry to sector %u...\n", file->directoryEntryPos.sectorNumberPhysical);
#endif

    if (result == AFATFS_OPERATION_SUCCESS) {
        if (afatfs_assert(file->directoryEntryPos.entryIndex >= 0)) {
            fatDirectoryEntry_t *entry = (fatDirectoryEntry_t *) sector + file->directoryEntryPos.entryIndex;

            switch (mode) {
               case AFATFS_SAVE_DIRECTORY_NORMAL:
                   /* We exaggerate the length of the written file so that if power is lost, the end of the file will
                    * still be readable (though the very tail of the file will be uninitialized data).
                    *
                    * This way we can avoid updating the directory entry too many times during fwrites() on the file.
                    */
                   entry->fileSize = file->physicalSize;
               break;
               case AFATFS_SAVE_DIRECTORY_DELETED:
                   entry->filename[0] = FAT_DELETED_FILE_MARKER;
                   //Fall through

               case AFATFS_SAVE_DIRECTORY_FOR_CLOSE:
                   // We write the true length of the file on close.
                   entry->fileSize = file->logicalSize;
            }

            // (sub)directories don't store a filesize in their directory entry:
            if (file->type == AFATFS_FILE_TYPE_DIRECTORY) {
                entry->fileSize = 0;
            }

            entry->firstClusterHigh = file->firstCluster >> 16;
            entry->firstClusterLow = file->firstCluster & 0xFFFF;
        } else {
            return AFATFS_OPERATION_FAILURE;
        }
    }

    return result;
}

/**
 * Set the cluster number that follows the given cluster. Pass 0xFFFFFFFF for nextCluster to terminate the FAT chain.
 *
 * Returns:
 *     AFATFS_OPERATION_SUCCESS     - On success
 *     AFATFS_OPERATION_IN_PROGRESS - Card is busy, call again later
 *     AFATFS_OPERATION_FAILURE     - When the filesystem encounters a fatal error
 */
static afatfsOperationStatus_e afatfs_FATSetNextCluster(uint32_t startCluster, uint32_t nextCluster)
{
    afatfsFATSector_t sector;
    uint32_t fatSectorIndex, fatSectorEntryIndex, fatPhysicalSector;
    afatfsOperationStatus_e result;

#ifdef AFATFS_DEBUG
    afatfs_assert(startCluster >= FAT_SMALLEST_LEGAL_CLUSTER_NUMBER);
#endif

    afatfs_getFATPositionForCluster(startCluster, &fatSectorIndex, &fatSectorEntryIndex);

    fatPhysicalSector = afatfs_fatSectorToPhysical(0, fatSectorIndex);

    result = afatfs_cacheSector(fatPhysicalSector, &sector.bytes, AFATFS_CACHE_READ | AFATFS_CACHE_WRITE, 0);

    if (result == AFATFS_OPERATION_SUCCESS) {
        if (afatfs.filesystemType == FAT_FILESYSTEM_TYPE_FAT16) {
            sector.fat16[fatSectorEntryIndex] = nextCluster;
        } else {
            sector.fat32[fatSectorEntryIndex] = nextCluster;
        }
    }

    return result;
}

/**
 * Starting from and including the given cluster number, find the number of the first cluster which matches the given
 * condition.
 *
 * searchLimit - Last cluster to examine (exclusive). To search the entire volume, pass:
 *                   afatfs.numClusters + FAT_SMALLEST_LEGAL_CLUSTER_NUMBER
 *
 * Condition:
 *     CLUSTER_SEARCH_FREE_AT_BEGINNING_OF_FAT_SECTOR - Find a cluster marked as free in the FAT which lies at the
 *         												beginning of its FAT sector. 
 *														The passed initial search 'cluster' must correspond to the first entry of a FAT sector.
 *     CLUSTER_SEARCH_FREE            				  - Find a cluster marked as free in the FAT
 *     CLUSTER_SEARCH_OCCUPIED        				  - Find a cluster marked as occupied in the FAT.
 *
 * Returns:
 *     AFATFS_FIND_CLUSTER_FOUND       - A cluster matching the criteria was found and stored in *cluster
 *     AFATFS_FIND_CLUSTER_IN_PROGRESS - The search is not over, call this routine again later with the updated *cluster value to resume
 *     AFATFS_FIND_CLUSTER_FATAL       - An unexpected read error occurred, the volume should be abandoned
 *     AFATFS_FIND_CLUSTER_NOT_FOUND   - The entire device was searched without finding a suitable cluster (the
 *                                       *cluster points to just beyond the final cluster).
 */
static afatfsFindClusterStatus_e afatfs_findClusterWithCondition(afatfsClusterSearchCondition_e condition, uint32_t *cluster, uint32_t searchLimit)
{
    afatfsFATSector_t sector;
    uint32_t fatSectorIndex, fatSectorEntryIndex;

    uint32_t fatEntriesPerSector = afatfs_fatEntriesPerSector();
    bool lookingForFree = condition == CLUSTER_SEARCH_FREE_AT_BEGINNING_OF_FAT_SECTOR || condition == CLUSTER_SEARCH_FREE;

    int jump;

    /* Get the FAT entry which corresponds to this cluster so we can begin our search there */
    afatfs_getFATPositionForCluster(*cluster, &fatSectorIndex, &fatSectorEntryIndex);

    switch (condition) {
        case CLUSTER_SEARCH_FREE_AT_BEGINNING_OF_FAT_SECTOR:
            jump = fatEntriesPerSector;

            /* We're supposed to call this routine with the cluster properly aligned */
            if (!afatfs_assert(fatSectorEntryIndex == 0)) {
                return AFATFS_FIND_CLUSTER_FATAL;
            }
			break;
			
        case CLUSTER_SEARCH_OCCUPIED:
        case CLUSTER_SEARCH_FREE:
            jump = 1;
			break;
        
		default:
            afatfs_assert(false);
            return AFATFS_FIND_CLUSTER_FATAL;
    }

    while (*cluster < searchLimit) {

#ifdef AFATFS_USE_FREEFILE
        /* If we're looking inside the freefile, we won't find any free clusters! Skip it! */
        if (afatfs.freeFile.logicalSize > 0 && *cluster == afatfs.freeFile.firstCluster) {
            *cluster += (afatfs.freeFile.logicalSize + afatfs_clusterSize() - 1) / afatfs_clusterSize();

            /* Maintain alignment */
            *cluster = roundUpTo(*cluster, jump);
            continue; // Go back to check that the new cluster number is within the volume
        }
#endif

        afatfsOperationStatus_e status = afatfs_cacheSector(afatfs_fatSectorToPhysical(0, fatSectorIndex), &sector.bytes, AFATFS_CACHE_READ | AFATFS_CACHE_DISCARDABLE, 0);

        switch (status) {
            case AFATFS_OPERATION_SUCCESS:
                do {
                    uint32_t clusterNumber;

                    switch (afatfs.filesystemType) {
                        case FAT_FILESYSTEM_TYPE_FAT16:
                            clusterNumber = sector.fat16[fatSectorEntryIndex];
                        break;
                        case FAT_FILESYSTEM_TYPE_FAT32:
                            clusterNumber = fat32_decodeClusterNumber(sector.fat32[fatSectorEntryIndex]);
                        break;
                        default:
                            return AFATFS_FIND_CLUSTER_FATAL;
                    }

                    if (fat_isFreeSpace(clusterNumber) == lookingForFree) {
                        /*
                         * The final FAT sector may have fewer than fatEntriesPerSector entries in it, so we need to
                         * check the cluster number is valid here before we report a bogus success!
                         */
                        if (*cluster < searchLimit) {
                            return AFATFS_FIND_CLUSTER_FOUND;
                        } else {
                            *cluster = searchLimit;
                            return AFATFS_FIND_CLUSTER_NOT_FOUND;
                        }
                    }

                    (*cluster) += jump;
                    fatSectorEntryIndex += jump;
                } while (fatSectorEntryIndex < fatEntriesPerSector);

                /* Move on to the next FAT sector */
                fatSectorIndex++;
                fatSectorEntryIndex = 0;
				break;
            
			case AFATFS_OPERATION_FAILURE:
                return AFATFS_FIND_CLUSTER_FATAL;
				break;
            
			case AFATFS_OPERATION_IN_PROGRESS:
                return AFATFS_FIND_CLUSTER_IN_PROGRESS;
				break;
        }
    }

    /* We looked at every available cluster and didn't find one matching the condition */
    *cluster = searchLimit;
    return AFATFS_FIND_CLUSTER_NOT_FOUND;
}

/**
 * Attempt to add a free cluster to the end of the given file. If the file was previously empty, the directory entry
 * is updated to point to the new cluster.
 *
 * Returns:
 *     AFATFS_OPERATION_SUCCESS     - The cluster has been appended
 *     AFATFS_OPERATION_IN_PROGRESS - Cache was busy, so call again later to continue
 *     AFATFS_OPERATION_FAILURE     - Cluster could not be appended because the filesystem ran out of space
 *                                    (afatfs.filesystemFull is set to true)
 *
 * If the file's operation was AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER, the file operation is cleared upon completion,
 * otherwise it is left alone so that this operation can be called as a sub-operation of some other operation on the
 * file.
 */
static afatfsOperationStatus_e afatfs_appendRegularFreeClusterContinue(afatfsFile_t *file)
{
	afatfsAppendFreeCluster_t *opState = &file->operation.state.appendFreeCluster;
	afatfsOperationStatus_e status;
	
	doMore:
	
	switch (opState->phase) {
		case AFATFS_APPEND_FREE_CLUSTER_PHASE_FIND_FREESPACE:
			switch (afatfs_findClusterWithCondition(CLUSTER_SEARCH_FREE, &opState->searchCluster, afatfs.numClusters + FAT_SMALLEST_LEGAL_CLUSTER_NUMBER)) {
				case AFATFS_FIND_CLUSTER_FOUND:
					afatfs.lastClusterAllocated = opState->searchCluster;
					
					/* Make the cluster available for us to write in */
					file->cursorCluster = opState->searchCluster;
					file->physicalSize += afatfs_clusterSize();
				
					if (opState->previousCluster == 0) {
						/* This is the new first cluster in the file */
						file->firstCluster = opState->searchCluster;
					}
					
					opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT1;
					goto doMore;
					break;
				
				case AFATFS_FIND_CLUSTER_FATAL:
				case AFATFS_FIND_CLUSTER_NOT_FOUND:
					/* We couldn't find an empty cluster to append to the file */
					opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_FAILURE;
					goto doMore;
					break;
				
				case AFATFS_FIND_CLUSTER_IN_PROGRESS:
					break;
			}
			break;
		
		case AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT1:
			/* Terminate the new cluster */
			status = afatfs_FATSetNextCluster(opState->searchCluster, 0xFFFFFFFF);
			if (status == AFATFS_OPERATION_SUCCESS) {
				if (opState->previousCluster) {
					opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT2;
				} else {
					opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FILE_DIRECTORY;
				}
				
				goto doMore;
			}
			break;
		
		case AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT2:
			/* Add the new cluster to the pre-existing chain */
			status = afatfs_FATSetNextCluster(opState->previousCluster, opState->searchCluster);
			if (status == AFATFS_OPERATION_SUCCESS) {
				opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FILE_DIRECTORY;
				goto doMore;
			}
			break;
		
		case AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FILE_DIRECTORY:
			if (afatfs_saveDirectoryEntry(file, AFATFS_SAVE_DIRECTORY_NORMAL) == AFATFS_OPERATION_SUCCESS) {
				opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_COMPLETE;
				goto doMore;
			}
			break;
		
		case AFATFS_APPEND_FREE_CLUSTER_PHASE_COMPLETE:
			if (file->operation.operation == AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER) {
				file->operation.operation = AFATFS_FILE_OPERATION_NONE;
			}
			
			return AFATFS_OPERATION_SUCCESS;
			break;
		
		case AFATFS_APPEND_FREE_CLUSTER_PHASE_FAILURE:
			if (file->operation.operation == AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER) {
				file->operation.operation = AFATFS_FILE_OPERATION_NONE;
			}
			
			afatfs.filesystemFull = true;
			
			return AFATFS_OPERATION_FAILURE; 
			break;
		
		default:
			;
	}
	
	return AFATFS_OPERATION_IN_PROGRESS;
}

static afatfsOperationStatus_e afatfs_extendSubdirectoryContinue(afatfsFile_t *directory)
{
	afatfsExtendSubdirectory_t *opState = &directory->operation.state.extendSubdirectory;
	afatfsOperationStatus_e status;
	uint8_t *sectorBuffer;
	uint32_t clusterNumber, physicalSector;
	uint16_t sectorInCluster;
	
	doMore:
	
//	printf("opState->phase: %u, %d\r\n", opState->phase, __LINE__);
	
	switch (opState->phase) {
		case AFATFS_EXTEND_SUBDIRECTORY_PHASE_ADD_FREE_CLUSTER:
			status = afatfs_appendRegularFreeClusterContinue(directory);
		
			if (status == AFATFS_OPERATION_SUCCESS) {
				opState->phase = AFATFS_EXTEND_SUBDIRECTORY_PHASE_WRITE_SECTORS;
				goto doMore;
			} else if (status == AFATFS_OPERATION_FAILURE) {
				opState->phase = AFATFS_EXTEND_SUBDIRECTORY_PHASE_FAILURE;
				goto doMore;
			}
			break;
		
		case AFATFS_EXTEND_SUBDIRECTORY_PHASE_WRITE_SECTORS:
			/* Now, zero out that cluster */
			afatfs_fileGetCursorClusterAndSector(directory, &clusterNumber, &sectorInCluster);
			physicalSector = afatfs_fileGetCursorPhysicalSector(directory);
		
			while (1) {
				status = afatfs_cacheSector(physicalSector, &sectorBuffer, AFATFS_CACHE_WRITE, 0);
				
				if (status != AFATFS_OPERATION_SUCCESS) {
					return status;
				}
				
				memset(sectorBuffer, 0, AFATFS_SECTOR_SIZE);
				
				/* If this is the first sector of a non-root directory, create the "." and ".." entries */
				if (directory->directoryEntryPos.sectorNumberPhysical != 0 && directory->cursorOffset == 0) {
					fatDirectoryEntry_t *dirEntries = (fatDirectoryEntry_t *) sectorBuffer;
					
					memset(dirEntries[0].filename, ' ', sizeof(dirEntries[0].filename));
					dirEntries[0].filename[0] = '.';
					dirEntries[0].firstClusterHigh = directory->firstCluster >> 16;
					dirEntries[0].firstClusterLow = directory->firstCluster & 0xFFFF;
					dirEntries[0].attrib = FAT_FILE_ATTRIBUTE_DIRECTORY;		// FAT_FILE_ATTRIBUTE_DIRECTORY = 0x10
					
					memset(dirEntries[1].filename, ' ', sizeof(dirEntries[1].filename));
					dirEntries[1].filename[0] = '.';
					dirEntries[1].filename[1] = '.';
					dirEntries[1].firstClusterHigh = opState->parentDirectoryCluster >> 16;
					dirEntries[1].firstClusterLow = opState->parentDirectoryCluster & 0xFFFF;
					dirEntries[1].attrib = FAT_FILE_ATTRIBUTE_DIRECTORY;
				}
				
				if (sectorInCluster < afatfs.sectorsPerCluster - 1) {
					/* Move to next sector */
					afatfs_assert(afatfs_fseekAtomic(directory, AFATFS_SECTOR_SIZE));
					sectorInCluster++;
					physicalSector++;
				} else {
					break;
				}
			}
			
			/* Seek back to the beginning of the cluster */
			afatfs_assert(afatfs_fseekAtomic(directory, -AFATFS_SECTOR_SIZE * (afatfs.sectorsPerCluster - 1)));
			opState->phase = AFATFS_EXTEND_SUBDIRECTORY_PHASE_SUCCESS;
			goto doMore;
			break;
		
		case AFATFS_EXTEND_SUBDIRECTORY_PHASE_SUCCESS:
			directory->operation.operation = AFATFS_FILE_OPERATION_NONE;		// AFATFS_FILE_OPERATION_NONE = 0
		
			if (opState->callback) {
				opState->callback(directory);
			}
			
			return AFATFS_OPERATION_SUCCESS;
			break;
		
		case AFATFS_EXTEND_SUBDIRECTORY_PHASE_FAILURE:
			directory->operation.operation = AFATFS_FILE_OPERATION_NONE;
			
			if (opState->callback) {
				opState->callback(NULL);
			}
			
			return AFATFS_OPERATION_FAILURE;
			break;
	}
	
//	printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
	return AFATFS_OPERATION_IN_PROGRESS;
}

/**
 * Queue an operation to add a cluster to a sub-directory.
 * 
 * The new cluster is zero-filled. "." and ".." entries are added if it is the first cluster of a new subdirectory.
 *
 * The directory must not be busy, otherwise AFATFS_OPERATION_FAILURE is returned immediately.
 *
 * The directory's cursor must lie at the end of the directory file (i.e. isEndOfAllocatedFile() would return true).
 *
 * You must provide parentDirectory if this is the first extension to the subdirectory, otherwise pass NULL for that argument.
 */
static afatfsOperationStatus_e afatfs_extendSubdirectory(afatfsFile_t *directory, afatfsFilePtr_t parentDirectory, afatfsFileCallback_t callback)
{
	/* FAT16 root directories cannot be extended */
	if (directory->type == AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY || afatfs_fileIsBusy(directory)) {
		return AFATFS_OPERATION_FAILURE;
	}
	
	/*
	 * We'll assume that we are never asked to append the first cluster of a root directory, since any
	 * reasonably-formatted volume should have a root!
	 */
	afatfsExtendSubdirectory_t *opState = &directory->operation.state.extendSubdirectory;
	
	directory->operation.operation = AFATFS_FILE_OPERATION_EXTEND_SUBDIRECTORY;		// AFATFS_FILE_OPERATION_EXTEND_SUBDIRECTORY = 9
	
	opState->phase = AFATFS_EXTEND_SUBDIRECTORY_PHASE_INITIAL;		// AFATFS_EXTEND_SUBDIRECTORY_PHASE_INITIAL = 0
	
	opState->parentDirectoryCluster = parentDirectory ? parentDirectory->firstCluster : 0;
	
	opState->callback = callback;
	
	afatfs_appendRegularFreeClusterInitOperationState(&opState->appendFreeCluster, directory->cursorPreviousCluster);
	
//	printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
	
	return afatfs_extendSubdirectoryContinue(directory);
}

/**
 * Allocate space for a new directory entry to be written, store the position of that entry in the finder, and set
 * the *dirEntry pointer to point to the entry within the cached FAT sector. This pointer's lifetime is only as good 
 * as the life of the cache, so don't dawdle.
 *
 * Before the first call to this function, call afatfs_findFirst() on the directory.
 *
 * The directory sector in the cache is marked as dirty, so any changes written through to the entry will be flushed out
 * in a subsequent poll cycle.
 *
 * Returns:
 *		AFATFS_OPERATION_IN_PROGRESS		- Call again later to continue.
 *		AFATFS_OPERATION_SUCCESS			- Entry has been inserted and *dirEntry and *finder have been updated.
 *		AFATFS_OPERATION_FAILURE			- When the directory is full.
 */
static afatfsOperationStatus_e afatfs_allocateDirectoryEntry(afatfsFilePtr_t directory, fatDirectoryEntry_t **dirEntry, afatfsFinder_t *finder)
{
	afatfsOperationStatus_e result;
	
	if (afatfs_fileIsBusy(directory)) {
//		printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
		return AFATFS_OPERATION_IN_PROGRESS;
	}
	
	while ((result = afatfs_findNext(directory, finder, dirEntry)) == AFATFS_OPERATION_SUCCESS) {
		if (*dirEntry) {
			if (fat_isDirectoryEntryEmpty(*dirEntry) || fat_isDirectoryEntryTerminator(*dirEntry)) {
				afatfs_cacheSectorMarkDirty(afatfs_getCacheDescriptorForBuffer((uint8_t *)*dirEntry));
				
				afatfs_findLast(directory);
//				printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
				return AFATFS_OPERATION_SUCCESS;
			}
		} else {
//			printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
			/* Need to extend directory size by adding a cluster */
			result = afatfs_extendSubdirectory(directory, NULL, NULL);

//			printf("result: %u, %s, %s, %d\r\n", result, __FILE__, __FUNCTION__, __LINE__);
			
			if (result == AFATFS_OPERATION_SUCCESS) {
				/* Continue the search in the newly-extended directory */
				continue;
			} else {
				/* The status (in progress or failure) of extending the directory becomes our status */
				break;
			}
		}
	}
	
	return result;
}

/**
 * Queue an operation to truncate the file to zero bytes in length.
 *
 * Returns true if the operation was successfully queued or false if the file is busy (try again later).
 *
 * The callback is called once the file has been truncated (some time after this routine returns).
 */
bool afatfs_ftruncate(afatfsFilePtr_t file, afatfsFileCallback_t callback)
{
    afatfsTruncateFile_t *opState;

    if (afatfs_fileIsBusy(file))
        return false;

    file->operation.operation = AFATFS_FILE_OPERATION_TRUNCATE;

    opState = &file->operation.state.truncateFile;
    opState->callback = callback;
    opState->phase = AFATFS_TRUNCATE_FILE_INITIAL;
    opState->startCluster = file->firstCluster;
    opState->currentCluster = opState->startCluster;

#ifdef AFATFS_USE_FREEFILE
    if ((file->mode & AFATFS_FILE_MODE_CONTIGUOUS) != 0) {
        // The file is contiguous and ends where the freefile begins
        opState->endCluster = afatfs.freeFile.firstCluster;
    } else
#endif
    {
        // The range of clusters to delete is not contiguous, so follow it as a linked-list instead
        opState->endCluster = 0;
    }

    // We'll drop the cluster chain from the directory entry immediately
    file->firstCluster = 0;
    file->logicalSize = 0;
    file->physicalSize = 0;

    afatfs_fseek(file, 0, AFATFS_SEEK_SET);

    return true;
}

static void afatfs_createFileContinue(afatfsFile_t *file)
{
	afatfsCreateFile_t *opState = &file->operation.state.createFile;
	fatDirectoryEntry_t *entry;
	afatfsOperationStatus_e status;
	
	doMore:
	
	switch (opState->phase) {
		case AFATFS_CREATEFILE_PHASE_INITIAL:
//			printf("afatfs.currentDirectory.type: %u\r\n", afatfs.currentDirectory.type);			// afatfs.currentDirectory.type = AFATFS_FILE_TYPE_DIRECTORY (3)
//			printf("afatfs.currentDirectory.attrib: %u\r\n", afatfs.currentDirectory.attrib);		// afatfs.currentDirectory.attrib = FAT_FILE_ATTRIBUTE_DIRECTORY (0x10, 16)
//			printf("afatfs.currentDirectory.cursorCluster: %u\r\n", afatfs.currentDirectory.cursorCluster);	// afatfs.currentDirectory.cursorCluster = 0
//			printf("afatfs.currentDirectory.cursorOffset: %u\r\n", afatfs.currentDirectory.cursorOffset);	// afatfs.currentDirectory.cursorOffset = 0
//			printf("afatfs.currentDirectory.cursorPreviousCluster: %u\r\n", afatfs.currentDirectory.cursorPreviousCluster);	// afatfs.currentDirectory.cursorPreviousCluster = 0
//			printf("afatfs.currentDirectory.directoryEntryPos.entryIndex: %d\r\n", afatfs.currentDirectory.directoryEntryPos.entryIndex);	// afatfs.currentDirectory.directoryEntryPos.entryIndex = 0
//			printf("afatfs.currentDirectory.directoryEntryPos.sectorNumberPhysical: %u\r\n", afatfs.currentDirectory.directoryEntryPos.sectorNumberPhysical);	// sectorNumberPhysical = 0
//			printf("afatfs.currentDirectory.firstCluster: %u\r\n", afatfs.currentDirectory.firstCluster);	// afatfs.currentDirectory.firstCluster = 2
//			printf("afatfs.currentDirectory.logicalSize: %u\r\n", afatfs.currentDirectory.logicalSize);		// afatfs.currentDirectory.logicalSize = 0
//			printf("afatfs.currentDirectory.mode: %u\r\n", afatfs.currentDirectory.mode);	// afatfs.currentDirectory.mode = AFATFS_FILE_MODE_READ(1) | AFATFS_FILE_MODE_WRITE(2) = 1 | 2 = 3
//			printf("afatfs.currentDirectory.physicalSize: %u\r\n", afatfs.currentDirectory.physicalSize);	// afatfs.currentDirectory.physicalSize = 0
//			printf("afatfs.currentDirectory.operation.operation: %u\r\n", afatfs.currentDirectory.operation.operation);	// afatfs.currentDirectory.operation.operation = 0
//			printf("afatfs.currentDirectory.operation.state.createFile.filename: %s\r\n", afatfs.currentDirectory.operation.state.createFile.filename);	// currentDirectory's filename = NULL
																																						// freeFile's filename = FREESPACE.E
//			printf("afatfs.currentDirectory.operation.state.createFile.phase: %u\r\n", afatfs.currentDirectory.operation.state.createFile.phase);	// phase = AFATFS_CREATEFILE_PHASE_INITIAL (0)
			afatfs_findFirst(&afatfs.currentDirectory, &file->directoryEntryPos);
//			printf("file->directoryEntryPos.entryIndex: %d\r\n", file->directoryEntryPos.entryIndex);	// (int8_t) file->directoryEntryPos.entryIndex = -1
			opState->phase = AFATFS_CREATEFILE_PHASE_FIND_FILE;
			goto doMore;
			break;
		
		case AFATFS_CREATEFILE_PHASE_FIND_FILE:
//			printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
			do {
				status = afatfs_findNext(&afatfs.currentDirectory, &file->directoryEntryPos, &entry);
//				printf("status: %u\r\n", status);	// status = 1
				
				switch (status) {
					case AFATFS_OPERATION_SUCCESS:
						/* Is this the last entry in the directory */
						if (entry == NULL || fat_isDirectoryEntryTerminator(entry)) {
							afatfs_findLast(&afatfs.currentDirectory);
							
//							printf("file->mode: %u\r\n", file->mode);	// file->mode = 48 (0x30) (AFATFS_FILE_MODE_CREATE | AFATFS_FILE_MODE_RETAIN_DIRECTORY)
							
							if ((file->mode & AFATFS_FILE_MODE_CREATE) != 0) {
								/* The file didn't already exist, so we can create it. Allocate a new directory entry */
								afatfs_findFirst(&afatfs.currentDirectory, &file->directoryEntryPos);
								
								opState->phase = AFATFS_CREATEFILE_PHASE_CREATE_NEW_FILE;
//								printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
								goto doMore;
							} else {
								/* File not found */
								opState->phase = AFATFS_CREATEFILE_PHASE_FAILURE;
								
								goto doMore;
							}
						} else if (strncmp(entry->filename, (char *)opState->filename, FAT_FILENAME_LENGTH) == 0) {
							/* We found a file with this name! */
							afatfs_fileLoadDirectoryEntry(file, entry);
							
							afatfs_findLast(&afatfs.currentDirectory);
							
							opState->phase = AFATFS_CREATEFILE_PHASE_SUCCESS;
							
							goto doMore;
						}	// else this entry doesn't match, fall through and continue the search.
						break;
					
					case AFATFS_OPERATION_FAILURE:
						break;
					
					case AFATFS_OPERATION_IN_PROGRESS:
						;
				}
			} while (status == AFATFS_OPERATION_SUCCESS);
			
			break;
		
		case AFATFS_CREATEFILE_PHASE_CREATE_NEW_FILE:
//			printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
			status = afatfs_allocateDirectoryEntry(&afatfs.currentDirectory, &entry, &file->directoryEntryPos);
		
			printf("status: %u, %s, %s, %d\r\n", status, __FILE__, __FUNCTION__, __LINE__);
		
            if (status == AFATFS_OPERATION_SUCCESS) {
                memset(entry, 0, sizeof(*entry));

                memcpy(entry->filename, opState->filename, FAT_FILENAME_LENGTH);
                entry->attrib = file->attrib;
                entry->creationDate = AFATFS_DEFAULT_FILE_DATE;
                entry->creationTime = AFATFS_DEFAULT_FILE_TIME;
                entry->lastWriteDate = AFATFS_DEFAULT_FILE_DATE;
                entry->lastWriteTime = AFATFS_DEFAULT_FILE_TIME;

#ifdef AFATFS_DEBUG_VERBOSE
                fprintf(stderr, "Adding directory entry for %.*s to sector %u\n", FAT_FILENAME_LENGTH, opState->filename, file->directoryEntryPos.sectorNumberPhysical);
#endif

                opState->phase = AFATFS_CREATEFILE_PHASE_SUCCESS;
                goto doMore;
            } else if (status == AFATFS_OPERATION_FAILURE) {
                opState->phase = AFATFS_CREATEFILE_PHASE_FAILURE;
                goto doMore;
            }
			break;
		
		case AFATFS_CREATEFILE_PHASE_SUCCESS:
//			printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
            if ((file->mode & AFATFS_FILE_MODE_RETAIN_DIRECTORY) != 0) {
                /*
                 * For this high performance file type, we require the directory entry for the file to be retained
                 * in the cache at all times.
                 */
                uint8_t *directorySector;

                status = afatfs_cacheSector(
                    file->directoryEntryPos.sectorNumberPhysical,
                    &directorySector,
                    AFATFS_CACHE_READ | AFATFS_CACHE_RETAIN,
                    0
                );

                if (status != AFATFS_OPERATION_SUCCESS) {
                    // Retry next time
                    break;
                }
            }

            afatfs_fseek(file, 0, AFATFS_SEEK_SET);

            // Is file empty?
            if (file->cursorCluster == 0) {
#ifdef AFATFS_USE_FREEFILE
                if ((file->mode & AFATFS_FILE_MODE_CONTIGUOUS) != 0) {
                    if (afatfs_fileIsBusy(&afatfs.freeFile)) {
                        // Someone else's using the freefile, come back later.
                        break;
                    } else {
                        // Lock the freefile for our exclusive access
                        afatfs.freeFile.operation.operation = AFATFS_FILE_OPERATION_LOCKED;
                    }
                }
#endif
            } else {
                // We can't guarantee that the existing file contents are contiguous
                file->mode &= ~AFATFS_FILE_MODE_CONTIGUOUS;

                // Seek to the end of the file if it is in append mode
                if ((file->mode & AFATFS_FILE_MODE_APPEND) != 0) {
                    // This replaces our open file operation
                    file->operation.operation = AFATFS_FILE_OPERATION_NONE;
                    afatfs_fseekInternal(file, file->logicalSize, opState->callback);
                    break;
                }

                // If we're only writing (not reading) the file must be truncated
                if (file->mode == (AFATFS_FILE_MODE_CREATE | AFATFS_FILE_MODE_WRITE)) {
                    // This replaces our open file operation
                    file->operation.operation = AFATFS_FILE_OPERATION_NONE;
                    afatfs_ftruncate(file, opState->callback);
                    break;
                }
            }

            file->operation.operation = AFATFS_FILE_OPERATION_NONE;
            opState->callback(file);
			break;
		
		case AFATFS_CREATEFILE_PHASE_FAILURE:
//			printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
            file->type = AFATFS_FILE_TYPE_NONE;
            opState->callback(NULL);		
			break;
	}
}

/**
 * Open (or create) a file in the CWD with the given filename
 *
 * file				- Memory location to store the newly opened file details
 * name				- Filename in "name.ext" format. No path
 * attrib			- FAT file attributes to give the file (if created)
 * fileMode			- Bitset of AFATFS_FILE_MODE_* constants. Including AFATFS_FILE_MODE_CREATE to create the file if
 *					  it does not exist.
 * callback			- Called when the operation is completed
 */
static afatfsFilePtr_t afatfs_createFile(afatfsFilePtr_t file, const char *name, uint8_t attrib, uint8_t fileMode, afatfsFileCallback_t callback)
{
//	printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
	
	/**
	 * INFO: Initialise file (afatfs.freeFile) members
	 */
	afatfsCreateFile_t *opState = &file->operation.state.createFile;
	
	/* Initialise file handle
	 *
	 * file->writeLockedCacheIndex = -1
	 * file->readRetainCacheIndex = -1
	 */
	afatfs_initFileHandle(file);
	
//	printf("writeLockedCacheIndex: %d\r\n", file->writeLockedCacheIndex);	// file->writeLockedCacheIndex = -1
//	printf("readRetainCacheIndex: %d\r\n", file->readRetainCacheIndex);		// file->readRetainCacheIndex = -2
	
	/* Queued the operation to finish the file creation */
	file->operation.operation = AFATFS_FILE_OPERATION_CREATE_FILE;

//	printf("file->operation.operation: %u\r\n", file->operation.operation);	// file->operation.operation = AFATFS_FILE_OPERATION_CREATE_FILE (1)
	
	/* Initialise file mode */
	file->mode = fileMode;
	
//	printf("file->mode: 0x%x\r\n", file->mode);		// file->mode = 48 (0x30)
	
	/* Initialise file's firstCluster, physicalSize, logicalSize, attrib and type */
	if (strcmp(name, ".") == 0) {
		file->firstCluster = afatfs.currentDirectory.firstCluster;
		file->physicalSize = afatfs.currentDirectory.physicalSize;
		file->logicalSize = afatfs.currentDirectory.logicalSize;
		file->attrib = afatfs.currentDirectory.attrib;
		file->type = afatfs.currentDirectory.type;
//		printf("file->firstCluster: %u\r\n", file->firstCluster);
//		printf("file->physicalSize: %u\r\n", file->physicalSize);
//		printf("file->logicalSize: %u\r\n", file->logicalSize);
//		printf("file->attrib: %u\r\n", file->attrib);
//		printf("file->type: %u\r\n", file->type);
	} else {
//		printf("name: %s\r\n", name);								// name = "FREESPAC.E"
		fat_convertFilenameToFATStyle(name, opState->filename);
//		printf("opState->filename: %s\r\n", opState->filename);		// opState->filename = "FREESPACE"
//		printf("afatfs.freefile.operation.state.createFile.filename: %s\r\n", afatfs.freeFile.operation.state.createFile.filename);	// filename = 
		file->attrib = attrib;
		
//		printf("file->attrib: %u\r\n", file->attrib);  // file->attrib = 0x1(FAT_FILE_ATTRIBUTE_READ_ONLY) | 0x4 (FAT_FILE_ATTRIBUTE_READ_ONLY) = 0x5
		
		if ((attrib & FAT_FILE_ATTRIBUTE_DIRECTORY) != 0) {
			file->type = AFATFS_FILE_TYPE_DIRECTORY;
//			printf("file->type: %u\r\n", file->type);
		} else {
			file->type = AFATFS_FILE_TYPE_NORMAL;
//			printf("file->type: %u\r\n", file->type);		// file->type = AFATFS_FILE_TYPE_NORMAL (1)
		}
	}
	
	/* Initialise file->operation.state.createFile.callback = callback */
	opState->callback = callback;
	
	if (strcmp(name, ".") == 0) {
		/* Since we already have the directory entry details, we can skip straight to the final operations required */
		opState->phase = AFATFS_CREATEFILE_PHASE_SUCCESS;		// AFATFS_CREATEFILE_PHASE_SUCCESS = 3
	} else {
		opState->phase = AFATFS_CREATEFILE_PHASE_INITIAL;		// AFATFS_CREATEFILE_PHASE_INITIAL = 0
//		printf("opState->phase: %u\r\n", opState->phase);
	}
	
	afatfs_createFileContinue(file);
	
	return file;
}

/**
 * Call to set up the initial state for finding the largest block of free space on the device whose corresponding FAT
 * sectors are themselves entirely free space (so the free space has dedicated FAT sectors of its own).
 */
static void afatfs_findLargestContiguousFreeBlockBegin(void)
{
	/* The first FAT sector has two reserved entries, so it isn't eligible for this search.
	 * Start at the next FAT sector.
	 */
	afatfs.initState.freeSpaceSearch.candidateStart = afatfs_fatEntriesPerSector();
	afatfs.initState.freeSpaceSearch.candidateEnd = afatfs.initState.freeSpaceSearch.candidateStart;
	afatfs.initState.freeSpaceSearch.bestGapStart = 0;
	afatfs.initState.freeSpaceSearch.bestGapLength = 0;
	afatfs.initState.freeSpaceSearch.phase = AFATFS_FREE_SPACE_SEARCH_PHASE_FIND_HOLE;
}

static void afatfs_freeFileCreated(afatfsFile_t *file)
{
	printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
	
	if (file) {
		/* Did the freefile already have allocated space? */
		if (file->logicalSize > 0) {
			/* We've completed freefile init, move on to the next init phase */
			afatfs.initPhase = AFATFS_INITIALISATION_FREEFILE_LAST + 1;
		} else {
			/* Allocate clusters for the freefile */
			afatfs_findLargestContiguousFreeBlockBegin();
			afatfs.initPhase = AFATFS_INITIALISATION_FREEFILE_FAT_SEARCH;
			printf("afatfs.initPhase: %u, %d\r\n", afatfs.initPhase, __LINE__);
		}
	} else {
		/* Failed to allocate an entry */
		afatfs.lastError = AFATFS_ERROR_GENERIC;
		afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_FATAL;
	}
}

/**
 * Bring the logical filesize up to date with the current cursor position.
 */
static void afatfs_fileUpdateFilesize(afatfsFile_t *file)
{
    file->logicalSize = MAX(file->logicalSize, file->cursorOffset);
}

/**
 * Returns true if the seek was completed, or false if it is still in progress.
 */
static bool afatfs_fseekInternalContinue(afatfsFile_t *file)
{
    afatfsSeek_t *opState = &file->operation.state.seek;
    uint32_t clusterSizeBytes = afatfs_clusterSize();
    uint32_t offsetInCluster = afatfs_byteIndexInCluster(file->cursorOffset);

    afatfsOperationStatus_e status;

    // Keep advancing the cursor cluster forwards to consume seekOffset
    while (offsetInCluster + opState->seekOffset >= clusterSizeBytes && !afatfs_isEndOfAllocatedFile(file)) {
        uint32_t nextCluster;

        status = afatfs_fileGetNextCluster(file, file->cursorCluster, &nextCluster);

        if (status == AFATFS_OPERATION_SUCCESS) {
            // Seek to the beginning of the next cluster
            uint32_t bytesToSeek = clusterSizeBytes - offsetInCluster;

            file->cursorPreviousCluster = file->cursorCluster;
            file->cursorCluster = nextCluster;

            file->cursorOffset += bytesToSeek;
            opState->seekOffset -= bytesToSeek;
            offsetInCluster = 0;
        } else {
            // Try again later
            return false;
        }
    }

    // If we didn't already hit the end of the file, add any remaining offset needed inside the cluster
    if (!afatfs_isEndOfAllocatedFile(file)) {
        file->cursorOffset += opState->seekOffset;
    }

    afatfs_fileUpdateFilesize(file); // TODO do we need this?

    file->operation.operation = AFATFS_FILE_OPERATION_NONE;

    if (opState->callback) {
        opState->callback(file);
    }

    return true;
}

/**
 * Find a sector in the cache which corresponds to the given physical sector index, or NULL if the sector isn't
 * cached. Note that the cached sector could be in any state including completely empty.
 */
static afatfsCacheBlockDescriptor_t* afatfs_findCacheSector(uint32_t sectorIndex)
{
    for (int i = 0; i < AFATFS_NUM_CACHE_SECTORS; i++) {
        if (afatfs.cacheDescriptor[i].sectorIndex == sectorIndex) {
            return &afatfs.cacheDescriptor[i];
        }
    }

    return NULL;
}

static void afatfs_fcloseContinue(afatfsFilePtr_t file)
{
    afatfsCacheBlockDescriptor_t *descriptor;
    afatfsCloseFile_t *opState = &file->operation.state.closeFile;

    /*
     * Directories don't update their parent directory entries over time, because their fileSize field in the directory
     * never changes (when we add the first cluster to the directory we save the directory entry at that point and it
     * doesn't change afterwards). So don't bother trying to save their directory entries during fclose().
     *
     * Also if we only opened the file for read then we didn't change the directory entry either.
     */
    if (file->type != AFATFS_FILE_TYPE_DIRECTORY && file->type != AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY
            && (file->mode & (AFATFS_FILE_MODE_APPEND | AFATFS_FILE_MODE_WRITE)) != 0) {
        if (afatfs_saveDirectoryEntry(file, AFATFS_SAVE_DIRECTORY_FOR_CLOSE) != AFATFS_OPERATION_SUCCESS) {
            return;
        }
    }

    // Release our reservation on the directory cache if needed
    if ((file->mode & AFATFS_FILE_MODE_RETAIN_DIRECTORY) != 0) {
        descriptor = afatfs_findCacheSector(file->directoryEntryPos.sectorNumberPhysical);

        if (descriptor) {
            descriptor->retainCount = MAX((int) descriptor->retainCount - 1, 0);
        }
    }

    // Release locks on the sector at the file cursor position
    afatfs_fileUnlockCacheSector(file);

#ifdef AFATFS_USE_FREEFILE
    // Release our exclusive lock on the freefile if needed
    if ((file->mode & AFATFS_FILE_MODE_CONTIGUOUS) != 0) {
        afatfs_assert(afatfs.freeFile.operation.operation == AFATFS_FILE_OPERATION_LOCKED);
        afatfs.freeFile.operation.operation = AFATFS_FILE_OPERATION_NONE;
    }
#endif

    file->type = AFATFS_FILE_TYPE_NONE;
    file->operation.operation = AFATFS_FILE_OPERATION_NONE;

    if (opState->callback) {
        opState->callback();
    }
}

#ifdef AFATFS_USE_FREEFILE

/**
 * Update the FAT to fill the contiguous series of clusters with indexes [*startCluster...endCluster) with the
 * specified pattern.
 *
 * AFATFS_FAT_PATTERN_TERMINATED_CHAIN - Chain the clusters together in linear sequence and terminate the final cluster
 * AFATFS_FAT_PATTERN_CHAIN            - Chain the clusters together without terminating the final entry
 * AFATFS_FAT_PATTERN_FREE             - Mark the clusters as free space
 *
 * Returns -
 *     AFATFS_OPERATION_SUCCESS        - When the entire chain has been written
 *     AFATFS_OPERATION_IN_PROGRESS    - Call again later with the updated *startCluster value in order to resume writing.
 */
static afatfsOperationStatus_e afatfs_FATFillWithPattern(afatfsFATPattern_e pattern, uint32_t *startCluster, uint32_t endCluster)
{
    afatfsFATSector_t sector;
    uint32_t fatSectorIndex, firstEntryIndex, fatPhysicalSector;
    uint8_t fatEntrySize;
    uint32_t nextCluster;
    afatfsOperationStatus_e result;
    uint32_t eraseSectorCount;

    // Find the position of the initial cluster to begin our fill
    afatfs_getFATPositionForCluster(*startCluster, &fatSectorIndex, &firstEntryIndex);

    fatPhysicalSector = afatfs_fatSectorToPhysical(0, fatSectorIndex);

    // How many consecutive FAT sectors will we be overwriting?
    eraseSectorCount = (endCluster - *startCluster + firstEntryIndex + afatfs_fatEntriesPerSector() - 1) / afatfs_fatEntriesPerSector();

    while (*startCluster < endCluster) {
        // The last entry we will fill inside this sector (exclusive):
        uint32_t lastEntryIndex = MIN(firstEntryIndex + (endCluster - *startCluster), afatfs_fatEntriesPerSector());

        uint8_t cacheFlags = AFATFS_CACHE_WRITE | AFATFS_CACHE_DISCARDABLE;

        if (firstEntryIndex > 0 || lastEntryIndex < afatfs_fatEntriesPerSector()) {
            // We're not overwriting the entire FAT sector so we must read the existing contents
            cacheFlags |= AFATFS_CACHE_READ;
        }

        result = afatfs_cacheSector(fatPhysicalSector, &sector.bytes, cacheFlags, eraseSectorCount);

        if (result != AFATFS_OPERATION_SUCCESS) {
            return result;
        }

#ifdef AFATFS_DEBUG_VERBOSE
        if (pattern == AFATFS_FAT_PATTERN_FREE) {
            fprintf(stderr, "Marking cluster %u to %u as free in FAT sector %u...\n", *startCluster, endCluster, fatPhysicalSector);
        } else {
            fprintf(stderr, "Writing FAT chain from cluster %u to %u in FAT sector %u...\n", *startCluster, endCluster, fatPhysicalSector);
        }
#endif

        switch (pattern) {
            case AFATFS_FAT_PATTERN_TERMINATED_CHAIN:
            case AFATFS_FAT_PATTERN_UNTERMINATED_CHAIN:
                nextCluster = *startCluster + 1;
                // Write all the "next cluster" pointers
                if (afatfs.filesystemType == FAT_FILESYSTEM_TYPE_FAT16) {
                    for (uint32_t i = firstEntryIndex; i < lastEntryIndex; i++, nextCluster++) {
                        sector.fat16[i] = nextCluster;
                    }
                } else {
                    for (uint32_t i = firstEntryIndex; i < lastEntryIndex; i++, nextCluster++) {
                        sector.fat32[i] = nextCluster;
                    }
                }

                *startCluster += lastEntryIndex - firstEntryIndex;

                if (pattern == AFATFS_FAT_PATTERN_TERMINATED_CHAIN && *startCluster == endCluster) {
                    // We completed the chain! Overwrite the last entry we wrote with the terminator for the end of the chain
                    if (afatfs.filesystemType == FAT_FILESYSTEM_TYPE_FAT16) {
                        sector.fat16[lastEntryIndex - 1] = 0xFFFF;
                    } else {
                        sector.fat32[lastEntryIndex - 1] = 0xFFFFFFFF;
                    }
                    break;
                }
            break;
            case AFATFS_FAT_PATTERN_FREE:
                fatEntrySize = afatfs.filesystemType == FAT_FILESYSTEM_TYPE_FAT16 ? sizeof(uint16_t) : sizeof(uint32_t);

                memset(sector.bytes + firstEntryIndex * fatEntrySize, 0, (lastEntryIndex - firstEntryIndex) * fatEntrySize);

                *startCluster += lastEntryIndex - firstEntryIndex;
            break;
        }

        fatPhysicalSector++;
        eraseSectorCount--;
        firstEntryIndex = 0;
    }

    return AFATFS_OPERATION_SUCCESS;
}

#endif

/**
 * Continue the file truncation.
 *
 * When truncation finally succeeds or fails, the current operation is cleared on the file (if the current operation
 * was a truncate), then the truncate operation's callback is called. This allows the truncation to be called as a
 * sub-operation without it clearing the parent file operation.
 */
static afatfsOperationStatus_e afatfs_ftruncateContinue(afatfsFilePtr_t file, bool markDeleted)
{
    afatfsTruncateFile_t *opState = &file->operation.state.truncateFile;
    afatfsOperationStatus_e status;

#ifdef AFATFS_USE_FREEFILE
    uint32_t oldFreeFileStart, freeFileGrow;
#endif

    doMore:

    switch (opState->phase) {
        case AFATFS_TRUNCATE_FILE_UPDATE_DIRECTORY:
            status = afatfs_saveDirectoryEntry(file, markDeleted ? AFATFS_SAVE_DIRECTORY_DELETED : AFATFS_SAVE_DIRECTORY_NORMAL);

            if (status == AFATFS_OPERATION_SUCCESS) {
#ifdef AFATFS_USE_FREEFILE
                if (opState->endCluster) {
                    opState->phase = AFATFS_TRUNCATE_FILE_ERASE_FAT_CHAIN_CONTIGUOUS;
                } else
#endif
                {
                    opState->phase = AFATFS_TRUNCATE_FILE_ERASE_FAT_CHAIN_NORMAL;
                }
                goto doMore;
            }
        break;
#ifdef AFATFS_USE_FREEFILE
        case AFATFS_TRUNCATE_FILE_ERASE_FAT_CHAIN_CONTIGUOUS:
            // Prepare the clusters to be added back on to the beginning of the freefile
            status = afatfs_FATFillWithPattern(AFATFS_FAT_PATTERN_UNTERMINATED_CHAIN, &opState->currentCluster, opState->endCluster);

            if (status == AFATFS_OPERATION_SUCCESS) {
                opState->phase = AFATFS_TRUNCATE_FILE_PREPEND_TO_FREEFILE;
                goto doMore;
            }
        break;
        case AFATFS_TRUNCATE_FILE_PREPEND_TO_FREEFILE:
            // Note, it's okay to run this code several times:
            oldFreeFileStart = afatfs.freeFile.firstCluster;

            afatfs.freeFile.firstCluster = opState->startCluster;

            freeFileGrow = (oldFreeFileStart - opState->startCluster) * afatfs_clusterSize();

            afatfs.freeFile.logicalSize += freeFileGrow;
            afatfs.freeFile.physicalSize += freeFileGrow;

            status = afatfs_saveDirectoryEntry(&afatfs.freeFile, AFATFS_SAVE_DIRECTORY_NORMAL);
            if (status == AFATFS_OPERATION_SUCCESS) {
                opState->phase = AFATFS_TRUNCATE_FILE_SUCCESS;
                goto doMore;
            }
        break;
#endif
        case AFATFS_TRUNCATE_FILE_ERASE_FAT_CHAIN_NORMAL:
            while (!afatfs_FATIsEndOfChainMarker(opState->currentCluster)) {
                uint32_t nextCluster;

                status = afatfs_FATGetNextCluster(0, opState->currentCluster, &nextCluster);

                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }

                status = afatfs_FATSetNextCluster(opState->currentCluster, 0);

                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }

                opState->currentCluster = nextCluster;

                // Searches for unallocated regular clusters should be told about this free cluster now
                afatfs.lastClusterAllocated = MIN(afatfs.lastClusterAllocated, opState->currentCluster - 1);
            }

            opState->phase = AFATFS_TRUNCATE_FILE_SUCCESS;
            goto doMore;
        break;
        case AFATFS_TRUNCATE_FILE_SUCCESS:
            if (file->operation.operation == AFATFS_FILE_OPERATION_TRUNCATE) {
                file->operation.operation = AFATFS_FILE_OPERATION_NONE;
            }

            if (opState->callback) {
                opState->callback(file);
            }

            return AFATFS_OPERATION_SUCCESS;
        break;
    }

    if (status == AFATFS_OPERATION_FAILURE && file->operation.operation == AFATFS_FILE_OPERATION_TRUNCATE) {
        file->operation.operation = AFATFS_FILE_OPERATION_NONE;
    }

    return status;
}

/**
 * Returns true if an operation was successfully queued to close the file and destroy the file handle. If the file is
 * currently busy, false is returned and you should retry later.
 *
 * If provided, the callback will be called after the operation completes (pass NULL for no callback).
 *
 * If this function returns true, you should not make any further calls to the file (as the handle might be reused for a
 * new file).
 */
bool afatfs_fclose(afatfsFilePtr_t file, afatfsCallback_t callback)
{
    if (!file || file->type == AFATFS_FILE_TYPE_NONE) {
        return true;
    } else if (afatfs_fileIsBusy(file)) {
        return false;
    } else {
        afatfs_fileUpdateFilesize(file);

        file->operation.operation = AFATFS_FILE_OPERATION_CLOSE;
        file->operation.state.closeFile.callback = callback;
        afatfs_fcloseContinue(file);
        return true;
    }
}

static void afatfs_funlinkContinue(afatfsFilePtr_t file)
{
    afatfsUnlinkFile_t *opState = &file->operation.state.unlinkFile;
    afatfsOperationStatus_e status;

    status = afatfs_ftruncateContinue(file, true);

    if (status == AFATFS_OPERATION_SUCCESS) {
        // Once the truncation is completed, we can close the file handle
        file->operation.operation = AFATFS_FILE_OPERATION_NONE;
        afatfs_fclose(file, opState->callback);
    }
}

/**
 * Size of a AFATFS supercluster in bytes
 */
static uint32_t afatfs_superClusterSize()
{
    return afatfs_fatEntriesPerSector() * afatfs_clusterSize();
}

/**
 * Continue to attempt to add a supercluster to the end of the given file.
 *
 * If the file operation was set to AFATFS_FILE_OPERATION_APPEND_SUPERCLUSTER and the operation completes, the file's
 * operation is cleared.
 *
 * Returns:
 *     AFATFS_OPERATION_SUCCESS     - On completion
 *     AFATFS_OPERATION_IN_PROGRESS - Operation still in progress
 */
static afatfsOperationStatus_e afatfs_appendSuperclusterContinue(afatfsFile_t *file)
{
    afatfsAppendSupercluster_t *opState = &file->operation.state.appendSupercluster;

    afatfsOperationStatus_e status;

    doMore:
    switch (opState->phase) {
        case AFATFS_APPEND_SUPERCLUSTER_PHASE_INIT:
            // Our file steals the first cluster of the freefile

            // We can go ahead and write to that space before the FAT and directory are updated
            file->cursorCluster = afatfs.freeFile.firstCluster;
            file->physicalSize += afatfs_superClusterSize();

            /* Remove the first supercluster from the freefile
             *
             * Even if the freefile becomes empty, we still don't set its first cluster to zero. This is so that
             * afatfs_fileGetNextCluster() can tell where a contiguous file ends (at the start of the freefile).
             *
             * Note that normally the freefile can't become empty because it is allocated as a non-integer number
             * of superclusters to avoid precisely this situation.
             */
            afatfs.freeFile.firstCluster += afatfs_fatEntriesPerSector();
            afatfs.freeFile.logicalSize -= afatfs_superClusterSize();
            afatfs.freeFile.physicalSize -= afatfs_superClusterSize();

            // The new supercluster needs to have its clusters chained contiguously and marked with a terminator at the end
            opState->fatRewriteStartCluster = file->cursorCluster;
            opState->fatRewriteEndCluster = opState->fatRewriteStartCluster + afatfs_fatEntriesPerSector();

            if (opState->previousCluster == 0) {
                // This is the new first cluster in the file so we need to update the directory entry
                file->firstCluster = file->cursorCluster;
            } else {
                /*
                 * We also need to update the FAT of the supercluster that used to end the file so that it no longer
                 * terminates there
                 */
                opState->fatRewriteStartCluster -= afatfs_fatEntriesPerSector();
            }

            opState->phase = AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FREEFILE_DIRECTORY;
            goto doMore;
        break;
        case AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FREEFILE_DIRECTORY:
            // First update the freefile's directory entry to remove the first supercluster so we don't risk cross-linking the file
            status = afatfs_saveDirectoryEntry(&afatfs.freeFile, AFATFS_SAVE_DIRECTORY_NORMAL);

            if (status == AFATFS_OPERATION_SUCCESS) {
                opState->phase = AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FAT;
                goto doMore;
            }
        break;
        case AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FAT:
            status = afatfs_FATFillWithPattern(AFATFS_FAT_PATTERN_TERMINATED_CHAIN, &opState->fatRewriteStartCluster, opState->fatRewriteEndCluster);

            if (status == AFATFS_OPERATION_SUCCESS) {
                opState->phase = AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FILE_DIRECTORY;
                goto doMore;
            }
        break;
        case AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FILE_DIRECTORY:
            // Update the fileSize/firstCluster in the directory entry for the file
            status = afatfs_saveDirectoryEntry(file, AFATFS_SAVE_DIRECTORY_NORMAL);
        break;
    }

    if ((status == AFATFS_OPERATION_FAILURE || status == AFATFS_OPERATION_SUCCESS) && file->operation.operation == AFATFS_FILE_OPERATION_APPEND_SUPERCLUSTER) {
        file->operation.operation = AFATFS_FILE_OPERATION_NONE;
    }

    return status;
}

/**
 * Continue any queued operations on the given file.
 */
static void afatfs_fileOperationContinue(afatfsFile_t *file)
{
    if (file->type == AFATFS_FILE_TYPE_NONE)
        return;

//	printf("operation: %u, %d\r\n", file->operation.operation, __LINE__);
	
    switch (file->operation.operation) {
        case AFATFS_FILE_OPERATION_CREATE_FILE:
            afatfs_createFileContinue(file);
        break;
        case AFATFS_FILE_OPERATION_SEEK:
            afatfs_fseekInternalContinue(file);
        break;
        case AFATFS_FILE_OPERATION_CLOSE:
            afatfs_fcloseContinue(file);
        break;
        case AFATFS_FILE_OPERATION_UNLINK:
             afatfs_funlinkContinue(file);
        break;
        case AFATFS_FILE_OPERATION_TRUNCATE:
            afatfs_ftruncateContinue(file, false);
        break;
#ifdef AFATFS_USE_FREEFILE
        case AFATFS_FILE_OPERATION_APPEND_SUPERCLUSTER:
            afatfs_appendSuperclusterContinue(file);
        break;
        case AFATFS_FILE_OPERATION_LOCKED:
            ;
        break;
#endif
        case AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER:
            afatfs_appendRegularFreeClusterContinue(file);
        break;
        case AFATFS_FILE_OPERATION_EXTEND_SUBDIRECTORY:
            afatfs_extendSubdirectoryContinue(file);
        break;
        case AFATFS_FILE_OPERATION_NONE:
            ;
        break;
    }
}

/**
 * Call to continue the search for the largest contiguous block of free space on the device.
 *
 * Returns:
 *     AFATFS_OPERATION_IN_PROGRESS - SD card is busy, call again later to resume
 *     AFATFS_OPERATION_SUCCESS - When the search has finished and afatfs.initState.freeSpaceSearch has been updated with the details of the best gap.
 *     AFATFS_OPERATION_FAILURE - When a read error occured
 */
static afatfsOperationStatus_e afatfs_findLargestContiguousFreeBlockContinue()
{
    afatfsFreeSpaceSearch_t *opState = &afatfs.initState.freeSpaceSearch;
    uint32_t fatEntriesPerSector = afatfs_fatEntriesPerSector();
    uint32_t candidateGapLength, searchLimit;
    afatfsFindClusterStatus_e searchStatus;

    while (1) {
        switch (opState->phase) {
            case AFATFS_FREE_SPACE_SEARCH_PHASE_FIND_HOLE:
                // Find the first free cluster
                switch (afatfs_findClusterWithCondition(CLUSTER_SEARCH_FREE_AT_BEGINNING_OF_FAT_SECTOR, &opState->candidateStart, afatfs.numClusters + FAT_SMALLEST_LEGAL_CLUSTER_NUMBER)) {
                    case AFATFS_FIND_CLUSTER_FOUND:
                        opState->candidateEnd = opState->candidateStart + 1;
                        opState->phase = AFATFS_FREE_SPACE_SEARCH_PHASE_GROW_HOLE;
                    break;

                    case AFATFS_FIND_CLUSTER_FATAL:
                        // Some sort of read error occured
                        return AFATFS_OPERATION_FAILURE;

                    case AFATFS_FIND_CLUSTER_NOT_FOUND:
                        // We finished searching the volume (didn't find any more holes to examine)
                        return AFATFS_OPERATION_SUCCESS;

                    case AFATFS_FIND_CLUSTER_IN_PROGRESS:
                        return AFATFS_OPERATION_IN_PROGRESS;
                }
            break;
            case AFATFS_FREE_SPACE_SEARCH_PHASE_GROW_HOLE:
                // Find the first used cluster after the beginning of the hole (that signals the end of the hole)

                // Don't search beyond the end of the volume, or such that the freefile size would exceed the max filesize
                searchLimit = MIN((uint64_t) opState->candidateStart + FAT_MAXIMUM_FILESIZE / afatfs_clusterSize(), afatfs.numClusters + FAT_SMALLEST_LEGAL_CLUSTER_NUMBER);

                searchStatus = afatfs_findClusterWithCondition(CLUSTER_SEARCH_OCCUPIED, &opState->candidateEnd, searchLimit);

                switch (searchStatus) {
                    case AFATFS_FIND_CLUSTER_NOT_FOUND:
                    case AFATFS_FIND_CLUSTER_FOUND:
                        // Either we found a used sector, or the search reached the end of the volume or exceeded the max filesize
                        candidateGapLength = opState->candidateEnd - opState->candidateStart;

                        if (candidateGapLength > opState->bestGapLength) {
                            opState->bestGapStart = opState->candidateStart;
                            opState->bestGapLength = candidateGapLength;
                        }

                        if (searchStatus == AFATFS_FIND_CLUSTER_NOT_FOUND) {
                            // This is the best hole there can be
                            return AFATFS_OPERATION_SUCCESS;
                        } else {
                            // Start a new search for a new hole
                            opState->candidateStart = roundUpTo(opState->candidateEnd + 1, fatEntriesPerSector);
                            opState->phase = AFATFS_FREE_SPACE_SEARCH_PHASE_FIND_HOLE;
                        }
                    break;

                    case AFATFS_FIND_CLUSTER_FATAL:
                        // Some sort of read error occured
                        return AFATFS_OPERATION_FAILURE;

                    case AFATFS_FIND_CLUSTER_IN_PROGRESS:
                        return AFATFS_OPERATION_IN_PROGRESS;
                }
            break;
        }
    }
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
//			printf("afatfs_cacheSector(0, &sector, AFATFS_CACHE_READ | AFATFS_CACHE_DISCARDABLE, 0): %d\r\n", afatfs_cacheSector(0, &sector, AFATFS_CACHE_READ | AFATFS_CACHE_DISCARDABLE, 0));
			if (afatfs_cacheSector(0, &sector, AFATFS_CACHE_READ | AFATFS_CACHE_DISCARDABLE, 0) == AFATFS_OPERATION_SUCCESS) {
				// printf("sector[AFATFS_SECTOR_SIZE - 2]: 0x%x, %s, %s, %d\r\n", sector[AFATFS_SECTOR_SIZE - 2], __FILE__, __FUNCTION__, __LINE__);	// 0x55
				// printf("sector[AFATFS_SECTOR_SIZE - 1]: 0x%x, %s, %s, %d\r\n", sector[AFATFS_SECTOR_SIZE - 1], __FILE__, __FUNCTION__, __LINE__);	// 0xAA
				if (afatfs_parseMBR(sector)) {
//					/* afatfs_parseMBR() successful */
//					printf("%s, %d\r\n", __FUNCTION__, __LINE__);
					afatfs.initPhase = AFATFS_INITIALISATION_READ_VOLUME_ID;
					goto doMore;
				} else {
					/* afatfs_parseMBR failed */
					afatfs.lastError = AFATFS_ERROR_BAD_MBR;
					afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_FATAL;
				}
			}
			break;
		
		case AFATFS_INITIALISATION_READ_VOLUME_ID:
//			printf("afatfs.partitionStartSector: %u, %s, %d\r\n", afatfs.partitionStartSector, __FUNCTION__, __LINE__);
			/* afatfs.partitionStartSector = 8192 for Sandisk MicroSD card 16GB */
			if (afatfs_cacheSector(afatfs.partitionStartSector, &sector, AFATFS_CACHE_READ | AFATFS_CACHE_DISCARDABLE, 0) == AFATFS_OPERATION_SUCCESS) {
//				printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
				if (afatfs_parseVolumeID(sector)) {
					/**
					 * If parse volume ID successful
					 * open the root directory
					 */
					afatfs_chdir(NULL);		// Initialise afatfsFile_t structure
					afatfs.initPhase++;		// increment initPhase to 2, so it should be state AFATFS_INITIALISATION_FREEFILE_CREATE as AFATFS_USE_FREEFILE is defined
				} else {
					/* afatfs_parseVolumeID failed  */
					afatfs.lastError = AFATFS_ERROR_BAD_FILESYSTEM_HEADER;
					afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_FATAL;
				}
			}
			break;
		
#ifdef AFATFS_USE_FREEFILE
		case AFATFS_INITIALISATION_FREEFILE_CREATE:
#if 1			
//			printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
			afatfs.initPhase = AFATFS_INITIALISATION_FREEFILE_CREATING;
		
			afatfs_createFile(&afatfs.freeFile, AFATFS_FREESPACE_FILENAME, FAT_FILE_ATTRIBUTE_SYSTEM | FAT_FILE_ATTRIBUTE_READ_ONLY,
					AFATFS_FILE_MODE_CREATE | AFATFS_FILE_MODE_RETAIN_DIRECTORY, afatfs_freeFileCreated);
#endif
			break;
		
		case AFATFS_INITIALISATION_FREEFILE_CREATING:
#if 0
//			printf("afatfs.initPhase: %u, %d\r\n", afatfs.initPhase, __LINE__);
			afatfs_fileOperationContinue(&afatfs.freeFile);
//			printf("afatfs.initPhase: %u, %d\r\n", afatfs.initPhase, __LINE__);
#endif	
			break;
		
		case AFATFS_INITIALISATION_FREEFILE_FAT_SEARCH:
#if 0
			if (afatfs_findLargestContiguousFreeBlockContinue() == AFATFS_OPERATION_SUCCESS) {
                // If the freefile ends up being empty then we only have to save its directory entry:
                afatfs.initPhase = AFATFS_INITIALISATION_FREEFILE_SAVE_DIR_ENTRY;

                if (afatfs.initState.freeSpaceSearch.bestGapLength > AFATFS_FREEFILE_LEAVE_CLUSTERS + 1) {
                    afatfs.initState.freeSpaceSearch.bestGapLength -= AFATFS_FREEFILE_LEAVE_CLUSTERS;

                    /* So that the freefile never becomes empty, we want it to occupy a non-integer number of
                     * superclusters. So its size mod the number of clusters in a supercluster should be 1.
                     */
                    afatfs.initState.freeSpaceSearch.bestGapLength = ((afatfs.initState.freeSpaceSearch.bestGapLength - 1) & ~(afatfs_fatEntriesPerSector() - 1)) + 1;

                    // Anything useful left over?
                    if (afatfs.initState.freeSpaceSearch.bestGapLength > afatfs_fatEntriesPerSector()) {
                        uint32_t startCluster = afatfs.initState.freeSpaceSearch.bestGapStart;
                        // Points 1-beyond the final cluster of the freefile:
                        uint32_t endCluster = afatfs.initState.freeSpaceSearch.bestGapStart + afatfs.initState.freeSpaceSearch.bestGapLength;

                        afatfs_assert(endCluster < afatfs.numClusters + FAT_SMALLEST_LEGAL_CLUSTER_NUMBER);

                        afatfs.initState.freeSpaceFAT.startCluster = startCluster;
                        afatfs.initState.freeSpaceFAT.endCluster = endCluster;

                        afatfs.freeFile.firstCluster = startCluster;

                        afatfs.freeFile.logicalSize = afatfs.initState.freeSpaceSearch.bestGapLength * afatfs_clusterSize();
                        afatfs.freeFile.physicalSize = afatfs.freeFile.logicalSize;

                        // We can write the FAT table for the freefile now
                        afatfs.initPhase = AFATFS_INITIALISATION_FREEFILE_UPDATE_FAT;
                    } // Else the freefile's FAT chain and filesize remains the default (empty)
                }

                goto doMore;
            }
#endif	
			break;
		
		case AFATFS_INITIALISATION_FREEFILE_UPDATE_FAT:
#if 0
			status = afatfs_FATFillWithPattern(AFATFS_FAT_PATTERN_TERMINATED_CHAIN, &afatfs.initState.freeSpaceFAT.startCluster, afatfs.initState.freeSpaceFAT.endCluster);

            if (status == AFATFS_OPERATION_SUCCESS) {
                afatfs.initPhase = AFATFS_INITIALISATION_FREEFILE_SAVE_DIR_ENTRY;

                goto doMore;
            } else if (status == AFATFS_OPERATION_FAILURE) {
                afatfs.lastError = AFATFS_ERROR_GENERIC;
                afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_FATAL;
            }
#endif		
			break;
		
		case AFATFS_INITIALISATION_FREEFILE_SAVE_DIR_ENTRY:
#if 0
			status = afatfs_saveDirectoryEntry(&afatfs.freeFile, AFATFS_SAVE_DIRECTORY_NORMAL);
			printf("status: %u, %s, %s, %d\r\n", status, __FILE__, __FUNCTION__, __LINE__);

            if (status == AFATFS_OPERATION_SUCCESS) {
                afatfs.initPhase++;
                goto doMore;
            } else if (status == AFATFS_OPERATION_FAILURE) {
//				printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
                afatfs.lastError = AFATFS_ERROR_GENERIC;
                afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_FATAL;
            }
#endif	
			break;
#endif
		
#ifdef AFATFS_USE_INTROSPECTIVE_LOGGING
		case AFATFS_INITIALISATION_INTROSPEC_LOG_CREATE:
			break;
		
		case AFATFS_INITIALISATION_INTROSPEC_LOG_CREATING:
			break;
#endif
		
		case AFATFS_INITIALISATION_DONE:
			printf("%s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
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
		afatfs_flush();
		
//		printf("afatfs.filesystemState: %d, %s, %s, %d\r\n", afatfs.filesystemState, __FILE__, __FUNCTION__, __LINE__);
		
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

/* +------------------------------------------------------------------------------------------------------------------------------- */
/* +------------------------------------------------------------------------------------------------------------------------------- */
/* +--------------------------------------------------------- FUNCTIONS ----------------------------------------------------------+ */
/* +------------------------------------------------------------------------------------------------------------------------------- */
/* +------------------------------------------------------------------------------------------------------------------------------- */
