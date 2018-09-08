
#include <stdio.h>			// debugging purposes
#include "platform.h"

#ifdef USE_SDCARD

#include "io.h"
#include "bus_spi.h"
#include "system.h"			// delay(), delayMicroseconds(), millis()

#include "sdcard.h"
#include "sdcard_standard.h"

#include "dma.h"

typedef enum {
	/* In these states, SD card runs at the initialisation 400kHz clockspeed */
	SDCARD_STATE_NOT_PRESENT = 0,
	SDCARD_STATE_RESET,											// 1
	SDCARD_STATE_CARD_INIT_IN_PROGRESS,							// 2
	SDCARD_STATE_INITIALISATION_RECEIVE_CID,					// 3
	
	/* In these states, SD card runs at full clock speed */
	SDCARD_STATE_READY,											// 4
	SDCARD_STATE_READING,										// 5
	SDCARD_STATE_SENDING_WRITE,									// 6
	SDCARD_STATE_WAITING_FOR_WRITE,								// 7
	SDCARD_STATE_WRITING_MULTIPLE_BLOCKS,						// 8
	SDCARD_STATE_STOPPING_MULTIPLE_BLOCK_WRITE					// 9
}sdcardState_e;

typedef enum {
	SDCARD_RECEIVE_SUCCESS,
	SDCARD_RECEIVE_BLOCK_IN_PROGRESS,
	SDCARD_RECEIVE_ERROR
}sdcardReceiveBlockStatus_e;

typedef struct sdcard_t {
	struct {
		uint8_t *buffer;
		uint32_t blockIndex;
		uint8_t chunkIndex;
		
		sdcard_operationCompleteCallback_c callback;
		uint32_t callbackData;
	}pendingOperation;
	
	uint32_t operationStartTime;
	
	uint8_t failureCount;
	
	uint8_t version;
	
	bool highCapacity;
	
	uint32_t multiWriteNextBlock;
	uint32_t multiWriteBlocksRemain;
	
	sdcardState_e state;
	
	sdcardMetaData_t metaData;
	
	sdcardCSD_t csd;
}sdcard_t;

static sdcard_t sdcard;

#ifdef SDCARD_DMA_CHANNEL_TX
	static bool useDMAForTx;
#else
	/* DMA channel is not available so we can hard-code this to allow the non-DMA paths to be stripped by optimisation */
	static const bool useDMAForTx = false;
#endif

#ifdef SDCARD_DETECT_PIN
static IO_t sdcardDetectPin = IO_NONE;
#endif

static IO_t sdcardCsPin = IO_NONE;

#define SET_CS_HIGH									IOHi(sdcardCsPin)
#define SET_CS_LOW									IOLo(sdcardCsPin)

#define SDCARD_INIT_NUM_DUMMY_BYTES					10
#define SDCARD_MAXIMUM_BYTE_DELAY_FOR_CMD_REPLY		8

/* Chosen so that CMD8 will have the same CRC as CMD0 */
#define SDCARD_IF_COND_CHECK_PATTERN				0xAB

#define SDCARD_MAX_CONSECUTIVE_FAILURES 			8

/**
 * Detect if a SD card is physically present in the memory slot.
 * 
 * @return
 *		result = true, card is present (IORead(sdcardDetectPin) returns 0), sdcardDetectPin = PC14
 *		result = false, card is not present (IORead(sdcardDetectPin) returns 1), sdcardDetectPin = PC14
 */
bool sdcard_isInserted(void)
{
	bool result = true;
	
#ifdef SDCARD_DETECT_PIN
	/* According to <<Micro-SD-Storage-Board-Schematic.pdf>>,
	 * when card is not present, card detect pin is connected to 3V3, 
	 * so IORead(sdcardDetectPin) is HIGH level (sdcardDetectPin is CARD DETECT PIN (PC14))
	 */
//	printf("IORead(sdcardDetectPin): %d, %s, %d\r\n", IORead(sdcardDetectPin), __FUNCTION__, __LINE__);
	result = IORead(sdcardDetectPin) != 0;
//	printf("result: %d, %s, %d\r\n", result, __FUNCTION__, __LINE__);
	
#ifdef SDCARD_DETECT_INVERTED		// for WAVESHARE SDCARD module, CD(card detect) pin is inverted
	result = !result;
#endif
#endif
	
	return result;
}

