#include "modes.h"
#include "../USB/Serial.h"
#include "../capture.h"
#include "proto.h"
#include "../core.h"

static bool VideoConnected = false;
static bool AudioConnected = false;
static Mutex UsbMutex;
static u64 LastConnection;
static u64 LastVideoSend; // last time a video packet went out, or the client connected
static bool VideoSeen;    // a video packet went out since the client connected

static bool ClientConnected() 
{
	return VideoConnected || AudioConnected;
}

static void DisconnectClient() 
{
	VideoConnected = false;
	AudioConnected = false;
	ProtoClientGlobalStateDisconnected();
}

static bool SendData(void* data, size_t length) 
{
	bool success;
	mutexLock(&UsbMutex);

	bool isTooLate = armGetSystemTick() - LastConnection > armNsToTicks(1.5E+9);
	if (isTooLate || !ClientConnected())
		success = false;
	else 
		success = UsbStreamingSend(data, length);

	if (success)
		LastConnection = armGetSystemTick();

	mutexUnlock(&UsbMutex);
	return success;
}

// While the game is not on the screen (HOME menu, a controller prompt, a system window) there is no video: the
// video thread then waits inside grc for the next frame and cannot do anything else. This thread gives the console
// screen back after a few seconds without video, and turns it off again when the video is back.
static Thread WatchdogThread;
static u8 alignas(0x1000) WatchdogStack[0x2000 + LOGGING_STACK_BOOST];

static void USB_WatchdogThread(void*)
{
	while (IsThreadRunning)
	{
		svcSleepThread(5E+8L);

		if (!ClientConnected() || !VideoConnected)
			continue;

		// The first picture of a connection can take about 3 seconds (the encoder starts with the client), so
		// until a video packet has gone out the screen is given back later, to not blink on at every start
		ProtoScreenIdle(armGetSystemTick() - LastVideoSend > armNsToTicks(VideoSeen ? 2.5E+9 : 6E+9));
	}
}

bool USB_ConnectClient()
{
	size_t num = 0;

	// Try this in a loop until a client connects
	do
		num = usbSerialWrite(PROTO_HANDSHAKE_HELLO, sizeof(PROTO_HANDSHAKE_HELLO), 1E+9);
	while (num == 0 && IsThreadRunning);

	if (num != sizeof(PROTO_HANDSHAKE_HELLO) || !IsThreadRunning)
		return false;

	u8 buffer[PROTO_HANDSHAKE_SIZE];
	if (usbSerialRead(buffer, sizeof(buffer), 1E+9) != sizeof(buffer))
		return false;

	LOG("USB request received\n");	
	ProtoParsedHandshake res = ProtoHandshake(ProtoHandshakeAccept_Any, buffer, sizeof(buffer));

	if (!UsbStreamingSend(&res.Result, sizeof(res.Result)))
	{
		ProtoClientGlobalStateDisconnected();
		return false;
	}

	if (res.Result.Code != Handshake_Ok)
	{
		ProtoClientGlobalStateDisconnected();
		return false;
	}

	VideoConnected = res.RequestedVideo;
	AudioConnected = res.RequestedAudio;

	if (VideoConnected)
		CaptureVideoConnected();

	if (AudioConnected)
		CaptureAudioConnected();
	
	LastConnection = armGetSystemTick();
	LastVideoSend = LastConnection;
	VideoSeen = false;
	return true;
}

static void USB_VideoStreamThread(void*)
{
	while (IsThreadRunning)
	{
		// Video streaming thread acts as master and receives the initial streaming request
		if (!ClientConnected()) {
			if (!USB_ConnectClient()) {
				svcSleepThread(5E+8L);
				continue;
			}
		}
		else if (VideoConnected)
		{
			// Do not check for errors here, in case grc failed we will be sending the error packet
			CaptureReadVideo();

			if (!IsThreadRunning)
				continue;

			if (!SendData(&VPkt, VPkt.Header.DataSize + sizeof(PacketHeader)))
			{
				DisconnectClient();
				svcSleepThread(5E+8);
			}
			else
			{
				LastVideoSend = armGetSystemTick();
				VideoSeen = true;
			}
		}
		else if (!VideoConnected)
		{
			svcSleepThread(1E+9);
		}
	}
}

static void USB_AudioStreamThread(void*)
{
	while (IsThreadRunning)
	{
		if (AudioConnected)
		{
			// Do not check for errors here, in case grc failed we will be sending the error packet
			CaptureReadAudio();
			
			if (!IsThreadRunning)
				continue;

			if (!SendData(&APkt, APkt.Header.DataSize + sizeof(PacketHeader)))
			{
				DisconnectClient();
				svcSleepThread(5E+8);
			}
		}
		else
		{
			svcSleepThread(1E+9);
		}
	}
}

static void USB_Init()
{
	VideoConnected = false;
	AudioConnected = false;
	mutexInit(&UsbMutex);

	Result rc = UsbStreamingInitialize();
	if (R_FAILED(rc)) 
		fatalThrow(rc);

	LaunchThread(&WatchdogThread, USB_WatchdogThread, NULL, WatchdogStack, sizeof(WatchdogStack), 0x2C);
}

static void USB_Exit()
{
	JoinThread(&WatchdogThread);

	mutexLock(&UsbMutex);
	VideoConnected = false;
	AudioConnected = false;
	UsbStreamingExit();
	mutexUnlock(&UsbMutex);
}

const StreamMode USB_MODE = {
	USB_Init, USB_Exit, 
	USB_VideoStreamThread, USB_AudioStreamThread,
	NULL, NULL,
};