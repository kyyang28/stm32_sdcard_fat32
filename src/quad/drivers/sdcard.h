#ifndef __SDCARD_H
#define __SDCARD_H

#include <stdint.h>
#include <stdbool.h>

typedef struct sdcardConfig_s {
	uint8_t useDma;
}sdcardConfig_t;

typedef struct sdcardMetaData_s {
	uint8_t manufacturerID;
	uint16_t oemID;
	
	char productName[5];
	
	uint8_t productRevisionMajor;
	uint8_t productRevisionMinor;
	uint32_t productSerial;
	
	uint16_t productionYear;
	uint8_t productionMonth;
	
	uint32_t numBlocks;				/* Card capacity in 512-byte blocks */
}sdcardMetaData_t;

typedef enum {
	SDCARD_BLOCK_OPERATION_READ,
	SDCARD_BLOCK_OPERATION_WRITE,
	SDCARD_BLOCK_OPERATION_ERASE
}sdcardBlockOperation_e;

typedef enum {
	SDCARD_OPERATION_IN_PROGRESS,
	SDCARD_OPERATION_BUSY,
	SDCARD_OPERATION_SUCCESS,
	SDCARD_OPERATION_FAILURE
}sdcardOperationStatus_e;

typedef void (*sdcard_operationCompleteCallback_c)(sdcardBlockOperation_e operation, uint32_t blockIndex, uint8_t *buffer, uint32_t callbackData);

bool sdcard_isInserted(void);
void sdcardInsertionDetectInit(void);
void sdcard_init(bool useDMA);

bool sdcard_poll(void);

#endif	// __SDCARD_H