void sdcardInsertionDetectInit(void)
{
#ifdef SDCARD_DETECT_PIN			// SDCARD_DETECT_PIN = PC14 in this case
//	printf("%s, %d\r\n", __FUNCTION__, __LINE__);
	sdcardDetectPin = IOGetByTag(IO_TAG(SDCARD_DETECT_PIN));
	IOInit(sdcardDetectPin, OWNER_SDCARD_DETECT, 0);
	IOConfigGPIO(sdcardDetectPin, IOCFG_IPU);
#endif
}

/**
 * SD card initialisation process. This must be called first before any other sdcard_ routine
 */
void sdcard_init(bool useDMA)
{
//	printf("useDMA: %d, %s, %d\r\n", useDMA, __FUNCTION__, __LINE__);
#ifdef SDCARD_DMA_CHANNEL_TX
	useDMAForTx = useDMA;
	if (useDMAForTx) {
		dmaInit(dmaGetIdentifier(SDCARD_DMA_CHANNEL_TX), OWNER_SDCARD, 0);
	}
#else
	/* DMA is not available */
	(void) useDMA;
#endif
	
#ifdef SDCARD_SPI_CS_PIN
	sdcardCsPin = IOGetByTag(IO_TAG(SDCARD_SPI_CS_PIN));
	IOInit(sdcardCsPin, OWNER_SDCARD_CS, 0);
	IOConfigGPIO(sdcardCsPin, SPI_IO_CS_CFG);
#endif	// SDCARD_SPI_CS_PIN
	
	/* Maximum frequency is initialised to roughly 400Khz */
	spiSetDivisor(SDCARD_SPI_INSTANCE, SDCARD_SPI_INITIALISATION_CLOCK_DIVISOR);
	
	/* SD card requires 1ms maximum initialisation delay after power is applied to it */
	delay(1000);
	
	/* delay(1000) postpones at least 74 dummy clock cycles with CS high so the SD card can start up */
	SET_CS_HIGH;
	
	spiTransfer(SDCARD_SPI_INSTANCE, NULL, NULL, SDCARD_INIT_NUM_DUMMY_BYTES);
	
	/* Wait for the transmission to finish before we enable the SD card, so it receives the required number of cycles */
	int time = 100000;
	while (spiIsBusBusy(SDCARD_SPI_INSTANCE)) {
		if (time-- == 0) {
			sdcard.state = SDCARD_STATE_NOT_PRESENT;
			sdcard.failureCount++;
			return;
		}
	}
	
	sdcard.operationStartTime = millis();
	sdcard.state = SDCARD_STATE_RESET;			// SDCARD_STATE_RESET = 1
	sdcard.failureCount = 0;
//	printf("sdcard.state: %d, %s, %d\r\n", sdcard.state, __FUNCTION__, __LINE__);
}

static void sdcard_select(void)
{
	SET_CS_LOW;
}

static void sdcard_deselect(void)
{
	/*
	 * As per the SD-card spec, give the card 8 dummy clocks so it can finish its operation
	 * spiTransferByte(SDCARD_SPI_INSTANCE, 0xFF)
	 */
	while (spiIsBusBusy(SDCARD_SPI_INSTANCE)) {
	}
	
	SET_CS_HIGH;
}

/*
 * Returns true if the card is ready to accept read/write commands.
 */
static bool sdcard_isReady(void)
{
	return sdcard.state == SDCARD_STATE_READY || sdcard.state == SDCARD_STATE_WRITING_MULTIPLE_BLOCKS;
}

