#pragma once
#include <cstdint>

// THE CULLING FOV, DECLARED ON THE CAMERA OBJECT ITSELF.
//
// This is the whole mechanism now. It replaced writing the game's own settings
// fields (viewbuild_hook's g_cockpitFovDeg / g_orbitFovDeg, removed 2026-08-26),
// which had no good setting: the value that stopped the periphery being culled
// was past the value where something started culling objects dead ahead. This
// sets it on combine::CAMERA, which is what the engine derives both its own
// projection and its cull frustum from -- one number, instead of two derived
// quantities being fought separately. MEASURED at 1.00x: the periphery is clear
// and what forward culling remains has moved further out.
//
// THE OFFSET IS MEASURED, not assumed: m_fFOV at +0x108, in RADIANS. See
// docs/mudrunner_pdb_findings.md section 3.5 for the reading that established
// it, and for the two ways SnowRunner's struct differs from MudRunner's
// (near/far swapped, width/height left at zero).
//
// The object is found the same way it was found then: sweeping for
// `m_tmViewProj == m_tmView * m_tmProj`, sixteen floats that must be the exact
// product of thirty-two others. That invariant needs nothing from this mod and
// cannot be broken by the mod's own edits to the constant buffer or by PROJ
// LOCK, because the object is checked against itself.
//
// EVERY MATCHING OBJECT IS WRITTEN. The sweep found two adjacent ones holding
// identical contents -- almost certainly a double buffer -- and there is no way
// yet to tell the main view's camera from a mirror's. Writing all of them is the
// honest version of a test: if the mirrors go wrong, that is a result and it
// tells us a discriminator is needed.
//
// THE SWEEP STOPS AT THE FIRST FIND (plus a megabyte, to catch that sibling),
// because nothing is written while it is running and the address space it would
// otherwise walk to the end of is 128 TB. Without that stop it reported cameras
// found and then ground on forever without ever using them. If it finds nothing
// it gives up after 12 GB rather than drag the frame rate down indefinitely --
// the failure the frustum search was removed for.
//
// THE ADDRESS IS NOT STATIC and cannot be. It is a heap allocation under ASLR,
// so it differs every launch -- there is no module-relative offset to record.
// What makes that a non-issue is that the set is SELF-MAINTAINING: every write
// re-checks that the object is still a camera before touching it, so a freed or
// recycled block is caught and a fresh search starts on its own, rather than a
// stale pointer quietly scribbling a float into whatever moved in.
//
// THERE IS NO OFF SWITCH, and there was one until 2026-08-27. Off means the
// game's own culling FOV, which is far too narrow for a headset -- a state
// worth reaching only to confirm the thing this exists to fix, and the log line
// says that better than a checkbox does. The RE-SEARCH button went with it: the
// search re-arms by itself on every visit to gameplay and on every invalidated
// camera, so there was nothing left for a button to ask for.
//
// THE SEARCH RE-ARMS ON A COCKPIT/ORBIT SWITCH while nothing has been found.
// That switch is the one moment in gameplay where the engine is demonstrably
// building camera state, so a sweep that came up empty just after a level load
// gets another go. Only while empty: re-arming with a good set in hand would
// throw it away and re-derive it every time you looked out of the cab.
//
// THE SEARCH ONLY RUNS IN GAMEPLAY, gated on drive_camera_live(). The camera
// object does not exist on the menu or in the garage, so a sweep started there
// cannot succeed -- and since a finished sweep does not restart itself, one
// wasted attempt at boot used to leave the FOV undeclared for the entire
// session. It now waits for the drive camera to tick, allows a second for the
// camera to be built, and takes one attempt per visit to gameplay; leaving and
// returning re-arms it. Nothing is written outside gameplay either, so a camera
// freed on the way to a menu is never written through.
namespace hooks {

// Multiplier on the rendered horizontal FOV. 1.0 declares exactly what we draw.
float camera_fov_factor();
void  set_camera_fov_factor(float f);

// What that multiplier currently works out to, in degrees -- for the settings
// UI, so the factor can be read as the frustum it actually produces.
float camera_fov_result_deg();

// Sweeps until the cameras are located, then writes. Called from Present.
void camera_fov_on_present();

// Also called from the projection builder: the FOV is an INPUT to the camera
// build, so the earlier in the frame it lands the sooner it takes effect.
void camera_fov_reassert();

// How the search is getting on, for the Advanced tab -- the only place that
// says whether the camera was ever found.
const char* camera_fov_status();

} // namespace hooks
