
#include <ctype.h>
#include "fat_standard.h"

bool fat_isDirectoryEntryTerminator(fatDirectoryEntry_t *entry)
{
	return entry->filename[0] == 0x00;
}

bool fat_isDirectoryEntryEmpty(fatDirectoryEntry_t *entry)
{
	return (unsigned char) entry->filename[0] == FAT_DELETED_FILE_MARKER;
}

/**
 * FAT32 cluster numbers are really only 28 bits, and the top 4 bits must be left alone and not treated as part of the 
 * cluster number (so various FAT drivers can use these bits for their own purposes, or they can be used in later extensions)
 */
uint32_t fat32_decodeClusterNumber(uint32_t clusterNumber)
{
	return clusterNumber & 0x0FFFFFFF;
}

bool fat16_isEndOfChainMarker(uint16_t clusterNumber)
{
	return clusterNumber >= 0xFFF8;
}

/* Pass the cluster number after fat32_decodeClusterNumber */
bool fat32_isEndOfChainMarker(uint32_t clusterNumber)
{
	return clusterNumber >= 0x0FFFFFF8;
}

/**
 * Convert the given "prefix.ext" style filename to the FAT format to be stored on disk.
 *
 * fatFilename must point to a buffer which is FAT_FILENAME_LENGTH (11) bytes long. The buffer is not null-terminated.
 */
void fat_convertFilenameToFATStyle(const char *filename, uint8_t *fatFilename)
{
	for (int i = 0; i < 8; i++) {
		if (*filename == '\0' || *filename == '.') {
			*fatFilename = ' ';
		} else {
			*fatFilename = toupper((unsigned char)*filename);
			filename++;
		}
		fatFilename++;
	}
	
	if (*filename == '.') {
		filename++;
	}
	
	for (int i = 0; i < 3; i++) {
		if (*filename == '\0') {
			*fatFilename = ' ';
		} else {
			*fatFilename = toupper((unsigned char)*filename);
			filename++;
		}
		
		fatFilename++;
	}
}