/**
 * The SD card spec requires 8 clock cycles to be sent by us on the bus after most commands so it can finish its
 * processing of that command. The easiest way for us to do this is to just wait for the bus to become idle before
 * we transmit a command, sending at least 8-bits onto the bus when we do so.
 */
static bool sdcard_waitForIdle(int maxBytesToWait)
{
	while (maxBytesToWait > 0) {
		uint8_t b = spiTransferByte(SDCARD_SPI_INSTANCE, 0xFF);
		if (b == 0xFF) {
			return true;
		}
		maxBytesToWait--;
	}
	
	return false;
}

/**
 * Wait for up to maxDelay 0xFF idle bytes to arrive from the card, returning the first non-idle byte found.
 *
 * Returns 0xFF on failure.
 */
static uint8_t sdcard_waitForNonIdleByte(int maxDelay)
{
	for (int i = 0; i < maxDelay + 1; i++) {	// + 1 so we can wait for maxDelay '0xFF' bytes before reading a response byte afterwards
		uint8_t response = spiTransferByte(SDCARD_SPI_INSTANCE, 0xFF);
		if (response != 0xFF) {
//			printf("response: %u\r\n", response);
			return response;
		}
	}
	
	/* returns 0xFF on failure */
	return 0xFF;
}

static uint8_t sdcard_sendCommand(uint8_t commandCode, uint32_t commandArgument)
{
	uint8_t command[6] = {
		0x40 | commandCode,
		commandArgument >> 24,
		commandArgument >> 16,
		commandArgument >> 8,
		commandArgument,
		0x95 /* static CRC. This CRC is valid for CMD0 with a 0 argument, and CMD8 with 0x1AB argument, which are the only commands that require a CRC */
	};
	
	/* Go ahead and send the command even if the card isn't idle if this is the reset command */
	if (!sdcard_waitForIdle(SDCARD_MAXIMUM_BYTE_DELAY_FOR_CMD_REPLY) && commandCode != SDCARD_COMMAND_GO_IDLE_STATE) {
		return 0xFF;
	}
	
	/* Send command on the CMD line */
	spiTransfer(SDCARD_SPI_INSTANCE, NULL, command, sizeof(command));
	
    /*
     * The card can take up to SDCARD_MAXIMUM_BYTE_DELAY_FOR_CMD_REPLY bytes to send the response, in the meantime
     * it'll transmit 0xFF filler bytes.
     */
	return sdcard_waitForNonIdleByte(SDCARD_MAXIMUM_BYTE_DELAY_FOR_CMD_REPLY);
}

static uint8_t sdcard_sendAppCommand(uint8_t commandCode, uint32_t commandArgument)
{
	sdcard_sendCommand(SDCARD_COMMAND_APP_CMD, 0);
	
	return sdcard_sendCommand(commandCode, commandArgument);
}

/**
 * Sends an IF_COND message to the card to check its version and validate its voltage requirements. Sets the global
 * sdCardVersion with the detected version (0, 1, or 2) and returns true if the card is compatible.
 */
static bool sdcard_validateInterfaceCondition(void)
{
	uint8_t ifCondReply[4];
	sdcard.version = 0;
	
	sdcard_select();
	
	/* (SDCARD_VOLTAGE_ACCEPTED_2_7_to_3_6 << 8) | SDCARD_IF_COND_CHECK_PATTERN = 0x1 << 8 | 0xAB = 0x100 | 0xAB = 0x1AB */
	uint8_t status = sdcard_sendCommand(SDCARD_COMMAND_SEND_IF_COND, (SDCARD_VOLTAGE_ACCEPTED_2_7_to_3_6 << 8) | SDCARD_IF_COND_CHECK_PATTERN);
	
	/* Do not deselect the card right away, because we'll want to read the rest of its reply if it's a V2 card */
	
	if (status == (SDCARD_R1_STATUS_BIT_ILLEGAL_COMMAND | SDCARD_R1_STATUS_BIT_IDLE)) {
		/* V1 card do not support this command */
		sdcard.version = 1;
	}else if (status == SDCARD_R1_STATUS_BIT_IDLE) {
		spiTransfer(SDCARD_SPI_INSTANCE, ifCondReply, NULL, sizeof(ifCondReply));
		
        /*
         * We don't bother to validate the SDCard's operating voltage range since the spec requires it to accept our
         * 3.3V, but do check that it echoed back our check pattern properly.
         */
		if (ifCondReply[3] == SDCARD_IF_COND_CHECK_PATTERN) {
			sdcard.version = 2;
		}
	}
	
	sdcard_deselect();
	
	return sdcard.version > 0;
}

