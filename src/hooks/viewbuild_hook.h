#pragma once

#include <cstdint>

// THE CPU-SIDE CAMERA STAGES -- six detours on the engine's own view and
// projection builders, and the largest file in the mod because the head pose,
// the AER eye offset, the horizon locks and PROJ LOCK all go in here.
//
// The file is named after the first of them, the view-matrix builder at
// exe+0x1153070, on the theory that post-rotating every view it produced would
// move all cameras coherently at a single point. That is true and was not
// enough: the engine recomputes the RENDER camera per frame downstream of it,
// so the offset that matters goes in at the per-frame sites (detour_fna,
// detour_projbuild, detour_combine2) and the builder carries the rest.
//
// Requires MinHook initialized. Returns false if the module offset cannot be
// resolved; the individual downstream hooks are best-effort after that and log
// their own failures.
namespace hooks {

bool install_viewbuild_hook();

// Called once per frame (from the Present hook): resets the per-frame view and
// projection-invocation counters, updates the pose-based map detection, and
// polls the one remaining keybind here (HOME = recenter).
void viewbuild_on_present();

// Current values of the game's own FOV fields (settings+0x60/+0x64). Read-only
// -- nothing writes them any more -- for the FOV-stability monitor, where they
// report what the game itself thinks the FOV is next to what we actually render.
// False if the settings pointer isn't up yet.
bool game_fov_fields(float& f0, float& f1);

// (THE SCREEN-STATE HUNT lived here: the game's own menu/garage/map/gameplay
// integer, a declaration UI, an automatic narrowing pass, and the gating that
// had been moved onto it. All removed 2026-08-24 -- see the note at the top of
// viewbuild_hook.cpp for what was measured and why finding the field was never
// the problem. map_by_pose() below is the screen marker again.)

// (THE DECLARED CULLING FOV lived here: a multiplier written into the game's own
// FOV fields every frame, a toggle to suppress that write, and the degrees it
// worked out to. All removed 2026-08-26 -- the value that stopped the periphery
// being culled was past the value that started culling objects dead ahead, so no
// setting satisfied both. hooks/camera_fov.h declares the FOV on
// combine::CAMERA instead, which does.)

// Orbit-camera horizon lock: head pitch overwrites the stick's elevation
// instead of adding to it (see detour_fna). Config file / in-game settings UI.
bool horizon_lock_enabled();
void set_horizon_lock_enabled(bool on);

// COCKPIT truck-relative look locks -- the in-cab counterpart to the orbit
// horizon lock above, but levelled against the TRUCK rather than the world, so
// the truck's real tilt on hills is preserved while the stick's look input
// stops contributing on the locked axis. Independently selectable; head
// pitch/yaw becomes the only source for whichever is on. See
// apply_truck_frame_basis() in viewbuild_hook.cpp. Config file / settings UI.
bool truck_level_lock_enabled();      // drops the stick's PITCH
void set_truck_level_lock_enabled(bool on);
bool truck_yaw_lock_enabled();        // drops the stick's YAW
void set_truck_yaw_lock_enabled(bool on);

// The MAP screen, identified from the GAME's own camera pose -- sampled at the
// builder BEFORE any head transform, so it is not contaminated by our injected
// rotation the way anything read back from the camera constant buffer would be.
//
// Measured: the map camera is pitched exactly -45 deg with its eye near the
// ORIGIN, against gameplay's world coordinates in the hundreds. Two independent
// properties -- an orbit camera can be held at 45 degrees, but not while also
// standing at the origin -- with hysteresis on both edges so a camera sweeping
// through the angle cannot flicker it.
//
// Needs no relocation streak, no drive-camera idle test and no screen-state
// field, so it works from the first frame and is indifferent to the startup
// sequence that defeated the old classifier.
bool map_by_pose();

// The cockpit head rotation, the head lean and the AER +-IPD/2 all go in at the
// camera builder (exe+0xDA1C20). See the long note at the top of
// viewbuild_hook.cpp for why that is the only injection point, and what the six
// rotation modes that used to live here were working around.
//
// Things that were exposed here and are deliberately gone -- listed so they are
// not reinvented, since each was removed for a measured reason rather than a
// stylistic one:
//
//   * The six rotation modes and their sliding-window/cone tuning. All were
//     working around a MISDIAGNOSIS: the 60/s figure that made the camera stage
//     look sim-rate and damped was g_buildSeq counting combine2, a different
//     stage. Measured, fna runs once per RENDERED frame (~200/s) and reaches
//     the render undamped, which is why one injection point now suffices.
//   * "Rotate at projection" and "lean at projection" (2026-08-12), for the
//     same reason -- lean, rotation and eye offset all go in at the builder.
//   * The per-caller FOV probe and the manual projection-invocation selector
//     (2026-08-15), and the invocation gate they became (retired 2026-08-25).
//     The mirror problem they were built to investigate is solved, but not by
//     counting: the main view is now told from the mirror by the ASPECT of the
//     frustum, which survives cinematics and transitions where an ordering
//     rule cannot. See proj_lock_should_lock().
//   * A blend-object stack hunter and the live code-dump entry points, which
//     were recon tools for an encrypted executable and have served their
//     purpose.

} // namespace hooks
