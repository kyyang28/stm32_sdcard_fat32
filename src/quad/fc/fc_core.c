
#include <stdio.h>		// testing purposes
#include "platform.h"
#include "rx.h"			// including time.h
#include "debug.h"
#include "system.h"
#include "pwm_output.h"
#include "mixer.h"
#include "asyncfatfs.h"
#include "blackbox.h"

uint8_t motorControlEnable = false;

bool isRXDataNew;

void mwArm(void)
{
	startBlackbox();
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
