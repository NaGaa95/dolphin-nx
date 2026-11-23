// nx2_gameshare: GameShare hosting (Nintendo Switch 2) shared by the NX ports. See nx2_gameshare.h.
// SPDX-License-Identifier: MIT
#if defined(__SWITCH2__)

#include "nx2_gameshare.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <switch2/runtime/gameshare.h>

#define NX2_GAMESHARE_BAD_EXIT MAKERESULT(Module_Libnx, LibnxError_LibAppletBadExit)
#define NX2_GAMESHARE_BAD_REPLY MAKERESULT(Module_Libnx, LibnxError_BadAppletReceiveMessage)
#define NX2_GAMESHARE_TIMEOUT MAKERESULT(Module_Libnx, LibnxError_Timeout)

static atomic_bool s_requested, s_swapRoles, s_active, s_separate;

// The host session and every stpl call that uses it
static Mutex s_hostMutex;
static GameShareHost s_host;
static bool s_hostOpen = false;
static NWindow s_guestWindow;
static bool s_guestWindowOpen = false;
static bool s_accountReady = false;

// The guest's input
static Mutex s_inputMutex;
static Nx2GameShareInput s_lastInput;
static bool s_haveInput = false;
static u64 s_lastButtons = 0;
static HidNpadIdType s_guestPadId = HidNpadIdType_No1;
static u32 s_guestPadStyle = 0;
static int s_sensorProbe = 0;

static void nx2GameShareFail(char* error, size_t error_size, const char* what, Result rc)
{
	if (error && error_size)
		snprintf(error, error_size, "%s failed (0x%08x, module %u description %u)", what, (unsigned)rc,
				 (unsigned)R_MODULE(rc), (unsigned)R_DESCRIPTION(rc));
}

static void nx2GameShareError(char* error, size_t error_size, const char* text)
{
	if (error && error_size)
		snprintf(error, error_size, "%s", text);
}

static bool nx2GameShareHostUser(AccountUid* uid)
{
	if (!s_accountReady)
		s_accountReady = R_SUCCEEDED(accountInitialize(AccountServiceType_Application));
	if (!s_accountReady)
		return false;
	memset(uid, 0, sizeof(*uid));
	if (R_SUCCEEDED(accountGetPreselectedUser(uid)) && accountUidIsValid(uid))
		return true;
	memset(uid, 0, sizeof(*uid));
	if (R_SUCCEEDED(accountGetLastOpenedUser(uid)) && accountUidIsValid(uid))
		return true;
	AccountUid users[ACC_USER_LIST_SIZE];
	memset(users, 0, sizeof(users));
	s32 count = 0;
	if (R_SUCCEEDED(accountListAllUsers(users, ACC_USER_LIST_SIZE, &count)) && count > 0 &&
		accountUidIsValid(&users[0]))
	{
		*uid = users[0];
		return true;
	}
	return false;
}

// The renderer's screen on the guest's window
static Nx2GameShareScreen s_screen;
static bool s_screenOpen = false;

void nx2GameShareCloseScreen(void)
{
	atomic_store(&s_separate, false);
	if (s_screenOpen && s_screen.close)
		s_screen.close(s_screen.user);
	s_screenOpen = false;
}

static void nx2GameShareCloseWindow(void)
{
	nx2GameShareCloseScreen();
	if (s_guestWindowOpen)
		nwindowClose(&s_guestWindow);
	s_guestWindowOpen = false;
}

static void nx2GameShareCloseHost(void)
{
	if (!s_hostOpen)
		return;
	mutexLock(&s_hostMutex);
	Result rc = gameshareHostClose(&s_host);
	// Timeout: the service has not given the work memory back yet (each call waits about a second)
	for (int attempt = 0; rc == NX2_GAMESHARE_TIMEOUT && attempt < 5; attempt++)
		rc = gameshareHostClose(&s_host);
	s_hostOpen = false;
	mutexUnlock(&s_hostMutex);
}

void nx2GameShareConfigure(bool requested, bool swap_roles)
{
	atomic_store(&s_requested, requested);
	atomic_store(&s_swapRoles, swap_roles);
}

bool nx2GameShareRequested(void) { return atomic_load(&s_requested); }
bool nx2GameShareSwapRoles(void) { return atomic_load(&s_swapRoles); }
void nx2GameShareSetSwapRoles(bool swap_roles) { atomic_store(&s_swapRoles, swap_roles); }
bool nx2GameShareActive(void) { return atomic_load(&s_active); }
bool nx2GameShareSeparateActive(void) { return atomic_load(&s_active) && atomic_load(&s_separate); }
void* nx2GameShareGuestWindow(void) { return nx2GameShareSeparateActive() ? &s_guestWindow : NULL; }

