
#include <stdio.h>
#include <stdint.h>

#include "target.h"

#ifdef BLACKBOX

#include "feature.h"
#include "configMaster.h"
#include "system.h"
#include "blackbox_io.h"

#define SLOW_FRAME_INTERVAL						4096

typedef enum BlackboxState {
	BLACKBOX_STATE_DISABLED = 0,				// 0
	BLACKBOX_STATE_STOPPED,						// 1
	BLACKBOX_STATE_PREPARE_LOG_FILE,			// 2
	BLACKBOX_STATE_SEND_HEADER,					// 3
	BLACKBOX_STATE_SEND_MAIN_FIELD_HEADER,		// 4
	BLACKBOX_STATE_SEND_GPS_H_HEADER,			// 5
	BLACKBOX_STATE_SEND_GPS_G_HEADER,			// 6
	BLACKBOX_STATE_SEND_SLOW_HEADER,			// 7
	BLACKBOX_STATE_SEND_SYSINFO,				// 8
	BLACKBOX_STATE_PAUSED,						// 9
	BLACKBOX_STATE_RUNNING,						// 10
	BLACKBOX_STATE_SHUTTING_DOWN				// 11
}BlackboxState;

static struct {
	uint32_t headerIndex;
	
	/* Since these fields are used during different blackbox states (never simultaneously)
	 * we can overlap them to save on RAM.
	 */
	union {
		int fieldIndex;
		uint32_t startTime;
	}u;
}xmitState;

static BlackboxState blackboxState = BLACKBOX_STATE_DISABLED;

static bool blackboxLoggedAnyFrames;

static uint16_t blackboxSlowFrameIterationTimer;

/* How many bytes can we write *this* iteration without overflowing transmit buffers or overstressing the OpenLog? */
int32_t blackboxHeaderBudget;

static int gcd(int num, int denom)
{
	if (denom == 0) {
		return num;
	}
	
	return gcd(denom, num % denom);
}

void validateBlackboxConfig(void)
{
	int div;
	
	if (BlackboxConfig()->rate_num == 0 || BlackboxConfig()->rate_denom == 0
			|| BlackboxConfig()->rate_num >= BlackboxConfig()->rate_denom) {
		BlackboxConfig()->rate_num = 1;
		BlackboxConfig()->rate_denom = 1;
	} else {
		/**
		 * Reduce the fraction the user entered as much as possible (makes the recorded/skipped frame pattern repeat
		 * itself more frequently)
		 */
		div = gcd(BlackboxConfig()->rate_num, BlackboxConfig()->rate_denom);
		
		BlackboxConfig()->rate_num /= div;
		BlackboxConfig()->rate_denom /= div;
	}
	
	/* If we have chosen an unsupported device, change the device to serial */
	switch (BlackboxConfig()->device) {
#ifdef USE_FLASHFS
		case BLACKBOX_DEVICE_FLASH:
#endif

#ifdef USE_SDCARD
		case BLACKBOX_DEVICE_SDCARD:
#endif
		case BLACKBOX_DEVICE_SERIAL:
			/* Device supported, leave the setting alone */
			break;
		
		default:
			BlackboxConfig()->device = BLACKBOX_DEVICE_SERIAL;
	}
}

/**
 * Start Blackbox logging if it is not already running.
 * Intended to be called upon arming.
 */
void startBlackbox(void)
{
	if (blackboxState == BLACKBOX_STATE_STOPPED) {
		/* Validate blackbox configuration */
		validateBlackboxConfig();
		
		/* Check if blackbox device is open or not */
		
		
		/* Initialise history */
		
		
		/* Build blackbox condition cache */
		
		
		/* Set blackboxState to BLACKBOX_STATE_PREPARE_LOG_FILE (2) */
		
	}
}

static void blackboxSetState(BlackboxState newState)
{
	/* Perform initial setup required for the new state */
	switch (newState) {
		case BLACKBOX_STATE_PREPARE_LOG_FILE:
			blackboxLoggedAnyFrames = false;
			break;
		
		case BLACKBOX_STATE_SEND_HEADER:
			blackboxHeaderBudget = 0;
			xmitState.headerIndex = 0;
			xmitState.u.startTime = millis();
			break;
		
		case BLACKBOX_STATE_SEND_MAIN_FIELD_HEADER:
		case BLACKBOX_STATE_SEND_GPS_G_HEADER:
		case BLACKBOX_STATE_SEND_GPS_H_HEADER:
		case BLACKBOX_STATE_SEND_SLOW_HEADER:
			xmitState.headerIndex = 0;
			xmitState.u.fieldIndex = -1;
			break;
		
		case BLACKBOX_STATE_SEND_SYSINFO:
			xmitState.headerIndex = 0;
			break;
		
		case BLACKBOX_STATE_RUNNING:
			/* Force a slow frame to be written on the first iteration */
			blackboxSlowFrameIterationTimer = SLOW_FRAME_INTERVAL;
			break;
		
		case BLACKBOX_STATE_SHUTTING_DOWN:
			xmitState.u.startTime = millis();
			break;
		
		default:
			;
	}
	
	blackboxState = newState;
}

static bool canUseBlackboxWithCurrentConfiguration(void)
{
	return feature(FEATURE_BLACKBOX) && 
		(BlackboxConfig()->device != BLACKBOX_SDCARD || feature(FEATURE_SDCARD));
}

void initBlackbox(void)
{
	if (canUseBlackboxWithCurrentConfiguration()) {
		blackboxSetState(BLACKBOX_STATE_STOPPED);
	} else {
		blackboxSetState(BLACKBOX_STATE_DISABLED);
	}
	
//	printf("blackboxState: %u, %s, %s, %d\r\n", blackboxState, __FILE__, __FUNCTION__, __LINE__);
}
#endif	// BLACKBOX
