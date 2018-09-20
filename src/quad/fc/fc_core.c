
#include <stdio.h>		// testing purposes
#include "platform.h"
#include "rx.h"			// including time.h
#include "debug.h"
#include "system.h"
#include "pwm_output.h"
#include "mixer.h"
#include "asyncfatfs.h"
#include "blackbox.h"
#include "runtime_config.h"
#include "led.h"

uint8_t motorControlEnable = false;

bool isRXDataNew;

void updateLEDs(void)
{
//	printf("armingFlags: %u, %s, %s, %d\r\n", armingFlags, __FILE__, __FUNCTION__, __LINE__);
	
	if (CHECK_ARMING_FLAG(ARMED)) {
//		printf("ARMED, %s, %s, %d\r\n", __FILE__, __FUNCTION__, __LINE__);
		LED3_ON; LED4_ON; LED5_ON; LED6_ON;
	} else {
//		printf("Not ARMED: %s, %d\r\n", __FUNCTION__, __LINE__);
		ENABLE_ARMING_FLAG(OK_TO_ARM);
	}
}

void mwArm(void)
{
	if (CHECK_ARMING_FLAG(OK_TO_ARM)) {
		if (CHECK_ARMING_FLAG(ARMED)) {
			return;
		}
		
//        if (IS_RC_MODE_ACTIVE(BOXFAILSAFE)) {
//            return;
//        }
		
		if (!CHECK_ARMING_FLAG(PREVENT_ARMING)) {
			ENABLE_ARMING_FLAG(ARMED);
			ENABLE_ARMING_FLAG(WAS_EVER_ARMED);
			
#ifdef BLACKBOX
			if (feature(FEATURE_BLACKBOX)) {
				startBlackbox();
			}
#endif
		}
	}
}

void processRx(timeUs_t currentTimeUs)
{
	calculateRxChannelsAndUpdateFailsafe(currentTimeUs);
	
	/* update RSSI */
	
	
	/* handle failsafe if necessary */
	
	
	/* calculate throttle status */
	
	
	/* handle AirMode at LOW throttle */
	
	
	/* handle rc stick positions */
	processRcStickPositions();
}

static void subTaskMotorUpdate(void)
{
	uint32_t startTime;
	if (debugMode == DEBUG_CYCLETIME) {
		startTime = micros();
		static uint32_t previousMotorUpdateTime;		// static keyword to keep the previous motor update time
		const uint32_t currentDeltaTime = startTime - previousMotorUpdateTime;
		debug[2] = currentDeltaTime;
//		debug[3] = currentDeltaTime - targetPidLooptime;		// TODO: targetPidLooptime is defined in pid.c
		previousMotorUpdateTime = startTime;
	}else if (debugMode == DEBUG_PIDLOOP) {
		startTime = micros();
	}
	
	mixTable();			// TODO: add &currentProfile->pidProfile later
//	mixTable(&currentProfile->pidProfile);
	
	if (motorControlEnable) {
//		printf("motorControlEnable: %s, %d\r\n", __FUNCTION__, __LINE__);
		writeMotors();
	}
}

static void subTaskMainSubprocesses(timeUs_t currentTimeUs)
{
#ifdef USE_SDCARD
	afatfs_poll();
#endif
	
#ifdef BLACKBOX
	if (/*!cliMode && */feature(FEATURE_BLACKBOX)) {
		handleBlackbox(currentTimeUs);
	}
#endif
}

void taskMainPidLoop(timeUs_t currentTimeUs)
{
	static bool runTaskMainSubprocesses;
	
	/* run subTaskMainSubprocesses */
	if (runTaskMainSubprocesses) {
		subTaskMainSubprocesses(currentTimeUs);
		runTaskMainSubprocesses = false;
	}
	
    /* DEBUG_PIDLOOP, timings for:
     * 0 - gyroUpdate()
     * 1 - pidController()
     * 2 - subTaskMainSubprocesses()
     * 3 - subTaskMotorUpdate()
	 */
	
	/* gyroUpdate */
	
	
	/* subTaskPidController */


	/* subTaskMotorUpdate */
	subTaskMotorUpdate();
	runTaskMainSubprocesses = true;
}
