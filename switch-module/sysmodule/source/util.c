#include <switch.h>
#include "util.h"
#include "modes/modes.h"

static Mutex screenModeMutex;

bool UtilSetConsoleScreenMode(bool on)
{
	// called from the video thread and from the USB watchdog thread
	mutexLock(&screenModeMutex);
	Result rc = lblInitialize();
	LOG("lblInitialize %x", rc);

	if (R_SUCCEEDED(rc))
	{
		LblBacklightSwitchStatus lblstatus = LblBacklightSwitchStatus_Disabled;
		if (R_SUCCEEDED(lblGetBacklightSwitchStatus(&lblstatus)))
		{
			LOG("lblGetBacklightSwitchStatus %x", rc);
			if (on && lblstatus == LblBacklightSwitchStatus_Disabled)
				rc = lblSwitchBacklightOn(0);
			else if (!on && lblstatus == LblBacklightSwitchStatus_Enabled)
				rc = lblSwitchBacklightOff(0);
		}
		lblExit();
	}
	
	LOG("UtilSetConsoleScreenMode(%d) %x", on, rc);
	mutexUnlock(&screenModeMutex);
	return R_SUCCEEDED(rc);
}