/**
 * Check if the SD Card has completed its startup sequence. Must be called with sdcard.state == SDCARD_STATE_INITIALIZATION.
 *
 * Returns true if the card has finished its init process.
 */
static bool sdcard_checkInitDone(void)
{
	sdcard_select();

	uint8_t status = sdcard_sendAppCommand(SDCARD_ACOMMAND_SEND_OP_COND, sdcard.version == 2 ? 1 << 30 /* We support high capacity cards */ : 0);
	
	sdcard_deselect();
	
	return status == 0x00;
}

static bool sdcard_readOCRRegister(uint32_t *result)
{
	sdcard_select();
	
	/* Send OCR command */
	uint8_t status = sdcard_sendCommand(SDCARD_COMMAND_READ_OCR, 0);
	
	uint8_t response[4];
	
	/* Receive the contents of OCR register and store to response[4] array */
	spiTransfer(SDCARD_SPI_INSTANCE, response, NULL, sizeof(response));
	
	if (status == 0) {
		sdcard_deselect();
		
		/* 
		 * response[0] = 0xC0
		 * response[1] = 0xFF
		 * response[2] = 0x80
		 * response[3] = 0x00
		 */
//		printf("res[0], res[1], res[2], res[3]: %u, %u, %u, %u\r\n", response[0], response[1], response[2], response[3]);
		*result = (response[0] << 24) | (response[1] << 16) | (response[2] << 8) | response[3];
		
		return true;
	}else {
		sdcard_deselect();
		
		return false;
	}
}

/**
 * Handle a failure of a SD card operation by resetting the card back to its initialization phase.
 *
 * Increments the failure counter, and when the failure threshold is reached, disables the card until
 * the next call to sdcard_init().
 */
static void sdcard_reset(void)
{
	if (!sdcard_isInserted()) {
		sdcard.state = SDCARD_STATE_NOT_PRESENT;
		return;
	}
	
	if (sdcard.state >= SDCARD_STATE_READY) {
		spiSetDivisor(SDCARD_SPI_INSTANCE, SDCARD_SPI_INITIALISATION_CLOCK_DIVISOR);
	}
	
	sdcard.failureCount++;
	
	if (sdcard.failureCount >= SDCARD_MAX_CONSECUTIVE_FAILURES) {
		sdcard.state = SDCARD_STATE_NOT_PRESENT;
	}else {
		sdcard.operationStartTime = millis();
		sdcard.state = SDCARD_STATE_RESET;
	}
}

/**
 * Attempt to receive a data block from the SD card.
 *
 * Return true on success, otherwise the card has not responded yet and you should retry later.
 */
