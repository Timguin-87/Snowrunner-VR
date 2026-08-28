#pragma once
#include <dxgi.h>
#include <cstdint>

// CB_GLOBAL_CAMERA INTERCEPTION, at the point the engine uploads it.
//
// The render view matrix is not reachable through the CPU logic camera, so the
// camera constant buffer is caught at UpdateSubresource / Map / Unmap instead.
// Hooking vtable slot 48 on the immediate context also covers deferred contexts
// (shared vtable). Requires MinHook initialized (hooks::init()).
//
// TWO JOBS. It publishes the last main-camera view and viewProj, which is what
// DIBR shift turns reverse-Z depth into view-space Z with; and in the COCKPIT
// it applies the AER eye offset here as a lateral shift of the view, because
// the cockpit's view arrives already built. The orbit camera does its own
// stereo further upstream, at FnA -- see viewbuild_hook.cpp.
namespace hooks {

void install_cbuffer_hook(IDXGISwapChain* swapchain);

// Last main-camera view and viewProj (row-major, column-vector M*v), captured
// before our own edits. DIBR shift derives the projection as viewProj * inverse(view)
// to convert reverse-Z depth into view-space Z. False until first seen.
bool main_camera_matrices(float view[16], float viewProj[16]);

// (ui_scale_factor() lived here, driving ui_hook.cpp's viewport shrink. Both
// are gone: the UI is a composition layer now and its size is the layer's own,
// in metres -- xr::ui_plane_size().)

// WHICH SCREEN WE ARE ON, from the camera. (These briefly answered from the
// game's own screen-state field; that mechanism was removed 2026-08-24 -- see
// the note at the top of viewbuild_hook.cpp.)
//
// True on the MAP, identified from the main camera's pose: pitched -45 deg with
// its eye near the origin. Used to bypass the HUD shrink (map UI stays at
// native scale/position) and to drive a reduced render FOV (xr_mirror.cpp's
// render_hfov_deg()). Known failure: a gameplay camera that reaches that pose
// reads as the map.
bool in_map_view();

// TRUE while the world is actually being played -- false on the map, in the
// garage and in the menus.
//
// LEGACY GATE: the old relocation/idle classifier. Sense is inverted from the
// old in_map_view_legacy(): it reads as "in gameplay" rather than "not in
// gameplay". Stays true in the PAUSE menu, since the world is loaded and the
// camera has not relocated.
//
// FINGERPRINT GATE: gameplay is the COMPLEMENT of the screens the marker lists
// can identify, qualified by the drive camera actually running -- not a
// positive match on a gameplay marker. Gameplay shares its interface with the
// garage and its world with everything else, so it may never have a shader of
// its own; asking for one meant this could be permanently false, which switches
// off every caller silently. Goes false in the pause menu, unlike the legacy
// answer, because the drive camera stops there.
bool in_gameplay();


// Whether the most recent camera-CB commit on the IMMEDIATE context was the
// MAIN camera. Fails OPEN: true until something proves otherwise.
//
// DIAGNOSTIC ONLY since 2026-08-25. depth_probe used to refuse scene-sized
// depth passes on this, to keep a reflection pass out of the accumulator; it
// measured one non-main commit per frame taking out all six of that frame's
// passes, because a latch on the last commit cannot say which pass a camera
// OWNS -- only which passes came after it. See the long note in
// capture_if_pass_ended(). Still counted there, because it is the only
// evidence we have on the question.
bool     imm_camera_is_main();

// How many non-main camera commits have been seen on the immediate context
// since the last call. Diagnostic, and it answers the question outright: zero
// means no other camera is committed where the capture can see it, and the
// guard above can never do anything.
uint32_t non_main_camera_commits();

} // namespace hooks
