
#include "fat_standard.h"

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
