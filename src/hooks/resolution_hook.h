#pragma once

// Render-resolution override, bypassing SnowRunner's own resolution clamp.
// See docs/resolution_clamp_strategies.md for how the clamp was found and why
// this is the injection point that works.
namespace hooks {

// Installs the two window hooks this needs. MUST run before the game creates
// its window, i.e. from DllMain, after hooks::init() (MinHook) and
// vrcfg::init() (so the size is already known). Idempotent. A no-op for
// ForceGameResolution=off, which leaves the game's window handling completely
// untouched. With 'auto' (vrcfg::resolution_unset(): no size chosen yet) they go
// in with nothing forced, so the size the headset asks for can be applied to the
// running game once it is known -- menu_hook.cpp's first-run notice does that.
void install_resolution_hooks();

// 0,0 = off. Otherwise the exact CLIENT size the game's main window is created
// at, overriding whatever resolution the game itself decided on. The engine
// sizes its swapchain from the window's client rect (it calls GetClientRect
// straight after creating the window), so this dictates the render resolution
// outright -- no video.dat edit, and independent of the game's own clamp.
//
// Set from the config file at startup and from the settings menu's "Render
// resolution" slider, which stores pixels rather than a percentage because the
// value is consumed before any OpenXR instance exists to scale against.
void set_force_resolution(int w, int h);
int  force_resolution_w();
int  force_resolution_h();

// DLSS UPSCALING STARTS HERE. With an upscaling mode selected (render/dlss.hpp)
// the window is sized to that mode's RENDER size instead of the forced size,
// and the forced size becomes the size of the HEADSET image, which DLSS brings
// each eye up to.
//
// True when the window really is at the reduced size: out* is the headset image
// size, render* the window's client size. Both follow a live apply, below.
bool upscale_session(int& outW, int& outH, int& renderW, int& renderH);
// The DLSS mode the window is sized for now, or dlss::kOff when it is not.
int  upscale_mode_applied();

// THE LIVE APPLY: the size and DLSS mode reach the running game, no restart.
// How, and what it took to find, is docs/resolution_clamp_strategies.md §11.
//
// install_live_resize() runs where install_frame_sync() does, once the game's
// code is decrypted. false from live_resize_available() means this session
// cannot do it, and the setting waits for the next launch as it always did: the
// signatures did not match, the game is fullscreen, or it was started with
// ForceGameResolution=off, so the window hooks above are not in.
void install_live_resize();
bool live_resize_available();

// Takes force_resolution_w/h() and dlss::mode() as they are when it runs, which
// is the next frame with nothing loading. Call it once a choice is final -- each
// apply rebuilds the game's render targets and can rebuild the headset images.
void request_resolution_apply();
// Asked for a while ago and still held back, which is what a load looks like.
bool resolution_apply_waiting();

// (A live ResizeTarget/ResizeBuffers pair behind an "Apply resolution now" button
// lived here until 2026-08-24. Both calls succeed and the picture does not change,
// so do not rebuild that: see the note in resolution_hook.cpp.)

} // namespace hooks
