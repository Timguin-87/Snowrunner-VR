#pragma once
#include <dxgi.h>
#include <cstdint>

// THE OPENXR SIDE: session, swapchains and layer submission.
//
// Driven entirely from the Present hook -- no separate thread. The first call
// lazily brings up an OpenXR instance and session on the game's own D3D11
// device; every Present after that renders one eye from the backbuffer,
// synthesizes the other, and submits both as a stereo projection layer, with
// the game's UI on a quad layer of its own on top.
//
// The name is older than the file. It started as a flat mirror -- the
// backbuffer on a head-locked quad -- and the mirror is now the desktop
// window's copy of one eye rather than the thing being submitted.
//
// Every entry point is best-effort: on any failure it logs once and falls back
// to doing nothing, so the game keeps rendering flat to its monitor.
namespace xr {

// Head pose relative to the recenter origin, consumed by the camera hook.
// Angles in radians; position offset in metres in the recentered head frame
// (x = right, y = up, z = backward, per OpenXR). `valid` is false until the
// first successful locate.
struct HeadLook {
    bool  valid = false;
    float yaw = 0, pitch = 0, roll = 0;
    float ox = 0, oy = 0, oz = 0;
};

// Thread-safe snapshot of the latest head pose (camera hook runs on the game's
// update thread, which differs from the Present thread that writes it).
HeadLook head_look();

// AER: the per-eye side offset (± IPD/2, metres) for the eye currently being
// rendered. The camera hooks add this along the camera right axis so each
// alternate frame renders one eye. 0 until stereo is up.
float eye_side_offset();

// THE RENDER PATH IS ALWAYS AER -- one XR frame per Present, one eye genuinely
// rendered, the other supplied. There is no render-method selector any more:
// "Double AER" was measured to be identical to it and is gone, and the frame
// warp that used to sit beside them as a third mode is the DIBR SHIFT option
// below, because it was never a different way of driving XR -- only a different
// way of producing the eye that was not rendered. The two settings that remain
// are independent and compose.

// STALE EYE WARP. Bring the un-rendered eye's own last real render to the
// current instant instead of resubmitting it unchanged. On by default; this is
// the only way to take it out of the picture, so if a judder survives with it
// off, the warp is not causing it.
bool warp_enabled();
void set_warp_enabled(bool on);

// WHAT THAT WARP IS BUILT FROM -- a ladder, not a set of independent flags,
// because each level contains the one before it:
//
//   0  headset rotation only   pose at capture vs now. Blind to the game
//                              camera, so stick look and vehicle turns pass
//                              through uncorrected.
//   1  headset + game rotation the game's own view matrix, which already has
//                              the head composed in. Cockpit only -- orbit
//                              falls back to the pose, see warp_homography_for().
//   2  headset + game 6-DoF    that, plus a depth reprojection by the camera's
//                              translation. Supersedes the homography wherever
//                              it lands, so level 1 is what fills the rest.
//
// set_warp_type() clamps out of range to 0 and logs. warp_type_key()/_from_key()
// are stable config tokens ("Headset"/"GameRotation"/"Game6Dof"), separate from
// the verbose log wording so a saved config never breaks if that changes.
//
// Which pixels the 6-DoF level treats as rigid with the camera is decided by
// render/rigid_mask.hpp -- a coverage mask of the truck's own shaders, not a
// tunable. There is nothing to configure there.
constexpr int kWarpTypeCount = 3;
int  warp_type();
void set_warp_type(int type);

// The two questions callers outside xr_mirror.cpp actually ask of the ladder,
// so its numbering stays in one file: whether the game camera contributes at
// all (the warp-source diagnostics are meaningless below that), and whether the
// 6-DoF level is selected (the scene-depth capture and the rigid mask both have
// to be provisioned for it).
bool warp_uses_game_rot();
bool warp_uses_6dof();
const char* warp_type_key(int type);                        // nullptr if out of range
bool warp_type_from_key(const char* key, int& outType);     // case-insensitive

// Diagnostics for the above, reset whenever the setting is toggled.
//   used             warps actually built from the game camera
//   staleSnapshot    fell back because the camera had not rebuilt since capture
//   orbitDeclined    fell back because this is not the cockpit camera -- the
//                    orbit camera swings around the truck rather than turning
//                    in place, which a rotation warp cannot represent
//   worstDisagreeDeg largest gap between the two sources' rotation magnitudes;
//                    stays small if the axis convention is right
struct WarpSourceStats {
    uint32_t used = 0;
    uint32_t staleSnapshot = 0;
    uint32_t orbitDeclined = 0;
    float    worstDisagreeDeg = 0.0f;
};
void warp_source_stats(WarpSourceStats& out);


// DIBR SHIFT. Reproject the rendered eye into the other one using scene depth,
// rather than leaving that eye entirely to the stale-eye warp. Off by default:
// it is the sole consumer of depth_probe's scene-depth capture, so that whole
// pipeline (a full-res R32_FLOAT texture plus up to kCaptureTail compute
// dispatches every frame) gates on this rather than running for everyone.
//
// The two settings compose rather than exclude: the shift lands where it has
// depth and a source, and the stale-eye warp fills every pixel it could not
// reach -- which is why the warp is not "the alternative to" this.
bool dibr_shift_enabled();
void set_dibr_shift_enabled(bool on);

// Map/garage screen: how much composited_fov_scale() shrinks the compositor
// window (and, in lockstep, the game's own render FOV) while hooks::in_map_view()
// is true. 1.0 = no shrink, smaller = smaller render plane. Config-file / UI
// tunable; clamped to [0.05, 1.0].
// BASE render size, 0.30 - 1.00. Shrinks the compositor window and the game's
// own render FOV together, so a lower value renders less scene into fewer
// pixels with no stretch. Everything downstream re-derives from it, and
// map_window_shrink() below is a percentage OF this -- not of the headset's
// native FOV -- so the two compose instead of competing.
float render_fov_scale();
void  set_render_fov_scale(float s);

float map_window_shrink();
void  set_map_window_shrink(float v);

// Vertical recenter (radians), applied as a pose pitch at composition-layer
// submission -- see g_vertOffset in xr_mirror.cpp. Config-file / UI tunable,
// clamped to [-0.5, 0.5].
float vertical_recenter();
void  set_vertical_recenter(float radians);

// The headset's OWN vertical offset, radians, negative = the display is centred
// below the optical axis. Read from the runtime's per-eye angleUp/angleDown and
// applied automatically at submission, in addition to vertical_recenter(). 0
// until the runtime has reported a frustum. For the settings UI, so the manual
// slider can be read as what it adds rather than as the whole correction.
float fov_center_pitch();


// --- UI plane ---------------------------------------------------------------
//
// The game's HUD, menus and map are drawn into their own layer instead of into
// the eye images (render/ui_layer.hpp) and submitted as a quad. These control
// where that quad goes.
//
// The MAP ignores the mode and size controls: it is forced head-locked at the
// render's own angular size, which is the only placement where a screen-space
// marker still lands on the map feature it belongs to. It DOES follow the
// distance -- hung on the eye its capture came from, so that eye stays exact
// whatever the distance is, and the distance sets the depth the other eye sees
// it at. Its apparent size is unaffected: the map plane's size is derived from
// the distance to hold the rendered view's subtense exactly.
//
// Size and distance are both in METRES and both real: the plane behaves like a
// physical board, so moving it further away makes it look smaller. Apparent
// size is the ratio of the two, and either slider can be used to reach it --
// the choice between them is about where the UI CONVERGES.

// True while the UI can actually be shown on its own layer. Everything that
// takes the UI out of the render follows this; false always means the UI is
// rendered into the frame as it was before, never that there is no UI.
bool  ui_plane_wanted();

// 0 = head-locked (follows the head, the original behaviour), 1 = forward
// (pinned in the world where the last explicit recenter put it).
enum { kUiPlaneHeadLocked = 0, kUiPlaneForward = 1 };
int   ui_plane_mode();
void  set_ui_plane_mode(int mode);

// Width of the plane in metres, clamped to [0.2, 8].
float ui_plane_size();
void  set_ui_plane_size(float metres);

// Metres from the eye, clamped to [0.3, 20]. Further away is easier on the
// eyes and smaller; the size above is what compensates for the second half.
float ui_plane_distance();
void  set_ui_plane_distance(float metres);

// A world-pinned plane is moved ONLY by request_recenter() -- the explicit
// one. A camera-mode change or a truck swap leaves it exactly where it is.


// Aspect-preserving variant: scales origW x origH by the same factor on both
// axes (no squash from forcing square). Also records the result so
// get_forced_render_wh() can return the exact same values elsewhere.
bool  desired_render_size_for(uint32_t origW, uint32_t origH, uint32_t& outW, uint32_t& outH);
bool  get_forced_render_wh(uint32_t& w, uint32_t& h);

// The OpenXR runtime's own recommended per-eye render size, unscaled and
// unshaped -- the reference the settings UI's resolution slider is expressed
// as a percentage of. False when no runtime is reachable yet. Brings up a
// lightweight XR instance on first call (no session/device needed) and caches
// the result, so it is cheap to poll from UI code.
bool  native_eye_size(uint32_t& w, uint32_t& h);

// The game's real backbuffer size, i.e. the pixel space viewports are
// expressed in. Unlike get_forced_render_wh() this reports what is actually
// being rendered, not what the (currently disabled) resolution-forcing
// experiment asked for. False before the first Present.
bool  render_canvas_wh(uint32_t& w, uint32_t& h);

// Called by the camera hooks at the start of the main-camera build; the first
// call per frame advances the AER eye parity (keeps offset + routing in lockstep).
void  advance_aer_parity();


// Called by the camera hooks (cockpit/orbit) every time they run. Lets
// Present's fallback parity-advance detect "no camera active" (menu/loading
// screen) generically, without needing to identify either state specifically.
void  note_camera_active();

// GROUND-TRUTH EYE IDENTITY -- the eye index and the offset that produced it,
// as ONE atomic snapshot.
//
// The camera hooks call record_applied_eye_offset() with the ±IPD/2 they
// actually baked into the frame they are building; it pairs that with the eye
// index in force at that instant (g_frameEye, set moments earlier by
// advance_aer_parity() in the same call) and publishes both together. Eye
// identity therefore travels WITH the content instead of being inferred from a
// frame counter -- a counter drifts whenever a build is skipped or doubled, and
// the drift persists, which shows up as one eye ghosting for seconds before
// swapping.
//
// One 64-bit store, so index and offset can never be read from different
// frames. That pairing is what DIBR shift needs: the disparity DIRECTION is the sign of
// the offset and the destination eye is the recorded index's partner, neither of
// which can be recovered reliably from a swapchain index alone.
// HOW the eye offset reached the render. The 6-DoF reprojection displaces the
// view matrix and re-projects through depth, which is only the same operation
// the renderer performed if the offset MOVED THE CAMERA. Applied as a
// projection shear it moves every depth alike, and with the opposite sign --
// see kMapEyeSign in viewbuild_hook.cpp and sixdof_model_applies().
constexpr int kEyeAppliedAsCameraMove = 0;
constexpr int kEyeAppliedAsProjShift  = 1;
// OR-ed on top: the render applied kMapEyeSign, i.e. it baked the eye offset on
// the side OPPOSITE to gameplay. That flip is a correction for how those screens
// render, not a description of where the camera was, so a reprojection that
// models camera geometry must not inherit it -- it would place the synthesized
// eye on the wrong side and invert the stereo for that eye.
//
// MEASURED 2026-08-26 during a level fly-in: submitted disparity came out at
// -1.1x the true disparity (recovered from the real renders either side with
// the camera's own motion cancelled) at every frame and in both eyes. Same
// magnitude, opposite sign. Because the synthesized eye alternates, the pair
// rocked by twice the disparity every frame.
constexpr int kEyeAppliedMapSign      = 2;

struct AppliedEye {
    bool  valid  = false;
    int   eye    = 0;      // 0 or 1, matching the XR swapchain index
    float offset = 0.0f;   // metres along the game's camera-right axis
    int   how    = kEyeAppliedAsCameraMove;
    // See kEyeAppliedMapSign. Set by whatever made the RENDER flip: a
    // confirmed map, or the menu/cutscene camera CALL SITE, which is a property
    // of where the view was built and is not under the screen-gate selector at
    // all. A level fly-in builds through that site while the marker gate calls
    // the same frame a garage, so nothing else in the chain knew the eye
    // convention had changed underneath it.
    bool  mapSign = false;
    // Was this recorded since the current frame's AER parity flip? The record
    // is otherwise sticky and keeps answering for the previous frame on any
    // screen where the camera build stops running while Present keeps flipping
    // the eye -- which reads as an eye identity that is wrong every other frame.
    bool  fresh  = false;
};

void       record_applied_eye_offset(float off, int how);
AppliedEye applied_eye();

// Relative head orientation quaternion (x,y,z,w), OpenXR axes, vs recenter origin.
void head_orientation(float q[4]);

// --- per-eye display canting -------------------------------------------------
// Headsets with outward-angled panels (Index, Bigscreen Beyond, some Pimax)
// report a rotation, not just a +/-IPD/2 translation, in XrView[eye].pose.
// It is measured once at startup and logged (including the "0.00 deg" result
// on the parallel-panel headsets that are the common case), then folded into
// head_look() so the rendered camera matches the pose we were already
// submitting. See g_cantQuat in xr_mirror.cpp.

// (current_eye_cant_yaw() lived here: the yaw of the eye being rendered, which
// the UI hook used to counter-shift screen-space HUD content so it would not
// inherit the cant and diverge between the eyes. Removed with the UI itself --
// a composition layer is placed by the compositor for each eye, so canted
// optics need no compensation from us at all.)

// (THE DESKTOP DEBUG HEAD POSE lived here: a synthetic rotation and lean driven
// by the mouse -- hold J or K and drag -- so the camera paths could be exercised
// with no headset attached. It overrode head_look() and made headset_hfov_deg()
// report a plausible 100 deg, which is what got the projection-based modes to
// engage without a runtime. Removed 2026-08-27: it was scaffolding for finding
// those paths, and they are found.)

// Synthetic cant (degrees of outward yaw per eye) for testing the canting path
// on hardware that has none. Applied to BOTH the render and the submitted
// pose, so a correct implementation is invisible at every value. Config-file /
// in-game settings UI tunable; clamped to [-15, 15].
float debug_eye_cant_deg();
void  set_debug_eye_cant_deg(float deg);

// Per-eye horizontal FOV of the headset in degrees (|angleLeft|+|angleRight|
// from xrLocateViews, averaged across both eyes). Returns 0 until the first XR
// frame locates the views. This is the raw headset HFOV.
float headset_hfov_deg();

// Render-plane horizontal FOV in degrees, after the render-size shrink.
// This is the frustum actually submitted to the compositor -- the FOV cone the
// game content must fill for a geometrically correct VR image.
float render_hfov_deg();

// Recenter the origin to the current head pose on the next located frame.
// THE EXPLICIT RECENTER (settings button, HOME key). Three things at once,
// because they are one gesture -- "put everything where I am looking":
//   * the play-space POSITION centre moves to the head,
//   * the game's forward YAW becomes the direction the head is facing,
//   * a world-pinned UI plane re-pins straight ahead.
// Pitch and roll are never recentered, so the horizon stays world-locked.
void request_recenter();

// POSITION ONLY -- for camera-mode changes and teleports, which must not swing
// the world round. Startup does neither of these: it takes the current position
// as centre but adopts the RUNTIME's forward (whatever the player set their
// play space up to face), rather than whatever direction the headset happened
// to be pointing when the level finished loading.
void request_position_recenter();

// Called every frame from the Present detour, before the real Present.
void mirror_on_present(IDXGISwapChain* swapchain);

// Called from the ResizeBuffers detour: releases cached backbuffer-derived
// resources so they are rebuilt against the new size.
void mirror_on_resize();

} // namespace xr