static sdcardReceiveBlockStatus_e sdcard_receiveDataBlock(uint8_t *buffer, int count)
{
//	printf("%s, %d\r\n", __FUNCTION__, __LINE__);
	uint8_t dataToken = sdcard_waitForNonIdleByte(8);
//	printf("dataToken: %u\r\n", dataToken);
	if (dataToken == 0xFF) {
//		printf("%s, %d\r\n", __FUNCTION__, __LINE__);
		return SDCARD_RECEIVE_BLOCK_IN_PROGRESS;
	}
	
	if (dataToken != SDCARD_SINGLE_BLOCK_READ_START_TOKEN) {
//		printf("%s, %d\r\n", __FUNCTION__, __LINE__);
		return SDCARD_RECEIVE_ERROR;
	}
	
	spiTransfer(SDCARD_SPI_INSTANCE, buffer, NULL, count);
	
	/* discard trailing CRC, we don't care */
	spiTransferByte(SDCARD_SPI_INSTANCE, 0xFF);
	spiTransferByte(SDCARD_SPI_INSTANCE, 0xFF);
	
	return SDCARD_RECEIVE_SUCCESS;
}

static bool sdcard_fetchCSD(void)
{
//	printf("%s, %d\r\n", __FUNCTION__, __LINE__);
	uint32_t readBlockLen, blockCount, blockCountMult;
	uint64_t capacityBytes;
	
	sdcard_select();
	
    /* The CSD command's data block should always arrive within 8 idle clock cycles (SD card spec). 
	 * This is because the information about card latency is stored in the CSD register itself, 
	 * so we can't use that yet!
     */
	bool success = sdcard_sendCommand(SDCARD_COMMAND_SEND_CSD, 0) == 0
		&& sdcard_receiveDataBlock((uint8_t *)&sdcard.csd, sizeof(sdcard.csd)) == SDCARD_RECEIVE_SUCCESS
		&& SDCARD_GET_CSD_FIELD(sdcard.csd, 1, TRAILER) == 1;
//	printf("success: %d, %s, %d\r\n", success, __FUNCTION__, __LINE__);
//	printf("size: %u\r\n", sizeof(sdcard.csd.data)/sizeof(sdcard.csd.data[0]));
//	printf("data: ");
//	for (int i = 0; i < sizeof(sdcard.csd.data)/sizeof(sdcard.csd.data[0]); i++) {
//		printf("0x%x ", sdcard.csd.data[i]);
//	}
//	printf("\r\n");
		
	if (success) {
		switch (SDCARD_GET_CSD_FIELD(sdcard.csd, 1, CSD_STRUCTURE_VER)) {
			case SDCARD_CSD_STRUCTURE_VERSION_1:	// for low capacity cards (max 2GB)
//				printf("%s, %d\r\n", __FUNCTION__, __LINE__);
				/* Block size in bytes (doesn't have to be 512) */
				readBlockLen = 1 << SDCARD_GET_CSD_FIELD(sdcard.csd, 1, READ_BLOCK_LEN);
				blockCountMult = 1 << (SDCARD_GET_CSD_FIELD(sdcard.csd, 1, CSIZE_MULT) + 2);
				blockCount = (SDCARD_GET_CSD_FIELD(sdcard.csd, 1, CSIZE) + 1) * blockCountMult;
//				printf("readBlockLen: %u\r\n", readBlockLen);
//				printf("blockCountMult: %u\r\n", blockCountMult);
//				printf("blockCount: %u\r\n", blockCount);
			
				/* We could do this in 32 bits, but it makes the 2GB case awkward */
				capacityBytes = (uint64_t) blockCount * readBlockLen;
				
				/* Re-express that capacity (max 2GB) in our standard 512-byte block size */
				sdcard.metaData.numBlocks = capacityBytes / SDCARD_BLOCK_SIZE;
				break;
			
			case SDCARD_CSD_STRUCTURE_VERSION_2:		// for high capacity cards
//				printf("%s, %d\r\n", __FUNCTION__, __LINE__);
				/* SDCARD_GET_CSD_FIELD(sdcard.csd, 2, CSIZE) = 30386 */
//				printf("SDCARD_GET_CSD_FIELD(sdcard.csd, 2, CSIZE): %u\r\n", SDCARD_GET_CSD_FIELD(sdcard.csd, 2, CSIZE));
				/* sdcard.metaData.numBlocks = 31116288 */
				sdcard.metaData.numBlocks = (SDCARD_GET_CSD_FIELD(sdcard.csd, 2, CSIZE) + 1) * 1024;
//				printf("sdcard.metaData.numBlocks: %u\r\n", sdcard.metaData.numBlocks);
				break;
			
			default:
				success = false;
		}
	}
	
	sdcard_deselect();
	
	return success;
}

