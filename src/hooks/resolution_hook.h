#pragma once

// Render-resolution override, bypassing SnowRunner's own resolution clamp.
// See docs/resolution_clamp_strategies.md for how the clamp was found and why
// this is the injection point that works.
namespace hooks {

// Installs the two window hooks this needs. MUST run before the game creates
// its window, i.e. from DllMain, after hooks::init() (MinHook) and
// vrcfg::init() (so the size is already known). Idempotent, and a no-op when
// no size is set. Also no-ops when the config has no forced size, leaving the
// game's window handling completely untouched.
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

// (apply_resolution_now() lived here -- a live ResizeTarget/ResizeBuffers pair
// behind an "Apply resolution now" button. Removed 2026-08-24: both calls
// succeed and the engine still does not re-derive its render size from them.
// The size is consumed at CreateWindowExW, so a restart is the only apply.
// See the note in resolution_hook.cpp before rebuilding it.)

} // namespace hooks