bool nx2GameShareGuestConnected(void)
{
	if (!nx2GameShareActive())
		return false;
	mutexLock(&s_hostMutex);
	const bool connected = (gameshareHostGetGuests(&s_host) & 1) != 0;
	mutexUnlock(&s_hostMutex);
	return connected;
}

Nx2GameShareResult nx2GameShareBegin(const Nx2GameShareScreen* screen, char* error, size_t error_size)
{
	nx2GameShareError(error, error_size, "");
	if (s_hostOpen)
		return Nx2GameShare_Started;
	AccountUid uid;
	if (!nx2GameShareHostUser(&uid))
	{
		nx2GameShareError(error, error_size, "no user account can host the session");
		return Nx2GameShare_Failed;
	}
	// The guest's display needs its window, and the renderer its screen there, before matchmaking, which then starts
	// a session with separate views (the session's work memory can leave too little room for the renderer after it).
	// libounce sizes the window 1280x720.
	const bool separate_screen = screen && screen->open;
	if (separate_screen)
	{
		const Result rc = nwindowCreateForDisplay(&s_guestWindow, "player1");
		if (R_FAILED(rc))
		{
			nx2GameShareFail(error, error_size, "opening the guest's display", rc);
			return Nx2GameShare_Failed;
		}
		s_guestWindowOpen = true;
		s_screen = *screen;
		if (!s_screen.open(&s_guestWindow, NX2_GAMESHARE_GUEST_WIDTH, NX2_GAMESHARE_GUEST_HEIGHT, s_screen.user))
		{
			nx2GameShareError(error, error_size, "the renderer could not present to the guest's display");
			nx2GameShareCloseWindow();
			return Nx2GameShare_Failed;
		}
		s_screenOpen = true;
	}
	atomic_store(&s_separate, separate_screen);

	GameShareHostConfig config;
	gameshareHostConfigDefault(&config, 1);
	config.mode = separate_screen ? StplStreamingMode_SeparateViews : StplStreamingMode_Mirror;
	config.touch_width = NX2_GAMESHARE_GUEST_WIDTH;
	config.touch_height = NX2_GAMESHARE_GUEST_HEIGHT;
	config.audio = (StplAudioMix){true, 1.0f, 1.0f, 1.0f};
	config.user = uid;
	memset(&s_host, 0, sizeof(s_host));
	s_hostOpen = true;
	const Result rc = gameshareHostCreate(&s_host, &config);
	// Closing the matchmaking applet with HOME or B can end it abnormally instead of as a cancel, and then
	// gameshareHostClose does not end the matchmaking: end it here
	if (rc == NX2_GAMESHARE_BAD_EXIT || rc == NX2_GAMESHARE_BAD_REPLY)
	{
		mutexLock(&s_hostMutex);
		stplEndMatchmaking();
		stplReleaseGuestWorkMemory();
		mutexUnlock(&s_hostMutex);
	}
	if (rc == GAMESHARE_RESULT_MATCHMAKING_CANCELLED || rc == NX2_GAMESHARE_BAD_EXIT || rc == NX2_GAMESHARE_BAD_REPLY)
	{
		nx2GameShareCloseHost();
		nx2GameShareCloseWindow();
		return Nx2GameShare_Cancelled;
	}
	if (R_FAILED(rc))
	{
		nx2GameShareFail(error, error_size, "starting the session", rc);
		nx2GameShareCloseHost();
		nx2GameShareCloseWindow();
		return Nx2GameShare_Failed;
	}
	atomic_store(&s_active, true);
	return Nx2GameShare_Started;
}

void nx2GameShareUpdate(void)
{
	if (!nx2GameShareActive())
		return;
	mutexLock(&s_hostMutex);
	const bool left = R_SUCCEEDED(gameshareHostUpdate(&s_host)) && gameshareHostGetGuestsLeft(&s_host);
	mutexUnlock(&s_hostMutex);
	if (left)
	{
		mutexLock(&s_inputMutex);
		s_haveInput = false;
		s_lastButtons = 0;
		mutexUnlock(&s_inputMutex);
	}
}