static bool sdcard_receiveCID(void)
{
	uint8_t cid[16];
	
	if (sdcard_receiveDataBlock(cid, sizeof(cid)) != SDCARD_RECEIVE_SUCCESS) {
		return false;
	}
	
//	printf("cid: ");
//	for (int i = 0; i < sizeof(cid)/sizeof(cid[0]); i++) {
//		printf("%u ", cid[i]);
//	}
//	printf("\r\n");

	/* Product_Manual[SanDisk_Secure_Digital_Card].pdf, Table 3-9. CID fields */
	sdcard.metaData.manufacturerID = cid[0];
	sdcard.metaData.oemID = (cid[1] << 8) | cid[2];
	sdcard.metaData.productName[0] = cid[3];
	sdcard.metaData.productName[1] = cid[4];
	sdcard.metaData.productName[2] = cid[5];
	sdcard.metaData.productName[3] = cid[6];
	sdcard.metaData.productName[4] = cid[7];
	sdcard.metaData.productRevisionMajor = cid[8] >> 4;
	sdcard.metaData.productRevisionMinor = cid[8] & 0x0F;
	sdcard.metaData.productSerial = (cid[9] << 24) | (cid[10] << 16) | (cid[11] << 8) | cid[12];
	sdcard.metaData.productionYear = (((cid[13] & 0x0F) << 4) | (cid[14] >> 4)) + 2000;
	sdcard.metaData.productionMonth = cid[14] & 0x0F;

#if 0
	printf("sdcard.metaData.manufacturerID: %u\r\n", sdcard.metaData.manufacturerID);
	printf("sdcard.metaData.oemID: 0x%x\r\n", sdcard.metaData.oemID);
	printf("sdcard.metaData.productName: %s\r\n", sdcard.metaData.productName);
	printf("sdcard.metaData.productRevisionMajor: %u\r\n", sdcard.metaData.productRevisionMajor);
	printf("sdcard.metaData.productRevisionMinor: %u\r\n", sdcard.metaData.productRevisionMinor);
	printf("sdcard.metaData.productSerial: 0x%x\r\n", sdcard.metaData.productSerial);
	printf("sdcard.metaData.productionYear: %u\r\n", sdcard.metaData.productionYear);
	printf("sdcard.metaData.productionMonth: %u\r\n", sdcard.metaData.productionMonth);
#endif
	
	return true;
}

static bool sdcard_setBlockLength(uint32_t blockLen)
{
	sdcard_select();
	
	uint8_t status = sdcard_sendCommand(SDCARD_COMMAND_SET_BLOCKLEN, blockLen);
	
	sdcard_deselect();
	
	return status == 0;
}

/**
 * Call periodically for the SD card to perform in-progress transfers.
 *
 * Returns true if the card is ready to accept commands.
 */
