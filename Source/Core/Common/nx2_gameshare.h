// nx2_gameshare: GameShare hosting (Nintendo Switch 2) shared by the NX ports.
// SPDX-License-Identifier: MIT
//
// Another Nintendo Switch or Switch 2 console joins a local GameShare session (libounce's gameshare/stpl host API, the
// system matchmaking applet) and plays the game with this console: it sees the game, hears it, and its controller,
// touch screen and six-axis sensor drive one player, who rumbles it. kaiHEN must run inside a game that supports
// GameShare.
//
// A port:
// - asks for a session from its launcher's handoff (nx2GameShareConfigure),
// - calls nx2GameShareBegin once its renderer exists and before the game starts. With a renderer that can present to
//   a second window (drako3d, or NVK with mesa-nx 82fafacaf5e's named-display windows) the session has separate
//   views: the guest's display ("player1") gets a window of its own (nx2GameShareGuestWindow), the renderer presents
//   the guest's picture there, and the guest's audio goes through nx2GameShareWriteGuestAudio. Otherwise (OpenGL)
//   the guest mirrors this console's screen and sound,
// - calls nx2GameShareUpdate once per frame,
// - reads the guest with nx2GameShareGetGuestInput / nx2GameShareGetGuestButtons and rumbles it with
//   nx2GameShareSetGuestRumble,
// - calls nx2GameShareEnd once the game has stopped and its renderer has destroyed what it made on the guest's
//   window.
// Compiled only for the Switch 2 build (__SWITCH2__).
#pragma once

#if defined(__SWITCH2__)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The guest's screen, and its touch screen's range
#define NX2_GAMESHARE_GUEST_WIDTH 1280
#define NX2_GAMESHARE_GUEST_HEIGHT 720

typedef enum {
	Nx2GameShare_Started,
	Nx2GameShare_Cancelled, // matchmaking cancelled (also by HOME or B in the applet): back to the launcher
	Nx2GameShare_Failed,
} Nx2GameShareResult;

// The guest's latest sample
typedef struct {
	uint64_t buttons; // HidNpadButton_* bits of the guest's controllers, merged
	int32_t stick[4]; // left x/y, right x/y (HidAnalogStickState range)
	bool touch_down;  // a touch in NX2_GAMESHARE_GUEST_WIDTH x NX2_GAMESHARE_GUEST_HEIGHT
	int32_t touch_x;
	int32_t touch_y;
	bool handheld;   // the guest plays on its console in handheld mode (its six-axis sensor is the console's)
	bool has_motion; // the six-axis sensor of the guest's controller, in HidSixAxisSensorState units
	uint64_t motion_sampling;
	float gyro[3];
	float accel[3];
} Nx2GameShareInput;

// The launcher asked for this game to run with GameShare; swap_roles: the guest takes this console's player
void nx2GameShareConfigure(bool requested, bool swap_roles);
bool nx2GameShareRequested(void);
// Off: the guest is the other player (player 2). On: the guest is player 1 and this console's controllers move to
// the next players.
bool nx2GameShareSwapRoles(void);
// During a session (the quick menu); the port then reassigns its players
void nx2GameShareSetSwapRoles(bool swap_roles);

// The renderer's screen on the guest's window. open makes what the renderer presents there with (a swapchain on
// window, an NWindow*) and returns false when it cannot; close destroys it. Both run on the thread that calls
// nx2GameShareBegin, nx2GameShareCloseScreen or nx2GameShareEnd, while the renderer does not present.
typedef struct {
	bool (*open)(void* window, int width, int height, void* user);
	void (*close)(void* user);
	void* user;
} Nx2GameShareScreen;

// screen: the renderer presents to the guest's window, and the session has separate views; NULL: the guest mirrors
// this console. Opens the guest's window and the renderer's screen on it (both before matchmaking, as the service
// expects), shows the matchmaking applet (blocking until a guest joins or it is cancelled) and starts streaming. On
// failure, error holds the reason.
Nx2GameShareResult nx2GameShareBegin(const Nx2GameShareScreen* screen, char* error, size_t error_size);
// Once per frame: the guest's connection state
void nx2GameShareUpdate(void);
// Destroys the renderer's screen on the guest's window (once; nx2GameShareEnd does it too): call it before the
// renderer goes if that happens before nx2GameShareEnd
void nx2GameShareCloseScreen(void);
// Ends the session and closes the guest's window
void nx2GameShareEnd(void);
// From a successful nx2GameShareBegin until nx2GameShareEnd
bool nx2GameShareActive(void);
// A session with separate views: the guest's window exists
bool nx2GameShareSeparateActive(void);
// The guest's window (an NWindow*) while a session with separate views runs, else NULL
void* nx2GameShareGuestWindow(void);
// A guest is connected now
bool nx2GameShareGuestConnected(void);

// The guest's audio in a session with separate views: 48 kHz interleaved stereo s16 frames. What the guest's queue
// has no room for is dropped. Cheap and ignored otherwise.
void nx2GameShareWriteGuestAudio(const int16_t* frames, uint32_t frame_count);
// The same for stereo s16 at another sample rate (converted to 48 kHz, one caller thread at a time)
void nx2GameShareWriteGuestAudioRate(const int16_t* frames, uint32_t frame_count, uint32_t sample_rate);

// The guest's controller, touch screen and six-axis sensor; false while no guest sample is available (while the
// service writes one, the last sample is returned)
bool nx2GameShareGetGuestInput(Nx2GameShareInput* out);
// The guest's buttons alone (for a quick menu, which reads them whatever the game reads)
uint64_t nx2GameShareGetGuestButtons(void);
// Rumbles the guest's controller: low and high frequency motors, 0 to 1
void nx2GameShareSetGuestRumble(float low, float high);

#ifdef __cplusplus
}
#endif

#endif // __SWITCH2__