void nx2GameShareEnd(void)
{
	atomic_store(&s_active, false);
	mutexLock(&s_inputMutex);
	s_haveInput = false;
	s_lastButtons = 0;
	s_guestPadStyle = 0;
	mutexUnlock(&s_inputMutex);
	nx2GameShareCloseHost();
	nx2GameShareCloseWindow();
}

void nx2GameShareWriteGuestAudio(const int16_t* frames, uint32_t frame_count)
{
	if (!nx2GameShareSeparateActive() || !frame_count)
		return;
	mutexLock(&s_hostMutex);
	u32 free_frames = 0;
	// (no guest to hear it, or its queue is full: this block is dropped)
	if ((gameshareHostGetGuests(&s_host) & 1) && R_SUCCEEDED(stplGetGuestAudioFreeFrames(0, &free_frames)) &&
		free_frames)
	{
		u32 written = 0;
		stplWriteGuestAudio(0, frames, frame_count < free_frames ? frame_count : free_frames, &written);
	}
	mutexUnlock(&s_hostMutex);
}

void nx2GameShareWriteGuestAudioRate(const int16_t* frames, uint32_t frame_count, uint32_t sample_rate)
{
	if (sample_rate == 48000 || !sample_rate)
	{
		nx2GameShareWriteGuestAudio(frames, frame_count);
		return;
	}
	if (!nx2GameShareSeparateActive() || !frame_count)
		return;
	// Linear interpolation, the position carried from block to block
	static int16_t* converted = NULL;
	static size_t capacity = 0;
	static uint32_t position = 0; // 16.16 position in the input, relative to the previous block's last frame
	static int16_t previous[2] = {0, 0};
	const uint32_t step = (uint32_t)(((uint64_t)sample_rate << 16) / 48000);
	const size_t needed = (size_t)(((uint64_t)frame_count << 16) / step) + 2;
	if (needed > capacity)
	{
		int16_t* grown = (int16_t*)realloc(converted, needed * 2 * sizeof(int16_t));
		if (!grown)
			return;
		converted = grown;
		capacity = needed;
	}
	size_t out = 0;
	// Input frame i is frames[i - 1] for i >= 1, previous for i == 0
	while ((position >> 16) < frame_count && out < capacity)
	{
		const uint32_t index = position >> 16;
		const int32_t fraction = (int32_t)(position & 0xFFFF);
		for (int channel = 0; channel < 2; channel++)
		{
			const int32_t a = index == 0 ? previous[channel] : frames[(index - 1) * 2 + channel];
			const int32_t b = frames[index * 2 + channel];
			converted[out * 2 + channel] = (int16_t)(a + (((b - a) * fraction) >> 16));
		}
		out++;
		position += step;
	}
	position -= frame_count << 16;
	previous[0] = frames[(frame_count - 1) * 2];
	previous[1] = frames[(frame_count - 1) * 2 + 1];
	nx2GameShareWriteGuestAudio(converted, (uint32_t)out);
}

// The guest's controllers, merged; false when none is connected. Called with both locks held.
static bool nx2GameShareReadPads(Nx2GameShareInput* input, bool* timed_out)
{
	static const HidNpadIdType ids[] = {HidNpadIdType_No1, HidNpadIdType_Handheld, HidNpadIdType_No2};
	bool connected = false;
	for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
	{
		StplGuestNpadState pad;
		memset(&pad, 0, sizeof(pad));
		const Result rc = stplGetGuestNpadState(0, ids[i], &pad);
		if (rc == NX2_GAMESHARE_TIMEOUT)
			*timed_out = true;
		if (R_FAILED(rc) || !pad.style || !(pad.state.attributes & HidNpadAttribute_IsConnected))
			continue;
		connected = true;
		if (ids[i] == HidNpadIdType_Handheld)
			input->handheld = true;
		s_guestPadId = ids[i];
		s_guestPadStyle = pad.style;
		input->buttons |= pad.state.buttons;
		const int32_t values[4] = {pad.state.analog_stick_l.x, pad.state.analog_stick_l.y, pad.state.analog_stick_r.x,
								   pad.state.analog_stick_r.y};
		for (int axis = 0; axis < 4; axis++)
			if (abs(values[axis]) > abs(input->stick[axis]))
				input->stick[axis] = values[axis];
	}
	return connected;
}