bool sdcard_poll(void)
{
//	printf("%s, %d\r\n", __FUNCTION__, __LINE__);
	uint8_t initStatus;
	bool sendComplete;
	
	doMore:
//	printf("sdcard.state: %d, %s, %d\r\n", sdcard.state, __FUNCTION__, __LINE__);
	switch (sdcard.state) {
		case SDCARD_STATE_RESET:
			sdcard_select();
			
			initStatus = sdcard_sendCommand(SDCARD_COMMAND_GO_IDLE_STATE, 0);
			
			sdcard_deselect();
		
//			printf("initStatus: %u, %s, %d\r\n", initStatus, __FUNCTION__, __LINE__);
			if (initStatus == SDCARD_R1_STATUS_BIT_IDLE) {
				/* Check sdcard voltage and version */
				if (sdcard_validateInterfaceCondition()) {
//					printf("sdcard.version: %u, %s, %d\r\n", sdcard.version, __FUNCTION__, __LINE__);
					sdcard.state = SDCARD_STATE_CARD_INIT_IN_PROGRESS;
//					printf("sdcard.state: %u, %s, %d\r\n", sdcard.state, __FUNCTION__, __LINE__);
					goto doMore;
				}else {
					/* Bad reply/voltage, we ought to refrain from accessing the card */
					sdcard.state = SDCARD_STATE_NOT_PRESENT;			// SDCARD_STATE_NOT_PRESENT = 0
//					printf("sdcard.state: %u, %s, %d\r\n", sdcard.state, __FUNCTION__, __LINE__);
				}
			}
		
			break;
		
		case SDCARD_STATE_CARD_INIT_IN_PROGRESS:
//			printf("SDCARD_STATE_CARD_INIT_IN_PROGRESS!!\r\n");
			if (sdcard_checkInitDone()) {
//				printf("%s, %d\r\n", __FUNCTION__, __LINE__);
				if (sdcard.version == 2) {
					/* Check for high capacity card */
					uint32_t ocr;
					
					if (!sdcard_readOCRRegister(&ocr)) {
						sdcard_reset();
						goto doMore;
					}
						
//					printf("ocr: 0x%x\r\n", ocr);		// ocr = 0xC0FF8000
					sdcard.highCapacity = (ocr & (1 << 30)) != 0;		// sdcard.highCapacity = 1
//					printf("sdcard.highCapacity: %d\r\n", sdcard.highCapacity);
				}else {
					/* Version 1 cards are always low-capacity */
					sdcard.highCapacity = false;
				}
				
				/* Now fetch the CSD and CID registers */
				if (sdcard_fetchCSD()) {
					sdcard_select();
					
					uint8_t status = sdcard_sendCommand(SDCARD_COMMAND_SEND_CID, 0);
					if (status == 0) {
						/* Keep the card selected to receive the response block */
						sdcard.state = SDCARD_STATE_INITIALISATION_RECEIVE_CID;
						goto doMore;
					}else {
						sdcard_deselect();
						
						sdcard_reset();
						goto doMore;
					}
				}
			}
			break;
		
		case SDCARD_STATE_INITIALISATION_RECEIVE_CID:
//			printf("%s, %d\r\n", __FUNCTION__, __LINE__);
			if (sdcard_receiveCID()) {
				sdcard_deselect();
				
                /* The spec is a little iffy on what the default block size is for Standard Size cards (it can be changed on
                 * standard size cards) so let's just set it to 512 explicitly so we don't have a problem.
                 */
				if (!sdcard.highCapacity && !sdcard_setBlockLength(SDCARD_BLOCK_SIZE)) {
					sdcard_reset();
					goto doMore;
				}
				
				/* Now we are done with init and we can switch to the full speed clock (<25MHz) */
				spiSetDivisor(SDCARD_SPI_INSTANCE, SDCARD_SPI_FULL_SPEED_CLOCK_DIVIDER);
				
				sdcard.multiWriteBlocksRemain = 0;
				
				sdcard.state = SDCARD_STATE_READY;
				
//				printf("sdcard.state: %u\r\n", sdcard.state);
				
				goto doMore;
			}	// else part keeps waiting for the CID to arrive
			break;
		
		case SDCARD_STATE_SENDING_WRITE:
			break;
		
		case SDCARD_STATE_WAITING_FOR_WRITE:
			break;
		
		case SDCARD_STATE_READING:
			break;
		
		case SDCARD_STATE_STOPPING_MULTIPLE_BLOCK_WRITE:
			break;
		
		case SDCARD_STATE_NOT_PRESENT:
			printf("SDCARD_STATE_NOT_PRESENT!!\r\n");
			break;
		default:
			;
	}
	
	return sdcard_isReady();
}

#endif	// #ifdef USE_SDCARD
