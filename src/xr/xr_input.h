#pragma once
#include <windows.h>
#include <xinput.h>
#include <openxr/openxr.h>

// THE VR CONTROLLERS AS AN XBOX PAD.
//
// SnowRunner only knows gamepads, mouse and keyboard; it has no idea the player
// is holding motion controllers. So their buttons are read through OpenXR and
// presented to the game as XInput pad 0 -- merged with a real pad if one is
// connected, which keeps working exactly as before.
//
//   triggers   -> LT / RT          grips          -> LB / RB
//   A B X Y    -> A B X Y          sticks         -> left / right stick
//   stick clicks -> L3 / R3        menu (left)    -> Start on a tap,
//                                                    Back (Select) held 0.5 s
//
// THE MENU BUTTON does both because a Quest controller has nothing else that
// could be Back: a tap reaches the game as Start when it is released, and a
// hold of half a second becomes Back for as long as it is held.
//
// THE D-PAD is reached by HOLDING L3: while the left stick is held down the
// right stick stops moving the camera and drives the D-pad instead, and the
// menu button becomes Back at once. L3 therefore does double duty, and the game
// only sees it as L3 when it was not used as that modifier: a quick tap reaches
// the game as a short press, a hold with nothing else touched reaches it after
// 200 ms. Controllers with a dedicated second button (WMR, HP Reverb G2,
// Index) also map that one to Back directly.
//
// Bound for Oculus Touch (every Quest through Virtual Desktop or Link), Valve
// Index, WMR, the HP Reverb G2 controllers and Vive wands; the runtime picks
// whichever matches the hardware.
namespace xrinput {

// Instance extensions this module would like enabled, if the runtime offers
// them. ensure_instance() asks before creating the instance.
bool wants_extension(const char* name);
void note_extension_enabled(const char* name);

// Creates the action set, suggests bindings for every known controller and
// attaches it to the session. Once per session, after xrCreateSession and
// before the first frame. Failure only costs the controller mapping.
bool init(XrInstance instance, XrSession session);

// Once per frame, after xrBeginFrame: syncs the actions and republishes the
// pad state. `focused` is whether the session is in XR_SESSION_STATE_FOCUSED --
// the only state in which the runtime delivers input at all.
void poll(XrSession session, bool focused);

// Destroys the action set. Before the instance goes.
void shutdown();

// The VR pad for XInput pad 0, merged with whatever `real` already holds (an
// actual pad). Returns false -- `real` untouched -- while the mapping is off or
// no controller has been seen this session. Thread-safe; called from the
// game's input thread through the XInput hook.
bool merge_into(DWORD& result, XINPUT_STATE* state);

// True while the VR pad is being presented (on, and a controller seen this
// session). The XInputGetCapabilities hook answers for pad 0 on this: the game
// asks for capabilities before it polls, so a pad that only exists in
// XInputGetState would never be polled when no real pad is plugged in.
bool present();

// Settings-menu / config switch. On by default.
bool enabled();
void set_enabled(bool on);

} // namespace xrinput