uint64_t nx2GameShareGetGuestButtons(void)
{
	if (!nx2GameShareActive())
		return 0;
	mutexLock(&s_inputMutex);
	mutexLock(&s_hostMutex);
	Nx2GameShareInput input;
	memset(&input, 0, sizeof(input));
	bool timed_out = false;
	if (nx2GameShareReadPads(&input, &timed_out))
		s_lastButtons = input.buttons;
	else if (!timed_out)
		s_lastButtons = 0;
	const uint64_t buttons = s_lastButtons;
	mutexUnlock(&s_hostMutex);
	mutexUnlock(&s_inputMutex);
	return buttons;
}

bool nx2GameShareGetGuestInput(Nx2GameShareInput* out)
{
	if (!nx2GameShareActive())
		return false;
	mutexLock(&s_inputMutex);
	mutexLock(&s_hostMutex);
	Nx2GameShareInput input;
	memset(&input, 0, sizeof(input));
	bool timed_out = false;
	const bool connected = nx2GameShareReadPads(&input, &timed_out);
	bool result;
	if (timed_out && !connected)
	{
		// The service was writing a sample: keep the last one until the next read
		*out = s_lastInput;
		result = s_haveInput;
	}
	else if (!connected)
	{
		s_haveInput = false;
		result = false;
	}
	else
	{
		HidTouchScreenState touch;
		memset(&touch, 0, sizeof(touch));
		if (R_SUCCEEDED(stplGetGuestTouchScreenState(0, &touch)) && touch.count > 0)
		{
			input.touch_down = true;
			input.touch_x = (int32_t)touch.touches[0].x;
			input.touch_y = (int32_t)touch.touches[0].y;
		}
		// The six-axis sensor that matches the guest's controller; the one that answers is tried first next time
		static const struct
		{
			HidNpadIdType pad;
			StplSixAxisSensor sensor;
		} probes[] = {
			{HidNpadIdType_Handheld, StplSixAxisSensor_Handheld}, {HidNpadIdType_No1, StplSixAxisSensor_FullKey},
			{HidNpadIdType_No1, StplSixAxisSensor_JoyDualLeft},   {HidNpadIdType_No1, StplSixAxisSensor_JoyDualRight},
			{HidNpadIdType_No1, StplSixAxisSensor_JoyLeft},       {HidNpadIdType_No1, StplSixAxisSensor_JoyRight},
			{HidNpadIdType_No2, StplSixAxisSensor_FullKey},
		};
		const int probe_count = (int)(sizeof(probes) / sizeof(probes[0]));
		for (int attempt = 0; attempt < probe_count; attempt++)
		{
			const int index = (s_sensorProbe + attempt) % probe_count;
			HidSixAxisSensorState state;
			memset(&state, 0, sizeof(state));
			if (R_FAILED(stplGetGuestSixAxisSensorState(0, probes[index].pad, probes[index].sensor, &state)) ||
				!(state.attributes & HidSixAxisSensorAttribute_IsConnected) || state.sampling_number == 0)
				continue;
			s_sensorProbe = index;
			input.has_motion = true;
			input.motion_sampling = state.sampling_number;
			input.gyro[0] = state.angular_velocity.x;
			input.gyro[1] = state.angular_velocity.y;
			input.gyro[2] = state.angular_velocity.z;
			input.accel[0] = state.acceleration.x;
			input.accel[1] = state.acceleration.y;
			input.accel[2] = state.acceleration.z;
			break;
		}
		s_lastInput = input;
		s_haveInput = true;
		*out = input;
		result = true;
	}
	mutexUnlock(&s_hostMutex);
	mutexUnlock(&s_inputMutex);
	return result;
}

void nx2GameShareSetGuestRumble(float low, float high)
{
	if (!nx2GameShareActive())
		return;
	mutexLock(&s_inputMutex);
	const HidNpadIdType pad_id = s_guestPadId;
	const u32 styles = s_guestPadStyle;
	mutexUnlock(&s_inputMutex);
	if (!styles)
		return;
	const u32 style = styles & (~styles + 1);
	low = low < 0.0f ? 0.0f : (low > 1.0f ? 1.0f : low);
	high = high < 0.0f ? 0.0f : (high > 1.0f ? 1.0f : high);
	const HidVibrationValue value = {low, 160.0f, high, 320.0f};
	StplVibrationDeviceHandle handles[2];
	s32 count = 0;
	mutexLock(&s_hostMutex);
	if (R_SUCCEEDED(stplGetGuestVibrationDeviceHandles(handles, 2, &count, 0, pad_id, style)))
		for (s32 i = 0; i < count; i++)
			stplSendGuestVibrationValue(handles[i], &value);
	mutexUnlock(&s_hostMutex);
}

#endif // __SWITCH2__
