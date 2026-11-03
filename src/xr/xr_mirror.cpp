#include "xr/xr_mirror.h"
#include "hooks/depth_probe.h"
#include "hooks/frame_sync.h"
#include "hooks/cbuffer_hook.h"
#include "hooks/camera_hook.h"      // logic_camera_idle_ms()
#include "hooks/shader_cull.h"       // the screen fingerprint
#include "hooks/viewbuild_hook.h"
#include "hooks/ui_hook.h"
#include "hooks/menu_hook.h"
#include "hooks/resolution_hook.h"
#include "hooks/marker_camera.h"    // marker_camera_placing()
#include "render/dibr.hpp"
#include "render/reproject.hpp"
#include "render/rigid_mask.hpp"
#include "render/mirror_mask.hpp"
#include "render/frame_dump.hpp"
#include "render/smudge_layer.hpp"
#include "render/winch_layer.hpp"
#include "render/ui_layer.hpp"
#include "render/cursor_overlay.hpp"
#include "render/dlss.hpp"
#include "render/motion_field.hpp"
#include "render/motion_overlay.hpp"
#include "xr/xr_input.h"
#include "common/dibr_policy.h"
#include "common/log.h"

#include <algorithm>
#include <d3d11.h>
#include <d3d10.h>        // ID3D10Multithread -- see protect_immediate_context()
#include <d3dcompiler.h>
#include <wrl/client.h>

// XR_USE_GRAPHICS_API_D3D11 / XR_USE_PLATFORM_WIN32 come from CMake.
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <vector>
#include <cmath>
#include <atomic>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace xr {
namespace {

// One latch so a failed init logs once and then stays quiet in passthrough.
bool g_failed = false;
bool g_inited = false;
// The engine resized the game's frame for us: see follow_game_frame().
std::atomic<bool> g_frameResized{false};

// AER: per-eye side offset the camera hooks read (± IPD/2 for the current eye).
std::atomic<float> g_eyeSide{0.0f};
std::atomic<bool>  g_stereo{false};
// Eye parity is advanced by the CAMERA RENDER (advance_aer_parity), not by
// Present, so the offset and the eye routing come from the same event and can't
// desync under deferred/multithreaded rendering. Present only marks a new frame.
std::atomic<int>   g_frameEye{0};       // eye of the frame currently being rendered
// The eye of the image in the backbuffer at this Present -- the eye of the
// camera build it was DRAWN with, which on the map, garage and menus is not the
// latest one (see BuildRec and update_on_screen()). -1 = not known; routing
// then falls back to g_frameEye.
std::atomic<int>   g_onScreenEye{-1};
std::atomic<bool>  g_newFrame{false};   // set at Present; consumed by the first camera build
std::atomic<float> g_ipd{0.062f};
// The rendered separation is g_ipd / this (world_scale()), and the camera's
// travel the head's / this (head_look()). Config is authoritative.
std::atomic<float> g_worldScale{1.0f};

// THE RENDER METHOD (xr_mirror.h). What the settings ask for; Synched
// Sequential is only in effect while the freeze actually runs, which needs the
// frame callbacks hooked and an XR session -- see synched_active().
std::atomic<int>   g_renderMethod{kRenderAER};
// SYNCHED SEQUENTIAL. How many updates behind its update a render is: 0 on the
// menus, 1 in-game (measured), learned from the camera in ss_choose_eye().
std::atomic<int>   g_ssLag{0};
// The frame being built, chosen at its first camera build and read at its
// Present: bit 63 valid, bit 62 first render of its moment, low 32 bits the
// moment (the index of the real update it shows).
std::atomic<uint64_t> g_ssFrame{0};
// Lag learning. Touched only by the one camera build per frame that wins
// g_newFrame, so frames take turns rather than racing. `g_ssObsHist`: the
// latest observations, newest in bit 0.
uint64_t g_ssPrevFp = 0;
int      g_ssObs = -1, g_ssObsRun = 0;
uint32_t g_ssObsHist = 0;
// For the periodic log line: new moments seen at a real / a frozen update,
// pairs completed, and second renders that found no first to pair with.
std::atomic<uint32_t> g_ssNewMoment[2] = {}, g_ssPairs{0}, g_ssOrphans{0};
// Frames with a camera build, by the kind of update just run -- a frozen
// update that builds no camera would leave its eye unrendered -- and frames
// with none at all (skipped, as in AER).
std::atomic<uint32_t> g_ssBuilds[2] = {}, g_ssStalled{0};
// The headset's frames under Synched Sequential, and the time spent waiting
// for them in xrWaitFrame -- with one per PAIR, the wait is the headroom left
// for rendering both eyes inside one refresh.
std::atomic<uint32_t> g_ssXrFrames{0}, g_ssWaitUs{0};
// The latest XR frame's predicted display time and period, for the Presents
// that run none (ss_first_eye_present). 0 = none yet. Present thread only.
XrTime     g_lastDisplayTime   = 0;
XrDuration g_lastDisplayPeriod = 0;

// WHICH CAMERA BUILD EACH PRESENT SHOWS. In gameplay the image at a Present is
// drawn with that frame's own camera build; on the map, in the garage and on
// the menus with the build BEFORE it. Measured 2026-10-27 with a 2 deg yaw bump
// on every fourth build of the title-screen camera: in 16 of 16 the GPU-side
// camera carried it one Present late. So every build leaves a record here --
// its Synched Sequential choice, the views it was built from and the camera
// direction it handed the game -- and a Present reads the record of the build
// its image actually came from: build count minus g_presLag.
//
// This is also what the AER eye flip on those screens (kMapEyeSign,
// viewbuild_hook.cpp) has been compensating: an image one build behind is
// routed to the other eye, and flipping that eye's offset puts the depth back
// -- while each eye's declared pose stays a frame off, which is why those
// screens never felt like gameplay.
struct BuildRec {
    uint32_t build = 0;            // g_advFlips of the frame it belongs to; 0 = empty
    int      eye = -1;             // the eye it was built for
    uint64_t ss = 0;               // g_ssFrame chosen for it (Synched Sequential)
    float    fwd[3] = {0, 0, 0};   // the camera direction handed to the game
    bool     haveFwd = false;
    float    eyePos[3] = {0, 0, 0};  // ...and its position, after the head and eye offsets
    bool     haveEyePos = false;
    float    eyeOff[3] = {0, 0, 0};  // ...the eye offset in it: eyePos less this is the head
    bool     haveEyeOff = false;
    int      builder = 0;            // CameraBuilder: which game builder made it
    bool     prevScreen = false;   // a map / garage / menu camera
    XrView   views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    bool     haveViews = false;
};
constexpr uint32_t kBuildRing = 8;
BuildRec g_buildRec[kBuildRing];            // guarded by g_buildRecLock
SRWLOCK  g_buildRecLock = SRWLOCK_INIT;
std::atomic<int> g_presLag{0};
// The build whose image is in the backbuffer at this Present, and its views --
// set by update_on_screen() at the top of every Present.
BuildRec g_onScreenRec;                     // Present thread only
bool     g_onScreenValid = false;           // Present thread only
// Voting state, Present thread only. `g_lagMeasured`: the lag in force came
// from votes rather than from the screen.
int       g_lagVote = -1, g_lagRun = 0;
ULONGLONG g_lagVoteTick = 0;
bool      g_lagMeasured = false;
// The main-camera snapshot the latest Present consumed (cbuffer_hook's
// CamSnapInfo::seq): an upload after it belongs to the frame being rendered.
// Written at Present, read at the TAA resolve -- the same thread here, atomic
// regardless.
std::atomic<uint32_t> g_snapSeqSeen{0};
std::atomic<bool>     g_snapSeqValid{false};
// Presents by where their build came from: the render's camera on a build, the
// newest build when it was uploaded, the old vote/screen measure. Present
// thread only; for the periodic line.
uint32_t g_onScreenBy[3] = {0, 0, 0};
// The camera builder of the image at the latest Present (onscreen_camera_builder).
std::atomic<int> g_onScreenBuilder{0};
// Set by set_render_method(); the Present thread drops any half-built pair.
std::atomic<bool>  g_ssReset{false};

// How far the middle of the eyes travelled in the world from one camera build to
// another: each build's eye with its own eye offset taken back off. Zero when the
// ring no longer holds either. For the motion-vector overlay, which shows motion
// relative to the head (render/motion_overlay.hpp).
void head_move_between(uint32_t from, uint32_t to, float out[3])
{
    out[0] = out[1] = out[2] = 0.0f;
    if (!from || !to) return;
    AcquireSRWLockShared(&g_buildRecLock);
    const BuildRec& a = g_buildRec[from % kBuildRing];
    const BuildRec& b = g_buildRec[to % kBuildRing];
    if (a.build == from && b.build == to && a.haveEyePos && a.haveEyeOff &&
        b.haveEyePos && b.haveEyeOff) {
        for (int i = 0; i < 3; ++i)
            out[i] = (b.eyePos[i] - b.eyeOff[i]) - (a.eyePos[i] - a.eyeOff[i]);
    }
    ReleaseSRWLockShared(&g_buildRecLock);
}

// Synched Sequential IN EFFECT: asked for, and the freeze running. The freeze
// needs the frame callbacks hooked and is held off without an XR session (see
// mirror_on_present), so asking is not enough.
bool synched_active()
{
    return g_renderMethod.load() == kRenderSynchedSequential && hooks::frame_freeze_on();
}

// WHICH EYE GOES FIRST alternates per pair: L,R then R,L then L,R. At every
// pair boundary the game's "previous frame" is then the SAME eye -- its own
// last render, one moment back -- so the first render of each pair gets exact
// same-eye motion vectors, history and previous depth from the game, the two
// eyes taking turns, and only the second render of a pair needs DLSS's vectors
// converted (render/dlss.cpp). With a fixed order one eye was always the one
// converting. `moment` is the index of the real update shown, and those are
// two apart, so bit 1 flips from pair to pair.
int ss_first_eye(uint32_t moment) { return (int)((moment >> 1) & 1u); }

// WHICH EYE THE FRAME BEING BUILT IS FOR, under Synched Sequential: the first
// render of a moment, then its frozen repeat with the other eye -- which eye
// starts is ss_first_eye().
//
// Which render is which depends on which update the frame SHOWS, and that is
// not always the update just run: in-game the render runs one update behind,
// on the menus it does not (both measured, docs/same_frame_stereo.md). So the
// lag is learned, from the camera. The game's own camera input -- `fp`,
// hashed before any of ours is applied -- changes when a new moment's state
// reaches it, and the update shown is then a real one: a change right after the
// frozen repeat means the render is one behind. Two agreeing observations in a
// row are needed to switch, so one odd frame cannot flip the eyes. A camera
// that is not moving teaches nothing, and the lag stays what it was.
//
// EXCEPT IN GAMEPLAY, where a change after a REAL update proves nothing. The
// camera's own motion -- the stick swinging the orbit round the truck -- is
// applied by the update it is run in, on every screen; only what the camera
// FOLLOWS (the truck, the world) runs a frame late in-game. With the truck
// parked and the camera swinging, every change came after a real update and
// the lag was taught the menus' 0 (seen in the log 2026-10-28), and with both
// moving the observations alternate and teach nothing. So on the gameplay
// camera the lag only ever goes UP, on two changes after a frozen update among
// the latest four -- which only something running a frame late can produce.
int ss_choose_eye(uint64_t fp, bool gameplay)
{
    const uint32_t cur = hooks::frame_update_index();
    g_ssBuilds[hooks::frame_update_frozen(cur) ? 1 : 0].fetch_add(1);
    if (fp != 0) {
        if (g_ssPrevFp != 0 && fp != g_ssPrevFp) {
            const int obs = hooks::frame_update_frozen(cur) ? 1 : 0;
            g_ssNewMoment[obs].fetch_add(1);
            if (obs == g_ssObs) ++g_ssObsRun;
            else { g_ssObs = obs; g_ssObsRun = 1; }
            g_ssObsHist = (g_ssObsHist << 1) | (uint32_t)obs;
            int want = -1;
            if (gameplay) {
                const uint32_t h = g_ssObsHist;
                const int recent = (int)((h & 1u) + ((h >> 1) & 1u) + ((h >> 2) & 1u) +
                                         ((h >> 3) & 1u));
                if (obs == 1 && recent >= 2) want = 1;
            } else if (g_ssObsRun >= 2) {
                want = obs;
            }
            if (want >= 0 && want != g_ssLag.load()) {
                g_ssLag.store(want);
                hooks::set_frame_render_lag(want);
                VRLOG("Synched Sequential: renders now run %d update(s) behind (%s)", want,
                      want ? "in-game" : "menus");
            }
        }
        g_ssPrevFp = fp;
    }
    const uint32_t shown = cur - (uint32_t)g_ssLag.load();
    const bool first = !hooks::frame_update_frozen(shown);
    const uint32_t moment = first ? shown : shown - 1;
    g_ssFrame.store((1ull << 63) | (first ? (1ull << 62) : 0ull) | moment);
    return ss_first_eye(moment) ^ (first ? 0 : 1);
}
std::atomic<int>   g_advCalls{0}, g_advFlips{0}, g_presents{0};  // parity diagnostics

// Presents on which the MAIN CAMERA VIEW MATRIX SEEN BY THE GPU changed. This is
// the number that settles whether the render camera is genuinely stuck at the
// build rate or is being interpolated per frame downstream of our injection.
std::atomic<int>   g_viewChanges{0};

// Which eye the game just rendered, for routing the backbuffer into the right
// XR swapchain.
//
// A 16-entry ring plus a settable pipeline delay used to sit here: the AER eye
// offset was once baked at camera-BUILD time on deferred worker threads, one or
// two frames before Present routed the pixels, so the routing had to look
// backwards to find the parity that matched what it was holding. Removed
// 2026-08-18 once gameplay's skew was gone.
//
// THE SKEW CAME BACK, as a fact about screens rather than a setting. On the map,
// the garage and the menus the image at a Present IS drawn with the camera
// built a frame earlier (measured 2026-10-27, see BuildRec) -- which the
// per-screen eye flip kMapEyeSign had been papering over. So routing reads the
// eye of the build ON SCREEN, worked out once per Present by update_on_screen()
// from which build's camera the GPU actually used; in gameplay that is the
// latest build, exactly as before.
int present_route_eye()
{
    const int e = g_onScreenEye.load();
    return e >= 0 ? (e & 1) : (g_frameEye.load() & 1);
}

// THE MAP'S UI, as a share of the rendered view, about the same centre. The
// map renders at the render size like everything else; only the quad its UI is
// shown on shrinks. The world markers follow it without being told --
// hooks/marker_camera.cpp builds their camera from the quad as submitted.
//
// (A map window shrink, MapWindowShrink, used to sit here: the map's render FOV
// and its compositor window shrunk together. Removed 2026-11-03, when this took
// over the job of keeping the map's UI inside the sweet spot of the lenses. It
// survives only as the fallback in composited_fov_scale().)
std::atomic<float> g_mapUiScale{0.4f};        // config is authoritative
// BASE render size: the compositor window and the game's own render FOV moved
// together, applied in TANGENT space at both ends so the two agree exactly
// (they did not before 2026-08-06 -- see render_hfov_deg()). Lowering it renders
// a narrower FOV into a smaller window, which is the honest way to buy
// performance: fewer pixels AND less scene, with no stretch, because the
// "submitted FOV == rendered FOV" invariant is preserved at both ends. Shrinking
// only the window while leaving the game's FOV full broke that invariant --
// confirmed in-headset as visible stretch/distortion.
//
// It sits INSIDE composited_fov_scale() rather than beside it, and that is the
// whole design: every consumer -- eye_frustum_half_angles() at submission,
// render_hfov_deg() driving PROJ LOCK, the culling margin -- already calls this
// function, so they all re-derive from the new base with no extra plumbing and
// cannot be left behind.
//
// Deliberately NOT combined with the removed Final-FOV trim, which lived in angle space and
// means something different -- see its comment above.
std::atomic<float> g_renderFovScale{1.00f};
float composited_fov_scale()
{
    float s = g_renderFovScale.load();
    // A GAME BUILD WITHOUT THE MARKER CAMERA. The game then places the map's
    // icons through its own camera, onto the whole window, so the UI can only
    // be smaller if the window is: on the map, the render and the window shrink
    // to the map UI size together, as the removed map window shrink did, and the
    // UI covers all of it (build_ui_quad_layer). Not in 2D, where the panel size
    // comes from the UI plane settings and a narrower render would only crop the
    // map.
    if (!hooks::marker_camera_available() && hooks::in_map_view() && !flat_screen_active())
        s *= g_mapUiScale.load();
    return s;
}
// (A rendered_fov_scale() helper used to live here, returning
// g_fovScale * composited_fov_scale() as a single number for render_hfov_deg()
// to multiply the FOV by. Removed 2026-08-06: collapsing the two into one
// scalar is what hid the fact that they belong in DIFFERENT spaces -- the trim
// scales the angle, the window scale must scale the tangent to match how
// eye_frustum_half_angles() applies it at submission. render_hfov_deg() now
// applies each in its own space; see there.)
// Vertical recenter amount (radians), a tilt of the whole eye -- the game's
// camera and the submitted pose together (submitted_vertical_pitch()) -- for
// correcting a vertically-miscentred image (e.g. black bar at bottom, image
// sitting too high). It moves which part of the world is drawn, never where
// the world is. Positive turns the view up. Config-file / in-game
// settings UI tunable so it can be re-derived/A-B-tested live against the
// vertical FOV-asymmetry bias (docs/vr_framework_learnings.md Fix 1) without
// a rebuild.
// DEFAULT 0 SINCE 2026-08-20. It was -0.150 rad (-8.6 deg), hand-tuned in the
// headset -- which is very close to this hardware's own -5.5 deg of vertical
// FOV asymmetry (fov_center_pitch()), so most of what that constant did was
// compensate for a term we can now read from the runtime. Leaving it non-zero
// would double-count it, and it was never right for any other headset anyway.
// What remains here is personal preference on top.
std::atomic<float> g_vertOffset{0.0f};

// --- UI plane -----------------------------------------------------------------
//
// The game's UI is redirected out of the render (render/ui_layer.hpp) and shown
// as a quad layer instead; these decide where that quad goes. See
// build_ui_quad_layer().
//
// Placement is two cases, not one:
//   MAP          forced head-locked at the render's own angular size times
//                g_mapUiScale, about the same centre. Nothing here applies.
//   EVERYTHING   a real width in metres at a real distance in metres, placed
//   ELSE         either head-locked or pinned in the world. The two are
//                independent the way physical objects are: moving it away
//                makes it smaller.
// kUiPlaneHeadLocked / kUiPlaneForward come from xr_mirror.h -- one definition,
// so the config file, the settings UI and the placement here cannot drift.
std::atomic<int>   g_uiPlaneMode{kUiPlaneForward};      // config is authoritative
std::atomic<float> g_uiPlaneDistM{7.00f};               // config is authoritative
// 1.9 m at 1.6 m away subtends about 61 deg, which is what the old
// fraction-of-the-view sizing produced at its 50% default on a ~100 deg
// headset -- i.e. the defaults look like they always did.
std::atomic<float> g_uiPlaneSizeM{8.00f};               // config is authoritative
// THE UI QUAD AS LAST SUBMITTED: its size and whether it was pinned. Read on
// the game's update thread by ui_plane_in_camera(), which has to describe the
// plane the compositor was actually given rather than derive its own.
std::atomic<float> g_uiQuadWidthM{0.0f};
std::atomic<float> g_uiQuadHeightM{0.0f};
std::atomic<bool>  g_uiQuadPinned{false};

// WORLD-PINNED POSE, in the runtime's own space -- which is what makes it
// survive a recenter. xr::request_recenter() re-snaps g_originQuat/g_originPos,
// our own origin INSIDE the runtime's space, and only the game camera reads
// those; the composition layers are submitted in g.space either way. So a pose
// stored here is untouched by a camera-mode change, by the Recenter button and
// by the HOME key alike, exactly as asked -- the UI moves only when the UI's
// own recenter is pressed.
SRWLOCK g_uiPinLock = SRWLOCK_INIT;
XrPosef g_uiPinPose = {{0, 0, 0, 1}, {0, 0, 0}};
bool    g_uiPinValid = false;
// WHERE a pending pin should put the plane. The two answers exist because the
// two ways of asking mean different things, and both should land the plane on
// whatever the GAME currently calls forward:
//
//   kUiPinToView            the direction the head is looking right now. Asked
//                           for by the explicit recenter, which sets the game's
//                           forward to that same direction in the same breath.
//   kUiPinToRuntimeForward  the runtime's own forward, level with the horizon.
//                           Asked for automatically -- at startup, and when
//                           switching into forward mode with nothing pinned yet
//                           -- because that is the forward startup adopts (see
//                           kRecenterYawReset). Pinning to the head instead put
//                           the plane wherever the headset happened to be
//                           pointing while the level loaded, and tilted if the
//                           player was looking down at the time.
enum : int { kUiPinNone = 0, kUiPinToView = 1, kUiPinToRuntimeForward = 2 };
std::atomic<int> g_uiPinRequest{kUiPinToRuntimeForward};

// Set once if the quad's own swapchain cannot be created. It turns the capture
// back off (see ui_plane_wanted), which puts the UI back into the render rather
// than leaving the player with no UI at all.
std::atomic<bool> g_uiPlaneFailed{false};

// --- per-eye display canting -------------------------------------------------
//
// Some headsets mount their panels at an outward angle (Valve Index ~5 deg,
// Bigscreen Beyond, several Pimax models); most (Quest 2/3/Pro, Pico) mount
// them parallel. OpenXR reports this as a non-identity ROTATION in
// XrView[eye].pose.orientation relative to the VIEW-space (head) origin --
// vrframework guide 09 §5: the eye term is "mostly a translation of about
// +/-IPD/2 along X (plus a possible small per-headset canting rotation)", and
// its OpenXR.cpp builds eyes[i] from the quaternion FIRST and only then
// overwrites the translation column, so canting rides along for free.
//
// We were reading only the POSITION difference between the two eye poses (for
// IPD) and driving the game camera purely from the head pose -- while
// submitting g.lastView[e].pose, which already CONTAINS the cant. On a canted
// headset that is a live mismatch: content rendered along the head-forward
// axis, declared to the compositor as having been rendered along the canted
// axis. This block closes that gap by feeding the cant into the render too.
//
// Latched once (it is a fixed hardware property; re-reading it per frame would
// only inject tracking noise into the camera -- same reasoning as
// g_cachedBaseHFov below).
// The XrView pair as it stood when the camera hook baked this frame's eye --
// see record_applied_eye_offset(). Read at submit time so the projection layer
// declares the pose the frame was actually rendered from.
//
// EVERY CONSUMER OF THIS GOES THROUGH reference_view_pose(). Read that first --
// the note below is the failure that produced the rule.
//
// The lag: update_head_pose() publishes the pose for display time N, the game
// builds the NEXT frame's camera from it, and that frame is submitted one
// Present later declared with g.lastView -- freshly located for display time
// N+1. The compositor compares a pose to itself, concludes the image is already
// current, and reprojects nothing, so the world sits one render behind the head.
// It drags on a turn and catches up when the next frame lands. Invisible at
// gameplay frame rates, plain on the heavier garage and menu screens, and plain
// anywhere against the UI plane, which is placed at display time and has no
// matching lag to hide behind.
//
// Why declaring this snapshot for AER's freshly-rendered eye makes it WORSE:
// the correction stops being common-mode. Today both eyes declare the same
// pose, so whatever the compositor does it does to both, and the residual shows
// up as world lag rather than as a stereo error. Declare the render pose for the
// fresh eye alone and it gets reprojected by one frame of head rotation while
// the stale eye does not -- the eyes split apart by that angle, growing with how
// fast the head is moving. Measured in-headset as a large inter-eye offset while
// looking around, worse than the lag it removed.
//
// Declaring it for BOTH AER eyes does not rescue it either: warp_stale_eye()
// has already rotated the stale eye toward g.lastView, so telling the
// compositor it is still at the older pose double-counts and it overshoots --
// the same split, mirrored.
//
// WHAT SHIPPED INSTEAD, 2026-08-20: make the warp target and the declared pose
// agree by construction. The stale eye is warped to g_renderView[stale] rather
// than to g.lastView, its retained copy records g_renderView as the instant it
// was captured at, and both eyes are declared at g_renderView. All three now
// name the same instant, so the two eyes stay a consistent pair and the
// compositor's correction is common-mode again -- with the correction actually
// happening, which is what removes the lag. The change is in the WARP as much as
// in the declaration, which is why bolting the declaration on alone could not
// have worked.
SRWLOCK  g_renderViewLock = SRWLOCK_INIT;
XrView   g_renderView[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
bool     g_renderViewValid = false;      // guarded by g_renderViewLock
// SYNCHED SEQUENTIAL: the views both renders of one moment are built from,
// latched at the Present before its first render and held through the
// second -- so the pair shares one head pose as well as one moment. Also
// guarded by g_renderViewLock; record_applied_eye_offset() snapshots these
// instead of g.lastView while they are valid.
XrView   g_ssViews[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
bool     g_ssViewsValid = false;
// The views of the camera build whose image is ON SCREEN this Present -- what
// that image was actually rendered from, and so what both AER and Synched
// Sequential declare for it. Also under g_renderViewLock; set by
// update_on_screen().
XrView   g_onScreenViews[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
bool     g_onScreenViewsValid = false;

SRWLOCK  g_cantLock = SRWLOCK_INIT;
float    g_cantQuat[2][4] = {{0,0,0,1}, {0,0,0,1}};  // per-eye, guarded by g_cantLock
bool     g_cantLatched = false;                      // guarded by g_cantLock
float    g_headRawQuat[4] = {0,0,0,1};               // guarded by g_cantLock
bool     g_headRawValid = false;                     // guarded by g_cantLock

// Synthetic cant, in degrees, for testing the canting path on a headset that
// has none (i.e. almost certainly the machine this is being developed on).
// Applied as an outward yaw of this many degrees per eye to BOTH the rendered
// camera and the submitted pose, so the compositor undoes exactly what the
// render applied. A correct implementation is therefore INVISIBLE at any
// slider value: the 3D world must not move, and the HUD must stay fused. Any
// visible divergence means a sign is wrong somewhere in the chain.
std::atomic<float> g_debugCantDeg{0.0f};

// Timestamp (GetTickCount) of the last time a CAMERA HOOK (cockpit/orbit) ran,
// regardless of whether it actually advanced parity. Present's fallback
// parity-advance uses this to detect "no camera active" (menu/loading screen)
// generically, without needing to identify either state specifically.
std::atomic<DWORD> g_lastCameraActivity{0};
std::atomic<bool> g_worldCursorMode{false};
POINT g_uiCursorSaved{};
bool g_uiCursorSavedValid = false;

bool winch_cursor_active()
{
    return g_worldCursorMode.load();
}

void update_world_cursor_hotkey()
{
    static bool previous = false;
    const bool down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (down && !previous) {
        const bool enabled = !g_worldCursorMode.load();
        if (enabled) {
            g_uiCursorSavedValid = GetCursorPos(&g_uiCursorSaved) != FALSE;
        } else if (g_uiCursorSavedValid) {
            SetCursorPos(g_uiCursorSaved.x, g_uiCursorSaved.y);
            g_uiCursorSavedValid = false;
        }
        g_worldCursorMode.store(enabled);
        VRLOG("cursor mode: %s (F8)", enabled ? "world/winch" : "UI");
    }
    previous = down;
}

// Render-resolution shaping. When enabled, the game backbuffer/window is
// forced to a test size (see desired_render_size_for) instead of the game's
// own configured resolution. 0 XR -> untouched (game keeps its configured
// resolution and renders flat).
//
// RESOLUTION-BOOSTING: STAND DOWN (2026-07-25). Forcing the backbuffer size
// (even to a modest, video.dat-matched 1350x1080) broke UI/rendering, and
// broke the no-headset fallback path too -- confirmed regression against
// today's changes specifically (an older build and the hook fully removed
// both render correctly at the same resolution). Disabled. Do not re-enable
// without isolating exactly which of today's hooks (RT-capture install,
// viewport-field memory patch, backbuffer-size override) caused it -- add
// them back ONE AT A TIME against this known-good baseline, not all together.
std::atomic<bool>  g_forceRenderSize{false};
std::atomic<int>   g_desktopEye{0};     // which eye the flat monitor shows

// THERE IS ONE RENDER PATH: AER, one XR frame per Present, one eye genuinely
// rendered and the other supplied. What used to be a three-way "render method"
// selector is now that path plus two independent options, which is what those
// modes always were underneath -- one of them was measured to be identical to
// this path (see the note where render_frame() used to be) and the other was
// never a different way of driving XR, only a different way of producing the
// un-rendered eye.
//
//   STALE EYE WARP    what the un-rendered eye is built from, and what fills
//                     whatever DIBR shift could not reach. g_warpEnabled plus
//                     g_warpType below.
//   DIBR SHIFT        whether the rendered frame is depth-reprojected into the
//                     other eye at all. g_dibrShift below.
//
// The two compose: the shift lands where it has depth and a source, the warp
// stands everywhere else. Neither is a mode, so neither excludes the other.

// The ONE empirical sign in the DIBR shift path. detour_fna offsets the eye along
// camRight = cross(camUp, fwd), and whether that axis points to SCREEN right is
// a handedness convention of the engine's basis, not something derivable here.
// Everything else about the warp direction comes from the sign of the baked
// offset, so if stereo ever reads inverted this is the single place to flip --
// and flipping it cannot desync from anything else, unlike the old chain of
// swapchain-index / g_sideSign / pipeline-delay-parity conventions.
constexpr float kDibrRightIsScreenRight = +1.0f;
// DIBR SHIFT: reproject the rendered eye into the other one using scene depth,
// instead of leaving that eye entirely to the stale-eye warp. Off by default --
// it needs the depth-capture pipeline (a full-res R32_FLOAT texture plus
// compute dispatches every frame, see depth_probe.h) which nothing else pays
// for, and its failure mode is disocclusion artefacts rather than a clean
// fallback.
std::atomic<bool> g_dibrShift{false};
// DIBR shift as it runs, rather than as set: off while DLSS upscales, because
// it reprojects the game's frame into the other eye on the frame's own pixel
// grid, and the eye images are then on a larger one (upscale_canvas()).
bool dibr_shift_runs();

// Stale-eye warp on/off. Nothing else switches it off, and without this there
// is no way to tell "the warp is wrong" from "what it is warping is wrong".
// Off, the stale eye reuses its last released image: older, but incapable of
// disagreeing with the fresh eye about how far the world turned.
std::atomic<bool> g_warpEnabled{true};   // config is authoritative (WarpEnabled)

// WHAT THE WARP IS BUILT FROM -- one setting, three nested strengths, because
// the two flags this replaced (WarpGameRotation and Stale6Dof) were never
// really independent. Each level contains the one before it:
//
//   kWarpHeadset   the headset pose at capture vs now. A homography, no depth,
//                  always available -- and blind to the game camera, so stick
//                  look and vehicle turns pass through uncorrected.
//   kWarpGameRot   the game's own view-matrix rotation instead, which already
//                  has the head composed into it (camera_hook injects upstream
//                  of the constant buffer) and so covers both halves with no
//                  decomposition to get wrong. Cockpit only: an orbit camera
//                  swings around the truck rather than turning in place, and a
//                  rotation warp cannot represent that, so orbit falls back to
//                  the pose. See warp_homography_for().
//   kWarp6Dof      that, plus a depth reprojection of the retained frame by the
//                  camera's TRANSLATION, which in a driving game is most of the
//                  residual. It supersedes the homography wherever it lands
//                  (it reads the un-warped retained eye and applies the whole
//                  old->new transform itself), so the level below it is what
//                  fills the pixels it cannot reach -- which is why this is a
//                  ladder and not three checkboxes.
//
// Defaults to the headset pose: the stronger levels change what every warped
// frame is built from, and their failure mode is a warp tracking something
// other than your head, which is worse than the under-correction it replaces.
//   kWarpMotion    kWarp6Dof with the game's own motion vectors moving each pixel
//                  (render/motion_field.hpp) where the rigid-truck mask used to
//                  classify it -- the mask and the truck-frame handling were
//                  stand-ins for vectors the engine was believed not to have.
//                  Needs the game's TAA, which draws the velocity buffer; every
//                  frame without a usable field runs kWarp6Dof exactly.
enum WarpType { kWarpHeadset = 0, kWarpGameRot = 1, kWarp6Dof = 2, kWarpMotion = 3 };
std::atomic<int> g_warpType{kWarpMotion};   // config is authoritative


// How many times the rendered-camera warp has run vs fallen back to the pose
// warp, and the worst disagreement seen between the two. Purely diagnostic:
// with the head as the only thing moving, the two must agree to a fraction of
// a degree (measured 2026-08-17: under 0.3). A large steady disagreement there
// means the axis convention below is wrong, which is the one failure mode of
// this feature that does NOT look like an obvious glitch.
std::atomic<uint32_t> g_warpCamUsed{0};
std::atomic<uint32_t> g_warpCamStale{0};
std::atomic<float>    g_warpCamDisagreeDeg{0.0f};

// Warps declined because this is not the cockpit camera -- the orbit camera
// swings around the truck rather than turning in place, which a rotation-only
// warp cannot represent. See cockpit_camera().
std::atomic<uint32_t> g_warpCamOrbit{0};

// --- AER: one XR frame per game Present -----------------------------------
//
// One eye is rendered per Present and the other is synthesized, so both eyes
// carry content from this instant rather than one of them being a frame stale.
// That is the whole of the scheduling; there is no pairing and no second mode.
//
// DECOUPLING THE RENDER RATE FROM THE SUBMIT RATE WAS TRIED AND REMOVED. The
// idea was to run the xrWaitFrame -> xrBeginFrame -> xrEndFrame cycle only when
// a display was actually due, letting the game render faster than the headset
// and submitting whatever the newest images were. It needed an estimate of the
// unblocked render interval to decide "due", and that estimate was a
// self-reinforcing collapse: a submitting Present includes the xrWaitFrame
// block, so blocking raised the average, which lowered the threshold, which
// made more Presents due, which blocked more. Subtracting the measured block
// time out of the interval fixed the arithmetic and not the outcome -- it
// settled at render == submit anyway, which is exactly what the design existed
// to avoid, and cost a pile of state and two QPC reads per Present to get
// there.
//
// So every Present runs a full XR cycle now, and xrWaitFrame paces the game to
// the headset. Worth knowing before reaching for the idea again: the block is
// also what keeps this on ONE thread, and off the D3D11 immediate-context
// hazard that a separate submit thread would create.

// --- head pose sharing (Present thread writes, camera hook reads) ---
SRWLOCK   g_poseLock = SRWLOCK_INIT;
HeadLook  g_headLook;                 // guarded by g_poseLock
float     g_relQuat[4] = {0,0,0,1};   // relative head orientation (x,y,z,w), guarded
// RECENTERING IS THREE SEPARATE THINGS, and they are requested independently
// because the events that want them want different subsets.
//
//   kRecenterPos        take the head's current POSITION as the play-space
//                       centre. Every recenter does this: the position origin
//                       is what the camera reads as a lean, so a stale one
//                       slides the view permanently off-centre.
//   kRecenterYawToView  make the direction the head is looking RIGHT NOW the
//                       game's forward. Only an explicit user recenter does
//                       this -- it is the one that moves the world under you.
//   kRecenterYawReset   adopt the RUNTIME's forward, i.e. drop our yaw origin
//                       back to identity. This is what startup does, so the
//                       game's forward is wherever the player set their play
//                       space up to face rather than wherever the headset
//                       happened to be pointing when the level loaded.
//
// Pitch and roll appear in none of them: they always read as the headset's true
// absolute tilt, so the horizon stays world-locked whatever is recentered.
enum : int {
    kRecenterPos       = 1 << 0,
    kRecenterYawToView = 1 << 1,
    kRecenterYawReset  = 1 << 2,
};
// First valid frame: centre on where the player is, face where their play space
// faces.
std::atomic<int> g_recenterRequest{kRecenterPos | kRecenterYawReset};
XrQuaternionf g_originQuat = {0, 0, 0, 1};
XrVector3f    g_originPos  = {0, 0, 0};
bool      g_haveOrigin = false;

struct Quat { float x, y, z, w; };
struct Vec3 { float x, y, z; };

Quat conj(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }
Quat mul(Quat a, Quat b)
{
    return {a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
            a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
            a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
            a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z};
}
Vec3 rotate(Quat q, Vec3 v)
{
    Quat p{v.x, v.y, v.z, 0};
    Quat r = mul(mul(q, p), conj(q));
    return {r.x, r.y, r.z};
}

// Yaw/pitch/roll of a relative head orientation, in the convention the camera
// hooks consume. Factored out of update_head_pose() so head_look() can re-run
// it on the SAME quaternion with the current eye's cant composed on -- adding
// a head-local cant to already-extracted Euler angles would be wrong the
// moment the head is pitched (a local yaw is not a global yaw), so the cant
// has to be applied as a quaternion and the angles re-derived.
void euler_from_rel(Quat rel, float& yaw, float& pitch, float& roll)
{
    // Forward/up of the relative orientation (OpenXR: -Z forward, +Y up).
    Vec3 fwd = rotate(rel, {0, 0, -1});
    Vec3 up  = rotate(rel, {0, 1, 0});
    yaw   = std::atan2(fwd.x, -fwd.z);
    pitch = std::asin(std::fmax(-1.0f, std::fmin(1.0f, fwd.y)));
    // Roll = angle of `up` about the FORWARD axis, independent of yaw/pitch. The
    // old up.x/up.y form got contaminated when yaw and pitch combined (phantom
    // roll on yaw-then-pitch). Reference frame: right/up derived from world-up.
    Vec3 wUp{0, 1, 0};
    Vec3 rRef{fwd.y*wUp.z - fwd.z*wUp.y, fwd.z*wUp.x - fwd.x*wUp.z, fwd.x*wUp.y - fwd.y*wUp.x}; // cross(fwd,wUp)
    float rlen = std::sqrt(rRef.x*rRef.x + rRef.y*rRef.y + rRef.z*rRef.z);
    roll = 0.0f;
    if (rlen > 1e-4f) {
        rRef.x/=rlen; rRef.y/=rlen; rRef.z/=rlen;
        Vec3 uRef{rRef.y*fwd.z - rRef.z*fwd.y, rRef.z*fwd.x - rRef.x*fwd.z, rRef.x*fwd.y - rRef.y*fwd.x}; // cross(rRef,fwd)
        roll = std::atan2(up.x*rRef.x + up.y*rRef.y + up.z*rRef.z,
                          up.x*uRef.x + up.y*uRef.y + up.z*uRef.z);
    }
}

// --- eye canting helpers (see g_cantQuat above) ------------------------------

// The synthetic test cant for one eye: an OUTWARD yaw about the eye's own up
// axis. OpenXR is right-handed with +Y up and -Z forward, so a positive
// rotation about +Y turns LEFT. Eye 0 is the left eye (see advance_aer_parity),
// whose outward direction is left -> +angle; eye 1 (right) -> -angle.
Quat debug_cant(int eye)
{
    const float deg = g_debugCantDeg.load();
    if (deg == 0.0f) return {0, 0, 0, 1};
    const float half = (eye == 0 ? +deg : -deg) * 0.5f * 0.0174532925f;
    return {0.0f, std::sin(half), 0.0f, std::cos(half)};
}

// The cant the RENDER must apply for `eye`: whatever the hardware reports,
// with the synthetic test cant composed on top. Identity when neither is
// present, which is the case on every headset with parallel panels.
Quat total_cant(int eye)
{
    Quat hw{0, 0, 0, 1};
    AcquireSRWLockShared(&g_cantLock);
    if (g_cantLatched) {
        hw = {g_cantQuat[eye][0], g_cantQuat[eye][1],
              g_cantQuat[eye][2], g_cantQuat[eye][3]};
    }
    ReleaseSRWLockShared(&g_cantLock);
    return mul(hw, debug_cant(eye));
}

bool cant_is_identity(const Quat& q)
{
    // 1 - |w| < 1e-7 is a rotation under ~0.03 deg -- below anything that could
    // matter, and the threshold that lets the no-cant path stay bit-identical.
    //
    // THIS IS A FAST-PATH PREDICATE, not a judgement about whether a cant is
    // large enough to matter to anything. It answers "can this rotation be
    // skipped entirely", so it has to be near-exact. Anything deciding whether
    // a cant is TOLERABLE wants cant_angle_deg() and a real threshold -- see
    // dibr_eyes_canted(), which used this and declined the whole feature on
    // 0.056 deg of measurement noise.
    return std::fabs(q.w) > 0.9999999f;
}

// The cant as an angle, the same figure the latch logs.
float cant_angle_deg(const Quat& q)
{
    float w = std::fabs(q.w);
    if (w > 1.0f) w = 1.0f;
    return 2.0f * std::acos(w) * 57.2957795f;
}

// Latch the per-eye cant from a fresh xrLocateViews result. cant[e] =
// inverse(q_head) * q_eye[e] -- the exact head->eye rotation, with no
// assumption that the two eyes are canted symmetrically. Needs the head
// (VIEW-space) orientation from the same displayTime, stashed by
// update_head_pose(); silently does nothing until that is available, and only
// ever runs once.
void latch_eye_cant(const XrView views[2])
{
    // Plausibility bound. Real panel canting tops out around 5-6 deg (Index,
    // Bigscreen Beyond); nothing ships anywhere near 20. Anything larger is a
    // head/eye pose pair that disagreed -- e.g. sampled mid-reprojection during
    // startup -- and must NOT be latched, because the latch is permanent: a
    // bogus cant yaws the render camera per eye for the rest of the session.
    // Rejecting simply defers to the next frame.
    //
    // (It used to also shift the HUD sideways, via a counter-shift that no
    // longer exists -- the UI is a composition layer now and the compositor
    // places it per eye. A bad latch can no longer slide the UI, so if that
    // symptom is ever reported again the cant is not the cause.)
    constexpr float kMaxPlausibleCantDeg = 20.0f;

    // Measured against the MID-EYE orientation, not the head pose.
    //
    // conj(head) * eye[e] was wrong, and the symptom was in the log all along:
    // both eyes reported an identical 1.657 deg. Canting is by definition
    // opposite between the eyes, so an identical value is not cant at all -- it
    // is a common head-vs-view-space offset the runtime already accounts for
    // when we submit the per-eye view pose to the compositor. Re-applying it to
    // the game camera double-counted it, and the HUD -- screen-space at the
    // time -- was shifted sideways by the same bogus angle on top.
    //
    // Against the mid-eye orientation a parallel-panel headset yields exactly
    // identity, and a genuinely canted one yields the true equal-and-opposite
    // split. Nothing to double-count either way.
    Quat qa{views[0].pose.orientation.x, views[0].pose.orientation.y,
            views[0].pose.orientation.z, views[0].pose.orientation.w};
    Quat qb{views[1].pose.orientation.x, views[1].pose.orientation.y,
            views[1].pose.orientation.z, views[1].pose.orientation.w};
    // q and -q are the same rotation; average without aligning signs and two
    // valid readings can cancel to zero.
    if (qa.x*qb.x + qa.y*qb.y + qa.z*qb.z + qa.w*qb.w < 0.0f) {
        qb.x = -qb.x; qb.y = -qb.y; qb.z = -qb.z; qb.w = -qb.w;
    }
    Quat mid{(qa.x+qb.x)*0.5f, (qa.y+qb.y)*0.5f,
             (qa.z+qb.z)*0.5f, (qa.w+qb.w)*0.5f};
    {
        const float l = std::sqrt(mid.x*mid.x + mid.y*mid.y +
                                  mid.z*mid.z + mid.w*mid.w);
        if (l < 1.0e-6f) return;              // degenerate pair -- try next frame
        mid.x /= l; mid.y /= l; mid.z /= l; mid.w /= l;
    }

    Quat c[2];
    for (int e = 0; e < 2; ++e) {
        const XrQuaternionf& qe = views[e].pose.orientation;
        c[e] = mul(conj(mid), Quat{qe.x, qe.y, qe.z, qe.w});
        float w = c[e].w; if (w > 1.0f) w = 1.0f; if (w < -1.0f) w = -1.0f;
        const float deg = 2.0f * std::acos(std::fabs(w)) * 57.2957795f;
        if (deg > kMaxPlausibleCantDeg) {
            static std::atomic<bool> warned{false};
            bool e2 = false;
            if (warned.compare_exchange_strong(e2, true))
                VRLOG("eye cant: implausible reading rejected (eye %d, %.2f deg) -- retrying",
                      e, deg);
            return;
        }
    }

    // Latch a SETTLED value, not the first valid one.
    //
    // The old latch fired on frame one and was permanent for the session, so a
    // reading taken while tracking was still converging stuck for the whole run
    // -- which is why a manual recenter (which re-arms the latch) fixed it and
    // nothing else did. Requiring the same value repeatedly before committing
    // makes that self-correcting: an unsettled reading simply fails to repeat.
    constexpr int   kStableFrames = 90;      // ~1 s at headset rates
    constexpr float kStableTolDeg = 0.20f;
    static Quat s_cand[2] = {{0,0,0,1},{0,0,0,1}};
    static int  s_stable = 0;

    AcquireSRWLockExclusive(&g_cantLock);
    if (g_cantLatched) { ReleaseSRWLockExclusive(&g_cantLock); return; }

    bool agrees = (s_stable > 0);
    for (int e = 0; e < 2 && agrees; ++e) {
        Quat d = mul(conj(s_cand[e]), c[e]);
        float w = d.w; if (w > 1.0f) w = 1.0f; if (w < -1.0f) w = -1.0f;
        if (2.0f * std::acos(std::fabs(w)) * 57.2957795f > kStableTolDeg)
            agrees = false;
    }
    if (agrees) {
        ++s_stable;
    } else {
        s_cand[0] = c[0]; s_cand[1] = c[1];
        s_stable = 1;
    }
    if (s_stable < kStableFrames) { ReleaseSRWLockExclusive(&g_cantLock); return; }

    for (int e = 0; e < 2; ++e) {
        g_cantQuat[e][0] = c[e].x; g_cantQuat[e][1] = c[e].y;
        g_cantQuat[e][2] = c[e].z; g_cantQuat[e][3] = c[e].w;
    }
    g_cantLatched = true;
    s_stable = 0;                             // re-arm cleanly for a re-latch
    ReleaseSRWLockExclusive(&g_cantLock);

    // Log it unconditionally, including the "0.00 deg, parallel panels" case --
    // "we measured it and there is none" is a materially different (and far
    // more useful) report than silence when someone sends in a log.
    for (int e = 0; e < 2; ++e) {
        Quat c{g_cantQuat[e][0], g_cantQuat[e][1], g_cantQuat[e][2], g_cantQuat[e][3]};
        float w = c.w; if (w > 1.0f) w = 1.0f; if (w < -1.0f) w = -1.0f;
        const float angDeg = 2.0f * std::acos(std::fabs(w)) * 57.2957795f;
        // Yaw of the cant on its own (the component that actually matters for
        // panel canting; pitch/roll are reported too in case a runtime does
        // something unexpected).
        Vec3 f = rotate(c, {0, 0, -1});
        Vec3 u = rotate(c, {0, 1, 0});
        VRLOG("eye cant[%d]: %.4f deg total (yaw=%.4f pitch=%.4f roll=%.4f deg)",
              e, angDeg,
              std::atan2(f.x, -f.z) * 57.2957795f,
              std::asin(std::fmax(-1.0f, std::fmin(1.0f, f.y))) * 57.2957795f,
              std::atan2(u.x, u.y) * 57.2957795f);
    }
}

// Row-major 3x3 (m[r*3+c]) for the 2x-mode rotation warp's homography.
struct Mat3 { float m[9]; };

void mat3_mul(const float A[9], const float B[9], float out[9])
{
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            float s = 0.0f;
            for (int k = 0; k < 3; ++k) s += A[r*3+k] * B[k*3+c];
            out[r*3+c] = s;
        }
}

// Homography for a pure camera ROTATION between two symmetric-frustum
// perspective views: H = K_old * R_rel * K_new^-1. Applied (via three dot
// products, see the warp pixel shader) to a homogeneous coordinate built
// from the CURRENT image's pixel, this projects to where that same world ray
// appears in the OLD (retained) image. Two separate K's -- not one shared --
// because FOV can differ between when the retained eye was captured and now
// (composited_fov_scale() changes with the render size slider).
// cx=cy=0.5 is safe specifically because this file always submits symmetric
// frusta (angleLeft=-angleRight, angleUp=-angleDown) -- not a general
// assumption. Also returns the relative rotation angle (radians) via
// outRelAngleRad, so the caller can skip the warp on a discontinuous camera
// jump (teleport/vehicle swap) instead of warping a plausible-looking wrong
// scene.
// The rendered camera's world->camera rotation, as the game's own view matrix
// carries it: row-major 3x3 in the upper-left, exactly the block dibr_projection
// transposes to invert. One value from one lock (cbuffer_hook's g_camSnapLock
// covers view and viewProj together), which is the whole reason this can be
// used where the 2026-08-09 attempt could not -- that one needed
// projBasis * cameraBufferView, two values published at different rates whose
// composite jumped whenever one updated without the other.
// Rotation only. A camera POSITION is deliberately not derived here: the main
// render view is CAMERA-RELATIVE (commit f14ff31 -- the engine renders with the
// camera at the origin), so -R^T*t out of this matrix is not a world position,
// and where it is meaningful it carries the AER +-IPD/2, which alternates every
// frame and reads as half an IPD of phantom camera motion between one eye's
// capture and the other's warp. Whether the camera is turning in place is
// settled by the camera MODE instead; see cockpit_camera().
bool rendered_cam_rot(float out9[9])
{
    float view[16], viewProj[16];
    if (!hooks::main_camera_matrices(view, viewProj)) return false;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out9[r*3+c] = view[r*4+c];
    return true;
}

// R_rel maps a direction in the NEW camera's frame to the OLD camera's frame,
// which is exactly what the homography's R term needs (the pixel shader walks
// current-image pixels back to where they sat in the retained image).
//
// With M = world->camera, a world ray is M_old*v in the old frame and M_new*v
// in the new one, so R_rel = M_old * M_new^T.
//
// AXIS CONVENTION. The game's camera looks down +Z -- dibr_projection's
// reverse-Z fit (z_ndc = A + B/z_view with z_view positive in front) only
// works out that way -- while OpenXR looks down -Z. The two frames therefore
// differ by F = diag(1,1,-1), and a rotation transforms as F*R*F.
//
// Called by warp_homography_for() at kWarpGameRot and above. The convention is
// checked live rather than asserted: that function records the disagreement
// between this rotation's magnitude and the head pose's into
// g_warpCamDisagreeDeg, which stays under a degree if F is right and blows up
// if it is not.
void relative_cam_rotation(const float mOld[9], const float mNew[9], bool flipZ,
                           float outR[9], float* outAngleRad)
{
    float mNewT[9];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) mNewT[r*3+c] = mNew[c*3+r];
    mat3_mul(mOld, mNewT, outR);

    if (flipZ) {
        // F*R*F with F = diag(1,1,-1) negates exactly the elements where the
        // Z index appears once.
        outR[2] = -outR[2]; outR[5] = -outR[5];
        outR[6] = -outR[6]; outR[7] = -outR[7];
    }

    if (outAngleRad) {
        // trace(R) = 1 + 2cos(theta)
        float t = outR[0] + outR[4] + outR[8];
        float c = 0.5f * (t - 1.0f);
        if (c > 1.0f) c = 1.0f; if (c < -1.0f) c = -1.0f;
        *outAngleRad = std::acos(c);
    }
}

// Same construction as the quaternion form below, but taking the rotation
// matrix directly. Kept as one body so the two paths cannot drift.
// THE WARP'S VIEW OF AN EYE: half-angles of the tangent SPAN (not of the
// enclosing symmetric frustum -- for an off-centre eye those differ) plus the
// principal point. Derived from render_eye_frustum_tan() so the warp cannot
// disagree with what was rendered.
//
// Symmetric case: hh == the old half-angle and cx == 0.5 exactly, so this is
// bit-identical to what the warp used before off-centre existed.
bool eye_warp_intrinsics(int eye, float& hh, float& vh, float& cx, float& cy)
{
    float tl, tr, tup, tdn;
    if (!render_eye_frustum_tan(eye, tl, tr, tup, tdn)) return false;
    const float dx = tr - tl, dy = tup - tdn;
    if (!(dx > 1.0e-4f) || !(dy > 1.0e-4f)) return false;
    hh = std::atan(dx * 0.5f);
    vh = std::atan(dy * 0.5f);
    cx = -tl / dx;
    cy =  tup / dy;
    return true;
}

// cx/cy are the PRINCIPAL POINT in normalised image coords -- where the eye's
// optical axis lands in the picture. 0.5, 0.5 for a symmetric frustum, which is
// what these two builders assumed outright until off-centre projection made it
// possible for the axis to sit somewhere else. Getting it wrong does not shift
// the image; it rotates it about the wrong point, so the error is zero at the
// centre and grows toward the edges -- the hardest kind to notice and the
// easiest to blame on something else.
Mat3 warp_homography_from_R(const float R[9], float hhOld, float vhOld,
                            float hhNew, float vhNew, float cx, float cy)
{
    const float fxOld = 0.5f / std::tan(hhOld), fyOld = 0.5f / std::tan(vhOld);
    const float fxNew = 0.5f / std::tan(hhNew), fyNew = 0.5f / std::tan(vhNew);
    const float Kold[9] = {
        fxOld,  0.0f,   -cx,
        0.0f,  -fyOld,  -cy,
        0.0f,   0.0f,   -1.0f,
    };
    const float KnewInv[9] = {
        1.0f/fxNew,  0.0f,        -cx/fxNew,
        0.0f,       -1.0f/fyNew,   cy/fyNew,
        0.0f,        0.0f,        -1.0f,
    };
    float RK[9]; mat3_mul(R, KnewInv, RK);
    Mat3 H{}; mat3_mul(Kold, RK, H.m);
    return H;
}

Mat3 compute_eye_warp_homography(const float qOld[4], float hhOld, float vhOld,
                                 const float qNew[4], float hhNew, float vhNew,
                                 float* outRelAngleRad, float cx = 0.5f,
                                 float cy = 0.5f)
{
    Quat qo{qOld[0], qOld[1], qOld[2], qOld[3]};
    Quat qn{qNew[0], qNew[1], qNew[2], qNew[3]};
    Quat qRel = mul(conj(qo), qn);   // NOT mul(conj(qn),qo) -- that's the exact inverse, warps backwards

    if (outRelAngleRad) {
        float w = qRel.w;
        if (w > 1.0f) w = 1.0f; if (w < -1.0f) w = -1.0f;
        *outRelAngleRad = 2.0f * std::acos(std::fabs(w));
    }

    Vec3 c0 = rotate(qRel, {1,0,0});
    Vec3 c1 = rotate(qRel, {0,1,0});
    Vec3 c2 = rotate(qRel, {0,0,1});
    float R[9] = {
        c0.x, c1.x, c2.x,
        c0.y, c1.y, c2.y,
        c0.z, c1.z, c2.z,
    };

    const float fxOld = 0.5f / std::tan(hhOld), fyOld = 0.5f / std::tan(vhOld);
    const float fxNew = 0.5f / std::tan(hhNew), fyNew = 0.5f / std::tan(vhNew);

    const float Kold[9] = {
        fxOld,  0.0f,   -cx,
        0.0f,  -fyOld,  -cy,
        0.0f,   0.0f,   -1.0f,
    };
    const float KnewInv[9] = {
        1.0f/fxNew,  0.0f,        -cx/fxNew,
        0.0f,       -1.0f/fyNew,   cy/fyNew,
        0.0f,        0.0f,        -1.0f,
    };

    float RK[9]; mat3_mul(R, KnewInv, RK);
    Mat3 H{}; mat3_mul(Kold, RK, H.m);
    return H;
}

struct XrState {
    XrInstance   instance   = XR_NULL_HANDLE;
    XrSystemId   systemId   = XR_NULL_SYSTEM_ID;
    XrSession    session    = XR_NULL_HANDLE;
    XrSpace      space      = XR_NULL_HANDLE;
    XrSwapchain  swapchain[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};  // one per eye (AER)
    XrSpace      viewSpace  = XR_NULL_HANDLE;  // head pose, located in `space`
    XrSessionState state    = XR_SESSION_STATE_UNKNOWN;
    bool running            = false;

    int64_t swapFormat = 0;
    // The eye images (the swapchains), and the game's frame. The same size,
    // except when DLSS upscales: then the frame is the render size and the
    // eye images are what DLSS brings it up to -- see upscale_canvas().
    uint32_t width = 0, height = 0;
    uint32_t bbWidth = 0, bbHeight = 0;

    std::vector<ComPtr<ID3D11RenderTargetView>> rtvs[2];  // per eye, one per XR image
    XrView   lastView[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};  // last located eye poses/FOVs
    bool     eyeFresh[2] = {false, false};  // this eye's swapchain image was (re)drawn THIS Present -- reset every Present
    bool     eyeEverRendered[2] = {false, false};  // has EVER been drawn -- never reset (avoids
                                                    // submitting a black layer on a stall; the
                                                    // swapchain image still holds its last content)
    uint32_t curEye = 0;                     // eye of the CURRENT backbuffer
    float    ipd = 0.064f;

    // Sub-rect of the (fixed-size) XR image actually drawn into for each eye, and
    // the source aspect that produced it -- when the blit source's aspect doesn't
    // match g.width/g.height (e.g. a square captured render vs the swapchain's
    // own shape), the blit is letterboxed into a centred sub-rect rather than
    // stretched, and only that sub-rect is submitted to the compositor. Persists
    // per-eye like eyeFresh/lastView (the stale eye is resubmitted as-is).
    // {0,0},{0,0} = not yet drawn -> callers fall back to the full image.
    XrRect2Di blitRect[2] = {};

    // SYNCHED SEQUENTIAL. Each eye's finished image waits here -- composited
    // exactly as AER would put it in the swapchain -- until the other render
    // of the same moment arrives; then both go into the swapchains together.
    // Released one at a time, a new left eye would sit beside the previous
    // pair's right eye for a frame, which is AER again.
    ComPtr<ID3D11Texture2D>        ssPending[2];
    ComPtr<ID3D11RenderTargetView> ssPendingRtv[2];
    uint32_t  ssPendingPair[2]  = {0, 0};
    bool      ssPendingValid[2] = {false, false};
    XrView    ssPendingView[2][2] = {{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}, {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
    XrRect2Di ssPendingRect[2]  = {};
    // The pair in the swapchains, and the views it was rendered from -- what
    // every Present declares until the next pair is complete.
    bool      ssCommitted = false;
    XrView    ssCommittedView[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};

    // Blit pipeline (backbuffer -> XR image, handles BGRA/size differences).
    ComPtr<ID3D11Device>          device;
    ComPtr<ID3D11DeviceContext>   ctx;
    ComPtr<ID3D11VertexShader>    vs;
    ComPtr<ID3D11PixelShader>     ps;
    ComPtr<ID3D11SamplerState>    sampler;
    // shiftUv + smudgeOn for the blit shader's splatter composite.
    ComPtr<ID3D11Buffer>          blitCb;
    ComPtr<ID3D11Texture2D>       staging;      // SRV-capable copy of the blit SOURCE (backbuffer
                                                 // or, when RT capture is active, the captured
                                                 // pre-downscale scene texture -- its size can
                                                 // differ from width/height; the blit shader
                                                 // resamples, so no size relation is required).
    ComPtr<ID3D11ShaderResourceView> stagingSrv;
    DXGI_FORMAT stagingFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t stagingW = 0, stagingH = 0;

    // Per-eye retained "last real render", for the 2x-mode rotation warp: the
    // eye NOT freshly rendered this Present gets ITS OWN last real render
    // warped forward to the current head orientation instead of resubmitted
    // unchanged. Sized/formatted like `staging` (the un-letterboxed blit
    // source), NOT the swapchain -- so the retained texture never bakes a
    // letterbox bar in as if it were scene content.
    ComPtr<ID3D11Texture2D>          prevEyeTex[2];
    ComPtr<ID3D11ShaderResourceView> prevEyeSrv[2];
    // A second view of the SAME typeless texture, in the plain (non-sRGB)
    // format. prevEyeSrv above is sRGB, which is correct for warp_stale_eye()
    // because it writes into the eye swapchain and that IS sRGB, so the
    // decode-on-read and encode-on-write cancel. DIBR shift is a RAW pixel mover --
    // its source is a straight CopyResource of the backbuffer -- so a fill
    // sampled through the sRGB view would arrive linear-valued and read far too
    // dark (near black in a dim scene). See stale_eye_fill().
    ComPtr<ID3D11ShaderResourceView> prevEyeSrvRaw[2];
    DXGI_FORMAT prevEyeFormat[2] = {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};
    uint32_t    prevEyeW[2] = {0, 0}, prevEyeH[2] = {0, 0};
    bool        prevEyeValid[2] = {false, false};   // false until that eye's first real render
    float       prevEyeOrient[2][4] = {{0,0,0,1}, {0,0,0,1}};  // orientation AT CAPTURE, as rendered (rendered_orientation())
    float       prevEyeHalfAngleH[2] = {0, 0}, prevEyeHalfAngleV[2] = {0, 0};  // frustum half-angles AT CAPTURE
    // Principal point AT CAPTURE, 0.5/0.5 unless the eye was rendered
    // off-centre. Stored beside the angles because they describe one frustum
    // between them and a warp built from half of each is a warp about the
    // wrong point.
    float       prevEyeCx[2] = {0.5f, 0.5f}, prevEyeCy[2] = {0.5f, 0.5f};
    // The RENDERED camera's world->camera rotation at capture, from the
    // game's own view matrix. Snapshotted alongside the head pose because
    // the stick rotates this without touching the head pose at all -- see
    // warp_homography_for() for why that matters.
    float       prevEyeCamRot[2][9] = {};
    bool        prevEyeCamValid[2] = {false, false};

    // Warp pipeline (rotation-only reprojection of prevEyeTex -> current pose).
    ComPtr<ID3D11PixelShader>   warpPs;      // reuses g.vs, no new vertex shader needed
    ComPtr<ID3D11SamplerState>  warpSampler; // BORDER addressing, black border -- unlike g.sampler (CLAMP)
    ComPtr<ID3D11Buffer>        warpCb;      // dynamic, 3x float4 homography rows

    // DIBR shift disocclusion fill: the stale eye's last real render, rotation-warped
    // to the current pose. See stale_eye_fill().
    ComPtr<ID3D11Texture2D>          dibrFill;
    ComPtr<ID3D11RenderTargetView>   dibrFillRtv;
    ComPtr<ID3D11ShaderResourceView> dibrFillSrv;
    // UAV on the same texture, so reproject::composite() can write OVER the
    // rotation-warped result rather than into a second buffer.
    ComPtr<ID3D11UnorderedAccessView> dibrFillUav;

    // Per-eye retained DEPTH, and the game matrices the retained frame was
    // rendered with. Together these are what turns the rotation-only warp into
    // a full camera reprojection -- a translation moves near pixels further
    // than far ones, so it cannot be done without per-pixel depth.
    // Intermediate for the AER whole-eye stale warp when 6-DoF is on. The XR
    // swapchain is created COLOR_ATTACHMENT only, so a compute shader cannot
    // write into it; the rotation warp goes here first, the reprojection
    // composites over it, and the result is then drawn to the swapchain.
    //
    // THE VIEW FORMATS ARE THE WHOLE POINT, and mirror what prevEyeTex does:
    //   RTV/SRV sRGB  -- the rotation warp samples an sRGB view and writes
    //                    through an sRGB target, so decode and encode cancel,
    //                    exactly as they did drawing straight to the swapchain.
    //   UAV     raw   -- a UAV cannot be sRGB. The reprojection therefore reads
    //                    prevEyeSrvRaw and writes raw bits, so it deposits the
    //                    same encoded values the rotation warp did.
    // Getting this wrong is not subtle and has bitten this file before: sampling
    // sRGB and writing raw (or the reverse) made an earlier fill read near-black.
    ComPtr<ID3D11Texture2D>           staleWarpTex;
    ComPtr<ID3D11RenderTargetView>    staleWarpRtv;
    ComPtr<ID3D11ShaderResourceView>  staleWarpSrv;
    ComPtr<ID3D11UnorderedAccessView> staleWarpUav;
    UINT staleWarpW = 0, staleWarpH = 0;
    DXGI_FORMAT staleWarpFormat = DXGI_FORMAT_UNKNOWN;

    ComPtr<ID3D11Texture2D>          prevEyeDepth[2];
    ComPtr<ID3D11ShaderResourceView> prevEyeDepthSrv[2];
    UINT     prevEyeDepthW[2] = {0, 0}, prevEyeDepthH[2] = {0, 0};
    // The camera-rigid coverage mask that belongs to the retained frame. Kept
    // per eye for the same reason the depth is: rigidmask's live texture is
    // cleared every Present and describes whichever eye the game rendered
    // last, which is not the eye being synthesized.
    // Valid only when copied with this eye's latest retain: the mask is not
    // drawn at all while the motion-vector warp's fields are arriving.
    ComPtr<ID3D11Texture2D>          prevEyeRigid[2];
    ComPtr<ID3D11ShaderResourceView> prevEyeRigidSrv[2];
    UINT     prevEyeRigidW[2] = {0, 0}, prevEyeRigidH[2] = {0, 0};
    bool     prevEyeRigidValid[2] = {false, false};
    // The retained frame's MIRRORS, for the motion-vector warp, which leaves
    // them to the rotation-only base (render/reproject.hpp, mirrorMaskSrv).
    // Valid only when copied with this eye's latest retain.
    ComPtr<ID3D11Texture2D>          prevEyeMirror[2];
    ComPtr<ID3D11ShaderResourceView> prevEyeMirrorSrv[2];
    UINT     prevEyeMirrorW[2] = {0, 0}, prevEyeMirrorH[2] = {0, 0};
    bool     prevEyeMirrorValid[2] = {false, false};
    float    prevEyeViewProj[2][16] = {};
    // The VIEW as well as the viewProj: the rigid branch needs the camera's
    // position at the retained instant, and only the view carries it.
    float    prevEyeView[2][16] = {};
    // The projection frozen beside this depth, for reading its reverse-Z back
    // as view metres. Taken from the depth slot rather than the live camera so
    // the two cannot describe different frusta.
    float    prevEyeProjA[2] = {0.0f, 0.0f};
    float    prevEyeProjB[2] = {0.1f, 0.1f};
    bool     prevEyeProjValid[2] = {false, false};
    bool     prevEyeMatricesValid[2] = {false, false};
    // The camera build the retained image was drawn with (0 = not known): the
    // motion-vector warp only applies a field measured against THIS image.
    uint32_t prevEyeBuild[2] = {0, 0};
    DXGI_FORMAT dibrFillFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t    dibrFillW = 0, dibrFillH = 0;

    ComPtr<ID3D11Texture2D>       desktopSave;  // last desktop-eye frame (single-eye monitor)
    DXGI_FORMAT desktopSaveFormat = DXGI_FORMAT_UNKNOWN;  // ALWAYS matches the swapchain
    uint32_t desktopSaveW = 0, desktopSaveH = 0;          // backbuffer exactly (raw CopyResource)
    HWND outputWindow = nullptr;

    // Menu overlay quad layer (headset-visible mod GUI): a separate, small XR
    // swapchain distinct from the eye swapchains above. Recreated whenever
    // hooks::menu_wants_quad()'s reported size changes (the desktop window
    // resized). Only acquired/rendered/submitted on Presents where the menu
    // is actually open.
    XrSwapchain menuSwapchain = XR_NULL_HANDLE;
    std::vector<ComPtr<ID3D11RenderTargetView>> menuRtvs;
    uint32_t menuSwapW = 0, menuSwapH = 0;
    int64_t menuSwapFormat = 0;

    // UI overlay quad layer (the GAME's HUD/menus/map, redirected out of the
    // render -- see render/ui_layer.hpp). Same shape as the menu quad above and
    // for the same reasons, but sized to the capture target rather than to the
    // desktop window, and rendered by a straight copy of that capture rather
    // than by ImGui.
    XrSwapchain uiSwapchain = XR_NULL_HANDLE;
    std::vector<ComPtr<ID3D11RenderTargetView>> uiRtvs;
    uint32_t uiSwapW = 0, uiSwapH = 0;
    int64_t uiSwapFormat = 0;
    ComPtr<ID3D11PixelShader> uiCopyPs;   // reuses g.vs, like the warp shader does
    // The UI is captured from one eye's frames only, so on the frames in
    // between there is nothing to blit and the layer simply re-submits the
    // image it last released -- the same trick the stale eye uses. These two
    // say whether there IS such an image, and how long it has gone unrefreshed.
    bool uiQuadImageValid = false;
    int  uiQuadIdle = 0;
} g;

bool upscaled_canvas() { return g.bbWidth && (g.width != g.bbWidth || g.height != g.bbHeight); }
bool dibr_shift_runs() { return g_dibrShift.load() && !upscaled_canvas(); }

// --- DIBR shift projection parameters, derived live from the game's own camera CB ---
// proj = viewProj * view^-1; the depth row gives z_ndc = A + B/z_view, and
// proj00 gives the horizontal focal length. Measured on this build: A=-2.9e-5,
// B=0.1 (near plane), proj00=0.73454 -> focal 987 px at 2688 wide.
// The derivation itself lives in common/dibr_policy.h so that this live read and
// the snapshot depth_probe freezes beside each capture cannot drift apart --
// two copies would disagree only during camera transitions, which is exactly
// when it matters and exactly when it is hardest to notice.
bool dibr_projection(float& a, float& b, float& p00)
{
    float view[16], viewProj[16];
    if (!hooks::main_camera_matrices(view, viewProj)) return false;
    return dibrpolicy::derive_projection(view, viewProj, a, b, p00);
}

// Last projection triple that passed validation.
//
// dibr_projection() reads the game's camera CB live, and that read can
// transiently yield nonsense -- a camera/FOV transition, or a CB holding some
// other pass's matrix. It already detected that and returned false. The three
// accessors that used to wrap it (dibr_proj_a/_b/dibr_focal_px) each threw the
// flag away, and had ALREADY been handed the bad numbers by reference: the
// sane defaults they set up locally were overwritten by exactly the values the
// check existed to reject.
//
// What that cost, measured from the 2026-08-16 frame dump: projB near zero
// collapses z_view, every pixel's disparity saturates the 512px clamp in
// dibr.cpp, and the "synthesized" eye becomes a rigid 512px translation of the
// rendered one -- pixel-identical either side of a seam sitting exactly 256px
// (the dump is half-res) from the trailing edge, in every frame of the burst.
// In the headset that is a full-frame stereo error that persists for as long
// as the bad matrix does, and NOTHING said so in the log.
//
// So: derived ONCE per frame rather than three times -- three separate calls
// could also observe different CB contents and pair A from one projection with
// B from another -- validated, and published only on success. A frame that
// fails validation reuses the last good triple; until there has been a good
// one, DIBR shift declines the frame and the stale-eye warp covers it.
//
// The reuse is BOUNDED. Depth keeps being captured every frame whether or not
// the camera CB reads cleanly, so a projection held across a long failure is
// paired with depth from a camera it no longer describes -- and the disparity
// is built from the two together, so a stale A/B/p00 with fresh depth is
// wrong in a way neither input can reveal on its own. A transient bad read
// (an FOV animation, a CB holding another pass's matrix) lasts a frame or
// two; a failure lasting longer than that is not transient, and the honest
// answer there is the one the never-had-a-good-one case already gives --
// decline, and let the stale-eye warp cover the frame.

struct DibrProj {
    float a = 0.0f, b = 0.1f, p00 = 0.73454f;
    bool valid = false;
    // Consecutive refreshes that fell back on this triple rather than
    // replacing it. Reset on every good read, so it measures the age of what
    // is being reused and not the total failure count.
    uint32_t reused = 0;
};
DibrProj g_dibrProj;

bool dibr_projection_refresh()
{
    float a = 0.0f, b = 0.1f, p00 = 0.73454f;
    // Ranges are deliberately loose -- this is a nonsense filter, not a
    // calibration. Measured on this build: a=-2.9e-5, b=0.1, p00=0.73454.
    const bool ok = dibr_projection(a, b, p00) &&
                    dibrpolicy::projection_plausible(a, b, p00);
    if (ok) {
        g_dibrProj.a = a; g_dibrProj.b = b; g_dibrProj.p00 = p00;
        g_dibrProj.valid = true;
        g_dibrProj.reused = 0;
        return true;
    }
    // Expire the held triple once it has covered more frames than a transition
    // plausibly lasts. Logged as its own event: "reusing last good" and "the
    // last good one has now been dropped" are different situations, and the
    // second is the one that explains DIBR shift going quiet.
    if (g_dibrProj.valid &&
        dibrpolicy::projection_reuse_expired(++g_dibrProj.reused)) {
        g_dibrProj.valid = false;
        VRLOG("DIBR shift: last-good projection expired after %u reused Presents "
              "(A=%.6g B=%.6g p00=%.6g still rejected) -- warp declines until "
              "the camera CB reads cleanly again",
              dibrpolicy::kMaxProjectionReusePresents, a, b, p00);
    }
    // Rate-limited rather than once-only: the failure that prompted this ran
    // for minutes, and "it is still happening" is the thing worth knowing.
    static std::atomic<uint32_t> bad{0};
    const uint32_t n = bad.fetch_add(1) + 1;
    if (n == 1 || (n % 600) == 0)
        VRLOG("DIBR shift: projection rejected (A=%.6g B=%.6g p00=%.6g) -- %s (%u so far)",
              a, b, p00, g_dibrProj.valid ? "reusing last good" : "warp skipped", n);
    return false;
}

// --- DIBR shift per-frame decision: counted, not logged once ------------------------
//
// The decision itself -- the enum, the ladder and its ORDER, and the two text
// forms -- lives in common/dibr_policy.h so it can be tested without a headset
// or a D3D device (tests/dibr_policy_tests.cpp). What stays here is only the
// part that needs process state: the counters and the periodic report.
//
// Counting rather than one-shot logging is the point. The four `if (!logged)`
// latches this replaced had a specific, bad failure mode: a decline that
// STARTS partway into a session -- a camera transition, a truck swap, an FOV
// animation -- printed nothing at all, because the latch had either already
// been spent or never fired at startup when the condition was absent. The one
// guard that did keep a count (the colour/depth eye mismatch) is the one whose
// scale we actually know, which is not a coincidence.
using dibrpolicy::Decision;

std::atomic<uint32_t> g_dibrDecisions[(size_t)Decision::Count]{};

void note_dibr_decision(Decision d)
{
    g_dibrDecisions[(size_t)d].fetch_add(1, std::memory_order_relaxed);
}

// SILENT WHILE THE WARP IS HEALTHY, which is the whole design constraint: this
// file has already deleted one periodic status line for burying the lines that
// matter. A window in which every frame warped prints nothing. A window with
// any decline in it prints exactly one line, naming only the reasons that
// actually occurred and how many times.
//
// Called once per Present. Records nothing while DIBR shift is off, so the counters
// stay empty and it stays quiet in AER.
void report_dibr_decisions()
{
    constexpr DWORD kPeriodMs = 10000;
    static DWORD last = GetTickCount();
    const DWORD now = GetTickCount();
    if (now - last < kPeriodMs) return;
    last = now;

    uint32_t n[(size_t)Decision::Count];
    uint32_t total = 0, declined = 0;
    for (size_t i = 0; i < (size_t)Decision::Count; ++i) {
        n[i] = g_dibrDecisions[i].exchange(0, std::memory_order_relaxed);
        total += n[i];
    }
    for (size_t i = dibrpolicy::kFirstDecline; i < (size_t)Decision::Count; ++i)
        declined += n[i];
    if (total == 0 || declined == 0) return;

    char line[512];
    int p = _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "DIBR shift %lus: %u/%u warped",
                        (unsigned long)(kPeriodMs / 1000),
                        n[(size_t)Decision::Warped] +
                            n[(size_t)Decision::WarpedPreHud],
                        total);
    for (size_t i = dibrpolicy::kFirstDecline; i < (size_t)Decision::Count && p > 0; ++i) {
        if (!n[i]) continue;
        const int w = _snprintf_s(line + p, sizeof(line) - p, _TRUNCATE,
                                  " | %s x%u",
                                  dibrpolicy::decision_tag((Decision)i), n[i]);
        if (w < 0) break;      // truncated; the reasons already named are enough
        p += w;
    }
    VRLOG("%s", line);
}

// Canted panels break the warp's founding assumption. dibr.hpp states the
// contract as dest_y = src_y EXACTLY, which holds only while the two rendered
// eyes are parallel -- and they are not, because total_cant() is folded into
// the per-eye render rotation (see head_look_for_eye). A canted headset would
// therefore get a synthesized eye carrying VERTICAL disparity, which is the
// one stereo error the visual system cannot fuse: it does not read as "wrong",
// it reads as eye strain.
//
// Witcher 3 VR refuses DIBR shift above 0.25 deg of runtime cant for the same
// reason, and that is the threshold used here. Ours declines per frame instead
// of at init, so the debug cant slider can be moved mid-session and the warp
// follows it.
//
// A REAL THRESHOLD, not cant_is_identity(). That predicate answers "is this
// rotation skippable" and is near-exact by design (~0.03 deg), which is far
// below the measurement noise in inverse(q_head) * q_eye. Measured 2026-08-24
// on parallel panels: the runtime reported 0.0560 deg per side with yaw, pitch
// and roll all printing 0.0000 -- i.e. nothing but float noise -- and the whole
// feature declined for the session on it. Every frame, every screen, silently
// enough that it read as "DIBR does nothing".
//
// What the tolerance costs is bounded and tiny: the vertical disparity a cant
// of t introduces is d*sin(t), so at 0.25 deg and 150 px of near-field
// disparity it is 0.65 px, against the several-pixel errors the warp already
// tolerates everywhere. At the noise floor that produced this it is 0.15 px.
constexpr float kMaxDibrCantDeg = 0.25f;
bool dibr_eyes_canted()
{
    return cant_angle_deg(total_cant(0)) > kMaxDibrCantDeg ||
           cant_angle_deg(total_cant(1)) > kMaxDibrCantDeg;
}

#define XR_TRY(expr, what)                                             \
    do {                                                              \
        XrResult _r = (expr);                                         \
        if (XR_FAILED(_r)) {                                          \
            VRLOG("XR fail (%d) at %s", int(_r), what);               \
            return false;                                             \
        }                                                            \
    } while (0)

// The blit also composites the windscreen-splatter layer, because this is the
// one place BOTH eyes pass through -- the rendered one and the synthesized one
// alike -- so a single shift parameter covers both cases. See smudge_layer.hpp.
//
// shiftUv is 0 for the eye that was actually rendered (its splatter goes back
// exactly where the game drew it) and the glass's disparity for the eye being
// synthesized. Sampling at uv - shift moves the content by +shift, matching the
// direction convention dibr.cpp's scatter uses.
//
// The explicit range test is deliberate: a shifted sample can fall outside the
// layer, and relying on the sampler's address mode to be CLAMP would smear the
// edge column across the frame if it ever changed to WRAP.
// The disparity is computed PER PIXEL from the windscreen depth we recorded
// ourselves (smudge_layer.hpp), because a windscreen is raked and curved -- its
// top is much further from the eye than its bottom, so no single distance can
// place splatter correctly across the whole screen.
//
//   z_view   = projB / (d - projA)
//   disp_px  = eyeSign * ipd * focalPx / z_view
//            = k * (d - projA),   k = eyeSign * ipd * focalPx / projB
//
// which is why the constant folds to one multiply-add and no divide. `fallback`
// carries the manual distance for texels where no glass depth was captured
// (d == 0 means nothing was drawn there).
const char kBlitHlsl[] =
    "Texture2D tex : register(t0); Texture2D smudge : register(t1);"
    "Texture2D glassDepth : register(t2); Texture2D winch : register(t3);"
    "SamplerState smp : register(s0);"
    "cbuffer Blit : register(b0) {"
    "  float k; float projA; float invWidth; float tOverB;"
    "  float smudgeOn; float haveGlassDepth; float winchOn; float _pad1; };"
    "struct VO { float4 p : SV_Position; float2 uv : TEXCOORD0; };"
    "VO vsmain(uint id : SV_VertexID) {"
    "  VO o; o.uv = float2((id<<1)&2, id&2);"
    "  o.p = float4(o.uv*float2(2,-2)+float2(-1,1), 0, 1); return o; }"
    // The splatter sits on the OUTER face of the glass while the depth we
    // captured is the INNER one -- our capture keeps the nearest surface, and
    // the pane has thickness. That bias makes the shift slightly too large, on
    // the synthesized eye only, so it alternates with the rendered eye and reads
    // as a small jitter rather than a static offset.
    //
    // disp = ipd*focal / (z + t), with z = projB/e and e = d - projA, folds to
    // k*e / (1 + (t/projB)*e) -- one extra constant and one divide, and
    // tOverB = 0 reproduces exactly what shipped.
    "float smudge_shift(float d, float k, float projA, float tOverB, float invW) {"
    "  float e = d - projA;"
    "  return (k * e) / (1.0 + tOverB * e) * invW;"
    "}"
    "float4 psmain(VO i) : SV_Target {"
    "  float4 c = tex.Sample(smp, i.uv);"
    // EVERY layer pixel is composited -- nothing is dropped. An earlier version
    // discarded pixels with no captured depth, on the theory that the flat
    // window planes and tint could not be placed. Keeping them is better for
    // two reasons: they can be placed, by borrowing depth from the glass beside
    // them; and they usefully COVER the warp's disocclusions. The glass sits in
    // front of the biggest holes DIBR shift makes -- the A-pillar and window frames are
    // near geometry, so they carry a large disparity and tear a gap behind
    // themselves that the fill can only smear. Tint drawn over that gap hides
    // it, so removing the tint made the frame worse, not cleaner.
    //
    // Depth comes from the pixel itself where we captured it, and otherwise from
    // the nearest captured sample in a widening cross. The taps step by r*r so a
    // few of them reach a long way (4 to 144 px) without a dense search: tint
    // and splatter lie on the SAME pane, so any nearby sample is the right depth
    // for it, and the left and right windows still get their own rather than
    // sharing one compromise value.
    "  if (smudgeOn > 0.5) {"
    "    float d = 0.0;"
    "    if (haveGlassDepth > 0.5) {"
    "      d = glassDepth.Sample(smp, i.uv).r;"
    "      if (d <= 0.0) {"
    "        [unroll] for (int r = 1; r <= 6; ++r) {"
    "          float o = float(r) * float(r) * 4.0 * invWidth;"
    "          d = max(d, glassDepth.Sample(smp, i.uv + float2( o, 0)).r);"
    "          d = max(d, glassDepth.Sample(smp, i.uv + float2(-o, 0)).r);"
    "          d = max(d, glassDepth.Sample(smp, i.uv + float2( 0, o)).r);"
    "          d = max(d, glassDepth.Sample(smp, i.uv + float2( 0,-o)).r);"
    "        }"
    "      }"
    "    }"
    // No depth found anywhere near: composite unshifted rather than discard.
    // Wrong by a few pixels beats a hole.
    "    float shift = (d > 0.0) ? smudge_shift(d, k, projA, tOverB, invWidth) : 0.0;"
    // ONE REFINEMENT PASS. This is a gather: for destination pixel uv we fetch
    // the layer at uv - shift, but `shift` was derived from the depth AT uv --
    // the destination -- while the pixel actually arriving there is the one at
    // the SOURCE. On a raked windscreen the depth changes across the shift, so
    // the two disagree and the splatter lands a few pixels out. Since the eyes
    // disagree by that error, and it alternates with the rendered eye, it reads
    // as a small jitter on the smudge rather than a static offset.
    //
    // Re-deriving the shift from the depth at the first estimate's source is one
    // step of a fixed-point iteration and removes most of it; a second step is
    // not worth its taps on a surface this smooth.
    "    if (d > 0.0 && haveGlassDepth > 0.5) {"
    "      [unroll] for (int it = 0; it < 3; ++it) {"
    "        float dn = glassDepth.Sample(smp, float2(i.uv.x - shift, i.uv.y)).r;"
    "        if (dn <= 0.0) break;"
    "        shift = smudge_shift(dn, k, projA, tOverB, invWidth);"
    "      }"
    "    }"
    "    float2 suv = float2(i.uv.x - shift, i.uv.y);"
    // NO HAZE IN THE DISOCCLUSION BESIDE AN OCCLUDER -- accepted, not a bug.
    // In the synthesized eye, the band beside the A-pillar or a mirror is
    // glass the rendered eye never saw: it is further away than the pillar's
    // edge (measured 2026-10-26 from a stereo screenshot: ~98 px of disparity
    // against the pillar's 143-159), so the splatter on it was never captured
    // and no shift can fetch it. Two fixes were tried that day and removed: a
    // search for the glass whose own disparity lands on the pixel, with far
    // glass cut in the cockpit (no visible change -- it can only find glass the
    // rendered eye saw), and each eye's own splatter kept from its previous
    // render (looked worse when looking around, and the band stayed).
    "    if (suv.x >= 0.0 && suv.x <= 1.0) {"
    // NO GRADING OF ANY KIND. An alpha curve and a brightness multiply lived
    // here, exposed as two sliders, on the theory that the splatter arrives
    // without the grading pass the rest of the frame gets and so reads too
    // strong. Removed 2026-08-24: they were guesses at a correction nobody
    // could state the right value for, and a knob whose correct setting is
    // unknown is a way to make the image wrong on purpose. The layer is
    // composited exactly as the game drew it, premultiplied.
    "      float4 s = smudge.Sample(smp, suv);"
    "      c.rgb = s.rgb + c.rgb * (1.0 - s.a);"
    "    }"
    "  }"
    // THE WINCH MARKERS, straight back where the game drew them. No shift and
    // no per-eye term of any kind: both eyes sample the same uv, which is what
    // "these are UI" means geometrically -- zero disparity, i.e. at infinity,
    // the same deal the HUD gets. See winch_layer.hpp.
    //
    // AFTER the smudge, because the smudge is on the glass and a marker seen
    // through the windscreen is drawn over it. Both layers are premultiplied,
    // so the composite is the same one-liner.
    "  if (winchOn > 0.5) {"
    "    float4 m = winch.Sample(smp, i.uv);"
    "    c.rgb = m.rgb + c.rgb * (1.0 - m.a);"
    "  }"
    // A UI COMPOSITE used to end this shader: the captured HUD was blended in
    // here so the eye DIBR shift synthesized would not be missing it. The UI is now a
    // composition layer of its own (build_ui_quad_layer), drawn by the
    // compositor after both eyes rather than painted into them, so there is
    // nothing left to put back -- and the eye images no longer contain any UI
    // to double.
    "  return c; }";

bool build_blit(ID3D11Device* dev)
{
    g.device = dev;
    dev->GetImmediateContext(&g.ctx);

    ComPtr<ID3DBlob> vsb, psb, err;
    if (FAILED(D3DCompile(kBlitHlsl, sizeof(kBlitHlsl) - 1, "blit", nullptr, nullptr,
                          "vsmain", "vs_5_0", 0, 0, &vsb, &err))) {
        VRLOG("blit VS compile failed: %s", err ? (char*)err->GetBufferPointer() : "?");
        return false;
    }
    if (FAILED(D3DCompile(kBlitHlsl, sizeof(kBlitHlsl) - 1, "blit", nullptr, nullptr,
                          "psmain", "ps_5_0", 0, 0, &psb, &err))) {
        VRLOG("blit PS compile failed: %s", err ? (char*)err->GetBufferPointer() : "?");
        return false;
    }
    if (FAILED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g.vs)) ||
        FAILED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g.ps)))
        return false;

    // UI plane copy: whichever view of the capture matches the swapchain it is
    // going into (see ensure_ui_swapchain), straight through with no colour
    // work of its own -- the gamma round trip is done by the view formats, in
    // hardware, exactly as the eye blit does it.
    // The VS is kBlitHlsl's, so VO here must keep its signature.
    {
        static const char kUiCopyHlsl[] =
            "Texture2D uiTex : register(t0); SamplerState smp : register(s0);"
            "struct VO { float4 p : SV_Position; float2 uv : TEXCOORD0; };"
            "float4 psmain(VO i) : SV_Target { return uiTex.Sample(smp, i.uv); }";
        ComPtr<ID3DBlob> ub, uerr;
        if (FAILED(D3DCompile(kUiCopyHlsl, sizeof(kUiCopyHlsl) - 1, "uicopy", nullptr, nullptr,
                              "psmain", "ps_5_0", 0, 0, &ub, &uerr)) ||
            FAILED(dev->CreatePixelShader(ub->GetBufferPointer(), ub->GetBufferSize(),
                                          nullptr, &g.uiCopyPs))) {
            VRLOG("ui plane: copy shader build FAILED: %s -- UI stays in the render",
                  uerr ? (char*)uerr->GetBufferPointer() : "?");
            g.uiCopyPs.Reset();
        }
    }

    {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 48;                 // three float4s
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &g.blitCb))) {
            VRLOG("blit CB creation FAILED -- splatter composite disabled");
            g.blitCb.Reset();
        }
    }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    return SUCCEEDED(dev->CreateSamplerState(&sd, &g.sampler));
}

// 2x-mode rotation warp: reuses g.vs (already a generic, parameter-free
// full-screen-triangle VS -- no need to recompile it), only a new PS + a
// homography cbuffer. The three h0/h1/h2 rows are dot()'d against the
// homogeneous pixel coordinate explicitly (not HLSL's mul() intrinsic) to
// sidestep the row/col-major default-convention footgun. w<=epsilon means
// the ray landed behind the OLD camera (can otherwise perspective-divide
// into an in-range but MIRRORED/ghost sample) -- route it out of [0,1] the
// same as a normal off-screen coordinate, so BORDER addressing (black,
// see build_warp_shader) handles both cases with one code path.
const char kWarpRotHlsl[] =
    "Texture2D tex : register(t0); SamplerState smp : register(s0);"
    "cbuffer WarpCB : register(b0) { float4 h0; float4 h1; float4 h2; };"
    "struct VO { float4 p : SV_Position; float2 uv : TEXCOORD0; };"
    "float4 psmain(VO i) : SV_Target {"
    "  float3 p = float3(i.uv, 1.0);"
    "  float x = dot(h0.xyz, p), y = dot(h1.xyz, p), w = dot(h2.xyz, p);"
    "  float2 uvOld = (w > 1e-5) ? float2(x / w, y / w) : float2(-2.0, -2.0);"
    "  return tex.Sample(smp, uvOld); }";

bool build_warp_shader(ID3D11Device* dev)
{
    ComPtr<ID3DBlob> psb, err;
    if (FAILED(D3DCompile(kWarpRotHlsl, sizeof(kWarpRotHlsl) - 1, "warp_rot", nullptr, nullptr,
                          "psmain", "ps_5_0", 0, 0, &psb, &err))) {
        VRLOG("warp PS compile failed: %s", err ? (char*)err->GetBufferPointer() : "?");
        return false;
    }
    if (FAILED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g.warpPs)))
        return false;

    // BORDER, not CLAMP (g.sampler): coordinates outside [0,1] on either axis
    // resolve to BorderColor (black, matching the letterbox clear colour)
    // instead of smearing the edge pixel -- with linear filtering this gives
    // a soft fade to black near the border rather than a hard cutoff.
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    sd.BorderColor[0] = sd.BorderColor[1] = sd.BorderColor[2] = 0.0f;
    sd.BorderColor[3] = 1.0f;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sd, &g.warpSampler)))
        return false;

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = 48;   // 3x float4, already 16-byte aligned
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&cbd, nullptr, &g.warpCb)))
        return false;

    VRLOG("warp shader built");
    return true;
}

// Uploads a Mat3 (row-major) as the three float4 rows the warp PS dot()s
// against. The unused 4th component of each row is left zeroed.
void update_warp_cb(ID3D11DeviceContext* ctx, const Mat3& H)
{
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(g.warpCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    float* f = reinterpret_cast<float*>(mapped.pData);
    f[0]=H.m[0]; f[1]=H.m[1]; f[2]=H.m[2]; f[3]=0.0f;
    f[4]=H.m[3]; f[5]=H.m[4]; f[6]=H.m[5]; f[7]=0.0f;
    f[8]=H.m[6]; f[9]=H.m[7]; f[10]=H.m[8]; f[11]=0.0f;
    ctx->Unmap(g.warpCb.Get(), 0);
}

// Format helpers for the sRGB-correct blit path.
DXGI_FORMAT typeless_of(DXGI_FORMAT f)
{
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return DXGI_FORMAT_B8G8R8A8_TYPELESS;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return DXGI_FORMAT_R10G10B10A2_TYPELESS;
        default: return f;
    }
}
DXGI_FORMAT srgb_of(DXGI_FORMAT f)
{
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        default: return f;  // 10-bit has no sRGB variant; leave as-is
    }
}

// Rebuilds the SRV-capable staging texture to match `src`, which may be the
// swapchain backbuffer OR (when RT capture is active) the captured pre-
// downscale scene texture. Its resolution is INDEPENDENT of the fixed XR
// swapchain size (width/height): the later blit is a full-screen-triangle +
// sampler pass, which resamples automatically regardless of source size.
bool ensure_staging(ID3D11Texture2D* src)
{
    D3D11_TEXTURE2D_DESC bd{};
    src->GetDesc(&bd);
    if (g.staging && bd.Format == g.stagingFormat &&
        bd.Width == g.stagingW && bd.Height == g.stagingH)
        return true;

    g.staging.Reset();
    g.stagingSrv.Reset();

    D3D11_TEXTURE2D_DESC sd = bd;
    sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sd.MiscFlags = 0;
    sd.CPUAccessFlags = 0;
    sd.Usage = D3D11_USAGE_DEFAULT;
    sd.SampleDesc.Count = 1;  // XR image is single-sampled; game source is too (MSAA excluded upstream)
    sd.Format = typeless_of(bd.Format);   // typeless so we can view it as sRGB
    if (FAILED(g.device->CreateTexture2D(&sd, nullptr, &g.staging))) {
        VRLOG("staging texture create failed (fmt=%d)", int(bd.Format));
        return false;
    }
    // sRGB SRV: sampling linearizes the game's sRGB-encoded pixels.
    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = srgb_of(bd.Format);
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;
    if (FAILED(g.device->CreateShaderResourceView(g.staging.Get(), &srvd, &g.stagingSrv)))
        return false;

    g.stagingFormat = bd.Format;
    g.stagingW = bd.Width; g.stagingH = bd.Height;
    VRLOG("staging: src fmt=%d %ux%u -> typeless=%d, sRGB SRV=%d",
          int(bd.Format), bd.Width, bd.Height, int(sd.Format), int(srvd.Format));
    return true;
}

// Per-eye retained "last real render" for the 2x-mode rotation warp. Sized/
// formatted to match g.staging (the un-letterboxed blit source), NOT the
// swapchain -- so the retained texture never bakes a letterbox bar in as if
// it were scene content. Whenever this ACTUALLY reallocates (format/size
// changed), prevEyeValid[eye] is cleared: otherwise a resize mid-session
// would leave it pointing at a freshly-allocated, uninitialized texture
// while still claiming to hold a valid previous render.
bool ensure_prev_eye_tex(int eye)
{
    if (!g.staging) return false;   // staging not built yet this Present
    if (g.prevEyeTex[eye] && g.prevEyeFormat[eye] == g.stagingFormat &&
        g.prevEyeW[eye] == g.stagingW && g.prevEyeH[eye] == g.stagingH)
        return true;

    g.prevEyeTex[eye].Reset();
    g.prevEyeSrv[eye].Reset();
    g.prevEyeSrvRaw[eye].Reset();
    g.prevEyeValid[eye] = false;

    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = g.stagingW; sd.Height = g.stagingH;
    sd.MipLevels = 1; sd.ArraySize = 1;
    sd.Format = typeless_of(g.stagingFormat);
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_DEFAULT;
    sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g.device->CreateTexture2D(&sd, nullptr, &g.prevEyeTex[eye]))) {
        VRLOG("prevEyeTex[%d] create failed (fmt=%d)", eye, int(g.stagingFormat));
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = srgb_of(g.stagingFormat);
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;
    if (FAILED(g.device->CreateShaderResourceView(g.prevEyeTex[eye].Get(), &srvd, &g.prevEyeSrv[eye])))
        return false;
    srvd.Format = g.stagingFormat;   // raw counterpart, for DIBR shift's fill
    if (FAILED(g.device->CreateShaderResourceView(g.prevEyeTex[eye].Get(), &srvd, &g.prevEyeSrvRaw[eye])))
        return false;

    g.prevEyeFormat[eye] = g.stagingFormat;
    g.prevEyeW[eye] = g.stagingW; g.prevEyeH[eye] = g.stagingH;
    return true;
}

// Retained per-eye DEPTH for the 6-DoF stale-eye reprojection. Sized and
// formatted from the depth ring's own copies (R32_FLOAT), which is what
// scene_depth_for_eye() hands out.
// Retained copy of the camera-rigid mask, sized from the live one.
bool ensure_prev_eye_rigid(int eye)
{
    ID3D11Texture2D* live = rigidmask::texture();
    if (!live || !g.device) return false;
    D3D11_TEXTURE2D_DESC sd{};
    live->GetDesc(&sd);

    if (g.prevEyeRigid[eye] &&
        g.prevEyeRigidW[eye] == sd.Width && g.prevEyeRigidH[eye] == sd.Height)
        return true;

    g.prevEyeRigid[eye].Reset();
    g.prevEyeRigidSrv[eye].Reset();
    g.prevEyeRigidW[eye] = g.prevEyeRigidH[eye] = 0;

    D3D11_TEXTURE2D_DESC td = sd;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = 0;
    td.MiscFlags = 0;
    if (FAILED(g.device->CreateTexture2D(&td, nullptr, &g.prevEyeRigid[eye])) ||
        FAILED(g.device->CreateShaderResourceView(g.prevEyeRigid[eye].Get(), nullptr,
                                                  &g.prevEyeRigidSrv[eye]))) {
        VRLOG("prevEyeRigid[%d] create failed (%ux%u)", eye, sd.Width, sd.Height);
        g.prevEyeRigid[eye].Reset();
        g.prevEyeRigidSrv[eye].Reset();
        return false;
    }
    g.prevEyeRigidW[eye] = sd.Width;
    g.prevEyeRigidH[eye] = sd.Height;
    return true;
}

// Same as ensure_prev_eye_rigid(), for the mirror mask.
bool ensure_prev_eye_mirror(int eye, ID3D11Texture2D* live)
{
    if (!live || !g.device) return false;
    D3D11_TEXTURE2D_DESC sd{};
    live->GetDesc(&sd);
    if (g.prevEyeMirror[eye] &&
        g.prevEyeMirrorW[eye] == sd.Width && g.prevEyeMirrorH[eye] == sd.Height)
        return true;
    g.prevEyeMirror[eye].Reset();
    g.prevEyeMirrorSrv[eye].Reset();
    g.prevEyeMirrorW[eye] = g.prevEyeMirrorH[eye] = 0;
    D3D11_TEXTURE2D_DESC td = sd;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = 0;
    td.MiscFlags = 0;
    if (FAILED(g.device->CreateTexture2D(&td, nullptr, &g.prevEyeMirror[eye])) ||
        FAILED(g.device->CreateShaderResourceView(g.prevEyeMirror[eye].Get(), nullptr,
                                                  &g.prevEyeMirrorSrv[eye]))) {
        VRLOG("prevEyeMirror[%d] create failed (%ux%u)", eye, sd.Width, sd.Height);
        g.prevEyeMirror[eye].Reset();
        g.prevEyeMirrorSrv[eye].Reset();
        return false;
    }
    g.prevEyeMirrorW[eye] = sd.Width;
    g.prevEyeMirrorH[eye] = sd.Height;
    return true;
}

bool ensure_prev_eye_depth(int eye)
{
    hooks::SceneDepth probe{};
    if (!hooks::scene_depth_for_eye(eye, probe) || !probe.srv) return false;

    ComPtr<ID3D11Resource> res;
    probe.srv->GetResource(&res);
    ComPtr<ID3D11Texture2D> tex;
    if (!res || FAILED(res.As(&tex))) return false;
    D3D11_TEXTURE2D_DESC sd{};
    tex->GetDesc(&sd);

    if (g.prevEyeDepth[eye] &&
        g.prevEyeDepthW[eye] == sd.Width && g.prevEyeDepthH[eye] == sd.Height)
        return true;

    g.prevEyeDepth[eye].Reset();
    g.prevEyeDepthSrv[eye].Reset();
    g.prevEyeDepthW[eye] = g.prevEyeDepthH[eye] = 0;

    D3D11_TEXTURE2D_DESC td = sd;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = 0;
    td.MiscFlags = 0;
    if (FAILED(g.device->CreateTexture2D(&td, nullptr, &g.prevEyeDepth[eye])) ||
        FAILED(g.device->CreateShaderResourceView(g.prevEyeDepth[eye].Get(), nullptr,
                                                  &g.prevEyeDepthSrv[eye]))) {
        VRLOG("prevEyeDepth[%d] create failed (%ux%u fmt=%d)",
              eye, sd.Width, sd.Height, (int)sd.Format);
        g.prevEyeDepth[eye].Reset();
        g.prevEyeDepthSrv[eye].Reset();
        return false;
    }
    g.prevEyeDepthW[eye] = sd.Width; g.prevEyeDepthH[eye] = sd.Height;
    return true;
}

// Intermediate for the AER stale-eye warp -- see the note at staleWarpTex.
// Sized to the RETAINED eye, not the swapchain, because the reprojection maps
// source pixels to destination pixels one for one; the final draw scales it
// into the swapchain rect exactly as the direct path always did.
bool ensure_stale_warp_tex(int eye)
{
    if (!g.prevEyeTex[eye] || !g.prevEyeW[eye] || !g.prevEyeH[eye]) return false;
    if (g.staleWarpTex && g.staleWarpW == g.prevEyeW[eye] &&
        g.staleWarpH == g.prevEyeH[eye] &&
        g.staleWarpFormat == g.prevEyeFormat[eye])
        return true;

    g.staleWarpUav.Reset(); g.staleWarpSrv.Reset();
    g.staleWarpRtv.Reset(); g.staleWarpTex.Reset();
    g.staleWarpW = g.staleWarpH = 0;
    g.staleWarpFormat = DXGI_FORMAT_UNKNOWN;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = g.prevEyeW[eye]; td.Height = g.prevEyeH[eye];
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = typeless_of(g.prevEyeFormat[eye]);   // typeless: three view types
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE |
                   D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(g.device->CreateTexture2D(&td, nullptr, &g.staleWarpTex))) {
        VRLOG("stale warp intermediate: create FAILED (%ux%u fmt=%d)",
              td.Width, td.Height, (int)td.Format);
        return false;
    }

    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = srgb_of(g.prevEyeFormat[eye]);
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = srgb_of(g.prevEyeFormat[eye]);
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = g.prevEyeFormat[eye];                // raw; UAVs cannot be sRGB
    ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    if (FAILED(g.device->CreateRenderTargetView(g.staleWarpTex.Get(), &rd, &g.staleWarpRtv)) ||
        FAILED(g.device->CreateShaderResourceView(g.staleWarpTex.Get(), &sd, &g.staleWarpSrv)) ||
        FAILED(g.device->CreateUnorderedAccessView(g.staleWarpTex.Get(), &ud, &g.staleWarpUav))) {
        VRLOG("stale warp intermediate: view creation FAILED (rtv/srv %d, uav %d)",
              (int)rd.Format, (int)ud.Format);
        g.staleWarpUav.Reset(); g.staleWarpSrv.Reset();
        g.staleWarpRtv.Reset(); g.staleWarpTex.Reset();
        return false;
    }
    g.staleWarpW = td.Width; g.staleWarpH = td.Height;
    g.staleWarpFormat = g.prevEyeFormat[eye];
    return true;
}

// Desktop single-eye save buffer. ALWAYS matches the swapchain backbuffer
// exactly -- it's a raw CopyResource to/from the backbuffer (see the desktop
// mirror trick below), which requires identical dimensions/format, so this is
// deliberately independent of the (possibly differently-sized) capture staging
// texture above.
bool ensure_desktop_save(ID3D11Texture2D* backbuffer)
{
    D3D11_TEXTURE2D_DESC bd{};
    backbuffer->GetDesc(&bd);
    if (g.desktopSave && bd.Width == g.desktopSaveW && bd.Height == g.desktopSaveH &&
        bd.Format == g.desktopSaveFormat)
        return true;

    g.desktopSave.Reset();
    D3D11_TEXTURE2D_DESC dsd = bd;
    dsd.BindFlags = 0; dsd.MiscFlags = 0; dsd.CPUAccessFlags = 0; dsd.Usage = D3D11_USAGE_DEFAULT;
    if (FAILED(g.device->CreateTexture2D(&dsd, nullptr, &g.desktopSave))) {
        g.desktopSave.Reset();   // non-fatal; desktop just mirrors both eyes
        return false;
    }
    g.desktopSaveW = bd.Width; g.desktopSaveH = bd.Height; g.desktopSaveFormat = bd.Format;
    return true;
}

PFN_xrGetD3D11GraphicsRequirementsKHR pfnGetReq = nullptr;

// Bring up JUST the XR instance + system (no session/device needed). Idempotent
// and reusable: the early render-size query and the full init() both call it, so
// the instance is created once and shared. Returns false with no runtime present.
bool ensure_instance()
{
    if (g.instance != XR_NULL_HANDLE) return true;
    // D3D11 always; plus whatever the controller mapping can use and the
    // runtime offers (the HP Reverb G2's controller profile is an extension).
    std::vector<const char*> exts = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    uint32_t extCount = 0;
    std::vector<XrExtensionProperties> avail;
    if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr)) && extCount) {
        avail.assign(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
        if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, avail.data())))
            avail.clear();
    }
    for (const XrExtensionProperties& e : avail)
        if (xrinput::wants_extension(e.extensionName)) exts.push_back(e.extensionName);
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    ici.enabledExtensionCount = (uint32_t)exts.size();
    ici.enabledExtensionNames = exts.data();
    strcpy_s(ici.applicationInfo.applicationName, "SnowRunnerVR");
    // Request 1.0, not XR_CURRENT_API_VERSION (1.1): VDXR/SteamVR runtimes are
    // 1.0 and reject a 1.1 instance with XR_ERROR_API_VERSION_UNSUPPORTED (-4).
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    XR_TRY(xrCreateInstance(&ici, &g.instance), "xrCreateInstance");
    for (size_t i = 1; i < exts.size(); ++i) xrinput::note_extension_enabled(exts[i]);

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_TRY(xrGetSystem(g.instance, &sgi, &g.systemId), "xrGetSystem");
    return true;
}

// Runtime's native per-eye render target size (before any square/scale shaping).
bool recommended_eye_size(uint32_t& w, uint32_t& h)
{
    if (!ensure_instance()) return false;
    uint32_t vc = 0;
    if (XR_FAILED(xrEnumerateViewConfigurationViews(
            g.instance, g.systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            0, &vc, nullptr)) || vc == 0)
        return false;
    std::vector<XrViewConfigurationView> views(vc, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    if (XR_FAILED(xrEnumerateViewConfigurationViews(
            g.instance, g.systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            vc, &vc, views.data())))
        return false;
    // Both eyes are identical on every runtime we target; take view 0.
    w = views[0].recommendedImageRectWidth;
    h = views[0].recommendedImageRectHeight;
    return w > 0 && h > 0;
}

// --- SteamVR UNDER PROTON ---------------------------------------------------
//
// Two things about it that Windows does not share, both measured by a Linux
// player (HP Reverb G2, SteamVR 2.18, Proton Experimental, 2026-10-23):
//
//   1. xrEndFrame does not return until the GPU has finished the frame it is
//      handed -- ~9 ms a call, up to 16. Its DXVK share (wineopenxr's queue
//      lock) is ~0.03 ms, so the wait is in the runtime itself. See end_frame().
//   2. A frame carrying more than ONE quad layer is rejected outright with
//      XR_ERROR_SWAPCHAIN_RECT_INVALID (-25): the menu + UI pair failed, and
//      so did two 64x64 test quads. SteamVR there only ever creates a single
//      quad overlay (openxr_quadlayer_0 in vrserver.txt). A rejected frame is a
//      dropped frame, so with the settings menu open nothing reached the
//      headset at all. See render_frame()'s layer list.
//
// Wine is detected by ntdll exporting wine_get_version, which only Wine's
// does. Both adaptations hang off this one answer; on Windows neither runs.
bool running_under_wine()
{
    static const bool wine = [] {
        using PFN_wine_get_version = const char* (CDECL*)();
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        auto ver = ntdll ? reinterpret_cast<PFN_wine_get_version>(
                               GetProcAddress(ntdll, "wine_get_version"))
                         : nullptr;
        if (ver)
            VRLOG("platform: Wine %s -- xrEndFrame runs on a worker thread, and each "
                  "frame carries at most one quad layer", ver());
        return ver != nullptr;
    }();
    return wine;
}

// Named for the constraint rather than the platform, because that is what the
// layer list is reasoning about.
//
// kTestSingleQuadOnWindows: OFF. True applies the one-quad layer list on
// Windows as well -- the only way to check it without a Linux machine. Open the
// mod menu (the UI plane should step aside, the menu stays) and set a screen to
// 2D (its panel should carry the HUD in its own image, then give way to the eye
// projection while the menu is open). It does NOT move xrEndFrame onto the
// worker; that stays Wine-only, because a different runtime's xrEndFrame on
// another thread proves nothing about SteamVR's.
constexpr bool kTestSingleQuadOnWindows = false;
bool single_quad_runtime() { return kTestSingleQuadOnWindows || running_under_wine(); }

// THE EYE IMAGES FOR A GAME FRAME OF THIS SIZE: the same size, unless DLSS
// upscales. Then the frame is its mode's render size, and the eye images are the
// size it is scaled up to (ForceGameResolution), which DLSS brings each eye up to
// at Present. Only when the frame really is that render size -- anything else (a
// window clamped by Windows, a fullscreen switch before the first Present) keeps
// the 1:1 canvas, where a stretch could at least not distort.
void eye_size_for_frame(uint32_t frameW, uint32_t frameH, uint32_t& w, uint32_t& h)
{
    w = frameW;
    h = frameH;
    int ow = 0, oh = 0, rw = 0, rh = 0;
    if (!hooks::upscale_session(ow, oh, rw, rh)) return;
    if ((int)frameW == rw && (int)frameH == rh) {
        w = (uint32_t)ow;
        h = (uint32_t)oh;
        VRLOG("XR canvas: %ux%u eye images from a %ux%u game frame -- DLSS %s upscales each eye",
              w, h, frameW, frameH, dlss::mode_name(hooks::upscale_mode_applied()));
    } else {
        VRLOG("DLSS upscaling: the game's frame is %ux%u, not the %dx%d its window was sized "
              "to -- no upscaling", frameW, frameH, rw, rh);
    }
}

// Two swapchains (one per eye) — AER routes alternate backbuffers into each, and
// both are submitted every frame (the stale eye is resubmitted). On failure
// whatever was made so far is left in `sc` and `rtvs` for the caller to destroy.
bool create_eye_images(ID3D11Device* dev, uint32_t w, uint32_t h, XrSwapchain (&sc)[2],
                       std::vector<ComPtr<ID3D11RenderTargetView>> (&rtvs)[2])
{
    for (int eye = 0; eye < 2; ++eye) {
        XrSwapchainCreateInfo xsci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        xsci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        xsci.format = g.swapFormat;
        xsci.width = w;
        xsci.height = h;
        xsci.sampleCount = 1;
        xsci.faceCount = 1;
        xsci.arraySize = 1;
        xsci.mipCount = 1;
        XR_TRY(xrCreateSwapchain(g.session, &xsci, &sc[eye]), "xrCreateSwapchain");

        uint32_t imgCount = 0;
        xrEnumerateSwapchainImages(sc[eye], 0, &imgCount, nullptr);
        std::vector<XrSwapchainImageD3D11KHR> images(imgCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        XR_TRY(xrEnumerateSwapchainImages(sc[eye], imgCount, &imgCount,
                                          (XrSwapchainImageBaseHeader*)images.data()),
               "xrEnumerateSwapchainImages");
        rtvs[eye].resize(imgCount);
        for (uint32_t i = 0; i < imgCount; ++i) {
            D3D11_RENDER_TARGET_VIEW_DESC rd{};
            rd.Format = (DXGI_FORMAT)g.swapFormat;
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            if (FAILED(dev->CreateRenderTargetView(images[i].texture, &rd, &rtvs[eye][i]))) {
                VRLOG("XR image RTV create failed");
                return false;
            }
        }
    }
    return true;
}

void destroy_eye_images(XrSwapchain (&sc)[2],
                        std::vector<ComPtr<ID3D11RenderTargetView>> (&rtvs)[2])
{
    for (int eye = 0; eye < 2; ++eye) {
        rtvs[eye].clear();     // our views on its images go before it
        if (sc[eye] != XR_NULL_HANDLE) {
            xrDestroySwapchain(sc[eye]);
            sc[eye] = XR_NULL_HANDLE;
        }
    }
}

bool init(IDXGISwapChain* swapchain)
{
    running_under_wine();   // decided, and logged, before anything depends on it

    ComPtr<ID3D11Device> dev;
    if (FAILED(swapchain->GetDevice(__uuidof(ID3D11Device), (void**)&dev))) {
        VRLOG("could not get D3D11 device from swapchain");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC scd{};
    swapchain->GetDesc(&scd);
    g.outputWindow = scd.OutputWindow;
    // REVERTED (2026-07-25): square XR canvas + letterbox split didn't pan out
    // (still not square in headset, UI missing, no quality gain) -- back to
    // the simple 1:1 relationship: whatever the backbuffer actually is, the
    // XR canvas mirrors it exactly, same as before all resolution experiments.
    {
        g.bbWidth  = scd.BufferDesc.Width;
        g.bbHeight = scd.BufferDesc.Height;
        eye_size_for_frame(g.bbWidth, g.bbHeight, g.width, g.height);
    }

    // The runtime's own recommendation, logged against what we are actually
    // giving it. These are independent numbers -- the canvas mirrors the
    // BACKBUFFER, which is whatever resolution the game ended up at, while the
    // recommendation is what the headset wants per eye -- and nothing has ever
    // compared them.
    //
    // It matters for sharpness in a way that is easy to misread: a canvas
    // smaller than the recommendation is upscaled by the compositor, which is
    // invisible in the centre and worst at the edges, because lens distortion
    // magnifies the periphery most. "Blurry only at the borders" is what an
    // upscale looks like through a headset, and the fix for it is resolution,
    // not sharpening.
    {
        uint32_t rw = 0, rh = 0;
        if (recommended_eye_size(rw, rh)) {
            const float sx = rw ? (float)g.width  / (float)rw : 0.0f;
            const float sy = rh ? (float)g.height / (float)rh : 0.0f;
            VRLOG("XR canvas %ux%u vs runtime recommendation %ux%u (%.2fx / %.2fx)%s",
                  g.width, g.height, rw, rh, sx, sy,
                  (sx < 0.98f || sy < 0.98f)
                      ? "  -- UPSCALED by the compositor; expect soft edges"
                      : "");
        }
    }

    // --- instance (shared with the early render-size query) ---
    if (!ensure_instance()) return false;

    XR_TRY(xrGetInstanceProcAddr(g.instance, "xrGetD3D11GraphicsRequirementsKHR",
                                 (PFN_xrVoidFunction*)&pfnGetReq), "getProc GraphicsRequirements");
    XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    XR_TRY(pfnGetReq(g.instance, g.systemId, &req), "xrGetD3D11GraphicsRequirements");
    // req.adapterLuid identifies the required GPU; the game's device already
    // runs on the HMD-connected adapter under VDXR, so we bind it directly.

    // --- session on the game's device ---
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = dev.Get();
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = g.systemId;
    XR_TRY(xrCreateSession(g.instance, &sci, &g.session), "xrCreateSession");

    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    XR_TRY(xrCreateReferenceSpace(g.session, &rsci, &g.space), "xrCreateReferenceSpace LOCAL");

    XrReferenceSpaceCreateInfo vci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    vci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;  // head-locked
    vci.poseInReferenceSpace.orientation.w = 1.0f;
    XR_TRY(xrCreateReferenceSpace(g.session, &vci, &g.viewSpace), "xrCreateReferenceSpace VIEW");

    // The controllers as an Xbox pad. Before the first frame: an action set can
    // only be attached once per session. Failure costs the mapping, not VR.
    xrinput::init(g.instance, g.session);

    // --- swapchain: pick a runtime format matching the backbuffer if we can ---
    uint32_t fmtCount = 0;
    xrEnumerateSwapchainFormats(g.session, 0, &fmtCount, nullptr);
    std::vector<int64_t> formats(fmtCount);
    xrEnumerateSwapchainFormats(g.session, fmtCount, &fmtCount, formats.data());
    // Prefer sRGB so the compositor de-gammas the game's sRGB pixels correctly.
    const int64_t preferred[] = {
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
        DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM};
    for (int64_t pref : preferred) {
        for (int64_t f : formats) if (f == pref) { g.swapFormat = f; break; }
        if (g.swapFormat) break;
    }
    if (!g.swapFormat && !formats.empty())
        g.swapFormat = formats[0];

    if (!create_eye_images(dev.Get(), g.width, g.height, g.swapchain, g.rtvs))
        return false;

    if (!build_blit(dev.Get()))
        return false;
    if (!build_warp_shader(dev.Get()))
        VRLOG("warp shader build FAILED -- 2x mode will fall back to stale-resubmit for the un-rendered eye");

    VRLOG("OpenXR AER initialized: %ux%u/eye, fmt=%lld, %u+%u images",
          g.width, g.height, (long long)g.swapFormat,
          (uint32_t)g.rtvs[0].size(), (uint32_t)g.rtvs[1].size());
    return true;
}

// Pose VALIDITY, reported on change. A runtime may return SUCCESS with
// untracked views -- headset off the head, focus on the desktop -- and storing
// those freezes the view, which in the headset reads as the world being glued
// to your face, since nothing counter-rotates any more. Observational only:
// it distinguishes "the runtime stopped giving poses" from "we stopped
// asking", which is not currently answerable after the fact.
void note_view_validity(const XrViewState& vst)
{
    const XrViewStateFlags need = XR_VIEW_STATE_ORIENTATION_VALID_BIT |
                                  XR_VIEW_STATE_POSITION_VALID_BIT;
    const bool ok = (vst.viewStateFlags & need) == need;
    static bool lastOk = true;
    if (ok == lastOk) return;
    lastOk = ok;
    VRLOG("xr: view pose %s (viewStateFlags=0x%llX, session state %d)",
          ok ? "VALID again" : "went INVALID",
          (unsigned long long)vst.viewStateFlags, (int)g.state);
}

void end_frame_wait();   // defined with end_frame(), above render_frame()

void poll_events()
{
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    // `type` must be reset to XR_TYPE_EVENT_DATA_BUFFER before EVERY poll: the
    // runtime overwrites it with the delivered event's own type, so reusing the
    // struct hands the next call a header claiming to be, say, a
    // SESSION_STATE_CHANGED. A runtime is entitled to reject that, which ends
    // the drain early and leaves the rest of the queue -- further state
    // changes, reference-space changes -- unread. Exactly what piles up when
    // focus is lost to the desktop and later regained.
    while (ev = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER},
           xrPollEvent(g.instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* s = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            // Every transition, not just the fatal one. Leaving VR and coming
            // back walks through several, and with no record there is no way to
            // tell afterwards which of them happened.
            VRLOG("xr: session state %d -> %d", (int)g.state, (int)s->state);
            g.state = s->state;
            if (s->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(g.session, &bi))) g.running = true;
            } else if (s->state == XR_SESSION_STATE_STOPPING) {
                end_frame_wait();   // no-op: mirror_on_present() already waited
                xrEndSession(g.session);
                g.running = false;
            } else if (s->state == XR_SESSION_STATE_LOSS_PENDING) {
                // Runtime restart, headset disconnect while running, etc.
                // Stop driving frames on a dying session -- render_frame()/
                // render_frame() would otherwise just start silently
                // failing xrWaitFrame/xrBeginFrame every Present from here on
                // with no diagnostic. NOT a full instance/session
                // destroy+recreate (out of scope for this pass); if the
                // runtime later sends a fresh READY (e.g. headset
                // reconnected), the branch above already re-begins the
                // session and resumes normally.
                VRLOG("xr: XR_SESSION_STATE_LOSS_PENDING -- halting frame submission");
                g.running = false;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            // Sent by any compliant runtime when the USER recenters via the
            // headset's own system-level gesture/controller button/menu (not
            // our Home key). Without this we'd have no way to react: our own
            // origin (g_originPos/g_originQuat) would go stale relative to
            // the runtime's space until the player also happened to press
            // Home.
            //
            // YAW RESET, not yaw-to-view: the player just told the RUNTIME
            // which way forward is, and startup's rule is that the runtime's
            // forward is the game's forward. Keeping our own yaw origin here
            // would be worse than stale -- it was measured against a space
            // that no longer exists, so it would apply a yaw nobody asked for.
            VRLOG("xr: XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING -- "
                  "adopting the runtime's new forward");
            g_recenterRequest.fetch_or(kRecenterPos | kRecenterYawReset);
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// Locates the head in LOCAL space and publishes yaw/pitch/roll + position
// relative to the recenter origin for the camera hook to consume.
void update_head_pose(XrTime displayTime)
{
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(g.viewSpace, g.space, displayTime, &loc)))
        return;
    constexpr XrSpaceLocationFlags kNeed =
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if ((loc.locationFlags & kNeed) != kNeed)
        return;

    Quat q{loc.pose.orientation.x, loc.pose.orientation.y,
           loc.pose.orientation.z, loc.pose.orientation.w};
    Vec3 p{loc.pose.position.x, loc.pose.position.y, loc.pose.position.z};

    // RAW (pre-recenter) head orientation, for latch_eye_cant() -- the cant is
    // head->eye, so both sides have to be in the same untransformed space.
    // Stored before the g_haveOrigin early-out below, which would otherwise
    // starve the latch on the very first frames.
    AcquireSRWLockExclusive(&g_cantLock);
    g_headRawQuat[0] = q.x; g_headRawQuat[1] = q.y;
    g_headRawQuat[2] = q.z; g_headRawQuat[3] = q.w;
    g_headRawValid = true;
    ReleaseSRWLockExclusive(&g_cantLock);

    const int req = g_recenterRequest.exchange(0);
    if (req) {
        // Build the yaw origin as a YAW-ONLY quaternion (extracted from the
        // current full orientation) rather than capturing the whole thing:
        // pitch and roll must keep reading as the headset's true absolute tilt,
        // not as a tilt relative to however the head happened to be held at the
        // moment of recentering.
        Vec3 fwd0 = rotate(q, {0, 0, -1});
        float yaw0 = std::atan2(fwd0.x, -fwd0.z);
        // NEGATED, and it has to be. The measured yaw and the quaternion that
        // produces it run in OPPOSITE directions: {0, sin(a/2), 0, cos(a/2)}
        // rotates +a about +Y, which takes forward (0,0,-1) to
        // (-sin a, 0, -cos a) -- and atan2(fwd.x, -fwd.z) reads that back as -a.
        //
        // Built from +yaw0 the origin therefore carried a measured yaw of -yaw0,
        // so conj(origin)*q left roughly 2*yaw0 instead of 0: recentering
        // DOUBLED the offset rather than cancelling it, and agreed only at zero.
        // That is the long-standing "recenter only works looking straight ahead".
        //
        // The same origin rotates the position offset below, so that was in the
        // wrong frame too by the same amount.
        if (req & kRecenterYawToView)
            g_originQuat = {0.0f, std::sin(-yaw0 * 0.5f), 0.0f, std::cos(-yaw0 * 0.5f)};
        else if (req & kRecenterYawReset)
            g_originQuat = {0.0f, 0.0f, 0.0f, 1.0f};   // the runtime's own forward
        // POSITION ALWAYS. Every request carries kRecenterPos; a camera change
        // asks for nothing else, and that is the point -- the world must not
        // swing round underneath the player just because they got into a
        // different truck, but the lean origin does have to follow them.
        if (req & kRecenterPos)
            g_originPos = loc.pose.position;
        g_haveOrigin = true;

        // Self-check, so this is verifiable from the log rather than by feel:
        // the residual is what recentring just achieved. For a yaw-to-view
        // request it must be ~0 at ANY recentre angle -- a residual tracking
        // 2*yaw0 is the sign-flip bug above returning. For the other kinds it
        // is simply the angle the head now sits at relative to the forward we
        // kept, and is expected to be non-zero.
        {
            Quat oq0{g_originQuat.x, g_originQuat.y, g_originQuat.z, g_originQuat.w};
            Vec3 rf = rotate(mul(conj(oq0), q), {0, 0, -1});
            VRLOG("recenter [%s%s%s]: head yaw %.1f deg -> residual %.2f deg",
                  (req & kRecenterPos)       ? "pos "       : "",
                  (req & kRecenterYawToView) ? "yaw->view " : "",
                  (req & kRecenterYawReset)  ? "yaw=runtime" : "",
                  yaw0 * 57.2957795f,
                  std::atan2(rf.x, -rf.z) * 57.2957795f);
        }

        // The UI plane rides along with an explicit recenter, and only with
        // that one -- it is the same gesture ("put everything where I am
        // looking"), which is why it no longer has a button of its own. A
        // camera change asks for position only and leaves the plane alone,
        // which is what makes a pinned plane survive getting into a truck.
        //
        // Only meaningful while the plane is world-pinned; head-locked has
        // nothing to move.
        if ((req & kRecenterYawToView) && g_uiPlaneMode.load() == kUiPlaneForward)
            g_uiPinRequest.store(kUiPinToView);

        // Re-arm the eye-cant latch as well.
        //
        // latch_eye_cant() is one-shot for the session, and the UI de-cant is
        // built from what it captured -- so a cant latched from a bad first
        // reading (tracking not settled, a runtime-supplied placeholder pose)
        // leaves the UI plane shifted sideways in PIXELS for the whole run.
        // That matches the reported symptom exactly: slid rather than rotated,
        // unaffected by recentering, and only ever cured by restarting the
        // game. Nothing else latched at startup produces a lateral offset that
        // survives a recenter.
        //
        // An EXPLICIT recenter is the natural place to redo it: that one
        // already means "re-establish the reference", the next frame with a
        // valid orientation re-latches, and the plausibility bound in
        // latch_eye_cant() still rejects nonsense. Deliberately not on the
        // automatic position-only recenters -- those now fire on every camera
        // change, and re-latching a fixed hardware property that often is
        // pointless churn on a value that cannot have moved.
        if (req & kRecenterYawToView) {
            AcquireSRWLockExclusive(&g_cantLock);
            g_cantLatched = false;
            ReleaseSRWLockExclusive(&g_cantLock);
        }
    }
    if (!g_haveOrigin)
        return;

    // Relative rotation: q_rel = inverse(origin) * head.
    Quat oq{g_originQuat.x, g_originQuat.y, g_originQuat.z, g_originQuat.w};
    Quat rel = mul(conj(oq), q);

    float yaw, pitch, roll;
    euler_from_rel(rel, yaw, pitch, roll);

    // Position relative to origin, expressed in the recentered head frame.
    Vec3 dp{p.x - g_originPos.x, p.y - g_originPos.y, p.z - g_originPos.z};
    Vec3 off = rotate(conj(oq), dp);

    AcquireSRWLockExclusive(&g_poseLock);
    g_headLook = {true, yaw, pitch, roll, off.x, off.y, off.z};
    g_relQuat[0]=rel.x; g_relQuat[1]=rel.y; g_relQuat[2]=rel.z; g_relQuat[3]=rel.w;
    ReleaseSRWLockExclusive(&g_poseLock);
}

// SnowRunner hit-tests winch points using the desktop cursor. Keep that pixel
// on the same world direction while the headset rotates. Inactive frames reset
// the baseline so menus never receive synthetic mouse movement.
void hold_winch_pointer_against_head(bool active)
{
    static bool havePrevious = false;
    static float previousYaw = 0.0f, previousPitch = 0.0f;
    static float carryX = 0.0f, carryY = 0.0f;
    if (!active) {
        havePrevious = false;
        carryX = carryY = 0.0f;
        return;
    }

    AcquireSRWLockShared(&g_poseLock);
    const HeadLook head = g_headLook;
    ReleaseSRWLockShared(&g_poseLock);
    if (!head.valid) return;

    const float oldYaw = previousYaw, oldPitch = previousPitch;
    const bool hadPrevious = havePrevious;
    previousYaw = head.yaw;
    previousPitch = head.pitch;
    havePrevious = true;
    if (!hadPrevious) return;

    float dyaw = head.yaw - oldYaw;
    while (dyaw > 3.14159265f) dyaw -= 6.28318531f;
    while (dyaw < -3.14159265f) dyaw += 6.28318531f;
    const float dpitch = head.pitch - oldPitch;
    if (std::fabs(dyaw) > 0.35f || std::fabs(dpitch) > 0.35f) {
        carryX = carryY = 0.0f;
        return;
    }

    RECT client{};
    if (!g.outputWindow || !GetClientRect(g.outputWindow, &client)) return;
    const float width = static_cast<float>(client.right - client.left);
    const float height = static_cast<float>(client.bottom - client.top);
    const XrFovf& fov = g.lastView[0].fov;
    const float horizontal = fov.angleRight - fov.angleLeft;
    const float vertical = fov.angleUp - fov.angleDown;
    if (width < 1.0f || height < 1.0f || horizontal < 0.05f || vertical < 0.05f)
        return;

    // g_headLook yaw uses the opposite sign from the reference implementation
    // (atan2(fwd.x, -fwd.z) vs atan2(-fwd.x, -fwd.z)).
    carryX -= dyaw * width / horizontal;
    carryY += dpitch * height / vertical;
    const int stepX = static_cast<int>(carryX);
    const int stepY = static_cast<int>(carryY);
    if (!stepX && !stepY) return;
    carryX -= static_cast<float>(stepX);
    carryY -= static_cast<float>(stepY);

    POINT point{};
    if (GetCursorPos(&point))
        SetCursorPos(point.x + stepX, point.y + stepY);
}

// The headset's OPTICAL per-eye HORIZONTAL FOV is a fixed hardware property --
// cached on first valid reading rather than re-derived from g.lastView[eye].fov
// live every single Present. Suspected cause of a reported AER-2x-mode
// artifact: a stable (not rotation-dependent) vertical ghost/double-image
// visible even with the head held still. If the runtime reports even tiny
// frame-to-frame noise in angleUp/angleDown (tracking/foveation jitter --
// not confirmed on this runtime, but plausible and NOT ruled out), the 2x
// warp's "old" (capture-time) and "new" (this-Present) vh would differ by
// that noise alone, indistinguishable to compute_eye_warp_homography() from
// a genuine scale change -- producing exactly this symptom with no actual
// rotation involved (the homography math itself checked out: R=Identity with
// matching old/new intrinsics reduces to an exact identity transform). A
// value that's semantically constant for the session shouldn't be resampled
// as if it might not be, independent of whether this turns out to be the
// full explanation.
std::atomic<float> g_cachedBaseHFov[2]{0.0f, 0.0f};

// Shared blitRect/aspect/base-angle prefix for both frustum-half-angle
// variants below -- they differ only in which scale they apply on top.
//
// ANCHORED ON HORIZONTAL, and it has to be, because that is the axis the
// content is actually rendered from. PROJ LOCK builds p00 = 1/tan(hfov/2) from
// render_hfov_deg(), i.e. from headset_hfov_deg(); the vertical then just
// follows the game's own projection aspect (p11 = p00 * p11_old/p00_old).
//
// This used to anchor on the runtime's VERTICAL half-angle and derive the
// horizontal as atan(tan(vh) * aspect). With a square canvas (aspect == 1) that
// declared hh == vh == the headset's VERTICAL half-angle, while the game had
// rendered a square frustum at the headset's HORIZONTAL half-angle -- so on any
// headset whose per-eye FOV is not square, every frame was declared under a
// window it was not drawn for. On a portrait-ish per-eye FOV (Quest 3) the
// declared window is the WIDER of the two, so the content was stretched to fill
// it: a uniform over-zoom of tan(vHalf)/tan(hHalf), needing a permanent ~105%
// Final-FOV trim to cancel. Uniform rather than a stretch precisely because the
// square canvas made the same error apply to both axes, which is why it read as
// "everything is slightly too big" and not as distortion.
//
// Averaged across eyes, deliberately: headset_hfov_deg() is an average and
// PROJ LOCK applies that one value to BOTH eyes, so declaring a per-eye number
// here would reintroduce a smaller version of the same disagreement.
void eye_frustum_base(uint32_t eye, float& aspect, float& baseH)
{
    XrRect2Di rect = g.blitRect[eye];
    if (rect.extent.width <= 0 || rect.extent.height <= 0)
        rect = {{0, 0}, {(int32_t)g.width, (int32_t)g.height}};  // not drawn yet
    aspect = rect.extent.height > 0
        ? (float)rect.extent.width / (float)rect.extent.height : 1.0f;

    baseH = g_cachedBaseHFov[eye].load();
    if (baseH <= 0.0f) {
        const float hfovDeg = headset_hfov_deg();
        if (hfovDeg > 0.0f) {
            baseH = hfovDeg * 0.5f * 0.0174532925f;
            g_cachedBaseHFov[eye].store(baseH);
            // The four RAW angles, because the derived halves cannot show
            // asymmetry: a frustum of -49/+45 and one of -47/+47 both report a
            // 47 deg half-angle, and only the first leaves an uncovered wedge.
            constexpr float kR2D = 57.2957795f;
            VRLOG("eye_frustum_base: eye%u half-angle %.4f deg, blit aspect %.4f | "
                  "raw L%.3f R%.3f U%.3f D%.3f deg",
                  eye, baseH * kR2D, aspect,
                  g.lastView[eye].fov.angleLeft  * kR2D,
                  g.lastView[eye].fov.angleRight * kR2D,
                  g.lastView[eye].fov.angleUp    * kR2D,
                  g.lastView[eye].fov.angleDown  * kR2D);
        }
    }
}

// WHERE THE DISPLAY IS ACTUALLY CENTRED, in radians, relative to the eye's
// optical axis. Negative = below it.
//
// A headset's per-eye frustum is asymmetric on BOTH axes, and the horizontal
// asymmetry is already handled (headset_hfov_deg() takes the widest half so the
// symmetric render encloses the real frustum). The vertical never was: vh below
// is derived from the blit rect's ASPECT and never consults the runtime at all,
// so we declared a frustum centred on the boresight while the panel is centred
// somewhere else. Measured on this hardware 2026-08-20:
//
//     U +44.000  D -55.000  ->  centre at -5.5 deg
//
// i.e. 11 degrees more display below the optical axis than above it. Forward
// therefore landed 5.5 deg above the middle of the panel, and the fix was a
// hand-dialled VerticalRecenter of -8 deg that only happened to be right for
// this headset.
//
// Applied as a TILT OF THE WHOLE EYE: the camera is pitched by it (head_look())
// and the submitted pose is pitched by the same amount (rendered_orientation()),
// so the symmetric frustum is aimed at the middle of the panel and the world
// still lands where it really is. Not as an asymmetric angleUp/angleDown, which
// was tried and reverted (docs/vr_framework_learnings.md Fix 1): declaring
// symmetric content under an asymmetric window warps it.
//
// IT USED TO TILT ONLY THE DECLARED POSE, with the content rendered level --
// telling the compositor the image pointed lower than it did. That moves the
// image down the panel, and the whole world with it: displayed rotated by the
// tilt in the HEAD's frame, a rotation that swings with the head. Inside a 3D
// scene nothing shows it, because everything moves together. A world-pinned
// quad is the one thing the compositor places truthfully, so it had to be
// counter-rotated to stay on that displaced world -- and on the screens where
// the quad is all there is (intro, loading, the pause menu) it then rolled
// visibly as the head turned left and right. Fixed 2026-10-29 by rendering the
// tilt instead of only declaring it; place_ui_plane_quad() lost its
// correction with it.
//
// CACHED like g_cachedBaseHFov, and for the same reason -- this is a fixed
// property of the optics, and resampling a semantically constant value every
// Present lets runtime jitter into a term that must not move.
std::atomic<float> g_cachedFovCenterPitch{0.0f};
std::atomic<bool>  g_fovCenterPitchValid{false};

float fov_center_pitch_cached()
{
    if (g_fovCenterPitchValid.load()) return g_cachedFovCenterPitch.load();

    float sum = 0.0f;
    int   valid = 0;
    for (int e = 0; e < 2; ++e) {
        const float up = g.lastView[e].fov.angleUp;
        const float dn = g.lastView[e].fov.angleDown;
        // A frustum with no extent has not been reported yet. Both signs are
        // the runtime's own convention (up positive, down negative), so the
        // centre is their mean, not their difference.
        if (std::fabs(up) + std::fabs(dn) > 0.01f) { sum += (up + dn) * 0.5f; ++valid; }
    }
    if (!valid) return 0.0f;   // no runtime yet: no correction, not a guess

    const float pitch = sum / (float)valid;
    g_cachedFovCenterPitch.store(pitch);
    g_fovCenterPitchValid.store(true);
    constexpr float kR2D = 57.2957795f;
    VRLOG("fov_center_pitch: display centre %.3f deg from the optical axis "
          "-- applied automatically, on top of VerticalRecenter", pitch * kR2D);
    return pitch;
}

// THE EYE'S VERTICAL TILT: the optics' own offset plus whatever the user
// dialled in on top. The camera (head_look()), the submitted pose
// (rendered_orientation()) and the head-locked quads all use this one function,
// because a quad left on the manual term alone would drift off centre by
// exactly the automatic one -- the same failure the manual term already had
// before the quads were taught to follow it.
//
// NO AUTOMATIC TERM UNDER OFF-CENTRE PROJECTION. That render already IS the
// eye's own lopsided frustum, aimed down the optical axis, so it covers the
// panel exactly; tilting it as well would push it off the top by the same
// amount. It used to be added anyway.
float submitted_vertical_pitch()
{
    const float automatic = offcenter_projection() ? 0.0f : fov_center_pitch_cached();
    return automatic + g_vertOffset.load();
}

// That tilt as a rotation about the eye's own right axis. Positive turns the
// view up (OpenXR: +Y up, -Z forward).
Quat vertical_tilt()
{
    const float half = submitted_vertical_pitch() * 0.5f;
    return {std::sin(half), 0.0f, 0.0f, std::cos(half)};
}

// THE ORIENTATION A FRAME IS RENDERED AT, given the runtime eye orientation it
// was built from: that eye's synthetic test cant, then the vertical tilt, both
// in the eye's own frame -- the rotations head_look() hands the camera, in the
// same order. The submission declares exactly this, and the stale-eye warp maps
// between two of these, so neither describes a frame other than the one drawn.
//
// The HARDWARE cant needs nothing here: the runtime's eye pose already
// contains it, and head_look() adds it to the camera to match.
Quat rendered_orientation(const XrQuaternionf& eyeOrientation, int eye)
{
    const Quat q{eyeOrientation.x, eyeOrientation.y, eyeOrientation.z,
                 eyeOrientation.w};
    return mul(mul(q, debug_cant(eye)), vertical_tilt());
}

// Frustum half-angles actually submitted for `eye` under its current
// blitRect and composited_fov_scale() -- the SAME formula the
// projection-view tail below uses for pv[e].fov, factored out so the 2x-mode
// warp's "new" (current-Present target) intrinsics are guaranteed to match
// what the compositor is actually told this Present.
//
// Deliberately does NOT know about the (since-removed) Final-FOV trim,
// and this is also what the warp's "old" (capture-time) intrinsics use -- an
// earlier version applied the trim for the capture side specifically
// (through the since-removed rendered_fov_scale()), reasoning that Kold
// should reflect "what the game actually rendered". That was wrong: the
// zoom effect from the trim isn't something the K-matrices need to track at
// all. It's already baked into the CONTENT (the GPU rendered a narrower
// slice of the world into the same pixel grid, via PROJ LOCK) purely by
// declaring that content under the always-real, always-untrimmed
// composited_fov_scale() window -- the compositor's own per-pixel stretch
// reproduces the zoom for free, no rescaling by us required, for the fresh
// eye. Using a DIFFERENT (trimmed) scale for Kold than Knew made the warp
// explicitly convert from the trimmed scale back to the real one every
// Present -- i.e. digitally UNDO the zoom on the stale eye while the fresh
// eye kept it untouched, producing two different effective zoom levels
// alternating every Present. Confirmed in-headset as a stable (non-rotation,
// non-jitter -- logged relAngle=0.000 with a constant nonzero old/new gap)
// ghosting artifact that got worse the further the trim was from 100%,
// exactly matching this mechanism. Both K's staying on this one function
// keeps the warp doing ONLY rotation, same as before the trim feature
// existed; the trim's zoom rides along in the content unchanged, same as the
// fresh eye.
void eye_frustum_half_angles(uint32_t eye, float& hh, float& vh)
{
    float aspect, baseH;
    eye_frustum_base(eye, aspect, baseH);
    // Horizontal is declared at exactly the headset's own horizontal FOV (times
    // the render size), which is what PROJ LOCK rendered -- so the image
    // spans the display's full width and is never cropped left/right, whatever
    // shape the canvas is. Vertical follows the CONTENT's aspect, because that
    // is what the game's projection used for p11. If the canvas is squarer than
    // the headset's per-eye FOV the result is honest letterboxing: we simply
    // have no content for those angles, and claiming otherwise is what the old
    // formula did.
    hh = std::atan(std::tan(baseH) * composited_fov_scale());

    // THE VERTICAL, from the headset when asked and from the canvas otherwise.
    //
    // These two branches MUST stay the mirror of what PROJ LOCK writes into
    // m[5], or the content is declared under a window it was not drawn for --
    // which is a stretch, not a crop. The pairing is: headset branch here <->
    // xr::render_vfov_deg() there; aspect branch here <-> m[0]*aspect there.
    const float vdeg = match_headset_vfov() ? render_vfov_deg() : 0.0f;
    if (vdeg > 1.0f)
        vh = vdeg * 0.5f * 0.0174532925f;
    else
        vh = (aspect > 1.0e-4f) ? std::atan(std::tan(hh) / aspect) : hh;
}
// THE ONE REFERENCE INSTANT, and the reason there has to be exactly one.
//
// Three separate places need to agree about "when is this frame": where the
// stale eye's retained copy was captured (retain_eye_for_warp), where the warp
// carries it to (warp_homography_for), and what we declare to the compositor
// (the submission loops). Any pair of those disagreeing is an error that
// survives into the headset -- and if they disagree PER EYE it is a stereo
// split rather than a shared lag, which is much worse. That is precisely how
// declaring g_renderView for the fresh eye alone failed; see g_renderView.
//
// So all three read this, and it returns the head/eye pose the frame's CAMERA
// was actually built from. g.lastView is a later locate that no frame was ever
// rendered from -- using it made the retained copy's recorded orientation one
// Present too new, aimed the warp at an instant nothing was rendered at, and
// told the compositor the image needed no correction when it did.
//
// Falls back to g.lastView before the first camera build, where there is no
// render instant yet and the two are the best available answer anyway.
XrPosef reference_view_pose(uint32_t eye)
{
    XrPosef out = g.lastView[eye].pose;
    AcquireSRWLockShared(&g_renderViewLock);
    // The build ON SCREEN, which is not the latest one on the map, garage and
    // menus -- see update_on_screen(). Declaring the latest build's pose there
    // put every eye a frame of head motion away from what it showed.
    if (g_onScreenViewsValid)   out = g_onScreenViews[eye].pose;
    else if (g_renderViewValid) out = g_renderView[eye].pose;
    ReleaseSRWLockShared(&g_renderViewLock);
    return out;
}

// VDXR has been observed to hand back a sentinel garbage predictedDisplayTime
// (exactly 11111111) before its first valid pose. Callers use this to avoid
// locating a head pose -- or submitting a layer built from one -- at that
// bogus time rather than trusting it blindly.
bool is_bogus_display_time(XrTime t)
{
    return t == 11111111;
}

// --------------------------------------------------------------------------
// Menu overlay quad layer (headset-visible mod GUI)
// --------------------------------------------------------------------------

// The eye swapchains deliberately prefer an sRGB format (g.swapFormat) so the
// compositor de-gammas the game's sRGB pixels correctly. ImGui's DX11 backend
// does the OPPOSITE: it writes already-gamma-space colour values directly,
// with no linear->sRGB conversion of its own, because it assumes a plain
// UNORM target. Binding an sRGB-format RTV on top of that made the menu quad
// look washed out/too bright in-headset -- the GPU was silently re-applying a
// linear->sRGB encode to values that were already gamma-encoded. Fixed by
// giving the menu swapchain its own, separately-negotiated UNORM format
// instead of reusing g.swapFormat.
//
// The game's UI plane deliberately does NOT reuse this. Its pixels come from
// the game's own shaders, not from ImGui, so the treatment that is already
// correct for them is the one the eye images get -- see ensure_ui_swapchain.
int64_t pick_menu_swap_format()
{
    uint32_t fmtCount = 0;
    xrEnumerateSwapchainFormats(g.session, 0, &fmtCount, nullptr);
    std::vector<int64_t> formats(fmtCount);
    xrEnumerateSwapchainFormats(g.session, fmtCount, &fmtCount, formats.data());
    const int64_t preferred[] = {
        DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM};
    for (int64_t pref : preferred)
        for (int64_t f : formats)
            if (f == pref) return pref;
    // No non-sRGB option offered -- fall back to whatever the eyes use rather
    // than fail the menu quad outright; the brightness mismatch is the lesser
    // problem of the two.
    VRLOG("menu quad: no UNORM swapchain format available, reusing eye format (will look too bright)");
    return g.swapFormat;
}

bool ensure_menu_swapchain(uint32_t w, uint32_t h)
{
    if (g.menuSwapchain != XR_NULL_HANDLE && g.menuSwapW == w && g.menuSwapH == h)
        return true;

    if (g.menuSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(g.menuSwapchain);
        g.menuSwapchain = XR_NULL_HANDLE;
        g.menuRtvs.clear();
    }

    g.menuSwapFormat = pick_menu_swap_format();

    XrSwapchainCreateInfo xsci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    xsci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    xsci.format = g.menuSwapFormat;
    xsci.width = w;
    xsci.height = h;
    xsci.sampleCount = 1;
    xsci.faceCount = 1;
    xsci.arraySize = 1;
    xsci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(g.session, &xsci, &g.menuSwapchain))) {
        VRLOG("menu quad: xrCreateSwapchain failed (%ux%u)", w, h);
        g.menuSwapchain = XR_NULL_HANDLE;
        return false;
    }

    uint32_t imgCount = 0;
    xrEnumerateSwapchainImages(g.menuSwapchain, 0, &imgCount, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> images(imgCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (XR_FAILED(xrEnumerateSwapchainImages(g.menuSwapchain, imgCount, &imgCount,
                                             (XrSwapchainImageBaseHeader*)images.data()))) {
        VRLOG("menu quad: xrEnumerateSwapchainImages failed");
        xrDestroySwapchain(g.menuSwapchain);
        g.menuSwapchain = XR_NULL_HANDLE;
        return false;
    }
    g.menuRtvs.resize(imgCount);
    for (uint32_t i = 0; i < imgCount; ++i) {
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = (DXGI_FORMAT)g.menuSwapFormat;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(g.device->CreateRenderTargetView(images[i].texture, &rd, &g.menuRtvs[i]))) {
            VRLOG("menu quad: RTV create failed");
            xrDestroySwapchain(g.menuSwapchain);
            g.menuSwapchain = XR_NULL_HANDLE;
            g.menuRtvs.clear();
            return false;
        }
    }

    g.menuSwapW = w;
    g.menuSwapH = h;
    VRLOG("menu quad: swapchain %ux%u, %u images", w, h, imgCount);
    return true;
}

// The smaller of the up/down half-angles of the eye views last submitted, and
// the smaller of the left/right ones that both eyes can see, in radians; 0
// until the first projection layer. See build_menu_quad_layer.
std::atomic<float> g_submittedHalfV{0.0f};
std::atomic<float> g_submittedHalfH{0.0f};

// Builds and renders a headset-visible quad layer for the mod settings panel
// when (and only when) the menu is open, reusing the SAME ImGui draw data
// hooks::menu_hook_update() already built earlier this Present (see
// swapchain_hook.cpp's Detour_Present ordering) -- never rebuilt here. Head-
// locked (viewSpace): floats a fixed distance in front of the eyes regardless
// of head orientation, so no pose math is needed. Cropped to what the menu
// actually drew -- the panel and any tooltip or popup reaching past it
// (hooks::menu_drawn_rect()) -- rather than the whole (mostly empty)
// desktop-sized texture.
bool build_menu_quad_layer(XrCompositionLayerQuad& quad)
{
    uint32_t w = 0, h = 0;
    if (!hooks::menu_wants_quad(w, h) || w == 0 || h == 0)
        return false;
    if (!ensure_menu_swapchain(w, h))
        return false;

    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t idx = 0;
    if (XR_FAILED(xrAcquireSwapchainImage(g.menuSwapchain, &ai, &idx)))
        return false;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage(g.menuSwapchain, &wi))) {
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(g.menuSwapchain, &ri);
        return false;
    }

    hooks::menu_render_to_extra_target(g.menuRtvs[idx].Get(), g.ctx.Get());
    cursoroverlay::draw(g.device.Get(), g.ctx.Get(), g.menuRtvs[idx].Get(),
                        g.outputWindow, w, h, false);

    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(g.menuSwapchain, &ri);

    // THE PANEL places the quad; WHAT WAS DRAWN sizes it. A tooltip or popup
    // reaching past the panel grows the quad around it, and the panel stays
    // exactly where it was.
    int px = 0, py = 0, pw = 0, ph = 0;
    if (!hooks::menu_panel_rect(px, py, pw, ph))
        return false;   // not laid out yet -- nothing to place it by
    int dx = px, dy = py, dw = pw, dh = ph;
    hooks::menu_drawn_rect(dx, dy, dw, dh);   // leaves the panel's rect if none
    // Small padding so the window border isn't clipped.
    constexpr int kPad = 12;
    const int x0 = (std::max)(0, dx - kPad);
    const int y0 = (std::max)(0, dy - kPad);
    const int x1 = (std::min)((int)w, dx + dw + kPad);
    const int y1 = (std::min)((int)h, dy + dh + kPad);
    if (x1 <= x0 || y1 <= y0)
        return false;   // dragged entirely off the texture

    // ONE PIXEL, ONE SIZE: metres per panel pixel, a metre away
    // (hooks::menu_quad_metres_per_px -- the default-width panel is 0.42 m at
    // 100%). The quad is as big as what it shows, so dragging the window wider
    // makes the panel wider in the headset rather than smaller, and the UI size
    // slider -- which scales the pixels -- makes all of it bigger.
    //
    // It used to be a fixed WIDTH in metres for whatever the crop came to, and
    // the window sized itself to its content: every tab with a longer line, and
    // every popup the crop had to take in, shrank the whole panel.
    const float mpp = hooks::menu_quad_metres_per_px();
    constexpr float kQuadDistM = 1.0f;

    // How big the panel may get before it scrolls instead (hooks reads this
    // next frame): the quad is head-locked, so anything past the edge of the
    // view could never be looked at. The smaller of the two eyes' half-angles
    // -- vertically as SUBMITTED, which the map view shrinks; horizontally what
    // BOTH eyes can see -- and a share of that, so the panel ends well inside
    // the picture rather than at its rim.
    {
        constexpr float kMenuFovShareV = 0.6f, kMenuFovShareH = 0.8f;
        const float halfV = g_submittedHalfV.load();
        const float halfH = g_submittedHalfH.load();
        hooks::set_menu_quad_max_size_m(
            halfH > 0.0f ? 2.0f * kQuadDistM * std::tan(halfH * kMenuFovShareH) : 0.0f,
            halfV > 0.0f ? 2.0f * kQuadDistM * std::tan(halfV * kMenuFovShareV) : 0.0f);
    }

    quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad.space = g.viewSpace;   // head-locked
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = g.menuSwapchain;
    quad.subImage.imageRect = {{x0, y0}, {x1 - x0, y1 - y0}};
    quad.subImage.imageArrayIndex = 0;
    // Follow the vertical tilt. Every eye is rendered and declared tilted by
    // this angle (see submitted_vertical_pitch()), so the middle of the image
    // is that far off the head's forward, and a quad left at identity would
    // sit off centre by exactly the tilt -- which is what made the top run off
    // the rendered image.
    //
    // Both the orientation AND the position are rotated: turning the quad alone
    // would tilt it in place without moving it, which fixes nothing. Positive
    // pitch takes the view forward toward +Y, so the panel rides up with it.
    const float vpitch = submitted_vertical_pitch();
    const float vhalf  = vpitch * 0.5f;
    const Quat  q{std::sin(vhalf), 0.0f, 0.0f, std::cos(vhalf)};
    // The PANEL's centre goes straight ahead along that tilt; the crop's centre
    // is wherever the extra windows put it relative to that (image y is down,
    // the quad's y is up).
    const float offX =  ((float)(x0 + x1) * 0.5f - ((float)px + (float)pw * 0.5f)) * mpp;
    const float offY = -((float)(y0 + y1) * 0.5f - ((float)py + (float)ph * 0.5f)) * mpp;
    const Vec3  c = rotate(q, {offX, offY, -kQuadDistM});
    quad.pose.orientation = {q.x, q.y, q.z, q.w};
    quad.pose.position = {c.x, c.y, c.z};
    quad.size = {(float)(x1 - x0) * mpp, (float)(y1 - y0) * mpp};
    return true;
}

// --------------------------------------------------------------------------
// UI quad layer (the GAME's HUD, menus and map)
// --------------------------------------------------------------------------

bool ensure_ui_swapchain(uint32_t w, uint32_t h)
{
    if (g.uiSwapchain != XR_NULL_HANDLE && g.uiSwapW == w && g.uiSwapH == h)
        return true;

    if (g.uiSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(g.uiSwapchain);
        g.uiSwapchain = XR_NULL_HANDLE;
        g.uiRtvs.clear();
    }
    // Nothing to re-submit until a fresh image has been released into the new
    // swapchain -- the old one's images are gone.
    g.uiQuadImageValid = false;
    g.uiQuadIdle = 0;

    // THE EYE FORMAT, not the menu's. The UI capture holds the values the
    // game's own UI shaders emit, which is exactly what the backbuffer holds --
    // so the colour path that is already known-correct for the backbuffer is
    // the one to copy. That path is: read through a view that decodes to
    // linear, write through a view that encodes back, hand the compositor an
    // _SRGB image it knows to decode. Reusing the menu's UNORM format instead
    // shipped a UI that was visibly too bright, because a plain UNORM image
    // hands the runtime gamma-encoded numbers and lets it treat them as linear.
    g.uiSwapFormat = g.swapFormat;

    XrSwapchainCreateInfo xsci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    xsci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    xsci.format = g.uiSwapFormat;
    xsci.width = w;
    xsci.height = h;
    xsci.sampleCount = 1;
    xsci.faceCount = 1;
    xsci.arraySize = 1;
    xsci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(g.session, &xsci, &g.uiSwapchain))) {
        VRLOG("ui plane: xrCreateSwapchain failed (%ux%u) -- UI returns to the render", w, h);
        g.uiSwapchain = XR_NULL_HANDLE;
        g_uiPlaneFailed.store(true);
        return false;
    }

    uint32_t imgCount = 0;
    xrEnumerateSwapchainImages(g.uiSwapchain, 0, &imgCount, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> images(imgCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (XR_FAILED(xrEnumerateSwapchainImages(g.uiSwapchain, imgCount, &imgCount,
                                             (XrSwapchainImageBaseHeader*)images.data()))) {
        VRLOG("ui plane: xrEnumerateSwapchainImages failed -- UI returns to the render");
        xrDestroySwapchain(g.uiSwapchain);
        g.uiSwapchain = XR_NULL_HANDLE;
        g_uiPlaneFailed.store(true);
        return false;
    }
    g.uiRtvs.resize(imgCount);
    for (uint32_t i = 0; i < imgCount; ++i) {
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = (DXGI_FORMAT)g.uiSwapFormat;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(g.device->CreateRenderTargetView(images[i].texture, &rd, &g.uiRtvs[i]))) {
            VRLOG("ui plane: RTV create failed -- UI returns to the render");
            xrDestroySwapchain(g.uiSwapchain);
            g.uiSwapchain = XR_NULL_HANDLE;
            g.uiRtvs.clear();
            g_uiPlaneFailed.store(true);
            return false;
        }
    }

    g.uiSwapW = w;
    g.uiSwapH = h;
    VRLOG("ui plane: swapchain %ux%u, %u images", w, h, imgCount);
    return true;
}

// Rebuilds an orientation as a LEVEL look along the same forward -- yaw then
// pitch, no roll. The head is rarely perfectly level when the pin is taken, and
// baking that tilt into a plane the player then has to read is the one part of
// "put it where I am looking" they did not ask for.
//
// Yaw and pitch are read off the forward vector rather than composed out of a
// matrix: for a -Z-forward, +Y-up right-handed space, Ry(t) takes (0,0,-1) to
// (-sin t, 0, -cos t) and Rx(p) takes it to (0, sin p, -cos p), which is the
// whole derivation.
Quat level_look(Quat q)
{
    Vec3 f = rotate(q, {0, 0, -1});
    const float len = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
    if (len < 1.0e-6f) return q;
    f = {f.x / len, f.y / len, f.z / len};
    // Straight up or straight down has no meaningful yaw to keep -- snapping to
    // whatever atan2 makes of two near-zero terms would spin the plane. Keep
    // the head's own orientation instead; it is at least continuous.
    if (std::fabs(f.y) > 0.999f) return q;

    const float yaw   = std::atan2(-f.x, -f.z);
    const float pitch = std::asin((std::max)(-1.0f, (std::min)(1.0f, f.y)));
    const Quat qy{0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
    const Quat qp{std::sin(pitch * 0.5f), 0.0f, 0.0f, std::cos(pitch * 0.5f)};
    return mul(qy, qp);
}

// THE MAP USES THE SAME DISTANCE AS EVERYTHING ELSE, and can, because of what
// the one-eye capture changed.
//
// It used to be pinned at 100 m on this reasoning: screen-space UI painted into
// a stereo pair sits at infinity -- identical pixels in both eyes, zero
// disparity -- so a quad at any near distance disagreed with it by
// (IPD/2)/distance in each eye, and only something effectively infinite lined
// up. That assumed the plane had to serve both eyes equally.
//
// It does not. The plane is hung on ONE eye (kUiCaptureEye) -- centred on its
// position, along its axis, subtending its frustum -- and the map's markers are
// placed for the plane itself, from between the eyes (hooks/marker_camera.h),
// which holds at ANY distance. Where the markers are the game's own, the
// capture comes from that one eye and overlays its own render exactly, at any
// distance too. Either way distance is free, and what it changes is real
// stereo depth rather than a misalignment.
//
// So there is nothing map-specific left to tune, and it follows the UI plane's
// own distance. The map's SIZE still tracks that distance (it is derived as
// 2*d*tan(hh)) so its apparent size never moves -- only the depth it sits at.

// Which eye's UI reaches the plane on the screens where it cannot be both eyes'
// (build_ui_quad_layer), and the eye a head-locked plane hangs on everywhere.
// Fixed rather than chosen: any consistent answer removes the alternation, and
// a stable one keeps UI the game projected itself sitting on a single eye's
// projection instead of switching between two.
constexpr int kUiCaptureEye = 0;

// And which eye's WINCH MARKERS reach both eyes, for the same reason and with
// the same trade -- see winch_layer.hpp. Kept as its own constant rather than
// shared with the UI: they are separate layers captured at separate sites, and
// tying them together would mean neither could be moved without moving the
// other for no reason.
constexpr int kWinchCaptureEye = 0;

// Copies this frame's capture into the quad's swapchain. Split out only so the
// builder can say plainly which frames refresh the image and which re-submit
// the last one.
bool refresh_ui_quad_image(ID3D11ShaderResourceView* src, uint32_t w, uint32_t h)
{
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t idx = 0;
    if (XR_FAILED(xrAcquireSwapchainImage(g.uiSwapchain, &ai, &idx)))
        return false;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (XR_FAILED(xrWaitSwapchainImage(g.uiSwapchain, &wi))) {
        xrReleaseSwapchainImage(g.uiSwapchain, &ri);
        return false;
    }

    ID3D11RenderTargetView* rtv = g.uiRtvs[idx].Get();
    D3D11_VIEWPORT vp{0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
    // Every texel is written by the draw, so no clear -- but the blend state
    // has to be forced off, or whatever the previous pass left bound would
    // blend the capture against this slot's older image instead of replacing it.
    hooks::set_self_targets(true);
    g.ctx->OMSetRenderTargets(1, &rtv, nullptr);
    hooks::set_self_targets(false);
    g.ctx->RSSetViewports(1, &vp);
    g.ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    g.ctx->IASetInputLayout(nullptr);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->VSSetShader(g.vs.Get(), nullptr, 0);
    g.ctx->PSSetShader(g.uiCopyPs.Get(), nullptr, 0);
    ID3D11SamplerState* smp = g.sampler.Get();
    g.ctx->PSSetShaderResources(0, 1, &src);
    g.ctx->PSSetSamplers(0, 1, &smp);
    g.ctx->Draw(3, 0);
    // The capture target is a render target again on the very next frame, and
    // D3D silently unbinds (and warns) if it is still bound as an SRV.
    ID3D11ShaderResourceView* nullSrv = nullptr;
    g.ctx->PSSetShaderResources(0, 1, &nullSrv);

    // Exactly one VR cursor: UI normally, final eye while the user has selected
    // manual world/winch mode with F8.
    if (!hooks::is_menu_open() && !winch_cursor_active()) {
        const DXGI_FORMAT fmt = static_cast<DXGI_FORMAT>(g.uiSwapFormat);
        const bool srgb = fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                          fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        cursoroverlay::draw(g.device.Get(), g.ctx.Get(), rtv, g.outputWindow,
                            w, h, srgb);
    }

    xrReleaseSwapchainImage(g.uiSwapchain, &ri);
    return true;
}
// PLACING A QUAD ON THE UI PLANE -- pinned in the world or head-locked, at the
// UI distance, hung on the capture eye so its pixels overlay that eye's render
// exactly.
//
// SHARED, and it has to be. In 2D screen mode the scene plane and the game's UI
// plane are submitted as two quads at the SAME pose and size, and the UI only
// sits on the screen because the two cannot drift. Two copies of this that
// agreed today would stop agreeing the first time one was touched.
//
// headLocked forces the head-locked branch: the 3D map needs it, because its
// markers are screen-space and only line up while the plane tracks the render.
void place_ui_plane_quad(XrCompositionLayerQuad& quad, XrTime displayTime,
                         bool headLocked)
{
    const float dist = g_uiPlaneDistM.load();

    // THE VERTICAL TILT, for the head-locked placement below.
    //
    // Every eye is rendered and declared tilted by it (rendered_orientation()),
    // so the middle of the image sits that far below the head's forward, and a
    // head-locked quad has to be tilted the same to sit on it. The world-pinned
    // placement needs none of it: the tilt is in the camera as well as the
    // declared pose, so the world is displayed where it really is, and a quad
    // pinned in it simply stays on it.
    //
    // ZERO ON A FLAT SCREEN, because in 2D there IS no projection layer -- the
    // scene is a quad too and pv[] is never submitted, and the game's camera
    // carries no tilt there (head_look()).
    const float vpitchRad = flat_screen_active() ? 0.0f : submitted_vertical_pitch();

    // THE HEAD, located once for both placements below. The world pin is taken
    // from it; the map's eye-relative placement needs it to express the
    // capture eye in the quad's own space. Without it both fall back to plain
    // head-centred placement.
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    constexpr XrSpaceLocationFlags kNeedLoc =
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    const bool haveHead =
        XR_SUCCEEDED(xrLocateSpace(g.viewSpace, g.space, displayTime, &loc)) &&
        (loc.locationFlags & kNeedLoc) == kNeedLoc;
    const Quat qh{loc.pose.orientation.x, loc.pose.orientation.y,
                  loc.pose.orientation.z, loc.pose.orientation.w};


    // WORLD-PINNED. Never when the caller demands head-locking (the 3D map, to
    // stay aligned with the render), and never before a pin has been taken.
    bool pinned = false;
    if (!headLocked && g_uiPlaneMode.load() == kUiPlaneForward) {
        // No head pose, no world pin: fall through to head-locked for this
        // frame rather than submit a placement built on a stale one.
        int pinReq = g_uiPinRequest.exchange(kUiPinNone);
        if (haveHead && (pinReq != kUiPinNone || !g_uiPinValid)) {
            // Never pinned and nobody asked: the plane still has to go
            // somewhere, and the runtime's forward is the same answer startup
            // gives the game's forward.
            if (pinReq == kUiPinNone) pinReq = kUiPinToRuntimeForward;
            // Identity IS the runtime's forward, and it is level by
            // construction -- no pitch to inherit from where the head was held.
            const Quat lq = (pinReq == kUiPinToView) ? level_look(qh)
                                                     : Quat{0.0f, 0.0f, 0.0f, 1.0f};
            AcquireSRWLockExclusive(&g_uiPinLock);
            g_uiPinPose.orientation = {lq.x, lq.y, lq.z, lq.w};
            g_uiPinPose.position = loc.pose.position;
            g_uiPinValid = true;
            ReleaseSRWLockExclusive(&g_uiPinLock);
            VRLOG("ui plane: pinned %s at %.2f %.2f %.2f",
                  pinReq == kUiPinToView ? "to the head's look" : "to runtime forward (level)",
                  loc.pose.position.x, loc.pose.position.y, loc.pose.position.z);
        }

        AcquireSRWLockShared(&g_uiPinLock);
        const bool have = haveHead && g_uiPinValid;
        const Quat qp{g_uiPinPose.orientation.x, g_uiPinPose.orientation.y,
                      g_uiPinPose.orientation.z, g_uiPinPose.orientation.w};
        const XrVector3f pivot = g_uiPinPose.position;
        ReleaseSRWLockShared(&g_uiPinLock);

        if (have) {
            // Straight out along the pinned gaze, from where the head was when
            // the pin was taken. Distance is read live so the slider still
            // moves the plane along that ray after pinning.
            const Vec3 off = rotate(qp, {0.0f, 0.0f, -dist});

            // EXACTLY WHERE IT WAS PINNED, with no correction. There used to be
            // one: while the vertical tilt was only DECLARED, the projection
            // layer showed the whole world rotated by it about the head, and
            // this quad was rotated by the same amount to stay on that world --
            // which, with nothing else on screen (intro, loading, the pause
            // menu), read as the plane rolling when you looked left and right.
            // The camera is tilted as well now (head_look()), the world is
            // where the runtime thinks it is, and a pinned plane needs nothing.
            quad.space = g.space;
            quad.pose.orientation = {qp.x, qp.y, qp.z, qp.w};
            quad.pose.position = {pivot.x + off.x, pivot.y + off.y, pivot.z + off.z};
            pinned = true;
        }
    }

    if (!pinned) {
        // HEAD-LOCKED, following the vertical recenter for the same reason the
        // menu quad does: the projection layer pitches every eye pose by that
        // angle, so a quad left at identity sits wherever the UNPITCHED forward
        // pointed -- off centre by exactly the recenter amount. On the map that
        // is not cosmetic, it is the alignment.
        const float vhalf = vpitchRad * 0.5f;
        Quat q{std::sin(vhalf), 0.0f, 0.0f, std::cos(vhalf)};
        Vec3 origin{0.0f, 0.0f, 0.0f};

        // HUNG ON THE CAPTURE EYE, not on the head.
        //
        // The capture came out of ONE eye (kUiCaptureEye), so the image it
        // holds is that eye's projection and nothing else's. Reproducing it
        // exactly therefore means reproducing that eye: centre the plane on the
        // eye's POSITION, aim it along the eye's AXIS, and let it subtend the
        // eye's frustum. Do that and the capture eye sees the plane overlaying
        // its own render pixel for pixel, at any distance at all.
        //
        // Two errors disappear together here, which is why it is one expression
        // rather than two corrections:
        //
        //   CANT. On outward-angled panels the eye's axis is head*cant, so an
        //   element at the centre of that render sits degrees off the head's
        //   forward. A quad on the head's forward is rotated by the cant
        //   against the image it came from.
        //
        //   THE HEAD/VIEW OFFSET. VIEW space is not the mid-eye -- measured on
        //   this runtime as a ~1.66 deg rotation common to both eyes (see
        //   latch_eye_cant, which had to switch to a mid-eye reference for
        //   exactly this reason). Correcting only for cant would leave that
        //   behind.
        //
        // conj(head) * eye carries both by construction, so neither has to be
        // named or measured separately.
        //
        // g.lastView, not reference_view_pose(): eye-to-head is rigid hardware,
        // so it only has to be read from ONE consistent instant, and both terms
        // here come from this Present's own locate. Mixing the render instant
        // in would fold a frame of head motion into a constant.
        //
        // This used to be map-only. That left normal menus centred on VIEW
        // space while their pixels (and the hardware cursor composited into
        // them) came from kUiCaptureEye. With head translation the two origins
        // acquired visible parallax. Keep every head-locked UI plane on the
        // eye that actually produced its texture; size remains an independent
        // presentation choice, but pose identity no longer changes by screen.
        //
        // While the world markers are placed from between the eyes the texture
        // is every frame's rather than this eye's, and the plane stays here all
        // the same: those markers are placed for wherever the plane is
        // (ui_plane_in_camera), and static UI has no eye of its own.
        if (haveHead) {
            const XrView& ev = g.lastView[kUiCaptureEye];
            const Quat qe{ev.pose.orientation.x, ev.pose.orientation.y,
                          ev.pose.orientation.z, ev.pose.orientation.w};
            const Vec3 pe{ev.pose.position.x - loc.pose.position.x,
                          ev.pose.position.y - loc.pose.position.y,
                          ev.pose.position.z - loc.pose.position.z};
            q      = mul(mul(conj(qh), qe), q);
            origin = rotate(conj(qh), pe);
        }

        quad.space = g.viewSpace;
        quad.pose.orientation = {q.x, q.y, q.z, q.w};
        // Straight out along whatever that orientation faces, from whatever it
        // is hung on -- reduces to the old (0, d sin, -d cos) exactly when the
        // eye term is absent.
        const Vec3 off = rotate(q, {0.0f, 0.0f, -dist});
        quad.pose.position = {origin.x + off.x, origin.y + off.y, origin.z + off.z};
    }
}


// Builds (and renders) the game UI's own composition layer. Head-locked or
// pinned in the world, sized as a fraction of the render's own angular extent
// -- except on the map, which is forced head-locked at the map UI scale of the
// full extent, with its markers placed for that plane.
// THE SCENE, ON A PLANE. 2D mode's whole substance: the frame that would have
// gone to the eye projection is shown on a quad instead, and the projection is
// not submitted at all.
//
// NO COPY AND NO SWAPCHAIN OF ITS OWN. The quad points straight at the eye
// swapchain the blit just released, with that eye's blitRect as the subImage --
// the runtime keeps presenting the last released image, exactly as the UI quad
// relies on. So a flat screen costs one more layer and nothing else.
//
// MONO by construction: one image, both eyes. That is what makes it 2D, and it
// is why eye_side_offset() has to read zero here -- see the note there.
//
// SIZED AND PLACED LIKE THE UI PLANE, because the UI is submitted as a second
// quad on top and the two have to coincide. Same size expression, same
// place_ui_plane_quad() call, never head-locked: on a panel the map's markers
// ride the panel rather than the render.
bool build_scene_quad_layer(XrCompositionLayerQuad& quad, XrTime displayTime)
{
    if (!flat_screen_active()) return false;

    const uint32_t eye = (uint32_t)present_route_eye();
    if (eye > 1 || g.swapchain[eye] == XR_NULL_HANDLE || !g.eyeEverRendered[eye])
        return false;

    const XrRect2Di rect = g.blitRect[eye];
    if (rect.extent.width <= 0 || rect.extent.height <= 0)
        return false;

    float hh = 0.0f, vh = 0.0f;
    eye_frustum_half_angles(eye, hh, vh);
    if (hh <= 1.0e-4f || vh <= 1.0e-4f)
        return false;

    quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
    // OPAQUE, unlike the two quads above it: this is the background, not an
    // overlay. Blending it against the void would show the void through it.
    quad.layerFlags = 0;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = g.swapchain[eye];
    quad.subImage.imageRect = rect;
    quad.subImage.imageArrayIndex = 0;

    const float widthM = g_uiPlaneSizeM.load();
    quad.size = {widthM, widthM * (std::tan(vh) / std::tan(hh))};

    place_ui_plane_quad(quad, displayTime, false);
    return true;
}

bool build_ui_quad_layer(XrCompositionLayerQuad& quad, XrTime displayTime)
{
    // THE MAP'S UI REDIRECT, reported once on the first map entry.
    //
    // WHAT IT CAN TELL YOU: whether the redirect ran at all this frame, and
    // whether the UI shaders this build KNOWS ABOUT are being seen.
    //
    // WHAT IT CANNOT: whether the set is complete. A shader that SHOULD be UI
    // but is in no role is not counted by anything here -- "7 of 7 recognised"
    // is equally true when an eighth is drawing into the render unnoticed. Read
    // it as "the machinery is running", never as "nothing is missing".
    //
    // Why the map is where it shows: the map applies the AER eye offset as a
    // projection SHEAR, so a UI draw left in the render is pushed sideways by
    // opposite amounts in the two eyes and each loses the far side of it. World
    // markers keep working, because for geometry at depth that shear is the
    // correct stereo -- and that asymmetry is the real diagnostic.
    if (hooks::in_map_view()) {
        static std::atomic<bool> s_logged{false};
        if (!s_logged.exchange(true))
            VRLOG("map UI: redirect %s -- ui layer %s this frame, %d of %d KNOWN "
                  "UI shaders seen (this cannot see a UI shader that is in no "
                  "role at all -- if part of the UI is split between the eyes, "
                  "look for a missing one in the Shader Cull tab)",
                  uilayer::drew_this_frame() ? "ran" : "did NOT capture",
                  uilayer::active() ? "active" : "inactive",
                  hooks::cull_role_seen_count(hooks::kCullUi),
                  hooks::cull_role_hash_count(hooks::kCullUi));
    }

    uint32_t w = 0, h = 0;
    if (!uilayer::active() || !uilayer::size(w, h)) {
        g.uiQuadImageValid = false;
        return false;
    }
    if (!g.uiCopyPs || !g.vs || !g.sampler || !g.ctx)
        return false;
    if (!ensure_ui_swapchain(w, h))
        return false;
    // Pick the read view the destination's encoding calls for. An _SRGB target
    // re-encodes on write, so it needs the decoding view for the two to cancel;
    // a plain UNORM one (only if the runtime offered no sRGB format at all)
    // needs the raw values.
    const DXGI_FORMAT dstFmt = (DXGI_FORMAT)g.uiSwapFormat;
    const bool dstIsSrgb = (dstFmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                            dstFmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    ID3D11ShaderResourceView* src = dstIsSrgb ? uilayer::srv_srgb() : uilayer::srv();
    if (!src) return false;

    // The angular extent of the RENDER, which is what the capture covers: the
    // UI is drawn across the whole canvas, the canvas is blitted into blitRect,
    // and blitRect is submitted as exactly these half-angles. So a quad
    // subtending 2*hh by 2*vh reproduces the UI's original placement pixel for
    // pixel, and every other size is that one scaled.
    float hh = 0.0f, vh = 0.0f;
    eye_frustum_half_angles(0, hh, vh);
    if (hh <= 1.0e-4f || vh <= 1.0e-4f)
        return false;

    // EVERY FRAME'S UI while the world markers are placed from between the eyes
    // (hooks::marker_camera_placing()): then nothing in the UI depends on which
    // eye a frame was rendered for, and either frame's is both eyes' UI.
    //
    // OTHERWISE ONE EYE'S UI, HALF THE RATE -- anything the game projected
    // itself is in the eye's frame where it was drawn, and two frames would hand
    // the plane two positions for it. This is where that eye is chosen, on the
    // finished frame rather than from inside its draws (see redirect_if_ui).
    // present_route_eye() is what the frame just rendered as, read after all of
    // it: no race with the parity flip, and no exposure to the ~1% of Presents
    // that carry no camera build.
    //
    // On the frames whose capture is discarded there is nothing to blit, and
    // the quad simply re-submits the image it last released: an XR swapchain
    // keeps presenting that until a new one is released, the same property the
    // stale eye's projection view relies on. So half-rate costs a blit every
    // other frame rather than a blank frame in between.
    //
    // The idle count bounds two different things at once. Against a cadence
    // where this site and the wanted parity never coincide -- 2x mode opens an
    // XR frame on only half the Presents, so it can see one parity forever --
    // it forces a refresh anyway: a rare wrong-eye frame beats a plane frozen
    // for the session. And when nothing is being captured at all, which is the
    // UI genuinely stopping (menu closed, HUD switched off), it retires the
    // plane instead of leaving the last image up.
    constexpr int kMaxUiQuadIdle = 4;
    const bool haveCapture = uilayer::drew_this_frame();
    const bool wantEye     = hooks::marker_camera_placing() ||
                             present_route_eye() == kUiCaptureEye;
    if (haveCapture && (wantEye || g.uiQuadIdle >= kMaxUiQuadIdle)) {
        if (!refresh_ui_quad_image(src, w, h)) return false;
        g.uiQuadImageValid = true;
        g.uiQuadIdle = 0;
    } else {
        if (!g.uiQuadImageValid) return false;
        ++g.uiQuadIdle;
        if (!haveCapture && g.uiQuadIdle > kMaxUiQuadIdle) {
            g.uiQuadImageValid = false;
            return false;
        }
    }

    quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = g.uiSwapchain;
    quad.subImage.imageRect = {{0, 0}, {(int32_t)w, (int32_t)h}};
    quad.subImage.imageArrayIndex = 0;

    // SIZE IS A REAL SIZE, in metres, and distance is a real distance -- so
    // pushing the plane away makes it smaller, the way a board on a wall does.
    // (It was briefly an angular fraction held constant across distance, which
    // made the distance slider look like it did nothing.)
    //
    // Height follows the capture's own angular aspect rather than its pixel
    // aspect: the texture is the square canvas, but only 2*hh by 2*vh of angle
    // is what the game drew into, so tan(vh)/tan(hh) is the shape that keeps
    // the UI unstretched.
    //
    // THE MAP IN 3D is the rendered view's own extent, scaled about its centre
    // by the map UI size, and neither slider applies. At 1 it overlays the
    // render as the UI was drawn into it. Smaller, the markers still point at
    // the map features under them, because their camera is built from this
    // quad as submitted (ui_plane_in_camera); anything outside the smaller
    // plane has no icon. Without that camera the window itself is the map UI
    // size (composited_fov_scale()) and the UI covers all of it. In 2D that
    // reasoning is gone -- the scene is on a plane of its own and the UI has to
    // match THAT, so both planes take the UI size and the markers line up on
    // the panel instead.
    const bool  mapLocked = hooks::in_map_view() && !flat_screen_active();
    const float dist   = g_uiPlaneDistM.load();
    const float shape  = std::tan(vh) / std::tan(hh);
    const float mapUi  = hooks::marker_camera_available() ? g_mapUiScale.load() : 1.0f;
    const float widthM = mapLocked ? (2.0f * dist * std::tan(hh) * mapUi)
                                   : g_uiPlaneSizeM.load();
    quad.size = {widthM, widthM * shape};

    place_ui_plane_quad(quad, displayTime, mapLocked);
    g_uiQuadWidthM.store(quad.size.width);
    g_uiQuadHeightM.store(quad.size.height);
    g_uiQuadPinned.store(quad.space == g.space);
    return true;
}

// A rotation larger than this between the retained eye's capture and now is
// not a real ~90Hz head turn (worst case a few degrees per Present) -- it's a
// discontinuous camera jump (teleport, vehicle swap, fast travel). Warping
// across one of those would produce a plausible-looking WRONG scene, worse
// than the stale-resubmit it falls back to instead.
constexpr float kMaxWarpAngleRad = 45.0f * 3.14159265f / 180.0f;

// How long DRIVE_CAMERA must be idle before DIBR shift hands over to AER. Matches
// cbuffer_hook's map/garage classifier so the two agree about what "the game
// is not driving a scene right now" means.
constexpr uint64_t kDibrIdleMs = 400;

// How many Presents an eye may go without being the REAL rendered eye before
// the partner is treated as starving, which is the one condition that makes
// showing both eyes the same image correct. AER parity alternates every frame,
// so in healthy operation this never exceeds 1; the margin is for the occasional
// Present with no camera build behind it.

// Retain `eye`'s just-blitted render (still sitting in g.staging) as its "last
// real render", for a FUTURE Present's warp when this eye is the stale one.
// The orientation/frustum stamped alongside it come from THIS eye's OWN
// g.lastView -- not the other eye's, or the warp's baseline would already be
// wrong before any rotation is applied.
//
// Must be called while g.staging still holds this eye's image (i.e. straight
// after the blit that copied it there).
// `src` overrides what gets retained; null means g.staging (the image just
// blitted). DIBR shift passes the PRE-HUD copy so that neither its disocclusion fill
// nor its rotation-warp fallback reintroduces a HUD into the synthesized eye.
void retain_eye_for_warp(uint32_t eye, ID3D11Texture2D* src = nullptr)
{
    // The retained copy exists solely to be warped by warp_stale_eye(),
    // which returns immediately when the warp is off -- so with the warp
    // disabled this was a full-resolution CopyResource every frame, into two
    // full-size textures held for nothing. Released below on the transition.
    if (!g_warpEnabled.load()) {
        if (g.prevEyeTex[0] || g.prevEyeTex[1]) {
            for (int e = 0; e < 2; ++e) {
                g.prevEyeSrv[e].Reset();
                g.prevEyeSrvRaw[e].Reset();
                g.prevEyeTex[e].Reset();
                g.prevEyeValid[e] = false;
                g.prevEyeFormat[e] = DXGI_FORMAT_UNKNOWN;
                g.prevEyeW[e] = g.prevEyeH[e] = 0;
            }
            VRLOG("warp: retained-eye textures released (warp disabled)");
        }
        return;
    }
    if (!ensure_prev_eye_tex((int)eye)) return;
    g.ctx->CopyResource(g.prevEyeTex[eye].Get(), src ? src : g.staging.Get());
    g.prevEyeBuild[eye] = g_onScreenValid ? g_onScreenRec.build : 0;

    // RETAIN THE DEPTH AND THE CAMERA TOO, for the 6-DoF reprojection. Only
    // while it is switched on: this is a full-resolution copy, and the rotation
    // warp does not need any of it.
    //
    // The depth comes from the ring slot tagged with THIS eye, so it is the
    // depth of the frame just retained rather than whatever was captured most
    // recently -- the same identity rule the main warp uses.
    g.prevEyeMatricesValid[eye] = false;
    if (warp_uses_6dof()) {
        hooks::SceneDepth sd{};
        float view[16], viewProj[16];
        if (hooks::scene_depth_for_eye((int)eye, sd) && sd.srv &&
            hooks::main_camera_matrices(view, viewProj)) {
            if (ensure_prev_eye_depth((int)eye)) {
                // Copy out of the ring: those slots are reused within a few
                // frames, and the retained frame may be consulted for longer.
                ComPtr<ID3D11Resource> depthRes;
                sd.srv->GetResource(&depthRes);
                if (depthRes) {
                    g.ctx->CopyResource(g.prevEyeDepth[eye].Get(), depthRes.Get());
                    // The mask for THIS frame, taken here rather than read live
                    // at reprojection time: swapchain_hook clears it later in
                    // this same Present, and by the time the other eye is
                    // synthesized it would describe the wrong eye's truck.
                    g.prevEyeRigidValid[eye] = false;
                    if (ID3D11Texture2D* liveMask = rigidmask::texture()) {
                        if (ensure_prev_eye_rigid((int)eye)) {
                            g.ctx->CopyResource(g.prevEyeRigid[eye].Get(), liveMask);
                            g.prevEyeRigidValid[eye] = true;
                        }
                    }
                    // The mirrors of THIS frame too, for the same reason.
                    g.prevEyeMirrorValid[eye] = false;
                    if (warp_uses_motion()) {
                        ID3D11Texture2D* liveMirror = mirrormask::texture();
                        if (liveMirror && ensure_prev_eye_mirror((int)eye, liveMirror)) {
                            g.ctx->CopyResource(g.prevEyeMirror[eye].Get(), liveMirror);
                            g.prevEyeMirrorValid[eye] = true;
                        }
                    }
                    std::memcpy(g.prevEyeViewProj[eye], viewProj, sizeof(viewProj));
                    std::memcpy(g.prevEyeView[eye], view, sizeof(view));
                    g.prevEyeProjA[eye] = sd.projA;
                    g.prevEyeProjB[eye] = sd.projB;
                    g.prevEyeProjValid[eye] = sd.projValid;
                    g.prevEyeMatricesValid[eye] = true;
                }
            }
        }
    }
    // The instant this image was RENDERED at, not the later locate that
    // happened to be current when it was retained -- see reference_view_pose().
    // The warp's source and target are now both render instants, so it maps
    // between two things that were really rendered. Tilted as the camera was,
    // so source and target are both what was drawn (rendered_orientation()).
    const Quat q = rendered_orientation(reference_view_pose(eye).orientation, (int)eye);
    g.prevEyeOrient[eye][0] = q.x; g.prevEyeOrient[eye][1] = q.y;
    g.prevEyeOrient[eye][2] = q.z; g.prevEyeOrient[eye][3] = q.w;
    if (!eye_warp_intrinsics((int)eye, g.prevEyeHalfAngleH[eye], g.prevEyeHalfAngleV[eye],
                             g.prevEyeCx[eye], g.prevEyeCy[eye]))
        eye_frustum_half_angles(eye, g.prevEyeHalfAngleH[eye], g.prevEyeHalfAngleV[eye]);
    g.prevEyeCamValid[eye] = rendered_cam_rot(g.prevEyeCamRot[eye]);
    g.prevEyeValid[eye] = true;
}

// THE CONSTANT PART of the src-eye -> dst-eye pixel mapping, for DIBR shift.
//
// Zero unless the two eyes were rendered at different frusta, which only
// off-centre projection does. When they were, the frusta are mirror images with
// the same tangent span, so they differ by a pure horizontal translation and
// the shift stays dest_x = src_x + this +/- disparity.
//
// A source pixel at tangent T sits at (T - tlSrc) * pxPerTan; the same world
// direction in the destination frustum sits at (T - tlDst) * pxPerTan. The
// difference is what this returns, and T drops out of it -- which is the whole
// reason a constant suffices.
float dibr_eye_pixel_offset(int srcEye, int dstEye, uint32_t widthPx)
{
    if (!offcenter_projection() || widthPx == 0) return 0.0f;
    float sl, sr, su, sdn, dl, dr, du, ddn;
    if (!render_eye_frustum_tan(srcEye, sl, sr, su, sdn)) return 0.0f;
    if (!render_eye_frustum_tan(dstEye, dl, dr, du, ddn)) return 0.0f;
    const float span = sr - sl;
    if (!(span > 1.0e-4f)) return 0.0f;
    // Spans that differ would mean the two eyes also disagree about SCALE, and
    // a constant could not express that. Not seen on any real headset, but a
    // silently wrong shift is worse than no shift.
    if (std::fabs((dr - dl) - span) > 0.01f * span) return 0.0f;
    return (sl - dl) * ((float)widthPx / span);
}

// `stale` wasn't rendered this Present -- rotate ITS OWN last real render to
// the current head pose and submit that, instead of leaving the compositor to
// re-show an unchanged image from the previous frame.
//
// Returns false without touching the swapchain (i.e. falling back to exactly
// that stale resubmit) when this eye has never rendered, the warp shader
// failed to build, or the rotation since capture is implausibly large.
//
// Shared by every caller that needs it, rather than inlined where it started;
// needs the identical operation, and this warp has been subtle enough to get
// wrong twice that two copies of it drifting apart is a real risk.

// Which orientation the stale-eye warp is built from. Both call sites share
// this so they cannot disagree -- the file's own history says this warp has
// been subtle enough to get wrong twice.
//
// TWO SOURCES, the second from kWarpGameRot up:
//
//   pose  -- the headset orientation at capture vs now. Always available, always
//            fresh (xrLocateViews runs every Present), but blind to the game
//            camera: stick look and vehicle turns pass through uncorrected.
//   camera -- the game's own view matrix rotation at capture vs now, which is
//            head AND game composed. Covers everything, but is only as fresh as
//            the last main-camera constant-buffer upload.
//
// The camera source falls back to the pose source when the snapshot has not
// moved since capture (byte-identical rotation = the game has not rebuilt its
// camera in the ~2 Presents since this eye was retained). Warping by an
// identity there would pin the stale eye to its capture orientation while the
// real eye tracks the head, i.e. exactly the judder the warp exists to remove.
//
// A 2026-08-17 experiment measured the two sources against each other with the
// head as the only thing moving: under 0.3 degrees apart over one frame. That
// is the evidence the axis convention here is right, and it is re-measured
// live into g_warpCamDisagreeDeg so a convention error cannot hide.
//
// COCKPIT ONLY. A rotation warp is only valid about the camera's OWN optical
// centre, and the orbit camera does not rotate about its own centre at all --
// it swings around the truck. MEASURED in the headset 2026-08-18: turning the
// orbit camera with this on gave double vision, while the cockpit (which really
// does rotate in place under stick look) improved. The reason is that an orbit
// leaves content AT the orbit radius stationary -- the truck, which fills the
// view and is what the eyes fuse on -- while a rotation-only warp displaces it
// by the full turn. Correcting the rotation without the translation it is
// coupled to is WORSE there than correcting nothing.
//
// Decided from the CAMERA MODE, not from measured camera translation. A
// translation test was tried first and rejected in testing: it only fires once
// enough sideways motion has accumulated, so every orbit turn began with the
// game warp applied and switched away mid-turn -- the wrong warp, visibly, for
// the first part of every swing. The mode marker is already exact and answers
// instantly, and orbit is excluded by what it IS rather than by catching it in
// the act.
//
// Same test viewbuild_hook uses to claim the cockpit at its own camera build,
// so the two cannot disagree about which camera is being warped.
bool cockpit_camera()
{
    return hooks::logic_mode() >= 0.75f && !hooks::in_map_view();
}

bool warp_homography_for(uint32_t stale, float hhNew, float vhNew,
                         Mat3& outH, float& outRelAngleRad)
{
    // Carry the stale eye to THIS frame's render instant, not to the current
    // locate. That is what the fresh eye already is, so both eyes end up in one
    // reference frame and the compositor moves them together.
    const Quat qn = rendered_orientation(reference_view_pose(stale).orientation,
                                         (int)stale);
    const float qNew[4] = {qn.x, qn.y, qn.z, qn.w};

    // THE TARGET INTRINSICS, taken here rather than from the caller. Both
    // callers pass eye_frustum_half_angles(), which describes the SYMMETRIC
    // enclosing frustum -- the right answer until off-centre projection made
    // "the frustum this eye is rendered at" a different thing. Overriding here
    // rather than changing two call sites keeps the one definition in one
    // place; in the symmetric case it reproduces exactly what was passed.
    float cx = 0.5f, cy = 0.5f;
    {
        float hh = 0.0f, vh = 0.0f, tcx = 0.5f, tcy = 0.5f;
        if (eye_warp_intrinsics((int)stale, hh, vh, tcx, tcy)) {
            hhNew = hh; vhNew = vh; cx = tcx; cy = tcy;
        }
    }

    float poseAngle = 0.0f;
    const Mat3 poseH = compute_eye_warp_homography(
        g.prevEyeOrient[stale], g.prevEyeHalfAngleH[stale], g.prevEyeHalfAngleV[stale],
        qNew, hhNew, vhNew, &poseAngle, cx, cy);

    if (warp_uses_game_rot() && g.prevEyeCamValid[stale]) {
        // Only while the cockpit is actually being DRIVEN. logic_mode() keeps
        // reading cockpit after a drive, in the garage and on the pause menu,
        // and since the main camera is recognised on those screens too
        // (cbuffer_hook.cpp, classify_cam) the rendered rotation there is the
        // garage's orbiting camera -- the case this path must not take.
        const bool cockpit = cockpit_camera() &&
                             hooks::logic_camera_idle_ms() <= kDibrIdleMs;
        float mNew[9] = {};
        const bool rebuilt = rendered_cam_rot(mNew) &&
            std::memcmp(mNew, g.prevEyeCamRot[stale], sizeof(mNew)) != 0;

        if (rebuilt && cockpit) {
            float R[9], camAngle = 0.0f;
            // flipZ: the game's camera looks down +Z (dibr_projection's reverse-Z
            // fit only closes that way) while the K matrices above are built for
            // OpenXR's -Z. The two frames differ by F = diag(1,1,-1) and a
            // rotation transforms as F*R*F.
            relative_cam_rotation(g.prevEyeCamRot[stale], mNew, /*flipZ=*/true,
                                  R, &camAngle);
            outH = warp_homography_from_R(R, g.prevEyeHalfAngleH[stale],
                                          g.prevEyeHalfAngleV[stale], hhNew, vhNew,
                                          g.prevEyeCx[stale], g.prevEyeCy[stale]);
            outRelAngleRad = camAngle;
            g_warpCamUsed.fetch_add(1);
            // Not the angle BETWEEN the two rotations (that needs a second
            // matrix build); the difference in how far each says the view
            // turned, which catches a flipped axis just as well and costs a
            // subtraction.
            const float d = std::fabs(camAngle - poseAngle) * 57.2957795f;
            if (d > g_warpCamDisagreeDeg.load()) g_warpCamDisagreeDeg.store(d);
            return true;
        }
        (rebuilt ? g_warpCamOrbit : g_warpCamStale).fetch_add(1);
    }

    outH = poseH;
    outRelAngleRad = poseAngle;
    return true;
}

// Where the 6-DoF reprojection's model of the world is actually true.
//
// It assumes the eye offset MOVES THE CAMERA -- it displaces the view matrix
// laterally and reprojects by the difference. That holds in gameplay, where the
// cockpit and orbit cameras really are offset in world space, and since
// 2026-10-27 it holds everywhere else too.
//
// The map, the garage and the menus used to be left out, because they hung their
// eye offset on a separate projection site with the opposite sign. So the
// reprojection displaced by the wrong magnitude in the wrong direction, and since
// that alternates with the eye it oscillated -- on near, high-frequency edges
// (the menu's bushes) and not on distant ground. Neither is true now: those
// cameras take their offset at the same build as gameplay (2026-08-12), with the
// same sign (kMapEyeSign). What IS different there is that the image at a Present
// is a build behind the latest, which apply_stale_6dof() and the depth capture
// both allow for by asking the build the image was drawn with.
//
// So a panning map and a turning garage go through the same warp as a drive. On
// a flat 2D screen the eye offset is zero, and it declines on the separation.
//
// THE ROTATION WARP IS NOT GATED and runs wherever this declines: it is built
// from the head pose and a homography, neither of which cares how the eye
// offset was applied. Only the depth-based reprojection is restricted.

// THE SCREENS WHERE NOTHING IS BEING DRIVEN and plain AER is simply correct.
// Gates the DIBR shift fallback.
//
// It asks for a screen to be POSITIVELY IDENTIFIED rather than inferred from a
// failure to identify anything, which is the distinction the whole thing turns
// on. Under the fingerprint gate, "not gameplay" includes the garage fallback,
// and the garage fallback is what you get when the gameplay markers merely
// stopped drawing for a moment -- so gating on it took DIBR shift and the 6-DoF
// warp off mid-drive, for seconds at a time:
//
//   DIRECT SCREEN -> GAMEPLAY (live markers: ... gameplay 2)
//   DIRECT SCREEN -> GARAGE   (live markers: ... gameplay 0)
//
// THE MAP is the only set left that may name a screen here. The menu's was
// removed 2026-08-26 -- one of its shaders also draws in a level, so the list
// went live mid-drive and claimed MENU while driving. The menu and the garage
// are both left to the idle timer,
// which handles them correctly without needing to know which one it is looking
// at. Under the legacy gate there is no marker set at all, and the pose
// classifier's !in_gameplay() is what it always was.
//
// THE BOOT MENU is separate from both and needs the first test: nothing has
// ever been driven, so the idle timer reads 0 -- "just ticked" -- and would
// call the main menu an active drive. That is what made it jitter.
bool on_static_screen()
{
    if (!hooks::logic_camera_ever_ticked()) return true;
    // The main-menu camera rendered this image: a menu screen, positively and
    // from the first frame of it (onscreen_camera_builder).
    if (g_onScreenBuilder.load() == kBuilderMenu) return true;
    if (hooks::screen_gate_direct()) {
        // THE MAP ALONE. The garage never qualified: it has no marker list,
        // and the engine's own DRIVE_CAMERA mode enum turned out to be
        // unreadable there (see camera_hook.h). A bare garage answer is the
        // fallback for "nothing claimed you", and acting on that is what took
        // DIBR shift off in the middle of a drive. The MENU was dropped for the
        // opposite reason -- its list claims too much, firing inside a level.
        //
        // Both still reach AER one step further down: the drive camera stops
        // ticking on either, so driveIdle fires on the idle timer instead.
        return hooks::direct_screen() == hooks::kScreenMap;
    }
    return !hooks::in_gameplay();
}

// NOTHING IS BEING DRIVEN: a static screen, or the drive camera has stopped, which
// is what answers for the garage and the pause menu.
bool nothing_driven()
{
    return on_static_screen() || hooks::logic_camera_idle_ms() > kDibrIdleMs;
}

// The facts the applied-eye record can be asked for directly. See
// dibrpolicy::sixdof_model_applies() for what each one rules out.
bool stale_6dof_model_valid()
{
    const AppliedEye ae = applied_eye();
    return dibrpolicy::sixdof_model_applies(
        warp_uses_6dof(),
        ae.valid,
        ae.fresh,
        ae.how == kEyeAppliedAsCameraMove,
        ae.mapSign);
}

// Defined below, next to the DIBR shift fill it was written for. Declared here
// because the AER path reaches it first. `baseH`: the homography the caller drew
// its rotation-only base with, for the mirror glass (reproject::Params::rotH).
bool apply_stale_6dof(uint32_t stale, ID3D11UnorderedAccessView* dstUav,
                      UINT dstW, UINT dstH, const Mat3* baseH = nullptr);

bool warp_stale_eye(uint32_t stale)
{
    if (!g_warpEnabled.load()) return false;
    if (!g.warpPs || !g.prevEyeValid[stale]) return false;

    float hhNew, vhNew, relAngleRad = 0.0f;
    eye_frustum_half_angles(stale, hhNew, vhNew);

    // (An attempt to warp by the RENDERED orientation rather than the head
    // pose was REVERTED 2026-08-09. The idea is sound -- the head delta is only
    // correct in mode 1 -- but the composite it needed,
    // projBasis * cameraBufferView, is two values published from different
    // places at different rates: the projection basis per camera build (~60 Hz)
    // and the camera-buffer snapshot on main-camera uploads, read together at
    // Present (~106/s). They were routinely from different builds, so the
    // composite jumped whenever one updated without the other -- a visible snap
    // every ~100 ms, in orbit as well, since this path is global. Doing this
    // properly needs E published as ONE coherent snapshot from a single site
    // that knows both, not composed at read time.)
    Mat3 H;
    warp_homography_for(stale, hhNew, vhNew, H, relAngleRad);
    if (relAngleRad >= kMaxWarpAngleRad) return false;

    // Upload the homography BEFORE anything samples it -- both the
    // intermediate pass below and the final draw read this same buffer.
    update_warp_cb(g.ctx.Get(), H);

    // 6-DoF, WHEN ENABLED, and this is where it matters most: in AER the stale
    // eye is not a patch inside a reprojected frame, it IS the whole eye, every
    // other frame. The same correction that is confined to disocclusion holes
    // under DIBR shift covers every pixel here.
    //
    // The XR swapchain image cannot be a compute destination (it is created
    // COLOR_ATTACHMENT only), so the rotation warp goes into an intermediate,
    // the reprojection composites over it, and the final draw copies that to
    // the swapchain with an IDENTITY homography -- the warping has already
    // happened by then.
    //
    // When the toggle is off, none of this runs and the path below is the same
    // single direct-to-swapchain draw it has always been.
    // prevEyeMatricesValid is checked HERE as well as inside
    // apply_stale_6dof(): without it the intermediate pass below would be
    // drawn and then thrown away on every frame that has no retained depth
    // for this eye, which is every frame until one has been captured.
    bool composited = false;
    if (stale_6dof_model_valid() && g.prevEyeMatricesValid[stale] &&
        ensure_stale_warp_tex((int)stale)) {
        ID3D11RenderTargetView* irtv = g.staleWarpRtv.Get();
        const float iclear[4] = {0, 0, 0, 1};
        D3D11_VIEWPORT ivp{0, 0, (float)g.staleWarpW, (float)g.staleWarpH, 0, 1};
        g.ctx->ClearRenderTargetView(irtv, iclear);
        g.ctx->OMSetRenderTargets(1, &irtv, nullptr);
        g.ctx->RSSetViewports(1, &ivp);
        g.ctx->IASetInputLayout(nullptr);
        g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.ctx->VSSetShader(g.vs.Get(), nullptr, 0);
        g.ctx->PSSetShader(g.warpPs.Get(), nullptr, 0);
        ID3D11ShaderResourceView* isrv = g.prevEyeSrv[stale].Get();   // sRGB
        ID3D11SamplerState* ismp = g.warpSampler.Get();
        ID3D11Buffer* icb = g.warpCb.Get();
        g.ctx->PSSetShaderResources(0, 1, &isrv);
        g.ctx->PSSetSamplers(0, 1, &ismp);
        g.ctx->PSSetConstantBuffers(0, 1, &icb);
        g.ctx->Draw(3, 0);
        ID3D11ShaderResourceView* inull[1] = {nullptr};
        g.ctx->PSSetShaderResources(0, 1, inull);
        ID3D11RenderTargetView* inullrtv[1] = {nullptr};
        g.ctx->OMSetRenderTargets(1, inullrtv, nullptr);

        composited = apply_stale_6dof(stale, g.staleWarpUav.Get(),
                                      g.staleWarpW, g.staleWarpH, &H);
        if (composited) {
            // The image is already in the destination view; the draw below must
            // not warp it a second time.
            const Mat3 ident{{1,0,0, 0,1,0, 0,0,1}};
            update_warp_cb(g.ctx.Get(), ident);
        }
    }

    XrRect2Di srect = g.blitRect[stale];  // reused AS-IS, not recomputed
    XrSwapchainImageAcquireInfo sai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo swi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    swi.timeout = XR_INFINITE_DURATION;
    uint32_t sidx = 0;
    if (XR_FAILED(xrAcquireSwapchainImage(g.swapchain[stale], &sai, &sidx)) ||
        XR_FAILED(xrWaitSwapchainImage(g.swapchain[stale], &swi)))
        return false;

    D3D11_VIEWPORT svp{(float)srect.offset.x, (float)srect.offset.y,
                       (float)srect.extent.width, (float)srect.extent.height, 0, 1};
    ID3D11RenderTargetView* srtv = g.rtvs[stale][sidx].Get();
    float clear[4] = {0, 0, 0, 1};
    g.ctx->ClearRenderTargetView(srtv, clear);
    g.ctx->OMSetRenderTargets(1, &srtv, nullptr);
    g.ctx->RSSetViewports(1, &svp);
    g.ctx->IASetInputLayout(nullptr);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->VSSetShader(g.vs.Get(), nullptr, 0);
    g.ctx->PSSetShader(g.warpPs.Get(), nullptr, 0);
    // The reprojected intermediate when it ran, otherwise the retained eye
    // itself. Both are sampled through an sRGB view and drawn into the sRGB
    // swapchain target, so the encoding round-trips either way.
    ID3D11ShaderResourceView* wsrv = composited ? g.staleWarpSrv.Get()
                                                : g.prevEyeSrv[stale].Get();
    ID3D11SamplerState* wsmp = g.warpSampler.Get();
    g.ctx->PSSetShaderResources(0, 1, &wsrv);
    g.ctx->PSSetSamplers(0, 1, &wsmp);
    ID3D11Buffer* wcb = g.warpCb.Get();
    g.ctx->PSSetConstantBuffers(0, 1, &wcb);
    g.ctx->Draw(3, 0);
    ID3D11ShaderResourceView* wnull = nullptr;
    g.ctx->PSSetShaderResources(0, 1, &wnull);

    if (!hooks::is_menu_open() && winch_cursor_active()) {
        cursoroverlay::draw_in_rect(
            g.device.Get(), g.ctx.Get(), srtv, g.outputWindow,
            g.width, g.height, svp.TopLeftX, svp.TopLeftY,
            svp.Width, svp.Height, true);
    }

    XrSwapchainImageReleaseInfo sri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(g.swapchain[stale], &sri);
    g.eyeFresh[stale] = true;
    g.eyeEverRendered[stale] = true;

    // Capture what was ACTUALLY drawn (post-warp), not the pre-warp source --
    // framedump exists here specifically to verify the warp itself looks right.
    ComPtr<ID3D11Resource> sres;
    srtv->GetResource(&sres);
    ComPtr<ID3D11Texture2D> stex;
    sres.As(&stex);
    framedump::capture(g.ctx.Get(), stex.Get(), stale);
    return true;
}

// The same rotation warp, rendered OFF-SCREEN instead of straight into the
// stale eye's swapchain image -- the fill source for DIBR shift's disocclusion holes.
//
// A DIBR hole is, by definition, a surface the rendered eye could not see but
// the synthesized eye can. The one image in the system that definitely DID see
// it is that eye's own last real render, one frame back: it is the right
// viewpoint, not an approximation of it. Rotation-warping it to the current
// pose first is what makes it usable -- the previous implementation sampled it
// unwarped, so the fill was pinned to a frame-old orientation while the rest of
// the image tracked the head, which is the flicker DIBR shift has always had.
//
// Returns nullptr when there is nothing usable (warp off, no retained frame,
// too large a pose jump), and DIBR shift then falls back to its stretch fill.
// Composites the 6-DoF camera reprojection of eye `stale`'s retained frame over
// whatever the rotation warp just drew into g.dibrFill.
//
// The two matrices it needs:
//
//   OLD -- the viewProj the retained frame was actually rendered with, kept
//   beside its depth by retain_eye_for_warp(). Inverted here to take a pixel
//   from that frame's clip space back to world space.
//
//   NEW -- the viewProj the STALE eye would have right now. That is not the
//   current camera: the current camera belongs to the eye the game just
//   rendered. So it is rebuilt as proj * view_stale, where proj is recovered
//   from the live pair (viewProj * inverse(view)) and view_stale is the live
//   view displaced laterally by the separation the warp used. The sign of that
//   displacement is taken from the warp's own eyeSign rather than re-derived,
//   so the two cannot end up disagreeing -- see dibrpolicy::synth_eye_view().
// Why the 6-DoF pass did or did not run on the last frame that tried, so
// "subtle" can be told apart from "never ran". Named rather than counted: the
// reasons are few and each is a different thing to go and fix.
const char* g_stale6dofWhy = "(off)";

// THE MOTION-VECTOR WARP (kWarpMotion), per stale eye it was asked for: moved
// by the game's motion, or which check took it off them. For the periodic line
// in apply_stale_6dof().
enum MotionWarpOutcome { kMotionUsed = 0, kMotionNoField, kMotionOtherFrame,
                         kMotionNotThisImage, kMotionSize, kMotionOutcomeCount };
uint32_t g_motionWarp[kMotionOutcomeCount] = {};
uint32_t g_motionMirrorMasked = 0;
uint32_t g_motionNoMask = 0;

// Composites the 6-DoF camera reprojection of eye `stale`'s retained frame into
// `dstUav`, over whatever is already there.
//
// The destination is a parameter because there are TWO stale-eye paths and both
// want this: DIBR shift's disocclusion fill (a few per cent of pixels) and AER's
// whole-eye stale warp (all of them, every other frame). The second is where the
// correction is far more visible, for the obvious reason.
bool apply_stale_6dof(uint32_t stale, ID3D11UnorderedAccessView* dstUav,
                      UINT dstW, UINT dstH, const Mat3* baseH)
{
    // Set on every path so the framedump line reports THIS frame, not a
    // leftover from whenever the state last changed.
    g_stale6dofWhy = "(off)";
    if (!warp_uses_6dof()) return false;
    // Named separately from the combined test below so the frame-dump line says
    // WHICH of the model's premises failed. These two are the level fly-in and
    // the map/garage projection site respectively -- see
    // dibrpolicy::sixdof_model_applies().
    {
        const AppliedEye ae = applied_eye();
        if (ae.valid && !ae.fresh) {
            g_stale6dofWhy = "applied-eye record is from an earlier frame";
            return false;
        }
        if (ae.valid && ae.how != kEyeAppliedAsCameraMove) {
            g_stale6dofWhy = "eye offset was a projection shift, not a camera move";
            return false;
        }
        if (ae.valid && ae.mapSign) {
            g_stale6dofWhy = "render used the map eye sign (opposite to gameplay)";
            return false;
        }
    }

    // See stale_6dof_model_valid(). Checked here as well as at the call sites,
    // so a future caller cannot reach the reprojection when its premise does
    // not hold.
    g_stale6dofWhy = "no eye record for this frame";
    if (!stale_6dof_model_valid()) return false;

    g_stale6dofWhy = "no destination UAV";
    if (!dstUav || !dstW || !dstH) return false;

    // The usual one. It means that eye has not been retained since the feature
    // was switched on, or that its retain could not find a matching depth
    // capture -- not that the reprojection itself failed.
    g_stale6dofWhy = "no retained depth/matrices for this eye";
    if (!g.prevEyeMatricesValid[stale] || !g.prevEyeDepthSrv[stale] ||
        !g.prevEyeSrvRaw[stale]) return false;

    // The depth and colour must describe the same pixels, or the reprojection
    // moves each by the other's geometry. Either on one grid, or -- when DLSS
    // upscaled the colour after it was rendered -- the depth on the smaller
    // render grid beneath it: the same picture at a lower resolution, so each
    // colour pixel takes the depth of the render pixel it came from.
    g_stale6dofWhy = "retained depth/colour size mismatch";
    const uint32_t depthW = g.prevEyeDepthW[stale], depthH = g.prevEyeDepthH[stale];
    const uint32_t colourW = g.prevEyeW[stale], colourH = g.prevEyeH[stale];
    const bool oneGrid = depthW == colourW && depthH == colourH;
    const bool renderGrid =
        upscaled_canvas() && depthW && depthH && depthW < colourW && depthH < colourH &&
        std::fabs((float)depthW * (float)colourH - (float)colourW * (float)depthH) <
            0.01f * (float)colourW * (float)depthH;
    if (!oneGrid && !renderGrid) {
        static bool logged = false;
        if (!logged) { logged = true;
            VRLOG("DIBR shift 6-DoF: retained depth %ux%u does not match colour %ux%u "
                  "-- reprojection skipped", depthW, depthH, colourW, colourH); }
        return false;
    }
    // The reprojection maps source pixels onto destination pixels one for one,
    // so a destination of a different size would silently rescale the image.
    g_stale6dofWhy = "destination size mismatch";
    if (dstW != g.prevEyeW[stale] || dstH != g.prevEyeH[stale]) return false;

    g_stale6dofWhy = "no applied-eye record";
    const xr::AppliedEye ae = xr::applied_eye();
    if (!ae.valid) return false;
    g_stale6dofWhy = "implausible separation";
    const float sep = dibrpolicy::full_separation_m(ae.offset);
    if (!(sep > dibrpolicy::kMinSeparationM) || !(sep < dibrpolicy::kMaxSeparationM))
        return false;

    g_stale6dofWhy = "no live camera matrices";
    float view[16], viewProj[16];
    if (!hooks::main_camera_matrices(view, viewProj)) return false;

    // proj = viewProj * inverse(view), then re-composed onto the stale eye's
    // own view. inverse(view) is exact: a view matrix is rigid.
    float invView[16], proj[16];
    dibrpolicy::rigid_inverse(view, invView);
    dibrpolicy::multiply_4x4(viewProj, invView, proj);

    // The stale eye is USUALLY the partner of the one just rendered, and used to
    // be assumed to be. It is not on any screen where no camera build runs: the
    // applied-eye record freezes there while the Present hook keeps advancing
    // the AER parity, so the two disagree every other frame. Displacing by a
    // full separation on those frames shifted the whole reprojection by an IPD,
    // alternating -- the main menu's jittering bush edges.
    //
    // Ask the rule instead of assuming. Direction still comes from the warp's
    // own eyeSign, so the two conventions cannot diverge.
    //
    // THE EYE ON SCREEN, FROM ITS BUILD. ae is the latest build, which in
    // gameplay is the image here. On the map, in the garage and on the menus the
    // image is the build before, so ae is already the next frame's -- the other
    // eye, which is the very eye being warped. Asked about that, the rule says
    // "same eye, no separation", and the stale eye is pulled onto the rendered
    // one: flat stereo on every warped frame.
    const int   shownEye = (g_onScreenValid && g_onScreenRec.eye >= 0)
                               ? (g_onScreenRec.eye & 1) : ae.eye;
    const float shownOffset = dibrpolicy::shown_eye_offset(shownEye, ae.eye, ae.offset);
    const float eyeSign = dibrpolicy::eye_sign(shownOffset, kDibrRightIsScreenRight);
    const float lateral = dibrpolicy::stale_eye_lateral(
        (int)stale, shownEye, sep);
    float staleView[16], newViewProj[16];
    dibrpolicy::synth_eye_view(view, eyeSign, lateral, staleView);
    dibrpolicy::multiply_4x4(proj, staleView, newViewProj);

    reproject::Params rp{};
    rp.depthWidth  = depthW;
    rp.depthHeight = depthH;
    g_stale6dofWhy = "retained viewProj singular";
    if (!dibrpolicy::invert_4x4(g.prevEyeViewProj[stale], rp.invOldViewProj)) {
        static bool logged = false;
        if (!logged) { logged = true;
            VRLOG("DIBR shift 6-DoF: retained viewProj is singular -- reprojection skipped"); }
        return false;
    }
    std::memcpy(rp.newViewProj, newViewProj, sizeof(newViewProj));

    // THE CAB. newViewProj moves every pixel by the camera's own travel, which
    // is right for the world and wrong for anything bolted to the camera --
    // and the cab is both bolted to it and at the near depth where that travel
    // displaces things most. rigid_view_at_old_position() keeps the rotation
    // and drops the translation, which is exact for camera-rigid geometry.
    float rigidView[16], rigidViewProj[16];
    dibrpolicy::rigid_view_at_old_position(staleView, g.prevEyeView[stale], rigidView);
    dibrpolicy::multiply_4x4(proj, rigidView, rigidViewProj);
    std::memcpy(rp.newViewProjRigid, rigidViewProj, sizeof(rigidViewProj));

    // THE GAME'S OWN MOTION (kWarpMotion), when this frame's render measured it
    // against exactly the image being moved: the render just before this one,
    // which under AER is the stale eye's retained frame. Then every pixel moves
    // by what the game says its surface did, and the rigid mask and orbit skip
    // below are not consulted. Anything else -- the game's TAA off, a skipped
    // frame, a field for another image -- runs the rigid path, which is the
    // 6-DoF level unchanged, if the frame has the mask that path needs.
    //
    // WHICH IT ONLY DOES WHILE SOMETHING IS BEING DRIVEN. The truck is rigid with
    // the camera because the camera is mounted on it. In the garage, on the map
    // and on the menus the truck is parked and the camera moves around it, so it
    // is static world like the ground it stands on, and takes the camera's travel
    // with everything else. Masked, it would keep the head correction alone and
    // slide against its own surroundings.
    const bool truckRigid = !nothing_driven();
    bool byMotion = false;
    if (warp_uses_motion()) {
        motionfield::Field mf;
        int outcome = kMotionUsed;
        if (!motionfield::latest(mf))                       outcome = kMotionNoField;
        else if (!g.prevEyeBuild[stale] || mf.prevBuild != g.prevEyeBuild[stale])
                                                            outcome = kMotionOtherFrame;
        else if (!g_onScreenValid || mf.curBuild != g_onScreenRec.build)
                                                            outcome = kMotionNotThisImage;
        else if (mf.w != depthW || mf.h != depthH)          outcome = kMotionSize;
        if (outcome == kMotionUsed) {
            rp.motionSrv    = mf.onPrev;
            rp.motionOwnSrv = mf.prevOwn;
            byMotion = true;
            // The debug overlay, painted by the resolve (render/motion_overlay.hpp).
            if (motionoverlay::enabled()) {
                rp.debugMotion = true;
                head_move_between(mf.prevBuild, mf.curBuild, rp.headMove);
            }
            // Mirror glass moves with its housing and is filled from the
            // rotation-only base: see Params::mirrorMaskSrv.
            if (g.prevEyeMirrorValid[stale] && g.prevEyeMirrorSrv[stale] &&
                g.prevEyeMirrorW[stale] == depthW && g.prevEyeMirrorH[stale] == depthH) {
                rp.mirrorMaskSrv = g.prevEyeMirrorSrv[stale].Get();
                ++g_motionMirrorMasked;
                if (baseH) {
                    std::memcpy(rp.rotH, baseH->m, sizeof(rp.rotH));
                    rp.haveRotH = true;
                }
            }
        }
        ++g_motionWarp[outcome];
        static ULONGLONG t0 = 0;
        const ULONGLONG now = GetTickCount64();
        if (!t0) t0 = now;
        if (now - t0 >= 10000) {
            t0 = now;
            VRLOG("motion warp: %u stale eyes moved by the game's motion vectors (%u with "
                  "the mirrors left to rotation, %u mirror draws marked); no vectors: no "
                  "field this frame %u, field for another image %u, not this image's %u, "
                  "size %u; of those, %u had no truck mask either and took the rotation warp",
                  g_motionWarp[kMotionUsed], g_motionMirrorMasked,
                  mirrormask::take_mark_count(), g_motionWarp[kMotionNoField],
                  g_motionWarp[kMotionOtherFrame], g_motionWarp[kMotionNotThisImage],
                  g_motionWarp[kMotionSize], g_motionNoMask);
            for (uint32_t& c : g_motionWarp) c = 0;
            g_motionMirrorMasked = 0;
            g_motionNoMask = 0;
        }
        // A FRAME THAT LOST ITS FIELD WHILE THEY WERE ARRIVING has no truck mask
        // either: nothing drew one (dibrpolicy::rigid_mask_marks()). Running the
        // rigid path without it would move the cab by the camera's own travel,
        // which is what the mask exists to stop, so this one goes to the caller's
        // rotation warp instead -- the road is a frame behind in one eye, rather
        // than the cab jumping. These are single frames around a hitch.
        if (!byMotion && truckRigid && !g.prevEyeRigidValid[stale]) {
            ++g_motionNoMask;
            g_stale6dofWhy = "no motion vectors this frame, and no truck mask for it";
            return false;
        }
    }

    // THE MASK IS THE WHOLE CLASSIFIER, and it is NOT gated on view mode.
    //
    // A depth band used to stand in for it, restricted to cockpit view because
    // that was the only place its premise held -- inside a cab nothing in the
    // WORLD is within a metre or two, so the near field is the truck by
    // construction. It was removed 2026-08-20: it could not tell the hood from
    // ground at the same distance, so it did nothing in the exterior cameras,
    // and where it did fire it was a proxy for a question the renderer can
    // answer exactly. These draws ARE the truck, at whatever depth.
    //
    // The size check is the one precondition: the mask has to describe the same
    // pixels as the retained depth, or it would be classifying the wrong ones.
    if (!byMotion && truckRigid && g.prevEyeRigidValid[stale] && g.prevEyeRigidSrv[stale] &&
        g.prevEyeRigidW[stale] == g.prevEyeDepthW[stale] &&
        g.prevEyeRigidH[stale] == g.prevEyeDepthH[stale])
        rp.rigidMaskSrv = g.prevEyeRigidSrv[stale].Get();

    // ORBIT LEAVES THEM ALONE. In the cockpit the rigid transform is exact --
    // the camera turns about its own centre, so keeping the rotation and
    // dropping the travel is what a cab bolted to it actually does. The orbit
    // camera swings AROUND the truck instead, so the truck is stationary in
    // view through the whole swing and any rotation applied to it is pure
    // displacement. Skipping leaves those pixels as the pose-only homography
    // warped them, which is the head correction and nothing else.
    //
    // Decided from the CAMERA MODE for the same reason warp_homography_for()
    // is: it answers instantly and exactly, where a measured-translation test
    // only fires once enough motion has accumulated and so gets the start of
    // every swing wrong.
    rp.rigidSkip = !byMotion && !cockpit_camera();

    // THE TERM THE RIGID BRANCH EXISTS TO REMOVE, measured rather than assumed.
    // If this reads ~0 while the truck is moving, the engine's view matrices
    // are camera-relative -- the camera's travel never entered the transform in
    // the first place, the cab artefact is coming from somewhere else, and this
    // whole branch is a no-op aimed at the wrong term. Cheaper to read it off a
    // log line than to rebuild to find out.
    {
        constexpr DWORD kPeriodMs = 10000;
        static DWORD last = 0;
        const DWORD now = GetTickCount();
        if (now - last >= kPeriodMs) {
            last = now;
            VRLOG("DIBR shift 6-DoF: camera travel %.4f m/frame | rigid mask %s "
                  "(%ux%u vs depth %ux%u) | logicMode %.2f",
                  dibrpolicy::camera_translation_delta(staleView,
                                                      g.prevEyeView[stale]),
                  rp.rigidMaskSrv ? "ON" : "off",
                  g.prevEyeRigidW[stale], g.prevEyeRigidH[stale],
                  g.prevEyeDepthW[stale], g.prevEyeDepthH[stale],
                  hooks::logic_mode());
        }
    }

    g_stale6dofWhy = "composite failed";
    if (!reproject::composite(g.ctx.Get(), g.prevEyeSrvRaw[stale].Get(),
                              g.prevEyeDepthSrv[stale].Get(), dstUav,
                              dstW, dstH, rp))
        return false;
    g_stale6dofWhy = "RAN";

    // One line the first time it actually composites, because "is this feature
    // even live" is otherwise unanswerable from a log -- the pass is meant to
    // be subtle, so its absence and its success look the same in the headset.
    static bool loggedFirst = false;
    if (!loggedFirst) {
        loggedFirst = true;
        VRLOG("DIBR shift 6-DoF: first reprojection composited (%ux%u, eye %u, "
              "separation %.4f m).", dstW, dstH, stale, sep);
    }
    return true;
}

ID3D11ShaderResourceView* stale_eye_fill(uint32_t stale)
{
    if (!g_warpEnabled.load() || !g.warpPs || !g.prevEyeValid[stale]) return nullptr;
    if (!g.prevEyeSrvRaw[stale]) return nullptr;

    // Sized/formatted like the retained eye, rebuilt only when that changes.
    if (!g.dibrFill || g.dibrFillFormat != g.prevEyeFormat[stale] ||
        g.dibrFillW != g.prevEyeW[stale] || g.dibrFillH != g.prevEyeH[stale]) {
        g.dibrFillSrv.Reset(); g.dibrFillRtv.Reset(); g.dibrFill.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = g.prevEyeW[stale]; td.Height = g.prevEyeH[stale];
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = g.prevEyeFormat[stale];
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        // UAV as well as RTV: the rotation warp draws into it, then the 6-DoF
        // reprojection composites over the top through the UAV.
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE |
                       D3D11_BIND_UNORDERED_ACCESS;
        if (!td.Width || !td.Height) return nullptr;
        g.dibrFillUav.Reset();
        if (FAILED(g.device->CreateTexture2D(&td, nullptr, &g.dibrFill)) ||
            FAILED(g.device->CreateRenderTargetView(g.dibrFill.Get(), nullptr, &g.dibrFillRtv)) ||
            FAILED(g.device->CreateShaderResourceView(g.dibrFill.Get(), nullptr, &g.dibrFillSrv))) {
            VRLOG("DIBR shift: stale-eye fill target create FAILED");
            g.dibrFillSrv.Reset(); g.dibrFillRtv.Reset(); g.dibrFill.Reset();
            return nullptr;
        }
        // Non-fatal: without it the fill is simply rotation-only, which is what
        // it has always been.
        if (FAILED(g.device->CreateUnorderedAccessView(g.dibrFill.Get(), nullptr,
                                                       &g.dibrFillUav))) {
            VRLOG("DIBR shift: stale-eye fill UAV create FAILED -- 6-DoF reprojection "
                  "unavailable, rotation warp only (fmt=%d)", (int)td.Format);
            g.dibrFillUav.Reset();
        }
        g.dibrFillFormat = td.Format; g.dibrFillW = td.Width; g.dibrFillH = td.Height;
    }

    float hhNew, vhNew, relAngleRad = 0.0f;
    eye_frustum_half_angles(stale, hhNew, vhNew);
    Mat3 H;
    warp_homography_for(stale, hhNew, vhNew, H, relAngleRad);
    if (relAngleRad >= kMaxWarpAngleRad) return nullptr;

    update_warp_cb(g.ctx.Get(), H);

    D3D11_VIEWPORT vp{0, 0, (float)g.dibrFillW, (float)g.dibrFillH, 0, 1};
    ID3D11RenderTargetView* rtv = g.dibrFillRtv.Get();
    const float clear[4] = {0, 0, 0, 1};
    g.ctx->ClearRenderTargetView(rtv, clear);
    g.ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g.ctx->RSSetViewports(1, &vp);
    g.ctx->IASetInputLayout(nullptr);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->VSSetShader(g.vs.Get(), nullptr, 0);
    g.ctx->PSSetShader(g.warpPs.Get(), nullptr, 0);
    // The RAW view, paired with the raw destination format above: DIBR shift moves
    // pixels without colour conversion, so the fill has to arrive in the same
    // encoding as g.srcCopy (a straight CopyResource of the backbuffer).
    // Sampling the sRGB view here instead decoded to linear with no re-encode
    // on write, which is what made every fill read as near-black.
    ID3D11ShaderResourceView* wsrv = g.prevEyeSrvRaw[stale].Get();
    ID3D11SamplerState* wsmp = g.warpSampler.Get();
    g.ctx->PSSetShaderResources(0, 1, &wsrv);
    g.ctx->PSSetSamplers(0, 1, &wsmp);
    ID3D11Buffer* wcb = g.warpCb.Get();
    g.ctx->PSSetConstantBuffers(0, 1, &wcb);
    g.ctx->Draw(3, 0);

    ID3D11ShaderResourceView* nsrv[1] = {nullptr};
    g.ctx->PSSetShaderResources(0, 1, nsrv);
    ID3D11RenderTargetView* nrtv[1] = {nullptr};
    g.ctx->OMSetRenderTargets(1, nrtv, nullptr);

    // 6-DoF pass, OVER the rotation-warped result.
    //
    // The homography above corrected where the head LOOKED. This corrects where
    // the camera MOVED, which needs the retained frame's own depth because a
    // translation displaces near pixels further than far ones. It writes only
    // the destination pixels it can reach, so where it has no source the
    // rotation-warped pixel stands and the result is never worse than before.
    apply_stale_6dof(stale, g.dibrFillUav.Get(), g.dibrFillW, g.dibrFillH, &H);
    return g.dibrFillSrv.Get();
}

// --- xrEndFrame, and the worker it runs on under Wine ------------------------
//
// SteamVR under Proton holds xrEndFrame until the GPU has finished the frame
// (see running_under_wine()). On Windows the call returns at once and the GPU
// works through the frame while the game builds the next one; there, the game
// thread sat in it for the whole GPU frame and then did its own CPU work with
// the GPU idle. At ~5 ms of game CPU that is past a 90 Hz frame: 60 fps,
// purple bars in the SteamVR graph, judder on head turns. Light scenes (~2 ms)
// squeezed under the deadline, which is why it came and went.
//
// So under Wine the call is handed to a worker thread. Everything that touches
// an XR swapchain -- every acquire, blit and release -- has already happened on
// the game thread by then, and the game thread waits for the worker before it
// touches the session again: end_frame_wait() runs at the top of
// mirror_on_present(), ahead of poll_events() and xrWaitFrame. End(N) still
// precedes everything of frame N+1, as the spec requires. Measured by the
// reporter: game thread blocked 0.01-2.9 ms instead of ~9, 86-90 fps instead
// of ~60, no purple.
//
// WHAT DOES OVERLAP is xrEndFrame and the game's own D3D11 work, and that is
// the part to be careful with. Current Proton (10, Experimental) only takes
// DXVK's submission-queue lock inside xrEndFrame, which is thread-safe by
// design. Proton 8 and 9 ALSO call FlushRenderingCommands there, which flushes
// the game's IMMEDIATE context -- from the worker, while the game thread keeps
// recording into it. That is a data race inside DXVK unless the context is
// lock-protected, so under Wine it is made so (protect_immediate_context()).
//
// Layers are deep-copied into the job: the originals are render_frame()'s
// locals. None of them carries a `next` chain, which is what makes a struct
// copy a complete one -- add a chained extension struct to a layer and this
// copy has to learn about it.
struct AsyncEndJob {
    XrCompositionLayerProjection     proj{};
    XrCompositionLayerProjectionView views[2]{};
    XrCompositionLayerQuad           quads[3]{};
    const XrCompositionLayerBaseHeader* layers[4]{};
    XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
    XrSession session = XR_NULL_HANDLE;
    XrResult  result = XR_SUCCESS;
    uint32_t  quadCount = 0;
    LARGE_INTEGER dispatched{}, finished{};   // QPC, for the timing line
};
AsyncEndJob g_endJob;
HANDLE g_endGo = nullptr, g_endDone = nullptr, g_endThread = nullptr;
bool   g_endPending = false;     // game thread only
bool   g_endAsyncFailed = false;

// A rejected frame is a dropped frame, and it used to be silent: the -25s were
// found with an instrumented build. The first few in full, then a running
// count, so a runtime that rejects every frame cannot flood the log.
void note_end_result(XrResult res, uint32_t layerCount, uint32_t quadCount)
{
    if (XR_SUCCEEDED(res)) return;
    static uint32_t rejected = 0;
    ++rejected;
    if (rejected <= 5 || rejected % 600 == 0)
        VRLOG("xrEndFrame FAILED %d (%u layer(s), %u quad(s)) -- %u frame(s) rejected "
              "so far, each one a dropped frame", (int)res, layerCount, quadCount, rejected);
}

// DXVK implements ID3D10Multithread on the immediate context as a spinlock
// taken by every entry point -- FlushRenderingCommands included -- so turning
// it on makes the worker's flush and the game thread's recording take turns.
// One uncontended lock per immediate-context call; Wine only.
void protect_immediate_context()
{
    ComPtr<ID3D10Multithread> mt;
    if (g.ctx && SUCCEEDED(g.ctx.As(&mt)) && mt) {
        mt->SetMultithreadProtected(TRUE);
        VRLOG("xrEndFrame: immediate context lock-protected (Proton 8/9 flush it "
              "from inside xrEndFrame)");
    } else {
        VRLOG("xrEndFrame: ID3D10Multithread unavailable -- on Proton 8/9 the "
              "worker's flush of the immediate context is unguarded");
    }
}

DWORD WINAPI end_frame_worker(LPVOID)
{
    for (;;) {
        WaitForSingleObject(g_endGo, INFINITE);
        g_endJob.result = xrEndFrame(g_endJob.session, &g_endJob.fei);
        QueryPerformanceCounter(&g_endJob.finished);
        SetEvent(g_endDone);
    }
}

// Blocks until the previously dispatched xrEndFrame has returned; a no-op when
// nothing is in flight, which on Windows is always. Also the one place the
// worker's results are looked at.
void end_frame_wait()
{
    if (!g_endPending) return;
    LARGE_INTEGER w0, w1;
    QueryPerformanceCounter(&w0);
    WaitForSingleObject(g_endDone, INFINITE);
    QueryPerformanceCounter(&w1);
    g_endPending = false;
    note_end_result(g_endJob.result, g_endJob.fei.layerCount, g_endJob.quadCount);

    // Every 10 s: how long xrEndFrame takes on the worker, and how long the
    // game thread still ends up waiting for it -- the two numbers that say
    // whether this is doing its job on a given machine.
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    static double sumEnd = 0.0, maxEnd = 0.0, sumWait = 0.0;
    static uint32_t frames = 0;
    static ULONGLONG last = GetTickCount64();
    const double endMs  = double(g_endJob.finished.QuadPart - g_endJob.dispatched.QuadPart) * 1000.0 / double(freq.QuadPart);
    const double waitMs = double(w1.QuadPart - w0.QuadPart) * 1000.0 / double(freq.QuadPart);
    sumEnd += endMs; sumWait += waitMs; maxEnd = (std::max)(maxEnd, endMs); ++frames;
    const ULONGLONG now = GetTickCount64();
    if (now - last >= 10000) {
        VRLOG("xrEndFrame on the worker: %.2f ms avg, %.2f max | game thread waited "
              "%.2f ms avg for it (%u frames)", sumEnd / frames, maxEnd, sumWait / frames, frames);
        sumEnd = maxEnd = sumWait = 0.0; frames = 0; last = now;
    }
}

XrResult end_frame_inline(XrSession session, const XrFrameEndInfo& in, uint32_t quadCount)
{
    const XrResult r = xrEndFrame(session, &in);
    note_end_result(r, in.layerCount, quadCount);
    return r;
}

// xrEndFrame for render_frame(). Synchronous on Windows, exactly as it always
// was; on the worker under Wine. There the result is not known yet, so this
// returns XR_SUCCESS and end_frame_wait() reports the real one a frame later.
XrResult end_frame(XrSession session, const XrFrameEndInfo& in, uint32_t quadCount)
{
    if (!running_under_wine() || g_endAsyncFailed || in.layerCount > 4)
        return end_frame_inline(session, in, quadCount);

    if (!g_endThread) {
        protect_immediate_context();   // before the first flush can race
        g_endGo   = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_endDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (g_endGo && g_endDone)
            g_endThread = CreateThread(nullptr, 0, end_frame_worker, nullptr, 0, nullptr);
        if (!g_endThread) {
            VRLOG("xrEndFrame: worker thread would not start -- staying synchronous");
            g_endAsyncFailed = true;
            return end_frame_inline(session, in, quadCount);
        }
        SetThreadPriority(g_endThread, THREAD_PRIORITY_ABOVE_NORMAL);
    }
    end_frame_wait();   // never two in flight (normally finished long ago)

    AsyncEndJob& j = g_endJob;
    j.session = session;
    j.fei = in;
    j.quadCount = quadCount;
    uint32_t nq = 0;
    bool haveProj = false;
    for (uint32_t i = 0; i < in.layerCount; ++i) {
        const XrCompositionLayerBaseHeader* l = in.layers[i];
        if (l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION && !haveProj &&
            reinterpret_cast<const XrCompositionLayerProjection*>(l)->viewCount <= 2) {
            j.proj = *reinterpret_cast<const XrCompositionLayerProjection*>(l);
            for (uint32_t v = 0; v < j.proj.viewCount; ++v) j.views[v] = j.proj.views[v];
            j.proj.views = j.views;
            j.layers[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&j.proj);
            haveProj = true;
        } else if (l->type == XR_TYPE_COMPOSITION_LAYER_QUAD && nq < 3) {
            j.quads[nq] = *reinterpret_cast<const XrCompositionLayerQuad*>(l);
            j.layers[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&j.quads[nq]);
            ++nq;
        } else {
            // Something this copy cannot hold: submit it here, rather than hand
            // the worker a pointer into a stack frame that is about to go away.
            return end_frame_inline(session, in, quadCount);
        }
    }
    j.fei.layers = in.layerCount ? j.layers : nullptr;
    QueryPerformanceCounter(&j.dispatched);
    g_endPending = true;
    SetEvent(g_endGo);
    return XR_SUCCESS;
}

// REMOVED 2026-08-24: the "Double AER (2x)" render mode and its render_frame().
//
// It was never double-rate. It ran a complete xrWaitFrame -> xrBeginFrame ->
// xrEndFrame cycle on every game Present, exactly as the path below does, and
// xrWaitFrame blocks until the runtime's next predicted display time -- so the
// game's render rate WAS the headset rate either way. Measured: ~86 presents/s
// in both, identical. It also failed the requirement in the obvious way when
// frames dropped, since pairing up frames you have already waited for turns 80
// fps into 40 submitted pairs when what is wanted is 80 fresh eyes.
//
// What it was reaching for -- decoupling RENDER rate from SUBMIT rate -- was
// then built into the single path, and has since been removed from there too
// for collapsing back to render == submit; see the AER note near the top of
// this file. A second render mode was never what that needed either way.

// AER: one XR frame per Present, and the only render path there is.
// The current eye (determined by present_route_eye) gets a fresh blit; the other
// eye is a stale resubmit -- the compositor reuses whatever was last released
// into that swapchain without any acquire/release on our side. Head pose and eye
// views are sampled once per Present (every frame, not once per pair).
//
// DIBR shift and the stale-eye warp are OPTIONS INSIDE this path, not
// alternatives to it: the warp decides what the un-rendered eye is built from,
// and DIBR shift decides whether the rendered frame is depth-reprojected into
// the other eye before that warp fills what the reprojection could not reach.
bool is_srgb_format(DXGI_FORMAT f)
{
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return true;
        default:                              return false;
    }
}

// THE MOTION-VECTOR OVERLAY on the eye just rendered (render/motion_overlay.hpp):
// its own motion, the motion the other eye's warp is taking as the next render's
// this frame. Drawn over the finished image in the swapchain, after g.staging --
// which is what gets retained -- was read, so the warp never moves a painted
// image along with the real one.
void paint_motion_overlay(uint32_t eye, ID3D11RenderTargetView* rtv,
                          float x, float y, float w, float h)
{
    motionfield::Own own;
    if (!motionfield::latest_own(own)) return;
    hooks::SceneDepth sd{};
    if (!hooks::scene_depth_for_eye((int)eye, sd) || !sd.srv) return;
    float view[16], viewProj[16];
    if (!hooks::main_camera_matrices(view, viewProj)) return;
    motionoverlay::Params p;
    if (!dibrpolicy::invert_4x4(viewProj, p.invViewProj)) return;
    std::memcpy(p.viewProj, viewProj, sizeof(viewProj));
    p.motionSrv = own.srv;
    p.depthSrv  = sd.srv;
    if (own.chained) head_move_between(own.prevBuild, own.curBuild, p.headMove);
    p.colourW = g.stagingW;
    p.colourH = g.stagingH;
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rtv->GetDesc(&rd);
    p.srgbTarget = is_srgb_format(rd.Format);
    motionoverlay::draw(g.ctx.Get(), rtv, x, y, w, h, p);
}

// THE PER-EYE COMPOSITE: `src` letterboxed into the eye's image, with the
// windscreen splatter on it (shifted by `dispK` for a synthesized eye -- see
// kBlitHlsl), then the winch markers and, aiming at a winch point, the cursor.
// Into that eye's swapchain image, acquired and released here; or, given
// `target`, into that render target instead with nothing XR touched -- which
// is how Synched Sequential holds an eye until its pair is complete.
// `rectOut` receives the letterboxed sub-rect either way.
//
// `projValid/A/B` is the projection the splatter's shift is built from, and only
// matters when `dispK` is not 0. `motionOverlay`: this is the eye rendered this
// frame, which takes the debug overlay when it is on (paint_motion_overlay).
bool blit_eye_image(uint32_t dstEye, ID3D11Texture2D* src, float dispK,
                    bool projValid, float projA, float projB,
                    ID3D11RenderTargetView* target = nullptr,
                    XrRect2Di* rectOut = nullptr, bool motionOverlay = false)
{
    if (!src || !ensure_staging(src)) return false;
    g.ctx->CopyResource(g.staging.Get(), src);

    D3D11_TEXTURE2D_DESC sdesc{};
    src->GetDesc(&sdesc);
    float srcAspect = sdesc.Height > 0
        ? (float)sdesc.Width / (float)sdesc.Height : 1.0f;
    float dstAspect = g.height > 0 ? (float)g.width / (float)g.height : 1.0f;
    float vw = (float)g.width, vh = (float)g.height;
    if (srcAspect > dstAspect) vh = vw / srcAspect;
    else                       vw = vh * srcAspect;
    float vx = 0.5f * ((float)g.width - vw), vy = 0.5f * ((float)g.height - vh);

    uint32_t idx = 0;
    if (!target) {
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        if (XR_FAILED(xrAcquireSwapchainImage(g.swapchain[dstEye], &ai, &idx)) ||
            XR_FAILED(xrWaitSwapchainImage(g.swapchain[dstEye], &wi)))
            return false;
    }

    D3D11_VIEWPORT vp{vx, vy, vw, vh, 0, 1};
    ID3D11RenderTargetView* rtv = target ? target : g.rtvs[dstEye][idx].Get();
    float clear[4] = {0, 0, 0, 1};
    g.ctx->ClearRenderTargetView(rtv, clear);
    g.ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g.ctx->RSSetViewports(1, &vp);
    g.ctx->IASetInputLayout(nullptr);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->VSSetShader(g.vs.Get(), nullptr, 0);
    g.ctx->PSSetShader(g.ps.Get(), nullptr, 0);
    ID3D11ShaderResourceView* smudgeSrv = smudgelayer::srv();
    ID3D11ShaderResourceView* glassSrv  = smudgelayer::depth_srv();
    ID3D11ShaderResourceView* winchSrv  = winchlayer::srv();
    const bool smudgeOn = (smudgeSrv != nullptr) && g.blitCb;
    const bool winchOn  = (winchSrv != nullptr) && g.blitCb;
    if (g.blitCb) {
        D3D11_MAPPED_SUBRESOURCE mc{};
        if (SUCCEEDED(g.ctx->Map(g.blitCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mc))) {
            float* p = static_cast<float*>(mc.pData);
            p[0] = dispK;
            p[1] = projValid ? projA : 0.0f;
            p[2] = (g.width > 0) ? 1.0f / (float)g.width : 0.0f;
            // Glass thickness as t/projB, the form smudge_shift()
            // wants. projB is the only piece of the projection the
            // folded disparity constant threw away, so it has to be
            // divided back in here rather than in the shader.
            p[3] = (projValid && projB != 0.0f)
                       ? smudgelayer::depth_offset() / projB
                       : 0.0f;
            p[4] = smudgeOn ? 1.0f : 0.0f;
            p[5] = glassSrv ? 1.0f : 0.0f;
            p[6] = winchOn ? 1.0f : 0.0f;
            p[7] = 0.0f;
            g.ctx->Unmap(g.blitCb.Get(), 0);
        }
        ID3D11Buffer* cb[1] = { g.blitCb.Get() };
        g.ctx->PSSetConstantBuffers(0, 1, cb);
    }
    ID3D11ShaderResourceView* srvs[4] = { g.stagingSrv.Get(), smudgeSrv,
                                          glassSrv, winchSrv };
    ID3D11SamplerState* smp = g.sampler.Get();
    g.ctx->PSSetShaderResources(0, 4, srvs);
    g.ctx->PSSetSamplers(0, 1, &smp);
    g.ctx->Draw(3, 0);
    // Release the layer bindings: both are render targets again on
    // the very next frame, and D3D silently unbinds (and warns) if
    // they are still bound as SRVs when that happens.
    ID3D11ShaderResourceView* nullSrvs[4] = {};
    g.ctx->PSSetShaderResources(0, 4, nullSrvs);

    if (motionOverlay && motionoverlay::enabled())
        paint_motion_overlay(dstEye, rtv, vx, vy, vw, vh);

    if (!hooks::is_menu_open() && winch_cursor_active()) {
        cursoroverlay::draw_in_rect(
            g.device.Get(), g.ctx.Get(), rtv, g.outputWindow,
            g.width, g.height, vx, vy, vw, vh, true);
    }

    // Dump what the HEADSET receives, not the source handed in.
    //
    // This used to capture `src`, i.e. the image BEFORE this shader
    // ran -- so anything the blit itself composites was invisible in
    // a frame dump. The windscreen splatter is composited here, which
    // made it impossible to photograph the very artefact a dump was
    // taken to show. Reading the swapchain texture back off its own
    // RTV needs no extra bookkeeping, and catches every later stage
    // by construction rather than one at a time.
    //
    // Taken before xrReleaseSwapchainImage, while the image is still
    // ours to read.
    {
        ComPtr<ID3D11Resource> res;
        rtv->GetResource(&res);
        ComPtr<ID3D11Texture2D> shown;
        if (res && SUCCEEDED(res.As(&shown)))
            framedump::capture(g.ctx.Get(), shown.Get(), dstEye);
    }

    const XrRect2Di rect{{(int32_t)vx, (int32_t)vy}, {(int32_t)vw, (int32_t)vh}};
    if (rectOut) *rectOut = rect;
    if (target) return true;
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(g.swapchain[dstEye], &ri);
    g.eyeFresh[dstEye] = true;
    g.eyeEverRendered[dstEye] = true;
    g.blitRect[dstEye] = rect;
    return true;
}

// SYNCHED SEQUENTIAL's holding images: one per eye, the size and format of that
// eye's swapchain images, so a finished one copies straight in, and drawn
// through the same view description the swapchain path uses.
bool ensure_ss_pending(uint32_t eye)
{
    if (eye > 1 || g.rtvs[eye].empty() || !g.rtvs[eye][0] || !g.device) return false;
    ComPtr<ID3D11Resource> res;
    g.rtvs[eye][0]->GetResource(&res);
    ComPtr<ID3D11Texture2D> image;
    if (!res || FAILED(res.As(&image))) return false;
    D3D11_TEXTURE2D_DESC want{};
    image->GetDesc(&want);
    if (g.ssPending[eye]) {
        D3D11_TEXTURE2D_DESC have{};
        g.ssPending[eye]->GetDesc(&have);
        if (have.Width == want.Width && have.Height == want.Height &&
            have.Format == want.Format && have.ArraySize == want.ArraySize &&
            have.SampleDesc.Count == want.SampleDesc.Count)
            return true;
        g.ssPending[eye].Reset();
        g.ssPendingRtv[eye].Reset();
        g.ssPendingValid[eye] = false;
    }
    D3D11_TEXTURE2D_DESC d = want;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    d.CPUAccessFlags = 0;
    d.MiscFlags = 0;
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    g.rtvs[eye][0]->GetDesc(&rd);
    if (FAILED(g.device->CreateTexture2D(&d, nullptr, &g.ssPending[eye])) ||
        FAILED(g.device->CreateRenderTargetView(g.ssPending[eye].Get(), &rd,
                                                &g.ssPendingRtv[eye]))) {
        g.ssPending[eye].Reset();
        g.ssPendingRtv[eye].Reset();
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            VRLOG("Synched Sequential: holding image for eye %u could not be created "
                  "(%ux%u fmt %d)", eye, want.Width, want.Height, (int)want.Format);
        return false;
    }
    return true;
}

// Both eyes of one moment into the swapchains, and the views they were rendered
// from become what every Present declares until the next pair is complete.
// `firstEye`: the eye that rendered the moment first (ss_first_eye()).
bool ss_commit_pair(uint32_t firstEye)
{
    for (uint32_t e = 0; e < 2; ++e) {
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        uint32_t idx = 0;
        if (XR_FAILED(xrAcquireSwapchainImage(g.swapchain[e], &ai, &idx)) ||
            XR_FAILED(xrWaitSwapchainImage(g.swapchain[e], &wi)))
            return false;
        ComPtr<ID3D11Resource> dst;
        g.rtvs[e][idx]->GetResource(&dst);
        if (dst) g.ctx->CopyResource(dst.Get(), g.ssPending[e].Get());
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(g.swapchain[e], &ri);
        g.eyeFresh[e] = true;
        g.eyeEverRendered[e] = true;
        g.blitRect[e] = g.ssPendingRect[e];
    }
    // Both renders were built from the same latched views; the first's
    // snapshot stands for the pair.
    g.ssCommittedView[0] = g.ssPendingView[firstEye & 1u][0];
    g.ssCommittedView[1] = g.ssPendingView[firstEye & 1u][1];
    g.ssCommitted = true;
    return true;
}

// HOW MANY BUILDS BEHIND IS THE IMAGE AT THIS PRESENT: 0 in gameplay, 1 on the
// map, garage and menus (see BuildRec). Voted from the camera: the GPU-side
// view is the camera the image was actually drawn with, so whichever of the
// last two builds' cameras it matches says the lag -- readable whenever the two
// differ, i.e. whenever the head or the camera moves, which in a headset is
// nearly always. Three agreeing votes to switch. With nothing to tell the two
// apart for two seconds the screen decides, the way it was measured.
int update_presented_lag()
{
    const uint32_t b = (uint32_t)g_advFlips.load();
    BuildRec r0, r1;
    AcquireSRWLockShared(&g_buildRecLock);
    r0 = g_buildRec[b % kBuildRing];
    r1 = g_buildRec[(b - 1) % kBuildRing];
    ReleaseSRWLockShared(&g_buildRecLock);

    const ULONGLONG now = GetTickCount64();
    float view[16], viewProj[16];
    if (r0.build == b && r1.build == b - 1 && r0.haveFwd && r1.haveFwd &&
        hooks::main_camera_matrices(view, viewProj)) {
        // Row 2 of the view is the camera's forward axis (either sign).
        const float gpu[3] = {view[8], view[9], view[10]};
        auto angle = [](const float* a, const float* c) {
            const float x = a[1] * c[2] - a[2] * c[1], y = a[2] * c[0] - a[0] * c[2],
                        z = a[0] * c[1] - a[1] * c[0];
            const float d = a[0] * c[0] + a[1] * c[1] + a[2] * c[2];
            return std::atan2(std::sqrt(x * x + y * y + z * z), std::fabs(d));
        };
        const float a0 = angle(gpu, r0.fwd), a1 = angle(gpu, r1.fwd);
        const float sep = angle(r0.fwd, r1.fwd);
        // A vote needs the two builds to differ (0.06 deg) and the GPU camera
        // to sit clearly on one of them.
        if (sep > 1.0e-3f && (std::min)(a0, a1) < 0.3f * sep) {
            const int v = (a0 < a1) ? 0 : 1;
            if (v == g_lagVote) ++g_lagRun;
            else { g_lagVote = v; g_lagRun = 1; }
            if (g_lagRun >= 3) {
                g_lagVoteTick = now;
                if (g_presLag.exchange(v) != v || !g_lagMeasured)
                    VRLOG("frame timing: images show the camera built %d frame(s) "
                          "before their Present (measured from the GPU camera)", v);
                g_lagMeasured = true;
            }
        }
    }
    if (now - g_lagVoteTick > 2000) {
        const int byScreen = r0.prevScreen ? 1 : 0;
        g_lagMeasured = false;
        if (g_presLag.exchange(byScreen) != byScreen)
            VRLOG("frame timing: images now show the camera built %d frame(s) before their "
                  "Present (by screen -- no camera motion to measure it from)", byScreen);
    }
    return g_presLag.load();
}

// THE BUILD ON SCREEN, worked out once at the top of every Present: the lag
// (update_presented_lag), the record of the build the backbuffer's image was
// drawn with, its eye for routing (present_route_eye) and its views for every
// pose this Present declares or warps to (reference_view_pose). In gameplay
// that is simply the latest build, exactly as before; on the map, garage and
// menus it is the one before.
// THE RENDER'S OWN CAMERA, when the frame uploaded one (cbuffer_hook.cpp): the
// build it sits on, else the newest build when it was uploaded -- the one a
// render takes. 0 = none this frame (nothing recognised as the main view since
// the last Present), and the old measure decides.
uint32_t render_camera_build(uint32_t newest, bool* matched = nullptr)
{
    hooks::CamSnapInfo si;
    if (!hooks::main_camera_snap(si)) return 0;
    if (g_snapSeqValid.load() && si.seq == g_snapSeqSeen.load()) return 0;   // not this frame's
    const uint32_t b = si.build ? si.build : si.newestAt;
    if (b == 0 || newest - b > 3u) return 0;
    if (matched) *matched = si.build != 0;
    return b;
}

void update_on_screen()
{
    // WHICH BUILD'S IMAGE THIS IS, from the render's own camera. It replaces
    // the vote below wherever the frame uploaded a main camera, which is every
    // frame with a 3D scene. The vote compares camera DIRECTIONS, which the two
    // renders of a Synched Sequential pair share, so under SS it rarely got a
    // vote in and the screen decided -- two seconds after the last vote, i.e.
    // a second or more into the map with every other image routed to the wrong
    // eye (the left/right jitter on opening the map, 2026-10-28).
    const uint32_t newest = (uint32_t)g_advFlips.load();
    bool matched = false;
    const uint32_t fromRender = render_camera_build(newest, &matched);
    {
        hooks::CamSnapInfo si;
        if (hooks::main_camera_snap(si)) {
            g_snapSeqSeen.store(si.seq);
            g_snapSeqValid.store(true);
        }
    }
    int lag;
    if (fromRender) {
        lag = (int)(newest - fromRender);
        ++g_onScreenBy[matched ? 0 : 1];
        // Keeps the vote's own timer quiet while this decides, so a frame
        // without a camera does not swing it to "by screen" at once.
        g_lagVoteTick = GetTickCount64();
        static ULONGLONG s_lastLog = 0;
        static int s_quiet = 0;
        if (g_presLag.exchange(lag) != lag) {
            const ULONGLONG now = GetTickCount64();
            if (now - s_lastLog >= 2000) {
                VRLOG("frame timing: images now show the camera built %d frame(s) before "
                      "their Present (the render's own camera%s)", lag,
                      s_quiet ? ", after more changes in between" : "");
                s_lastLog = now;
                s_quiet = 0;
            } else {
                ++s_quiet;
            }
        }
        g_lagMeasured = true;
    } else {
        lag = update_presented_lag();
        ++g_onScreenBy[2];
    }
    // Where the Presents' builds came from, every 10 s.
    {
        static ULONGLONG t0 = 0;
        static uint32_t lagCount[3] = {0, 0, 0};
        ++lagCount[lag <= 0 ? 0 : (lag == 1 ? 1 : 2)];
        const ULONGLONG now = GetTickCount64();
        if (!t0) t0 = now;
        if (now - t0 >= 10000) {
            VRLOG("frame timing: Presents in 10 s by the render's camera on a build %u, by "
                  "its upload time %u, by the old vote/screen %u | images 0/1/2+ builds "
                  "behind %u/%u/%u", g_onScreenBy[0], g_onScreenBy[1], g_onScreenBy[2],
                  lagCount[0], lagCount[1], lagCount[2]);
            g_onScreenBy[0] = g_onScreenBy[1] = g_onScreenBy[2] = 0;
            lagCount[0] = lagCount[1] = lagCount[2] = 0;
            t0 = now;
        }
    }
    const uint32_t onScreen = newest - (uint32_t)lag;
    AcquireSRWLockShared(&g_buildRecLock);
    g_onScreenRec = g_buildRec[onScreen % kBuildRing];
    ReleaseSRWLockShared(&g_buildRecLock);
    g_onScreenValid = (g_onScreenRec.build == onScreen && onScreen != 0);
    g_onScreenEye.store(g_onScreenValid ? g_onScreenRec.eye : -1);
    {
        const int builder = g_onScreenValid ? g_onScreenRec.builder : kBuilderUnknown;
        const int was = g_onScreenBuilder.exchange(builder);
        if (builder != was && builder != kBuilderUnknown)
            VRLOG("SCREEN CAMERA -> %s camera on screen (map markers %s, drive camera %s)",
                  builder == kBuilderMenu ? "MENU" : "IN-GAME",
                  hooks::in_map_view() ? "say map" : "say not map",
                  hooks::logic_camera_idle_ms() > kDibrIdleMs ? "idle" : "ticking");
    }
    AcquireSRWLockExclusive(&g_renderViewLock);
    g_onScreenViewsValid = g_onScreenValid && g_onScreenRec.haveViews;
    if (g_onScreenViewsValid) {
        g_onScreenViews[0] = g_onScreenRec.views[0];
        g_onScreenViews[1] = g_onScreenRec.views[1];
    }
    ReleaseSRWLockExclusive(&g_renderViewLock);
}

// THE IMAGE AN EYE IS BUILT FROM: the game's frame, or DLSS's image of it
// (render/dlss.hpp) -- upscaled to the eye images' size, and sharpened. Once
// per Present that shows a new render, before anything copies the frame into
// an eye; a Present that is skipped (stalled, not rendering) never asks, and
// its frame stays out of DLSS's history. Everything after this -- both eyes'
// blits, the retained copy the stale-eye warp works from, DIBR shift -- sees
// the same image.
ID3D11Texture2D* eye_source(ID3D11Texture2D* backbuffer, int eye)
{
    if (ID3D11Texture2D* img = dlss::image_for_present(g.ctx.Get(), backbuffer, eye))
        return img;
    return backbuffer;
}

// One Present's render under Synched Sequential: composited into its eye's
// holding image exactly as AER would put it in the swapchain (the windscreen
// splatter is this render's own, so unshifted), and -- when it is the second
// render of a moment whose first is waiting -- the pair is committed.
//
// `r` is the record of the build this image was DRAWN with, which on the map,
// garage and menus is the build before the latest (update_presented_lag): its
// eye, its moment and the views its camera was built from all come from there,
// so each eye is paired and declared with what it actually shows.
void ss_present_eye(ID3D11Texture2D* src, const BuildRec& r)
{
    const uint64_t rec = r.ss;
    if (!(rec >> 63)) return;
    const bool     first  = ((rec >> 62) & 1u) != 0;
    const uint32_t moment = (uint32_t)rec;
    const uint32_t eye    = (uint32_t)(r.eye & 1);   // which eye goes first alternates
    if (!ensure_ss_pending(eye)) return;

    XrRect2Di rect{};
    if (!blit_eye_image(eye, src, 0.0f, false, 0.0f, 0.0f,
                        g.ssPendingRtv[eye].Get(), &rect))
        return;
    g.ssPendingRect[eye]  = rect;
    g.ssPendingPair[eye]  = moment;
    g.ssPendingValid[eye] = true;
    // The views THIS render was built from: its build's own snapshot, not
    // g_ssViews, which may already hold the next moment's by now.
    for (int v = 0; v < 2; ++v)
        g.ssPendingView[eye][v] = r.haveViews ? r.views[v] : g.lastView[v];

    if (!first) {
        const uint32_t other = eye ^ 1u;   // rendered this moment first
        if (g.ssPendingValid[other] && g.ssPendingPair[other] == moment) {
            if (ss_commit_pair(other)) g_ssPairs.fetch_add(1);
            g.ssPendingValid[0] = g.ssPendingValid[1] = false;
        } else {
            g_ssOrphans.fetch_add(1);
        }
    }

    // How it is going, every 10 s: pairs completed per second, the learned
    // lag, where the game's camera input changed (in-game: at frozen updates
    // when what it follows moves, at real ones when the camera itself swings),
    // second renders that found no first, and the camera hold -- how many
    // second renders came in with a different camera input and were built from
    // their first's instead (viewbuild_hook.cpp, ss_hold_camera).
    // The rate first: pairs per second, the game's renders per second (two per
    // pair at best) and the headset's frames, with how long each waited for
    // its refresh -- near 0 means the PC is what limits the pair rate.
    static ULONGLONG t0 = 0;
    static uint32_t pairs0 = 0;
    static int presents0 = 0;
    const ULONGLONG now = GetTickCount64();
    if (t0 == 0) { t0 = now; pairs0 = g_ssPairs.load(); presents0 = g_presents.load(); }
    if (now - t0 >= 10000) {
        const uint32_t pairs = g_ssPairs.load();
        const int presents = g_presents.load();
        const double secs = (double)(now - t0) / 1000.0;
        const uint32_t xrFrames = g_ssXrFrames.exchange(0);
        const uint32_t waitUs = g_ssWaitUs.exchange(0);
        hooks::SsHoldStats hold;
        hooks::take_ss_hold_stats(hold);
        VRLOG("Synched Sequential: %.1f pairs/s from %.0f renders/s, %.0f headset "
              "frames/s (%.1f ms waiting for each), renders %d update(s) behind, images %d "
              "build(s) behind, camera input changed at real/frozen updates %u/%u, "
              "unpaired %u, camera builds at real/frozen updates %u/%u, frames without "
              "one %u | camera held for %u of %u second renders (moved up to %.2f deg, "
              "%.1f cm, truck frame %.2f deg), no first to hold to %u",
              (pairs - pairs0) / secs, (presents - presents0) / secs, xrFrames / secs,
              xrFrames ? waitUs / 1000.0 / xrFrames : 0.0, g_ssLag.load(),
              g_presLag.load(),
              g_ssNewMoment[0].exchange(0), g_ssNewMoment[1].exchange(0),
              g_ssOrphans.exchange(0), g_ssBuilds[0].exchange(0), g_ssBuilds[1].exchange(0),
              g_ssStalled.exchange(0), hold.moved, hold.seconds, hold.maxDeg, hold.maxCm,
              hold.maxTruckDeg, hold.unmatched);
        t0 = now;
        pairs0 = pairs;
        presents0 = presents;
    }
}

// The eye views at display time `t`: the runtime's latest (g.lastView), the IPD
// and the cant -- and, with `latchSs`, the views both renders of the next
// moment are built from under Synched Sequential.
void locate_eye_views(XrTime t, bool latchSs)
{
    XrViewState vst{XR_TYPE_VIEW_STATE};
    XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = t;
    vli.space = g.space;
    uint32_t vc = 0;
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    if (XR_SUCCEEDED(xrLocateViews(g.session, &vli, &vst, 2, &vc, views)) && vc == 2) {
        note_view_validity(vst);
        g.lastView[0] = views[0];
        g.lastView[1] = views[1];
        // ...and the views that go with it, for both renders of the moment.
        if (latchSs) {
            AcquireSRWLockExclusive(&g_renderViewLock);
            g_ssViews[0] = views[0];
            g_ssViews[1] = views[1];
            g_ssViewsValid = true;
            ReleaseSRWLockExclusive(&g_renderViewLock);
        }
        float dx = views[1].pose.position.x - views[0].pose.position.x;
        float dy = views[1].pose.position.y - views[0].pose.position.y;
        float dz = views[1].pose.position.z - views[0].pose.position.z;
        float d = std::sqrt(dx*dx + dy*dy + dz*dz);
        if (d > 0.03f && d < 0.12f) { g.ipd = d; g_ipd.store(d); }
        if (vst.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)
            latch_eye_cant(views);   // see render_frame() for why it is gated
    }
}

// SYNCHED SEQUENTIAL: ONE HEAD POSE PER MOMENT. The next render is either the
// first of a new moment -- then it gets a fresh pose, as in AER -- or the
// frozen repeat of the one just rendered, which must be built from the SAME
// pose or the pair would disagree by however far the head moved in a frame.
// Which one is known in advance: the next update's kind is ours to decide, and
// the lag says which update the next render shows.
bool ss_next_render_starts_moment()
{
    const uint32_t cur = hooks::frame_update_index();
    const uint32_t nextShown = cur + 1 - (uint32_t)g_ssLag.load();
    const bool nextFrozen = (nextShown == cur + 1) ? hooks::frame_next_update_frozen()
                                                   : hooks::frame_update_frozen(nextShown);
    return !nextFrozen;
}

// SYNCHED SEQUENTIAL AT THE FULL PAIR RATE: a Present whose image is the FIRST
// render of a moment. It runs no XR frame. The image is set aside for its pair
// and the game goes straight on to the second render; the XR frame runs at the
// Present that completes the pair. So the headset paces PAIRS -- one per
// refresh -- instead of renders, and the game renders twice per refresh when
// the PC can. Every Present used to run a full XR frame, as AER does, which
// held the game to one render per refresh: 80 FPS was 40 pairs a second.
//
// Not the render/submit decoupling that was removed from AER (see the note
// near the top of this file): that guessed from timing when a display was due,
// and the guess collapsed back to one render per refresh. Nothing here is
// guessed -- the pair says when there is something to submit.
void ss_first_eye_present(IDXGISwapChain* swapchain)
{
    ComPtr<ID3D11Texture2D> backbuffer;
    if (SUCCEEDED(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuffer))) {
        ss_present_eye(eye_source(backbuffer.Get(), (int)(g_onScreenRec.eye & 1)),
                       g_onScreenRec);
        framedump::end_frame();
        if (g.desktopSave) {
            if (g_desktopEye.load() == (g_onScreenRec.eye & 1))
                g.ctx->CopyResource(g.desktopSave.Get(), backbuffer.Get());
            else                          g.ctx->CopyResource(backbuffer.Get(), g.desktopSave.Get());
        }
    }
    // The next render can begin a moment here: on the map, garage and menus the
    // image is a build late, so the first render of one is on screen while the
    // first of the next is about to be built. Its pose is latched now, for the
    // display time the coming XR frame should get.
    if (ss_next_render_starts_moment()) {
        const XrTime t = g_lastDisplayTime + g_lastDisplayPeriod;
        update_head_pose(t);
        locate_eye_views(t, true);
    }
    g.eyeFresh[0] = g.eyeFresh[1] = false;
    g_newFrame.store(true);
}

void render_frame(IDXGISwapChain* swapchain)
{
    g_presents.fetch_add(1);

    // A Present with no camera build behind it carries no new render -- the
    // engine repeated a frame. g_newFrame is set at the end of every Present and
    // cleared by the first camera build of the next one, so finding it still set
    // here means no build happened in between.
    //
    // Routing such a Present is what makes the wrong-eye state PERSIST rather
    // than being a one-frame blip: the ring records the same parity twice, so the
    // same eye is warped twice while the other never receives its render, and
    // because g_newFrame is still pending the next build flips only once --
    // permanently shifting the parity phase against the Present count until the
    // next stall happens to shift it back. Measured in a frame dump as two
    // consecutive frames warping the same eye with near-zero change between them.
    //
    // The frame is skipped wholesale instead: no route, no blit, no warp. Both
    // eyes keep the images they already hold and the layer is resubmitted as-is,
    // which is exactly right -- there is no new content to show.
    const bool stalled = g_newFrame.load();

    // Did the view matrix the GPU actually uses move since the previous Present?
    // cbuffer_hook captures it before our own edits, so this is the ENGINE's
    // camera, not ours. Compared with an epsilon well below any real camera
    // motion but above float noise.
    {
        float view[16], viewProj[16];
        if (hooks::main_camera_matrices(view, viewProj)) {
            static float prev[16] = {0};
            float d = 0.0f;
            for (int i = 0; i < 16; ++i) d += std::fabs(view[i] - prev[i]);
            if (d > 1e-6f) g_viewChanges.fetch_add(1);
            for (int i = 0; i < 16; ++i) prev[i] = view[i];
        }
    }

    // Build/present rate report. builds/s is what actually limits stereo update
    // rate; presents/s above it is pure waste under AER.
    {
        static DWORD t0 = GetTickCount();
        static int p0 = 0, f0 = 0;
        const DWORD now = GetTickCount();
        const int dt = (int)(now - t0);
        if (dt >= 2000) {
            const int p = g_presents.load(), f = g_advFlips.load();
            const int dp = p - p0, df = f - f0;
            const double buildHz = df * 1000.0 / dt;
            const int c = g_advCalls.load();
            const int vc = g_viewChanges.load();
            static int c0 = 0, v0 = 0;
            const int dc = c - c0, dv = vc - v0;
            c0 = c; v0 = vc;
            // (The per-second "AER rate" line reported here -- presents,
            // buildCalls, firstPerFrame and renderCamMoved -- answered whether
            // the renderer was capped and whether the camera was interpolated
            // downstream of our injection point. Both are settled; the counters
            // are still maintained for a debugger, the line was noise.)
            (void)dc; (void)dv; (void)buildHz; (void)df;
            t0 = now; p0 = p; f0 = f;
        }
    }

    // FOV-stability monitor. Two distinct failures look the same in the headset,
    // so measure the projection ACTUALLY rendered (derived from the main camera's
    // viewProj*inverse(view)) rather than trusting what we wrote:
    //   - if our settings write is being overwritten, f0 drifts from ours;
    //   - if f0 holds but the rendered HFOV still moves, the projection is built
    //     from some other field and writing settings is pointless.
    // Split per eye, because ANY per-frame FOV animation under AER means the two
    // eyes are rendered with different projections -- which strains the eyes on
    // its own, regardless of the IPD axis being right.
    {
        float pa=0, pb=0.1f, p00=0.73454f;
        if (dibr_projection(pa, pb, p00) && p00 > 0.01f) {
            const float hfov = 2.0f * std::atan(1.0f / p00) * 57.29578f;
            const uint32_t e = (uint32_t)(g_frameEye.load() & 1);
            static float mn[2] = {1e9f, 1e9f}, mx[2] = {-1e9f, -1e9f}, last[2] = {0,0};
            static DWORD t0 = GetTickCount();
            mn[e] = (std::min)(mn[e], hfov); mx[e] = (std::max)(mx[e], hfov);
            last[e] = hfov;
            const DWORD now = GetTickCount();

            // PROJ LOCK MISS -- the one failure that is a bug rather than a
            // reading. The compositor is told render_hfov_deg(); the GPU
            // rendered `hfov`. Those are the same number by construction as
            // long as the lock reaches the main view, and when it does not the
            // compositor stretches the difference across the display: a
            // narrower render inside a wider declared frustum IS the magnified
            // world you see, and the ratio below is exactly how magnified.
            //
            // Separate from the spread line above because the cause is
            // different -- that one asks whether the FOV is moving, this one
            // asks whether the lock is landing at all -- and because it names
            // the screen, which is where the answer has been every time (the
            // fly-in after a map transfer, the main menu before it). Named from
            // the camera classifiers, which is all there is again.
            {
                static int   s_miss = 0;
                static DWORD s_last = 0;
                const float target = render_hfov_deg();
                const bool  bad = target > 1.0f && std::fabs(hfov - target) > 2.0f;
                s_miss = bad ? s_miss + 1 : 0;
                if (s_miss >= 30 && now - s_last >= 2000) {
                    s_last = now;
                    const float tr = std::tan(target * 0.5f * 0.0174532925f);
                    const float rr = std::tan(hfov   * 0.5f * 0.0174532925f);
                    VRLOG("PROJ LOCK MISS: rendering %.2f deg, submitting %.2f "
                          "(%.2fx %s) on %s -- the lock is not reaching the main view",
                          hfov, target,
                          rr > 1.0e-4f ? tr / rr : 0.0f,
                          hfov < target ? "magnified" : "shrunk",
                          hooks::in_map_view() ? "the map"
                              : (hooks::in_gameplay() ? "gameplay" : "a garage/menu screen"));
                }
            }
            if (now - t0 >= 1000) {
                const float spread = (std::max)(mx[0], mx[1]) - (std::min)(mn[0], mn[1]);
                const float eyeGap = std::fabs(last[0] - last[1]);
                if (spread > 0.10f || eyeGap > 0.10f) {
                    float f0 = 0, f1 = 0; hooks::game_fov_fields(f0, f1);
                    VRLOG("FOV rendered: eye0=%.2f eye1=%.2f (gap %.2f) range[%.2f..%.2f] "
                          "spread %.2f deg | game's own fields f0=%.2f f1=%.2f (untouched)",
                          last[0], last[1], eyeGap,
                          (std::min)(mn[0], mn[1]), (std::max)(mx[0], mx[1]), spread,
                          f0, f1);
                }
                mn[0]=mn[1]=1e9f; mx[0]=mx[1]=-1e9f; t0 = now;
            }
        }
    }
    static uint32_t stalls = 0, framesSeen = 0;
    ++framesSeen;
    const bool ss = synched_active();
    if (stalled && ss) g_ssStalled.fetch_add(1);
    if (stalled && ++stalls % 32 == 0)
        VRLOG("AER: %u/%u Presents had no camera build (frame skipped)", stalls, framesSeen);
    if (ss && g_ssReset.exchange(false)) {
        g.ssPendingValid[0] = g.ssPendingValid[1] = false;
        g.ssCommitted = false;
        g_lastDisplayTime = 0;
    }

    // SYNCHED SEQUENTIAL AT THE FULL PAIR RATE. The image on screen is the
    // first render of a moment: nothing new can be shown until its pair is
    // complete, so this Present runs no XR frame -- see ss_first_eye_present().
    // Never two in a row, so a pairing gone wrong can only halve the headset's
    // frames, never stop them; and not before an XR frame has given a display
    // time to predict from.
    static bool s_heldLast = false;
    if (ss && !stalled && !s_heldLast && g_lastDisplayTime != 0 && g_onScreenValid &&
        (g_onScreenRec.ss >> 63) && ((g_onScreenRec.ss >> 62) & 1u)) {
        s_heldLast = true;
        ss_first_eye_present(swapchain);
        return;
    }
    s_heldLast = false;

    XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    LARGE_INTEGER waitStart{}, waitEnd{};
    QueryPerformanceCounter(&waitStart);
    if (XR_FAILED(xrWaitFrame(g.session, &fwi, &fs)))
        return;
    QueryPerformanceCounter(&waitEnd);
    if (ss) {
        static LARGE_INTEGER freq{};
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        g_ssXrFrames.fetch_add(1);
        g_ssWaitUs.fetch_add((uint32_t)((waitEnd.QuadPart - waitStart.QuadPart) * 1000000 /
                                        freq.QuadPart));
    }

    XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
    XrResult beginRes = xrBeginFrame(g.session, &fbi);
    if (XR_FAILED(beginRes)) {
        static bool loggedBeginFail = false;
        if (!loggedBeginFail) { loggedBeginFail = true;
            VRLOG("xrBeginFrame failed: %d", (int)beginRes); }
        return;
    }

    const bool validTime = !is_bogus_display_time(fs.predictedDisplayTime);
    if (validTime) {
        g_lastDisplayTime   = fs.predictedDisplayTime;
        g_lastDisplayPeriod = fs.predictedDisplayPeriod;
    }

    // SYNCHED SEQUENTIAL: ONE HEAD POSE PER MOMENT -- see
    // ss_next_render_starts_moment(). In AER every render takes a fresh one.
    const bool latchPose = !ss || ss_next_render_starts_moment();

    if (validTime && latchPose) {
        update_head_pose(fs.predictedDisplayTime);
    } else if (!validTime) {
        static bool loggedSentinel = false;
        if (!loggedSentinel) { loggedSentinel = true;
            VRLOG("xr: ignoring VDXR sentinel predictedDisplayTime"); }
    }

    // Locate eye views and update IPD each frame.
    locate_eye_views(fs.predictedDisplayTime, ss && latchPose);

    if (!ss && g_ssViewsValid) {
        AcquireSRWLockExclusive(&g_renderViewLock);
        g_ssViewsValid = false;
        ReleaseSRWLockExclusive(&g_renderViewLock);
    }

    hold_winch_pointer_against_head(winch_cursor_active());

    // What DIBR shift published this frame, hoisted so the projection views below can
    // see it. Both eyes of an DIBR shift pair derive from ONE render, which is what
    // makes it safe to declare that render's pose for both -- see the submit
    // site. AER cannot do this: its two eyes come from different frames.
    bool     dibrPairThisFrame = false;
    uint32_t dibrRealEye = 0;
    float    dibrSeparationM = 0.0f;

    if (fs.shouldRender && !stalled && ss) {
        // SYNCHED SEQUENTIAL: this frame's render is one eye of a moment --
        // held until the other render of the same moment has arrived, then
        // both go into the swapchains together. No warp and no DIBR: there is
        // no stale eye to supply. Until a pair completes, the last one keeps
        // being submitted, so the eyes never show two different moments.
        ComPtr<ID3D11Texture2D> backbuffer;
        if (SUCCEEDED(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuffer))) {
            uint32_t eye = (uint32_t)present_route_eye();
            if (g_onScreenValid) {
                ss_present_eye(eye_source(backbuffer.Get(), (int)(g_onScreenRec.eye & 1)),
                               g_onScreenRec);
                if (g_onScreenRec.ss >> 63) eye = (uint32_t)(g_onScreenRec.eye & 1);
            } else {
                g_ssOrphans.fetch_add(1);
            }
            framedump::end_frame();
            if (g.desktopSave) {
                if ((int)eye == g_desktopEye.load()) g.ctx->CopyResource(g.desktopSave.Get(), backbuffer.Get());
                else                                 g.ctx->CopyResource(backbuffer.Get(), g.desktopSave.Get());
            }
        }
    } else if (fs.shouldRender && !stalled) {
        ComPtr<ID3D11Texture2D> backbuffer;
        if (SUCCEEDED(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuffer))) {
            ID3D11Texture2D* blitSrc = backbuffer.Get();

            // Must be called exactly once per Present (it advances the ring).
            uint32_t eye = (uint32_t)present_route_eye();

            // The eye's image: DLSS's when it made one -- see eye_source().
            // DIBR shift does not run while it upscales (dibr_shift_runs()),
            // so `eye` is final for the upscale here.
            const bool dibrRuns = dibr_shift_runs();
            blitSrc = eye_source(backbuffer.Get(), (int)eye);

            // DIBR shift: synthesize the OTHER eye from this one.
            //
            // Everything here is derived from xr::applied_eye() -- the eye index
            // and the +-IPD/2 the camera hook ACTUALLY baked, published together
            // in one atomic. The old implementation took the eye from the parity
            // ring and the shift direction from the swapchain index, which meant
            // the disparity direction depended on a chain of independently-tuned
            // sign conventions (g_sideSign, kProjEyeSign, g_eyeSide, and the
            // ring's own delay parity). Any one of them flipping silently
            // reversed the stereo. The offset's SIGN is the direction, by
            // definition, so none of that chain is consulted any more.
            // PAUSE MENU / LOADING / MAIN MENU: give BOTH eyes the same frame.
            //
            // These screens stop driving the camera, so the AER parity stops
            // advancing and `eye` stops alternating. The partner eye then never
            // gets a fresh render, and the stale-eye warp faithfully reproduces
            // whatever it last held -- a frame from BEFORE the menu opened. That
            // is the "pause menu in one eye only" symptom, and it is why a
            // loading screen looks fine: it lasts long enough for both eyes to
            // have been rendered with it.
            //
            // Nothing is lost by going mono here. The content is a flat
            // screen-space panel with no parallax to preserve, so the same
            // image in both eyes is not an approximation -- it is what the
            // screen actually is, and it is what plain AER ends up showing.
            //
            // The map and garage are EXCLUDED: they idle the drive camera the
            // same way but have real 3D behind them (and their own window
            // shrink), so they keep the normal path.
            // Presents since each eye was last the REAL rendered eye, updated
            // after the blits below. This measures the failure DIRECTLY instead
            // of inferring it from a camera timer: the partner eye goes stale
            // exactly when the AER parity stops advancing, whatever the reason.
            static int s_sinceReal[2] = {0, 0};
            const bool parityStalled =
                dibrpolicy::mono_screen(s_sinceReal[0], s_sinceReal[1]);

            // This REPLACES the old "drive camera idle and not the map" test.
            // Going mono is only ever correct when the partner eye is starving,
            // and the timer does not answer that question -- it answers a
            // different one that merely correlated on the pause menu.
            //
            // The GARAGE is where the two come apart, measured from a frame
            // dump: both eyes byte-identical (L->R +0 px, r=1.000) while the
            // image alternated between two states every frame (adjacent r=0.976
            // vs skip-one r=0.994) with a rigid -3/+3 px swing that was the SAME
            // at near and far depths. That last part is the tell: the garage
            // hangs its AER eye offset on a projection-matrix shift (kMapEyeSign,
            // viewbuild_hook.cpp), which translates the whole image rather than
            // producing depth-varying parallax. The parity was alternating
            // perfectly well -- the game was handing us correct alternate-eye
            // renders, and the mono blit was showing BOTH eyes whichever one
            // arrived, turning a stable stereo pair into a 90 Hz shared
            // oscillation. Plain AER routes each render to its own eye and is
            // stable, which is exactly why AER looked fine there.
            //
            // The LOADING SCREEN needs the opposite and the same test gives it:
            // its drive camera is not idle long enough to have tripped the timer,
            // so mono never fired and it depended on the warp running to fill the
            // partner eye. When the warp declines, that eye falls back to a
            // rotation-warped pre-loading-screen frame -- the world, not the
            // loading screen. Its parity genuinely does stall, so this catches it.
            const bool monoScreen = parityStalled;

            ID3D11Texture2D* warped = nullptr;
            // The eye DIBR shift would synthesize: the partner of the one just
            // rendered. Initialised HERE rather than only on the success path
            // below, because the DECLINE path uses it too.
            //
            // It used to default to 0 and be assigned only after every guard
            // passed, so a declined frame fell back to warping eye 0 whatever
            // had actually been rendered. When eye 0 was the rendered one that
            // overwrote the fresh render with a stale warp and left eye 1
            // untouched -- which is the long-standing "pause menu shows in one
            // eye only". It was intermittent while declines were, and became
            // constant once the drive-camera-idle fallback made menus decline
            // every frame by design.
            uint32_t warpedDstEye = eye ^ 1u;

            // --- per-frame decision record, for frame-dump forensics ---------
            // Only written while a burst is running, and read back under the
            // same frame number the burst writes (fN_LR.jpg). The occasional
            // DIBR shift frame that lands with no DIBR shift has survived several
            // rounds of reasoning-backwards-from-the-image; this makes the
            // frame say which branch produced it instead. Costs nothing outside
            // a dump.
            const char* dibrWhy = "(not DIBR shift)";
            int   dbgDepthEye = -9, dbgAeEye = -9;
            float dbgAeOff = 0.0f;
            // The exact triple the disparity is built from. A frame that warps
            // with everything nominal and still shifts nothing has to fail in
            // one of these or in the depth, and those are the only two places
            // left once the guards have all passed.
            dibr::WarpParams dbgWp{};
            bool  dbgHaveWp = false;

            // THE PROJECTION THIS FRAME'S COMPOSITES MUST USE.
            //
            // The windscreen splatter is shifted by the same disparity rule the
            // scene is, so it has to be built from the SAME projection the warp
            // consumed -- which, now that the warp takes a triple frozen beside
            // its depth capture, is no longer necessarily the live read. Using
            // the live one here would shift the glass by a slightly different
            // rule than the world behind it during exactly the transitions the
            // freeze exists to survive.
            //
            // Defaults to the live read, which is correct in AER (no bundle is
            // selected there) and is the fallback when DIBR shift declines.
            // WHICH IMAGE THE RENDERED EYE IS BLITTED FROM, and whether the
            // blit therefore owns the UI composite.
            //
            // For the smudge to sit UNDER the HUD, the image the smudge is
            // composited onto must not have the HUD in it yet. DIBR shift already
            // reprojects the pre-HUD copy for the synthesized eye; blitting the
            // rendered eye from that same copy puts both eyes on the same
            // footing, and the blit then adds smudge and UI in that order.
            //
            // Null means "no pre-HUD copy this frame" -- outside gameplay, or a
            // capture that could not be trusted. Then both eyes fall back to the
            // finished backbuffer, which already contains the UI, and the blit
            // adds only the smudge. That is the old ordering, kept for the case
            // where the alternative would be an eye with no HUD at all.
            ID3D11Texture2D* preUiSrc = nullptr;

            float frameProjA = g_dibrProj.a;
            float frameProjB = g_dibrProj.b;
            bool  frameProjValid = g_dibrProj.valid;

            if (dibrRuns) {
                const xr::AppliedEye ae = xr::applied_eye();
                // ASK FOR THE EYE IN THE BACKBUFFER, rather than taking whatever
                // capture was newest and then checking whether it happened to
                // match. The ring keeps several tagged captures, so a frame
                // whose newest depth belongs to the other eye now finds the
                // right one instead of being declined -- see
                // hooks::scene_depth_for_eye() and section 2.1 of
                // docs/dibr_shift_comparison_witcher3.md.
                hooks::SceneDepth sd{};
                const bool haveDepth =
                    ae.valid && hooks::scene_depth_for_eye(ae.eye, sd);
                ID3D11ShaderResourceView* depth = haveDepth ? sd.srv : nullptr;
                // Still recorded for the frame dump: with selection by identity
                // this now always agrees with the applied eye when a capture was
                // found, so a disagreement in a dump means the ring itself is
                // wrong rather than the pairing.
                const int depthEye = haveDepth ? sd.eye : -1;

                // PAUSE MENU / LOADING / MAP: drop to plain AER while the drive
                // camera is idle. DIBR shift synthesizes an eye from the frame's scene
                // depth, and on those screens no scene is being driven -- the
                // depth stops meaning what the warp assumes, and the declined-
                // frame path (thousands of colour/depth eye-mismatch skips in a
                // session) is dominated by exactly these. AER renders both eyes
                // for real, so it is simply correct there, and the cost that
                // makes DIBR shift worth having does not apply to a static screen.
                //
                // The SETTING is not changed: dibr_shift_enabled() still reports
                // on, so the depth capture, mirror mask and UI layer keep their
                // resources rather than being freed and rebuilt every time a
                // menu opens. Only the per-frame decision moves.
                // DIBR SHIFT IN GAMEPLAY, PLAIN AER EVERYWHERE ELSE. in_gameplay()
                // is the
                // marker for that: it goes false on entering the garage (and on
                // the map and in menus), and no timer hysteresis or pose test is
                // needed on top -- an earlier attempt to recognise the garage by
                // latching the idle timer was chasing a symptom, since the
                // garage's real problem was the mono blit above, not the warp.
                //
                // The idle test STAYS as a second trigger because it catches the
                // PAUSE MENU, which in_gameplay() cannot: the world is still
                // loaded there so the camera has not relocated and the
                // classifier stays true.
                //
                // WHICH SCREENS FORCE THE FALLBACK is on_static_screen() now --
                // menu and map named directly, garage left to the idle timer.
                // See the note there; the short version is that "failed to
                // identify gameplay" and "identified a static screen" are
                // different claims, and only the second is safe to switch the
                // render mode on.
                //
                // The idle test STAYS as the second trigger and catches the
                // PAUSE MENU, which no screen classifier can: the world is still
                // loaded there so the camera has not relocated.
                const bool offWorldScreen = on_static_screen();
                const bool driveIdle = nothing_driven();
                {
                    static bool wasIdle = false;
                    if (driveIdle != wasIdle) {
                        wasIdle = driveIdle;
                        VRLOG("DIBR shift: %s -- %s",
                              driveIdle ? (offWorldScreen ? "on a static screen (menu/map/boot)"
                                                          : "drive camera idle")
                                        : "drive camera moving",
                              driveIdle ? "falling back to AER" : "resuming DIBR shift");
                    }
                }

                // Refresh before the guards below so the rejection is logged
                // even on a frame that is skipped for some other reason.
                dibr_projection_refresh();
                // The full rendered separation, sanity-checked for the same
                // reason the projection is: it is the other input the disparity
                // is built from, and a bad one saturates the clamp just as
                // thoroughly. No human IPD is outside this band.
                const float dibrIpd = dibrpolicy::full_separation_m(ae.offset);

                dbgDepthEye = depthEye;
                dbgAeEye    = ae.valid ? ae.eye : -1;
                dbgAeOff    = ae.offset;

                // Every branch sets `decision` and nothing else: the frame-dump
                // string and the periodic tally are both derived from it below,
                // so a new guard cannot be added to one and forgotten in the
                // other.
                dibrpolicy::GateInputs gateIn;
                gateIn.eyesCanted      = dibr_eyes_canted();
                gateIn.driveIdle       = driveIdle;
                gateIn.appliedEyeValid = ae.valid;
                gateIn.appliedEye      = ae.valid ? ae.eye : -1;
                gateIn.appliedEyeFresh = ae.fresh;
                gateIn.appliedEyeIsCameraMove =
                    (ae.how == kEyeAppliedAsCameraMove);
                gateIn.appliedEyeMapSign = ae.mapSign;
                gateIn.haveDepth       = (depth != nullptr);
                gateIn.depthEye        = depthEye;
                // The projection FROZEN BESIDE THIS DEPTH, not the live read.
                // Both now come from the same frame by construction; the live
                // g_dibrProj remains only as the fallback for a capture taken
                // before the camera CB was readable.
                gateIn.projectionValid = haveDepth && sd.projValid;
                gateIn.fullSeparationM = dibrIpd;
                Decision decision = dibrpolicy::gate(gateIn);

                // The cant decline is permanent for the session, so it gets one
                // loud line with the measured angle. The periodic tally alone
                // would say the warp never runs but not why, and this is the
                // one decline the user could act on (or at least understand).
                if (decision == Decision::DeclinedCantedEyes) {
                    static bool loggedCant = false;
                    if (!loggedCant) {
                        loggedCant = true;
                        VRLOG("DIBR shift: eyes are canted %.3f/%.3f deg (limit %.2f) -- warp "
                              "declined for the session. Its disparity shift is horizontal-only, "
                              "which is exact only for parallel eyes; on canted panels it would "
                              "add VERTICAL disparity. Falling back to AER, which renders "
                              "both eyes for real and is unaffected.",
                              cant_angle_deg(total_cant(0)),
                              cant_angle_deg(total_cant(1)), kMaxDibrCantDeg);
                    }
                }

                if (decision == Decision::Warped) {
                    // The camera sat at ae.offset along its right axis; the eye
                    // being synthesized sits at -ae.offset. So the FULL rendered
                    // separation is 2*|offset| -- taken from the baked value
                    // rather than from g_ipd, so any scaling the camera hook
                    // applied is reproduced exactly instead of assumed away.
                    dibr::WarpParams wp;
                    wp.ipd     = dibrIpd;
                    wp.focalPx = 0.5f * (float)g.width * sd.p00;
                    wp.eyeSign = dibrpolicy::eye_sign(ae.offset, kDibrRightIsScreenRight);
                    // Zero unless the eyes were rendered at different frusta.
                    wp.eyeOffsetPx = dibr_eye_pixel_offset((int)ae.eye,
                                                           (int)(ae.eye ^ 1), g.width);
                    // One validated triple -- A and B must come from the SAME
                    // projection, which three independent live reads could not
                    // guarantee.
                    wp.projA   = sd.projA;
                    wp.projB   = sd.projB;
                    // Hand the same triple to the splatter composite below.
                    frameProjA = sd.projA;
                    frameProjB = sd.projB;
                    frameProjValid = sd.projValid;
                    wp.fillMode = dibr::fill_mode();
                    // The eye we are about to synthesize, rotation-warped from
                    // its own last real render -- see stale_eye_fill(). Null on
                    // the first frames or across a big pose jump, in which case
                    // the warp falls back to its stretch fill.
                    ID3D11ShaderResourceView* fill = stale_eye_fill((uint32_t)(ae.eye ^ 1));
                    // The frame WITHOUT its HUD, if the capture is up. The HUD
                    // is screen-space but sits over scene depth, so reprojecting
                    // the finished backbuffer drags it sideways and leaves a
                    // hole; reprojecting this cannot. The synthesized eye then
                    // simply has no HUD, alternating with the rendered eye.
                    // GAMEPLAY ONLY. On the pause menu, loading screens, the
                    // map and the garage the UI *is* the picture -- reprojecting
                    // a pre-HUD copy there hands the synthesized eye a frame with
                    // no menu in it, which reads as the menu appearing in one eye
                    // only. Those screens have no HUD-over-scene-depth problem to
                    // solve in the first place.
                    // Reproject the frame WITHOUT its HUD when that copy is
                    // available for this frame -- the HUD is screen-space but
                    // sits over scene depth, so reprojecting the finished frame
                    // drags it sideways and leaves a hole behind it.
                    //
                    // The camera-idle test is what catches the PAUSE menu.
                    // in_gameplay() alone does not: the world is still loaded so
                    // the classifier stays true, and the pause overlay was being
                    // excluded from the synthesized eye and appeared in one eye
                    // only. DRIVE_CAMERA stops ticking while paused, which is the
                    // same signal ui_hook already uses to spot menu/map/garage.
                    //
                    // Null on any frame the capture could not be trusted, and the
                    // finished backbuffer is used instead: a dragged HUD for one
                    // frame, rather than an eye built from the wrong image.
                    //
                    // Not when DLSS's sharpened frame stands in for the
                    // backbuffer (eye_source()): the copy predates the
                    // sharpening, and the eye built from it would be the one
                    // left soft. All that costs is a dragged HUD, and only while
                    // the HUD is in the frame at all -- not while the UI has a
                    // layer of its own.
                    const bool hudIsOverlay = hooks::in_gameplay() &&
                                              hooks::logic_camera_idle_ms() < 400 &&
                                              blitSrc == backbuffer.Get();
                    ID3D11Texture2D* dibrSrc =
                        hudIsOverlay ? hooks::pre_hud_texture() : nullptr;
                    // Recorded before the fallback below overwrites it, so
                    // the blits can tell a real pre-HUD copy from the backbuffer
                    // standing in for one.
                    preUiSrc = dibrSrc;
                    if (!dibrSrc) dibrSrc = blitSrc;
                    warped = dibr::warp(g.ctx.Get(), dibrSrc, depth, wp, fill);
                    // Silent unless the warp has become expensive enough to be
                    // worth knowing about; see report_gpu_cost_if_high().
                    dibr::report_gpu_cost_if_high();
                    dbgWp = wp; dbgHaveWp = true;
                    decision = dibrpolicy::warp_outcome(warped != nullptr,
                                                      dibrSrc != blitSrc);
                }

                // ONE place where the decision becomes both outputs, so the
                // dumped frame and the tally can never describe it differently.
                note_dibr_decision(decision);
                dibrWhy = dibrpolicy::decision_name(decision);

                // The rendered image belongs to the eye the camera hook recorded,
                // not to whatever the ring routed. In DIBR shift that record is the
                // authority: it is the only value guaranteed to describe the
                // frame actually sitting in the backbuffer.
                if (ae.valid) eye = (uint32_t)ae.eye;
            }

            // Outside the DIBR shift branch on purpose: it must still run on the
            // Present after the shift is switched off, so the last window's
            // declines are reported rather than silently dropped. Records
            // nothing while it is off, so it stays quiet there.
            report_dibr_decisions();

            ensure_desktop_save(backbuffer.Get());

            // Blits one image into one eye's XR swapchain. Factored out so DIBR shift
            // can submit BOTH eyes each frame: the rendered one into its own
            // eye, the synthesized one into the other.
            // `smudgeShiftUv` places the captured windscreen splatter for THIS
            // eye: 0 for the eye the game actually rendered, the glass's
            // disparity for the eye being synthesized. See smudge_layer.hpp.
            // `dispK` is the per-pixel disparity constant for the splatter
            // composite: 0 for the eye actually rendered, which needs no shift.
            auto blit_into_eye = [&](uint32_t dstEye, ID3D11Texture2D* src,
                                     float dispK = 0.0f, bool rendered = false) -> bool {
                return blit_eye_image(dstEye, src, dispK, frameProjValid, frameProjA,
                                      frameProjB, nullptr, nullptr, rendered);
            };

            // Disparity debug view (fill mode 3) outputs an UNWARPED map derived
            // from the rendered eye's depth. Sending it only to eye^1 showed the
            // rendered eye's map in the other eye, which reads as a displaced
            // duplicate and is impossible to judge. Show it to BOTH eyes instead
            // so it can be assessed on its own terms.
            // Roll the parity tracker read by monoScreen above. `eye` is final
            // by now (DIBR shift re-derives it from applied_eye()), and this counts
            // REAL renders only -- a synthesized or mono blit must not reset it,
            // or the starvation it is meant to detect would hide itself.
            s_sinceReal[eye] = 0;
            if (s_sinceReal[eye ^ 1u] < 1000) ++s_sinceReal[eye ^ 1u];

            const bool disparityView = (dibr::fill_mode() == 3);
            if (warped && disparityView) {
                blit_into_eye(0, warped);
                blit_into_eye(1, warped);
            } else {
                // The eye actually rendered. From the pre-HUD copy when one
                // exists, so the smudge lands under the HUD rather than over it.
                blit_into_eye(eye, preUiSrc ? preUiSrc : blitSrc, 0.0f, true);
                // Same frame to the partner eye on a static screen -- see
                // monoScreen above. Done here rather than by warping, because
                // the warp's source is a pre-menu image.
                if (monoScreen) blit_into_eye(eye ^ 1u, blitSrc);

                // RETAIN BEFORE THE SECOND BLIT. retain_eye_for_warp() copies
                // out of g.staging, and blit_into_eye() overwrites staging with
                // whatever it is handed -- so retaining after the synthesized
                // blit would store the SYNTHESIZED image as this eye's "last
                // real render", and next frame's fill would be a synthesis of a
                // synthesis. AER retains further down instead, where it has no
                // second blit to race.
                // The finished frame, exactly as AER retains it. The stale-eye
                // warp shifts nothing by depth, so where a disocclusion lands on
                // UI, drawing the UI there is what should be on screen anyway.
                // blitSrc explicitly, NOT whatever was just blitted: the
                // rendered eye may now come from the pre-HUD copy, and
                // retain_eye_for_warp() reads g.staging by default. Passing the
                // finished frame keeps the stale-eye fill exactly what it was.
                if (dibrRuns)
                    retain_eye_for_warp(eye, blitSrc);

                // The synthesized eye's splatter is shifted by the disparity of
                // the GLASS rather than of the scene behind it -- the whole
                // point of capturing it separately. Same formula and the same
                // sign convention dibr.cpp's scatter uses, evaluated at one
                // fixed depth, because splatter stuck to a surface at a roughly
                // constant distance has a roughly constant disparity.
                float dispK = 0.0f;
                if (warped && dbgHaveWp && dbgWp.projB != 0.0f) {
                    // Per-pixel constant: disp_px = k * (d - projA). Folds the
                    // reverse-Z inversion and the disparity formula into one
                    // multiply, so the shader needs no divide.
                    dispK = dbgWp.eyeSign * dbgWp.ipd * dbgWp.focalPx / dbgWp.projB;
                }
                if (warped) {
                    blit_into_eye(warpedDstEye, warped, dispK);
                    // Both eyes now hold one render's geometry, so the layer can
                    // declare the pose that render was built from.
                    dibrPairThisFrame = true;
                    dibrRealEye       = eye;
                    dibrSeparationM   = dbgWp.ipd;
                }
            }

            // Only one eye is rendered per XR frame, so the other one
            // used to be left as an untouched stale resubmit -- the compositor
            // re-showing a frame-old image, pinned to a frame-old orientation
            // while the head kept moving. Rotate it to the current pose the same
            // way 2x mode does; it is the identical situation, and 1x is where
            // the stale eye is stale for a whole XR frame rather than half of
            // one, so it has MORE to gain.
            //
            // DIBR shift normally does NOT rotation-warp: it fills the other eye every
            // frame by depth-reprojecting the rendered one, which is strictly
            // more information, and doing both would overwrite the better image
            // with the worse one.
            //
            // But DIBR shift can decline a frame -- no scene depth captured, a
            // colour/depth eye mismatch, or the warp rejecting degenerate
            // projection parameters -- and that happens for whole stretches at a
            // time outside gameplay, where no scene-sized depth is produced at
            // all. Left alone, the partner eye is then a stale resubmit pinned
            // to a frame-old orientation, which is exactly the artefact AER
            // fixed for itself. So on those frames DIBR shift falls back to what AER
            // does: the retained copy is already there (taken between the blits
            // above), so the rotation warp is all that is missing.
            //
            // The retain has to run while g.staging still holds this eye's
            // image, i.e. straight after its blit and before anything else
            // copies into staging.
            //
            // DIBR shift retains too (above, before its second blit) but never
            // rotation-warps INTO the swapchain: the DIBR result is strictly
            // more information than a rotation-only warp of an older frame, and
            // the retained copy is used as its disocclusion fill instead.
            const bool shift = dibrRuns;
            if (!shift) {
                retain_eye_for_warp(eye);
                warp_stale_eye(eye ^ 1u);
            } else if (!warped && !monoScreen) {
                // DIBR shift declined this frame -- fall back to plain AER so
                // the partner eye is at least brought to the current pose
                // instead of being resubmitted stale. Retain already ran above.
                //
                // SEED FIRST if that eye has nothing retained. warp_stale_eye()
                // declines when prevEyeValid is false, which stays false for as
                // long as the eye has never been the RENDERED one -- and if the
                // parity stops alternating, that is forever. Its swapchain image
                // then keeps the black it was created with, every frame, which
                // is the whole-eye black a frame dump shows.
                //
                // Seeding gives the warp something to work from, so the eye is
                // reconstructed by the same rotation warp as any other stale eye
                // rather than being handed a flat copy. The seed is one frame off
                // by an IPD, which the very next real render corrects.
                if (!g.prevEyeValid[warpedDstEye]) {
                    retain_eye_for_warp(warpedDstEye);
                    static std::atomic<uint32_t> seeds{0};
                    const uint32_t n = seeds.fetch_add(1) + 1;
                    if (n == 1 || (n % 60) == 0)
                        VRLOG("DIBR shift: eye %u had nothing retained -- seeded it for the "
                              "stale-eye warp (%u so far; a rising count means the "
                              "AER parity is not alternating)", warpedDstEye, n);
                }
                warp_stale_eye(warpedDstEye);
            }

            // One line per dumped frame, keyed to the image it describes. Read
            // it next to fN_LR.jpg: the frame that shows no DIBR shift names
            // its own cause, rather than being inferred from pixels afterwards.
            // Guarded by active(), so this is silent in normal play.
            if (framedump::active()) {
                // Depth CONTENT, not just its label. Costs a stall, which is
                // why it is inside the dump guard.
                hooks::DepthDiag dd;
                if (dbgHaveWp) hooks::scene_depth_diag(g.ctx.Get(), dd);
                // The colour the warp was actually shifting, so the two sizes
                // can be compared directly rather than assumed to agree.
                unsigned cw = 0, ch = 0;
                if (blitSrc) {
                    D3D11_TEXTURE2D_DESC cd{};
                    blitSrc->GetDesc(&cd);
                    cw = cd.Width; ch = cd.Height;
                }

                VRLOG("DUMP f%-2d eye=%u -> %s | mono=%d sinceReal=%d,%d "
                      "depthEye=%d aeEye=%d aeOff=%+.4f warpedDst=%d "
                      "warpGpu=%.2fms(avg %.2f) 6dof=%s/%.2fms",
                      framedump::frame_index(), eye, dibrWhy,
                      (int)monoScreen, s_sinceReal[0], s_sinceReal[1],
                      dbgDepthEye, dbgAeEye, dbgAeOff,
                      warped ? (int)warpedDstEye : -1,
                      dibr::gpu_ms(), dibr::gpu_avg_ms(),
                      g_stale6dofWhy, reproject::gpu_ms());
                if (dbgHaveWp) {
                    // maxDisp is what the whole chain is FOR: if it is large
                    // and the image did not move, the failure is downstream of
                    // the maths; if it is ~0, one of the inputs on this line
                    // explains it directly.
                    const float zNear = (dbgWp.projB != 0.0f && dd.maxWarp > 0.0f)
                                        ? dbgWp.projB / (dd.maxWarp - dbgWp.projA) : -1.0f;
                    const float maxDisp = (zNear > 0.0f)
                                        ? dbgWp.ipd * dbgWp.focalPx / zNear : -1.0f;
                    VRLOG("     f%-2d warp: ipd=%.4f focalPx=%.1f eyeSign=%+.0f "
                          "projA=%.5f projB=%.5f fill=%d | depth %ux%u vs colour %ux%u "
                          "-> zNear=%.2fm maxDisp=%.1fpx",
                          framedump::frame_index(), dbgWp.ipd, dbgWp.focalPx,
                          dbgWp.eyeSign, dbgWp.projA, dbgWp.projB, dbgWp.fillMode,
                          dd.width, dd.height, cw, ch, zNear, maxDisp);
                    // The three-way discriminator -- see DepthDiag in
                    // depth_probe.h for how to read it.
                    VRLOG("     f%-2d depth: slot%d cov=%.3f max=%.5f caps=%d | "
                          "other cov=%.3f max=%.5f caps=%d | passes=%d deferredDropped=%d "
                          "| restarts first=%d eye=%d cam=%d (lastMove=%.4fm)",
                          framedump::frame_index(), dd.warpSlot,
                          dd.covWarp, dd.maxWarp, dd.capsWarp,
                          dd.covOther, dd.maxOther, dd.capsOther,
                          dd.framePasses, dd.deferred,
                          dd.rsFirst, dd.rsEye, dd.rsCam, dd.lastCamMove);
                }
            }

            framedump::end_frame();

            // Desktop mirror: save the chosen eye's backbuffer; restore it on the other.
            if (g.desktopSave) {
                if ((int)eye == g_desktopEye.load()) g.ctx->CopyResource(g.desktopSave.Get(), backbuffer.Get());
                else                                 g.ctx->CopyResource(backbuffer.Get(), g.desktopSave.Get());
            }
        }
    }

    // Build projection views and submit. The stale eye reuses its last released
    // swapchain image automatically -- no acquire/release needed on our side.
    XrCompositionLayerProjectionView pv[2]{};
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    bool haveLayer = g.eyeEverRendered[0] && g.eyeEverRendered[1];
    if (haveLayer) {
        for (int e = 0; e < 2; ++e) {
            XrRect2Di rect = g.blitRect[e];
            if (rect.extent.width <= 0 || rect.extent.height <= 0)
                rect = {{0, 0}, {(int32_t)g.width, (int32_t)g.height}};
            float hh, vh;
            eye_frustum_half_angles((uint32_t)e, hh, vh);
            pv[e].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            // RENDER-TIME POSE AUTHORITY, for an DIBR shift pair only.
            //
            // The pixels in both eyes were produced from the camera the hook
            // baked mid-frame; g.lastView is a LATER locate that no frame was
            // rendered from. Declaring it leaves the compositor's reprojection
            // correcting toward the wrong reference -- an error common to both
            // eyes, so it does not break stereo, it adds swim proportional to
            // render latency.
            //
            // BOTH eyes at the render instant. AER's two eyes do come from
            // different frames, but the stale one is warped to this frame's
            // render instant (warp_homography_for) rather than to the current
            // locate, so by here they share one reference and a single pose
            // describes when each was made. Declaring one eye's build pose
            // while the other kept the current locate is what split the eyes
            // apart on 2026-08-20; see g_renderView.
            pv[e].pose = reference_view_pose((uint32_t)e);
            // Synched Sequential: the views the pair in the swapchains was
            // rendered from -- one head pose for both eyes.
            if (ss && g.ssCommitted) pv[e].pose = g.ssCommittedView[e].pose;
            if (dibrPairThisFrame) {
                AcquireSRWLockShared(&g_renderViewLock);
                const bool haveRv = g_renderViewValid || g_onScreenViewsValid;
                ReleaseSRWLockShared(&g_renderViewLock);
                const XrPosef rendered = reference_view_pose(dibrRealEye);
                const XrPosef other    = reference_view_pose(dibrRealEye ^ 1u);
                if (haveRv && (uint32_t)e == dibrRealEye) {
                    pv[e].pose = rendered;
                } else if (haveRv) {
                    // The synthesized eye sits at the separation the WARP used,
                    // along the captured inter-pupillary axis. Declaring the
                    // runtime's own eye position instead would describe a
                    // baseline the image was never built with, whenever the
                    // camera hook scaled the offset.
                    //
                    // Less the WORLD SCALE, which is not such a scaling: it is a
                    // world drawn bigger or smaller around the real eyes, and AER
                    // declares both of its eyes at their real poses whatever it
                    // is set to. Rebasing at the separation it rendered would put
                    // this eye a scaled IPD from the other and have the
                    // compositor correct the scale away in this eye alone.
                    const auto to_policy = [](const XrPosef& p) {
                        dibrpolicy::EyePose o;
                        o.orientation[0] = p.orientation.x;
                        o.orientation[1] = p.orientation.y;
                        o.orientation[2] = p.orientation.z;
                        o.orientation[3] = p.orientation.w;
                        o.position[0] = p.position.x;
                        o.position[1] = p.position.y;
                        o.position[2] = p.position.z;
                        return o;
                    };
                    dibrpolicy::EyePose synth;
                    if (dibrpolicy::rebase_synth_eye(to_policy(rendered),
                                                    to_policy(other),
                                                    dibrSeparationM * g_worldScale.load(),
                                                    synth)) {
                        pv[e].pose.orientation = {synth.orientation[0],
                                                  synth.orientation[1],
                                                  synth.orientation[2],
                                                  synth.orientation[3]};
                        pv[e].pose.position = {synth.position[0],
                                               synth.position[1],
                                               synth.position[2]};
                    }
                    // On failure the runtime pose stands: a slightly wrong
                    // declared baseline beats an invented one.
                }
            }
            {
                // WHAT THE CAMERA WAS AIMED AT: the vertical tilt and the
                // SYNTHETIC cant, the same two rotations head_look() gave it
                // (rendered_orientation()). A real hardware cant is already
                // baked into g.lastView[e].pose above -- adding it here too
                // would double it. The debug cant has no such source, so it has
                // to be declared here as well as rendered, otherwise the slider
                // would just tilt each eye's render with nothing to undo it (a
                // broken-stereo test, not a simulation of a canted headset).
                const Quat nq = rendered_orientation(pv[e].pose.orientation, e);
                pv[e].pose.orientation.x = nq.x; pv[e].pose.orientation.y = nq.y;
                pv[e].pose.orientation.z = nq.z; pv[e].pose.orientation.w = nq.w;
            }
            pv[e].fov.angleLeft  = -hh;  pv[e].fov.angleRight = hh;
            pv[e].fov.angleUp    =  vh;  pv[e].fov.angleDown  = -vh;

            // OFF-CENTRE: the render already IS this eye's own frustum, so it
            // is declared verbatim over the whole image and the asymmetry crop
            // below has nothing left to do -- there is no surplus to trim.
            //
            // Both branches read render_eye_frustum_tan(), which is also what
            // PROJ LOCK built the matrix from, so the declaration cannot drift
            // from the render.
            bool declaredOffcenter = false;
            if (offcenter_projection()) {
                float tl, tr, tup, tdn;
                if (render_eye_frustum_tan((int)e, tl, tr, tup, tdn)) {
                    pv[e].fov.angleLeft  = std::atan(tl);
                    pv[e].fov.angleRight = std::atan(tr);
                    pv[e].fov.angleUp    = std::atan(tup);
                    pv[e].fov.angleDown  = std::atan(tdn);
                    declaredOffcenter = true;
                    static std::atomic<bool> loggedOff[2]{false, false};
                    if (!loggedOff[e].exchange(true))
                        VRLOG("eye%d off-centre projection: rendered and declared "
                              "[%.2f, %.2f] x [%.2f, %.2f] deg over the full image",
                              (int)e, pv[e].fov.angleLeft * 57.2957795f,
                              pv[e].fov.angleRight * 57.2957795f,
                              pv[e].fov.angleDown * 57.2957795f,
                              pv[e].fov.angleUp * 57.2957795f);
                }
            }

            // FOV asymmetry / off-center projection alignment:
            // If the runtime reports an asymmetric FOV (e.g. Pimax with parallel projections or canted panels),
            // the rendered image is symmetric covering [-baseH, +baseH]. We crop the swapchain subImage rect
            // and declare the exact per-eye asymmetric FOV so that the optical center (0 deg) lines up with the lens.
            float aspect, baseH;
            eye_frustum_base((uint32_t)e, aspect, baseH);
            const float tanBase = std::tan(baseH);
            if (tanBase > 1.0e-4f && fov_asymmetry_align() && !declaredOffcenter) {
                const float rawL = g.lastView[e].fov.angleLeft;
                const float rawR = g.lastView[e].fov.angleRight;
                if (std::fabs(std::fabs(rawL) - std::fabs(rawR)) > 0.01f) {
                    const float tanL = std::tan(rawL);
                    const float tanR = std::tan(rawR);
                    float u0 = (tanL + tanBase) / (2.0f * tanBase);
                    float u1 = (tanR + tanBase) / (2.0f * tanBase);
                    u0 = (std::max)(0.0f, (std::min)(1.0f, u0));
                    u1 = (std::max)(0.0f, (std::min)(1.0f, u1));
                    if (u1 > u0 + 0.05f) {
                        const int32_t origX = rect.offset.x;
                        const int32_t origW = rect.extent.width;
                        rect.offset.x = origX + (int32_t)std::round((float)origW * u0);
                        rect.extent.width = (int32_t)std::round((float)origW * (u1 - u0));

                        const float scale = composited_fov_scale();
                        pv[e].fov.angleLeft  = std::atan(tanL * scale);
                        pv[e].fov.angleRight = std::atan(tanR * scale);

                        static std::atomic<bool> loggedAsym[2]{false, false};
                        if (!loggedAsym[e].exchange(true)) {
                            VRLOG("eye%d FOV asymmetry aligned: raw [%.2f, %.2f] deg -> subImage rect [%d..%d] (width %d of %d), fov [%.2f, %.2f] deg",
                                  e, rawL * 57.2957795f, rawR * 57.2957795f,
                                  rect.offset.x, rect.offset.x + rect.extent.width,
                                  rect.extent.width, origW,
                                  pv[e].fov.angleLeft * 57.2957795f, pv[e].fov.angleRight * 57.2957795f);
                        }
                    }
                }
            }

            pv[e].subImage.swapchain = g.swapchain[e];
            pv[e].subImage.imageRect = rect;
            pv[e].subImage.imageArrayIndex = 0;
        }
        proj.space = g.space;
        proj.viewCount = 2;
        proj.views = pv;
        // The vertical extent both eyes actually show, for the settings
        // panel's height limit (build_menu_quad_layer).
        g_submittedHalfV.store((std::min)((std::min)(pv[0].fov.angleUp, -pv[0].fov.angleDown),
                                          (std::min)(pv[1].fov.angleUp, -pv[1].fov.angleDown)));
        // And the horizontal, for its width limit: the narrower side of each
        // eye, rendered AND on the panel -- a head-locked quad is drawn by the
        // compositor, so the panel's own edge (the nose side) bounds it even
        // where the render reaches further.
        {
            float halfH = 10.0f;
            for (int e = 0; e < 2; ++e) {
                halfH = (std::min)(halfH, (std::min)(pv[e].fov.angleRight, -pv[e].fov.angleLeft));
                const XrFovf& raw = g.lastView[e].fov;
                if (raw.angleRight - raw.angleLeft > 0.01f)
                    halfH = (std::min)(halfH, (std::min)(raw.angleRight, -raw.angleLeft));
            }
            g_submittedHalfH.store(halfH < 10.0f ? halfH : 0.0f);
        }
    }

    // Only submit the eye projection layer when the runtime is actually
    // asking us to render this frame (fs.shouldRender), and never off a
    // pose/view locate that may have used the VDXR bogus-time sentinel
    // (validTime) -- matches the documented OpenXR pattern of zero layers
    // when shouldRender is false (e.g. headset not being worn, system UI has
    // focus), rather than always resubmitting the last-good layer regardless.
    const bool submitLayer = haveLayer && fs.shouldRender && validTime;

    // Menu overlay: a second, independent quad layer, built (and its own
    // small XR swapchain rendered into) only while the mod settings menu is
    // open -- see build_menu_quad_layer(). Submitted regardless of
    // submitLayer/validTime: it's headset UI, not scene content, so it
    // doesn't need a fresh eye pose/view locate to be meaningful.
    XrCompositionLayerQuad menuQuad{};
    const bool haveMenuQuad = build_menu_quad_layer(menuQuad);

    // ONE QUAD PER FRAME on SteamVR under Proton (see running_under_wine()).
    // What gives way, in order: the UI plane, then a 2D screen's panel -- never
    // the mod menu, which has to stay usable. With the menu closed a 2D panel
    // keeps the quad and the UI rides inside its image instead (the redirect
    // stands down, see ui_stays_in_frame()); with the menu open the panel steps
    // aside for the eye projection, the way the UI plane already does. On
    // Windows oneQuad is false and the list below is exactly what it was.
    const bool oneQuad = single_quad_runtime();

    // The game's own UI, on its own plane. UNDER the mod menu and OVER the eye
    // projection -- layer order is submission order, and the settings panel has
    // to stay readable on top of whatever the game is showing.
    //
    // Gated on validTime: this renders a full-canvas copy into an XR image, and
    // doing that for a layer that will not be submitted is pure cost.
    XrCompositionLayerQuad uiQuad{};
    const bool haveUiQuad = validTime &&
                            !(oneQuad && (haveMenuQuad || flat_screen_active())) &&
                            build_ui_quad_layer(uiQuad, fs.predictedDisplayTime);

    // 2D SCREEN MODE: the scene on a plane, INSTEAD of the eye projection. Not
    // as well as -- submitting both would leave the stereo world behind the
    // panel, which is neither of the two things the setting offers.
    XrCompositionLayerQuad sceneQuad{};
    const bool haveSceneQuad = submitLayer && !(oneQuad && haveMenuQuad) &&
                               build_scene_quad_layer(sceneQuad, fs.predictedDisplayTime);

    const XrCompositionLayerBaseHeader* layers[4];
    uint32_t layerCount = 0;
    if (submitLayer && !haveSceneQuad)
        layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&proj);
    if (haveSceneQuad)
        layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&sceneQuad);
    if (haveUiQuad)
        layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&uiQuad);
    if (haveMenuQuad)
        layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&menuQuad);

    XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
    fei.displayTime = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = layerCount;
    fei.layers = layerCount ? layers : nullptr;
    // A black headset with a working desktop means either no layer is being
    // submitted or the runtime is rejecting it. Rejections are logged by
    // end_frame() -- from the worker's result a frame later under Wine. (The
    // every-2s "XR submit: ..." heartbeat that lived here was removed
    // 2026-08-09: steady-state noise that buried the lines that matter.)
    end_frame(g.session, fei, (uint32_t)haveSceneQuad + haveUiQuad + haveMenuQuad);

    // Reset fresh flags and signal the next camera build to advance AER parity.
    g.eyeFresh[0] = g.eyeFresh[1] = false;
    g_newFrame.store(true);
}


} // namespace

HeadLook head_look()
{
    // NO HEAD INPUT ON A FLAT SCREEN. The panel is a fixed window onto the
    // game, so the game's camera has to be the game's own: leave head tracking
    // in and moving your head would pan the world while the panel stayed put,
    // which is the one thing a screen must never do.
    //
    // Reported as "no head pose" rather than as a zeroed one, because that is
    // what it is, and because all five injection sites in viewbuild_hook.cpp
    // already branch on it -- it is the same state they sit in before the
    // headset comes up, so this needs no new path anywhere.
    //
    // THE PANEL ITSELF STILL FOLLOWS YOUR HEAD, and that is a different
    // mechanism: place_ui_plane_quad() puts the quad head-locked or world-
    // pinned per the UI setting, which the compositor honours whatever the
    // game's camera is doing. Head-locked, the screen comes with you; pinned,
    // you can look away from it. Neither moves the view inside it.
    if (flat_screen_active()) return HeadLook{};

    AcquireSRWLockShared(&g_poseLock);
    HeadLook h = g_headLook;
    Quat rel{g_relQuat[0], g_relQuat[1], g_relQuat[2], g_relQuat[3]};
    ReleaseSRWLockShared(&g_poseLock);

    // THE WORLD SCALE divides the head's travel here, as advance_aer_parity()
    // divides the eye separation by it -- both are how you judge the world's
    // size. Drawn twice the size, a 10 cm lean has to move the camera 5 cm, or
    // moving your head says life size while the stereo says double. Turning
    // stays one to one: an angle is the same at any size.
    const float scale = g_worldScale.load();
    h.ox /= scale; h.oy /= scale; h.oz /= scale;

    // Per-eye display canting. This is the single choke point every camera
    // consumer already shares (camera_hook's cockpit path, viewbuild_hook's
    // view post-rotation and detour_fna), so folding the cant in here gets it
    // into all of them with their existing g_yawSign/g_pitchSign/g_rollSign
    // conventions untouched -- rather than adding a per-eye term to five call
    // sites that each apply those signs differently.
    //
    // POST-multiply: the cant is expressed in the eye's own (head-local)
    // frame, so it composes on the right of the head rotation, exactly as
    // vrframework's `hmd * eye` ordering does (guide 09 §2).
    //
    // Bit-identical no-op on a parallel-panel headset with the debug slider at
    // zero, which is the overwhelmingly common case -- the early-out below is
    // what guarantees that, not just "the numbers work out the same".
    //
    // THE VERTICAL TILT goes on after the cant, through the same choke point
    // and for the same reason: the submission declares every eye tilted by
    // submitted_vertical_pitch() (rendered_orientation()), so the camera has to
    // be tilted by it too, or the world is displayed rotated by it. It is not
    // a cant (one value for both eyes) but composes the same way, about the
    // eye's own right axis. Zero only on a headset whose display is centred on
    // its optical axis -- on most, the early-out no longer applies.
    if (!h.valid) return h;
    const Quat cant = total_cant(g_frameEye.load() & 1);
    const Quat tilt = vertical_tilt();
    if (cant_is_identity(cant) && cant_is_identity(tilt)) return h;
    euler_from_rel(mul(mul(rel, cant), tilt), h.yaw, h.pitch, h.roll);
    return h;
}

// CONCLUSION (2026-07-25): forcing the backbuffer to 5000x5000 -- confirmed via
// log to have reached every stage (CreateSwapChain, the XR swapchain, the
// staging texture) -- produced NO visible detail improvement in-headset. The
// engine's real rendering detail is generated at a fixed resolution we have
// not located; backbuffer/viewport size beyond that is pure upscaling. The
// alternate RT-capture candidates are all unrelated buffers (mip-chain
// downsamples, a motion-vector buffer, fixed-size unrelated assets) -- no
// hidden higher-detail source exists to capture instead. Raising resolution
// further needs a genuine RenderDoc/IDA session; disabled (g_forceRenderSize
// defaults false) rather than waste GPU memory inflating for no benefit. The
// viewport-field pin that went with it (kViewportW/H in viewbuild_hook.cpp)
// was removed 2026-10-26: it only ever wrote while this was on, and it wrote to
// a remembered data address that a game update could repurpose.

// TEST MODE (2026-07-25): fixed target, bypassing the OpenXR-computed edge and
// the aspect-preserving scale math entirely -- probing empirically what the
// game/monitor will actually allow, one concrete W,H at a time, rather than
// theorizing further. Change and rebuild to try different values. Currently:
// 1350x1080 (5:4), the tightest-to-square shape that stays within the 1080
// monitor-height ceiling we've seen hold firm across every prior test.
constexpr uint32_t kTestW = 1350, kTestH = 1080;

bool desired_render_size_for(uint32_t /*origW*/, uint32_t /*origH*/, uint32_t& outW, uint32_t& outH)
{
    if (!g_forceRenderSize.load()) return false;
    outW = kTestW;
    outH = kTestH;
    return true;
}

// Cached after the first success: the settings UI polls this every frame the
// menu is open, and it is an XR round trip (two xrEnumerateViewConfigurationViews
// calls plus a vector allocation), not a field read. The recommended size never
// changes for a given runtime+headset within a session.
bool native_eye_size(uint32_t& w, uint32_t& h)
{
    static std::atomic<uint32_t> s_w{0}, s_h{0};
    uint32_t cw = s_w.load(), ch = s_h.load();
    if (cw && ch) { w = cw; h = ch; return true; }
    if (!recommended_eye_size(cw, ch)) return false;
    s_w.store(cw);
    s_h.store(ch);
    w = cw;
    h = ch;
    return true;
}

bool render_canvas_wh(uint32_t& w, uint32_t& h)
{
    // The game's ACTUAL backbuffer size (from the swapchain desc at init),
    // whatever chose it. g.width/g.height are the eye images, which differ
    // from it while DLSS upscales.
    w = g.bbWidth; h = g.bbHeight;
    return w > 0 && h > 0;
}

bool upscale_canvas(uint32_t& w, uint32_t& h)
{
    if (!g.bbWidth || (g.width == g.bbWidth && g.height == g.bbHeight)) return false;
    w = g.width; h = g.height;
    return true;
}

// ZERO IN 2D, and this is not optional. A flat panel is one image shown to both
// eyes, but AER still alternates the camera by +/-IPD/2 every frame -- so the
// panel would shake left and right at half the frame rate. There is no stereo
// to keep on a mono quad, so the offset simply stops being applied.
float eye_side_offset() { return flat_screen_active() ? 0.0f : g_eyeSide.load(); }

float world_scale() { return g_worldScale.load(); }

// Takes effect at the next frame's eye choice (advance_aer_parity), the moment
// a new eye offset is set anyway.
void set_world_scale(float scale)
{
    float s = std::isfinite(scale) ? scale : 1.0f;
    s = (std::max)(0.5f, (std::min)(2.0f, s));
    g_worldScale.store(s);
}


std::atomic<uint64_t> g_appliedEye{0};
// Bumped on every record, and SNAPSHOTTED at each AER parity flip. The record
// is fresh exactly when the two differ -- i.e. when a camera build has baked an
// eye into the frame we are currently assembling. Cheaper and more direct than
// a frame stamp, and it needs no bits in the packed value.
std::atomic<uint32_t> g_eyeRecordSerial{0};
std::atomic<uint32_t> g_eyeSerialAtFlip{0};

// Packed as one 64-bit value so the index and the offset are always read from
// the same frame: bit 63 = valid, bit 34 = how, bits 32..33 = eye,
// bits 0..31 = float bits.
void record_applied_eye_offset(float off, int how)
{
    uint32_t bits;
    std::memcpy(&bits, &off, sizeof(bits));
    const uint64_t eye = (uint64_t)(g_frameEye.load() & 1);
    const uint64_t hw  = (uint64_t)((how & kEyeAppliedAsProjShift) ? 1 : 0);
    const uint64_t ms  = (uint64_t)((how & kEyeAppliedMapSign) ? 1 : 0);
    g_eyeRecordSerial.fetch_add(1);
    g_appliedEye.store((1ull << 63) | (ms << 35) | (hw << 34) |
                       (eye << 32) | (uint64_t)bits);

    // SNAPSHOT THE VIEWS THIS RENDER IS BEING BUILT FROM.
    //
    // The projection layer has to describe the geometry the pixels were
    // produced with, and that geometry is fixed HERE -- mid-frame, when the
    // camera hook bakes the eye -- not at Present, where the runtime's freshly
    // located views describe a slightly later head pose the frame was never
    // rendered from. Submitting those instead leaves the compositor correcting
    // toward the wrong reference, which reads as swim proportional to render
    // latency.
    //
    // Cheap: two poses under an exclusive lock, a handful of times per frame.
    //
    // Under Synched Sequential, the views latched for the moment being
    // rendered: both renders of it are built from those, and g.lastView may
    // already be a later locate by the second.
    XrView snap[2];
    AcquireSRWLockExclusive(&g_renderViewLock);
    for (int e = 0; e < 2; ++e)
        snap[e] = g_renderView[e] = g_ssViewsValid ? g_ssViews[e] : g.lastView[e];
    g_renderViewValid = true;
    ReleaseSRWLockExclusive(&g_renderViewLock);
    {
        const uint32_t b = (uint32_t)g_advFlips.load();
        AcquireSRWLockExclusive(&g_buildRecLock);
        BuildRec& r = g_buildRec[b % kBuildRing];
        if (r.build == b) { r.views[0] = snap[0]; r.views[1] = snap[1]; r.haveViews = true; }
        ReleaseSRWLockExclusive(&g_buildRecLock);
    }
}

AppliedEye applied_eye()
{
    const uint64_t v = g_appliedEye.load();
    AppliedEye out;
    if (!(v & (1ull << 63))) return out;
    out.valid = true;
    out.eye   = (int)((v >> 32) & 1);
    out.how   = (v & (1ull << 34)) ? kEyeAppliedAsProjShift : kEyeAppliedAsCameraMove;
    out.mapSign = (v & (1ull << 35)) != 0;
    out.fresh = g_eyeRecordSerial.load() != g_eyeSerialAtFlip.load();
    const uint32_t bits = (uint32_t)(v & 0xFFFFFFFFull);
    std::memcpy(&out.offset, &bits, sizeof(out.offset));
    return out;
}

bool synched_sequential_active() { return synched_active(); }

bool xr_session_running() { return !g_failed && g.running; }

bool render_in_progress(RenderInfo& out)
{
    // The render's own camera, as its Present will read it (update_on_screen),
    // else the newest build: in the middle of the frame that IS the render's.
    // NOT the newest build minus the Present's lag -- on the map, garage and
    // menus the next camera is built AFTER the scene, so from here the image
    // is not a build behind yet, and subtracting the lag named the build
    // before. Measured 2026-10-28 with DLSS: under Synched Sequential half the
    // renders' eyes disagreed with their Present's on exactly those screens,
    // and each eye's DLSS history took in the other eye's frames.
    const uint32_t newest = (uint32_t)g_advFlips.load();
    const uint32_t fromRender = render_camera_build(newest);
    const uint32_t b = fromRender ? fromRender : newest;
    BuildRec r;
    AcquireSRWLockShared(&g_buildRecLock);
    r = g_buildRec[b % kBuildRing];
    ReleaseSRWLockShared(&g_buildRecLock);
    if (r.build != b || b == 0 || r.eye < 0) return false;
    out.build   = b;
    out.eye     = r.eye & 1;
    out.synched = (r.ss >> 63) != 0 && synched_active();
    out.moment  = out.synched ? (uint32_t)r.ss : r.build;
    return true;
}

bool ss_build_info(SsBuildInfo& out)
{
    if (!synched_active()) return false;
    const uint64_t f = g_ssFrame.load();
    if (!(f >> 63)) return false;
    out.build  = (uint32_t)g_advFlips.load();
    out.moment = (uint32_t)f;
    out.first  = ((f >> 62) & 1u) != 0;
    return true;
}

void note_built_camera(const float fwd[3], bool previousBuildScreen, const float eye[3],
                       int builder, const float eyeOffset[3])
{
    const uint32_t b = (uint32_t)g_advFlips.load();
    AcquireSRWLockExclusive(&g_buildRecLock);
    BuildRec& r = g_buildRec[b % kBuildRing];
    if (r.build == b) {
        r.fwd[0] = fwd[0]; r.fwd[1] = fwd[1]; r.fwd[2] = fwd[2];
        r.haveFwd = true;
        r.prevScreen = previousBuildScreen;
        if (eye) {
            r.eyePos[0] = eye[0]; r.eyePos[1] = eye[1]; r.eyePos[2] = eye[2];
            r.haveEyePos = true;
        }
        if (eyeOffset) {
            r.eyeOff[0] = eyeOffset[0]; r.eyeOff[1] = eyeOffset[1]; r.eyeOff[2] = eyeOffset[2];
            r.haveEyeOff = true;
        }
        r.builder = builder;
    }
    ReleaseSRWLockExclusive(&g_buildRecLock);
}

bool match_built_camera(const float eye[3], const float fwd[3], float tolM,
                        uint32_t& build, float& distM)
{
    const float fl = std::sqrt(fwd[0]*fwd[0] + fwd[1]*fwd[1] + fwd[2]*fwd[2]);
    if (!(fl > 1.0e-6f)) return false;
    constexpr float kCos2Deg = 0.99939f;
    const uint32_t newest = (uint32_t)g_advFlips.load();
    bool found = false;
    AcquireSRWLockShared(&g_buildRecLock);
    // Newest first, replaced only by a STRICTLY nearer one: among equals --
    // a still head on a still screen builds the same camera twice -- the
    // newest that existed at upload wins, which is the one the render took.
    for (uint32_t k = 0; k < kBuildRing && k < newest; ++k) {
        const uint32_t b = newest - k;
        const BuildRec& r = g_buildRec[b % kBuildRing];
        if (r.build != b || !r.haveEyePos || !r.haveFwd) continue;
        const float dx = eye[0] - r.eyePos[0], dy = eye[1] - r.eyePos[1],
                    dz = eye[2] - r.eyePos[2];
        const float d = std::sqrt(dx*dx + dy*dy + dz*dz);
        if (!(d <= tolM)) continue;
        // Either sign: the view's forward row and the direction handed over
        // need not share a convention.
        const float c = std::fabs(fwd[0]*r.fwd[0] + fwd[1]*r.fwd[1] + fwd[2]*r.fwd[2]) / fl;
        if (c < kCos2Deg) continue;
        if (!found || d < distM - 1.0e-4f) { found = true; build = b; distM = d; }
    }
    ReleaseSRWLockShared(&g_buildRecLock);
    return found;
}

// How far a camera may sit from every recorded build and still be taken for the
// nearest. The same allowance cbuffer_hook.cpp gives a camera that is only
// NEAR a build: a camera that is a build's own sits on it to the millimetre, and
// the nearest of two builds is still the right eye at any tolerance, because
// the eye offset alone puts the two renders of a pair 6 cm apart.
constexpr float kUiCameraMatchTolM = 1.0f;

// THE PLANE, IN THE FRAME OF THE CAMERA A MARKER IS PROJECTED WITH.
//
// The camera is matched to the build it came from by its own position and
// direction, as the GPU's camera is in cbuffer_hook.cpp. That build's record
// holds the eye it was rendered for, both eye poses at that instant and the eye
// offset it applied, which is everything the plane's placement depends on. The
// plane is placed the way place_ui_plane_quad() last placed it -- pinned in the
// runtime's space, or hung on the capture eye and tilted with the render -- and
// only then expressed in the camera's frame. Turning OpenXR's axes (-Z forward)
// into the game's (+Z forward) is the last step, so everything before it is the
// placement's own arithmetic.
//
// FROM BETWEEN THE EYES, not from the eye the build was rendered for: the plane
// is measured from the middle of the two eye poses, and `origin` is that point
// in the game -- the build's own eye offset taken back off. Both eyes' builds
// then put every marker in the same place, so the UI of either eye's frame is
// the UI of both (marker_camera_placing()).
bool ui_plane_in_camera(const float eye[3], const float fwd[3], UiPlaneInCamera& out)
{
    if (g_failed || !g.running || flat_screen_active()) return false;
    const float w = g_uiQuadWidthM.load(), h = g_uiQuadHeightM.load();
    if (!(w > 1.0e-3f) || !(h > 1.0e-3f)) return false;

    uint32_t b = 0;
    float matchM = 0.0f;
    if (!match_built_camera(eye, fwd, kUiCameraMatchTolM, b, matchM)) return false;
    BuildRec r;
    AcquireSRWLockShared(&g_buildRecLock);
    r = g_buildRec[b % kBuildRing];
    ReleaseSRWLockShared(&g_buildRecLock);
    if (r.build != b || !r.haveViews || !r.haveEyeOff || r.eye < 0) return false;
    const int e = r.eye & 1;

    // The camera's axes: the orientation this build was rendered and declared
    // at. Its position: the middle of the two eyes.
    const XrView& cv = r.views[e];
    const Quat qc = rendered_orientation(cv.pose.orientation, e);
    const Vec3 pc{(r.views[0].pose.position.x + r.views[1].pose.position.x) * 0.5f,
                  (r.views[0].pose.position.y + r.views[1].pose.position.y) * 0.5f,
                  (r.views[0].pose.position.z + r.views[1].pose.position.z) * 0.5f};

    // The quad's centre and orientation, in the same space.
    Quat qq{0.0f, 0.0f, 0.0f, 1.0f};
    Vec3 pq{0.0f, 0.0f, 0.0f};
    bool pinned = false;
    if (g_uiQuadPinned.load()) {
        AcquireSRWLockShared(&g_uiPinLock);
        if (g_uiPinValid) {
            qq = {g_uiPinPose.orientation.x, g_uiPinPose.orientation.y,
                  g_uiPinPose.orientation.z, g_uiPinPose.orientation.w};
            pq = {g_uiPinPose.position.x, g_uiPinPose.position.y, g_uiPinPose.position.z};
            pinned = true;
        }
        ReleaseSRWLockShared(&g_uiPinLock);
    }
    if (!pinned) {
        const XrView& ce = r.views[kUiCaptureEye];
        qq = mul(Quat{ce.pose.orientation.x, ce.pose.orientation.y,
                      ce.pose.orientation.z, ce.pose.orientation.w}, vertical_tilt());
        pq = {ce.pose.position.x, ce.pose.position.y, ce.pose.position.z};
    }
    const Vec3 off = rotate(qq, {0.0f, 0.0f, -g_uiPlaneDistM.load()});
    pq = {pq.x + off.x, pq.y + off.y, pq.z + off.z};

    const Quat ic  = conj(qc);
    const Quat rel = mul(ic, qq);
    const Vec3 c  = rotate(ic, {pq.x - pc.x, pq.y - pc.y, pq.z - pc.z});
    const Vec3 ax = rotate(rel, {1.0f, 0.0f, 0.0f});
    const Vec3 ay = rotate(rel, {0.0f, 1.0f, 0.0f});
    const Vec3 az = rotate(rel, {0.0f, 0.0f, -1.0f});   // into the quad, away from the eye
    out.center[0] = c.x;  out.center[1] = c.y;  out.center[2] = -c.z;
    out.right[0]  = ax.x; out.right[1]  = ax.y; out.right[2]  = -ax.z;
    out.up[0]     = ay.x; out.up[1]     = ay.y; out.up[2]     = -ay.z;
    out.normal[0] = az.x; out.normal[1] = az.y; out.normal[2] = -az.z;
    out.origin[0] = -r.eyeOff[0]; out.origin[1] = -r.eyeOff[1]; out.origin[2] = -r.eyeOff[2];
    out.width  = w;
    out.height = h;
    out.build  = b;
    out.eye    = e;
    out.matchM = matchM;
    out.pinned = pinned;
    return true;
}

uint32_t current_build() { return (uint32_t)g_advFlips.load(); }

int onscreen_camera_builder() { return g_onScreenBuilder.load(); }

bool have_built_camera()
{
    const uint32_t newest = (uint32_t)g_advFlips.load();
    bool any = false;
    AcquireSRWLockShared(&g_buildRecLock);
    for (uint32_t k = 0; k < kBuildRing && k < newest && !any; ++k) {
        const BuildRec& r = g_buildRec[(newest - k) % kBuildRing];
        any = r.build == newest - k && r.haveEyePos;
    }
    ReleaseSRWLockShared(&g_buildRecLock);
    return any;
}

int  render_method() { return g_renderMethod.load(); }

void set_render_method(int method)
{
    const int m = (method == kRenderSynchedSequential) ? kRenderSynchedSequential : kRenderAER;
    if (g_renderMethod.exchange(m) == m) return;
    g_ssReset.store(true);
    VRLOG("render method -> %s%s", m == kRenderSynchedSequential ? "Synched Sequential"
                                                                 : "Alternate Eye Rendering",
          (m == kRenderSynchedSequential && !hooks::frame_sync_ready())
              ? " (waiting for the frame callbacks)" : "");
}

bool synched_sequential_available() { return hooks::frame_sync_ready(); }

const char* render_method_key(int method)
{
    return method == kRenderSynchedSequential ? "SynchedSequential" : "AlternateEye";
}

bool render_method_from_key(const char* key, int& method)
{
    if (!key) return false;
    if (_stricmp(key, "SynchedSequential") == 0) { method = kRenderSynchedSequential; return true; }
    if (_stricmp(key, "AlternateEye") == 0)      { method = kRenderAER; return true; }
    return false;
}

bool dibr_shift_active() { return dibr_shift_runs() && !synched_active(); }
bool warp_active()       { return g_warpEnabled.load() && !synched_active(); }

bool warp_enabled() { return g_warpEnabled.load(); }
void set_warp_enabled(bool on)
{
    g_warpEnabled.store(on);
    VRLOG("AER stale-eye warp -> %s", on ? "ON" : "OFF (plain stale resubmit)");
}

const char* kWarpTypeName[kWarpTypeCount] = {
    "headset rotation only",
    "headset + game camera rotation",
    "headset + game camera 6-DoF",
    "headset + game camera 6-DoF + the game's motion vectors"
};
// Stable config-file tokens, deliberately separate from the names above so the
// log wording can change without invalidating a saved config.
const char* kWarpTypeKey[kWarpTypeCount] = { "Headset", "GameRotation", "Game6Dof",
                                             "MotionVectors" };

int warp_type() { return g_warpType.load(); }
bool warp_uses_game_rot() { return g_warpType.load() >= kWarpGameRot; }
bool warp_uses_6dof()     { return g_warpType.load() >= kWarp6Dof; }
bool warp_uses_motion()   { return g_warpType.load() >= kWarpMotion; }

void set_warp_type(int type)
{
    if (type < 0 || type >= kWarpTypeCount) {
        VRLOG("set_warp_type: %d out of range, defaulting to headset rotation", type);
        type = kWarpHeadset;
    }
    const int was = g_warpType.exchange(type);
    if (was == type) return;

    // The counters describe the PREVIOUS setting's run; clearing them here
    // makes every log line after a change belong to one source.
    g_warpCamUsed.store(0);
    g_warpCamStale.store(0);
    g_warpCamOrbit.store(0);
    g_warpCamDisagreeDeg.store(0.0f);

    // Retained depth is only copied while 6-DoF is selected, so the per-eye
    // records are stale the moment it is selected again and must not be
    // reprojected until each eye has been retained afresh. Dropping them on the
    // way DOWN as well releases the textures nothing is going to read.
    const bool sixWas = was  >= kWarp6Dof;
    const bool sixNow = type >= kWarp6Dof;
    if (sixWas != sixNow) {
        for (int e = 0; e < 2; ++e) g.prevEyeMatricesValid[e] = false;
        if (!sixNow) {
            for (int e = 0; e < 2; ++e) {
                g.prevEyeDepthSrv[e].Reset();
                g.prevEyeDepth[e].Reset();
                g.prevEyeDepthW[e] = g.prevEyeDepthH[e] = 0;
                g.prevEyeRigidSrv[e].Reset();
                g.prevEyeRigid[e].Reset();
                g.prevEyeRigidW[e] = g.prevEyeRigidH[e] = 0;
                g.prevEyeRigidValid[e] = false;
            }
            reproject::release();
        }
    }
    VRLOG("stale-eye warp type -> %s", kWarpTypeName[type]);
}

const char* warp_type_key(int type)
{
    return (type >= 0 && type < kWarpTypeCount) ? kWarpTypeKey[type] : nullptr;
}

bool warp_type_from_key(const char* key, int& outType)
{
    if (!key) return false;
    for (int i = 0; i < kWarpTypeCount; ++i)
        if (_stricmp(key, kWarpTypeKey[i]) == 0) { outType = i; return true; }
    return false;
}

void warp_source_stats(WarpSourceStats& out)
{
    out.used            = g_warpCamUsed.load();
    out.staleSnapshot   = g_warpCamStale.load();
    out.orbitDeclined    = g_warpCamOrbit.load();
    out.worstDisagreeDeg = g_warpCamDisagreeDeg.load();
}

bool dibr_shift_enabled() { return g_dibrShift.load(); }

void set_dibr_shift_enabled(bool on)
{
    if (g_dibrShift.exchange(on) == on) return;
    // Nothing here leaves a BeginFrame open across Presents, so there is no
    // in-flight XR frame to close -- just clear the fresh flags so one recorded
    // under the old setting cannot linger into the new one.
    g.eyeFresh[0] = g.eyeFresh[1] = false;
    VRLOG("DIBR shift -> %s", on ? "ON (depth-reproject the rendered eye into the other)"
                                 : "OFF (the un-rendered eye is the stale eye alone)");
}

float render_fov_scale() { return g_renderFovScale.load(); }
void set_render_fov_scale(float s)
{
    if (s < 0.30f) s = 0.30f;
    if (s > 1.00f) s = 1.00f;
    g_renderFovScale.store(s);
    VRLOG("render FOV scale -> %.2f (%.1f deg of %.1f deg headset)", s,
          headset_hfov_deg() * s, headset_hfov_deg());
}

float map_ui_scale() { return g_mapUiScale.load(); }

void set_map_ui_scale(float v)
{
    g_mapUiScale.store((std::max)(0.2f, (std::min)(1.0f, v)));
}

float vertical_recenter() { return g_vertOffset.load(); }

float fov_center_pitch() { return fov_center_pitch_cached(); }

void set_vertical_recenter(float radians)
{
    g_vertOffset.store((std::max)(-0.5f, (std::min)(0.5f, radians)));
}

// THE SINGLE GATE on taking the UI out of the render. ui_layer follows this
// answer exactly, and every one of these terms is a reason the plane could not
// be shown -- so a false here always means "draw the UI into the frame the way
// you always did", never "no UI".
bool ui_plane_wanted()
{
    if (g_failed || !g.running) return false;          // no session to submit a layer to
    if (g_uiPlaneFailed.load()) return false;          // its swapchain would not create
    if (!g.uiCopyPs) return false;                     // nothing to copy it with
    if (hooks::hide_hud_enabled()) return false;       // hiding the HUD still hides it
    return true;
}

int ui_plane_mode() { return g_uiPlaneMode.load(); }

void set_ui_plane_mode(int mode)
{
    const int m = (mode == kUiPlaneForward) ? kUiPlaneForward : kUiPlaneHeadLocked;
    if (g_uiPlaneMode.exchange(m) == m) return;
    // Switching INTO world-pinned with no pin ever taken has nowhere to put the
    // plane; arm one so it appears where the player is looking rather than at
    // the origin. An existing pin is deliberately left alone -- going head-
    // locked and back should return the plane to where it was left.
    if (m == kUiPlaneForward) {
        AcquireSRWLockShared(&g_uiPinLock);
        const bool have = g_uiPinValid;
        ReleaseSRWLockShared(&g_uiPinLock);
        if (!have) g_uiPinRequest.store(kUiPinToRuntimeForward);
    }
}

float ui_plane_distance() { return g_uiPlaneDistM.load(); }

void set_ui_plane_distance(float metres)
{
    g_uiPlaneDistM.store((std::max)(0.3f, (std::min)(20.0f, metres)));
}

float ui_plane_size() { return g_uiPlaneSizeM.load(); }

void set_ui_plane_size(float metres)
{
    g_uiPlaneSizeM.store((std::max)(0.2f, (std::min)(10.0f, metres)));
}


// (enforce_render_size() lived here: a post-creation window resize, the third
// of three attempts to raise the render resolution from inside the process. It
// was never called -- ResizeBuffers from inside Present returns
// DXGI_ERROR_INVALID_CALL because the engine holds its own backbuffer
// reference, a plain SetWindowPos does nothing, and neither did simulating a
// completed interactive drag with WM_ENTERSIZEMOVE/WM_EXITSIZEMOVE. Superseded
// entirely by ForceGameResolution, which sets the size at CreateWindowExW
// before the engine ever reads it. Removed 2026-08-15 with its only caller of
// desired_render_size().)

// Called by the camera hooks (cockpit/orbit) every time they run, regardless
// of whether they advance parity this frame -- see g_lastCameraActivity.
void note_camera_active()
{
    g_lastCameraActivity.store(GetTickCount());
}

// Called by the camera hooks at the start of the main-camera build. The FIRST
// call after a Present flips the eye and sets its ±IPD/2 offset; later calls in
// the same frame are no-ops. The offset and the Present routing both read
// g_frameEye, so they stay in lockstep.
void advance_aer_parity(uint64_t cameraFingerprint, bool gameplayCamera)
{
    g_advCalls.fetch_add(1);
    bool expected = true;
    if (g_newFrame.compare_exchange_strong(expected, false)) {
        g_advFlips.fetch_add(1);
        // The frame turns over here, so anything recorded before this instant
        // belongs to the PREVIOUS frame. See AppliedEye::fresh.
        g_eyeSerialAtFlip.store(g_eyeRecordSerial.load());
        // AER flips; Synched Sequential chooses, by the moment being rendered.
        int e = synched_active() ? ss_choose_eye(cameraFingerprint, gameplayCamera)
                                 : (g_frameEye.load() ^ 1);
        g_frameEye.store(e);
        {
            const uint32_t b = (uint32_t)g_advFlips.load();
            AcquireSRWLockExclusive(&g_buildRecLock);
            BuildRec& r = g_buildRec[b % kBuildRing];
            r = BuildRec{};
            r.build = b;
            r.eye = e;
            r.ss = g_ssFrame.load();
            ReleaseSRWLockExclusive(&g_buildRecLock);
        }
        // The world scale is applied here and only here: every 3D view takes its
        // eye offset from g_eyeSide, and DIBR shift reads back what was baked.
        float ipd = g_ipd.load() / g_worldScale.load();
        // eye 0 -> swapchain[0] = left XrView; the left eye sits at +IPD/2 along
        // the game's right axis under our sign convention (verified: -IPD/2 gave
        // inverted depth in both cockpit AND orbit). Global flip fixes both.
        g_eyeSide.store(e == 0 ? +ipd*0.5f : -ipd*0.5f);
        g_stereo.store(true);
    }
    // Parity counters (advCalls/advFlips/presents) still accumulate for anyone
    // stepping through with a debugger; the periodic log line was removed —
    // pipeline delay=1 is confirmed stable, no longer needs a heartbeat.
}

// MATCH THE HEADSET'S VERTICAL FOV. Off by default because it changes the
// shape of every rendered frame, and the setup it was written for (a very wide
// headset on a square canvas) is not the one most people have.
std::atomic<bool> g_matchVfov{false};

// THE ASYMMETRY ALIGNMENT. OFF BY DEFAULT since 2026-09-20 -- measured, on a
// Quest 3, and the numbers are the whole argument.
//
// WHAT IT DOES. The render is symmetric; a headset's per-eye frustum is not.
// So a sub-rectangle of the rendered image is declared, holding exactly the
// angles that eye really covers, which puts the image on the LENS rather than
// on the boresight. The arithmetic is sound and was checked: the rect lands on
// the declared angles to within a fifth of a pixel, pixels-per-tangent agree on
// both axes, and composited_fov_scale() cancels out of the crop fractions so it
// stays right at any render scale.
//
// WHY IT IS OFF ANYWAY. It was added for Pimax, but it is not Pimax-specific --
// it fires on ANY asymmetric frustum, and a Quest 3 reports 14 degrees of it:
//
//     eye0 raw L-54.00 R40.00  ->  subImage [0..2472] of 3072
//     eye1 raw L-40.00 R54.00  ->  subImage [600..3072] of 3072
//
// That is 600 px of 3072 dropped per eye -- 19.5% of the width, on opposite
// sides -- leaving 1872 px of binocular overlap, 61%. In the headset that reads
// as the two eyes agreeing only in the middle with a fifth of each image
// unmatched at the outer edge, which is precisely how it was reported.
//
// So this is a trade, not an improvement: lens-accurate placement against a
// fifth of each eye going monocular. On hardware with a small asymmetry the
// first is nearly free; at 14 degrees it is not, and the second wins.
//
// WHAT OFF MEANS. The symmetric frustum is declared over the whole image:
// geometrically exact for what was rendered, both eyes showing the same angular
// range, centred on the eye axis instead of the lens.
//
// THE ACTUAL FIX IS NEITHER. Render each eye at its own asymmetric frustum and
// nothing needs cropping, because nothing is surplus -- see
// offcenter_projection(). It costs DIBR shift its outer coverage, which is why
// it is not simply the default either.
std::atomic<bool> g_alignAsym{false};

// OFF-CENTRE PROJECTION. Render each eye at the frustum the runtime actually
// reports for it, shear and all, instead of a symmetric one that encloses it.
//
// This is the correct thing to do and it is not the default, because under AER
// it costs something real. The two eyes then look at genuinely different
// angular regions -- on a Pimax at wide FOV, eye0 covers [-70, +42] and eye1
// [-42, +70], overlapping over only about half their width. DIBR shift
// synthesizes one eye from the other, so outside that overlap it has no source
// and the outer wedge falls to the stale-eye warp instead. With a symmetric
// render both eyes cover the union, so the shift can reach all of it.
//
// So: sharper and lens-exact, at the cost of the shift's outer coverage. Worth
// having as a choice rather than a decision made for everybody.
std::atomic<bool> g_offcenter{false};

// 2D SCREEN MODES. The map and the garage can each be shown as a flat panel
// instead of a stereo world: the frame is put on a quad on the UI plane and the
// eye projection is not submitted at all.
//
// WHAT "GARAGE" REALLY COVERS, because the dropdown cannot say it. The garage
// is not identifiable -- it shares its interface with the menus and its world
// with gameplay, so no marker list can name it, and SnowRunner does not tick
// DRIVE_CAMERA there so the engine's own mode enum is unreadable (camera_hook.h,
// shader_cull.cpp). kScreenGarage is the FALLBACK bucket: "not the map, and the
// drive camera is not running". That is the garage, the main menu, every pause
// menu, loading screens and the level fly-in. Setting it to 2D flattens all of
// them. That is not a bug to be fixed later -- it is the resolution the
// classifier has, and pretending otherwise would mean inventing a garage test
// that has been looked for and does not exist.
std::atomic<int> g_mapRender{kScreenRender3D};
std::atomic<int> g_garageRender{kScreenRender3D};

// THE MAP, by whichever classifier is selected. Separate from on_static_screen()
// because that one deliberately answers a different question (is the world being
// played), and folds the garage in with it.
bool on_map_screen()
{
    return hooks::screen_gate_direct()
               ? (hooks::direct_screen() == hooks::kScreenMap)
               : hooks::in_map_view();
}

bool offcenter_projection() { return g_offcenter.load(std::memory_order_relaxed); }

// TRUE while the screen we are on is set to 2D. Read by everything that has to
// change shape for it: the fov scale, the eye offset, both quads and the layer
// list. Map first -- in_gameplay() already reads false there, so the garage
// bucket would otherwise swallow it.
bool flat_screen_active()
{
    if (on_map_screen())
        return g_mapRender.load(std::memory_order_relaxed) == kScreenRender2D;
    if (!hooks::in_gameplay())
        return g_garageRender.load(std::memory_order_relaxed) == kScreenRender2D;
    return false;
}

bool ui_stays_in_frame()
{
    // single_quad_runtime() first: it is a cached constant, false on Windows,
    // and this runs for every UI draw.
    return single_quad_runtime() && flat_screen_active();
}

int map_render_mode()    { return g_mapRender.load(std::memory_order_relaxed); }
int garage_render_mode() { return g_garageRender.load(std::memory_order_relaxed); }

void set_map_render_mode(int mode)
{
    const int m = (mode == kScreenRender2D) ? kScreenRender2D : kScreenRender3D;
    if (g_mapRender.exchange(m, std::memory_order_relaxed) == m) return;
    VRLOG("RENDER MAP -> %s", m == kScreenRender2D ? "2D (flat panel)" : "3D");
}

void set_garage_render_mode(int mode)
{
    const int m = (mode == kScreenRender2D) ? kScreenRender2D : kScreenRender3D;
    if (g_garageRender.exchange(m, std::memory_order_relaxed) == m) return;
    VRLOG("RENDER GARAGE -> %s (also the menus and the fly-in -- see g_garageRender)",
          m == kScreenRender2D ? "2D (flat panel)" : "3D");
}

const char* screen_render_mode_key(int mode)
{
    return (mode == kScreenRender2D) ? "2D" : "3D";
}

bool screen_render_mode_from_key(const char* key, int& out)
{
    if (!key) return false;
    if (_stricmp(key, "3D") == 0) { out = kScreenRender3D; return true; }
    if (_stricmp(key, "2D") == 0) { out = kScreenRender2D; return true; }
    return false;
}

void set_offcenter_projection(bool on)
{
    if (g_offcenter.exchange(on, std::memory_order_relaxed) == on) return;
    VRLOG("OFF-CENTRE PROJECTION -> %s", on ? "ON (per-eye asymmetric frustum)"
                                            : "off (symmetric enclosing frustum)");
}

bool fov_asymmetry_align() { return g_alignAsym.load(std::memory_order_relaxed); }

void set_fov_asymmetry_align(bool on)
{
    if (g_alignAsym.exchange(on, std::memory_order_relaxed) == on) return;
    VRLOG("FOV ASYMMETRY ALIGN -> %s", on ? "ON (subImage crop, lens-aligned)"
                                          : "off (symmetric, full image)");
}

bool match_headset_vfov() { return g_matchVfov.load(std::memory_order_relaxed); }

void set_match_headset_vfov(bool on)
{
    if (g_matchVfov.exchange(on, std::memory_order_relaxed) == on) return;
    VRLOG("MATCH HEADSET VFOV -> %s (headset V %.2f deg, H %.2f deg)",
          on ? "ON" : "off", headset_vfov_deg(), headset_hfov_deg());
}

// The scaled vertical, the twin of render_hfov_deg(). Scale in TANGENT space
// for the same reason spelled out there: eye_frustum_half_angles() applies the
// identical factor when it builds the window we submit, and an angle-space
// scale here would silently disagree with it for any scale != 1.
float render_vfov_deg()
{
    const float vfov = headset_vfov_deg();
    if (vfov <= 0.0f) return 0.0f;
    const float s = composited_fov_scale();
    if (s == 1.0f) return vfov;
    float halfRad = vfov * 0.5f * 0.0174532925f;
    if (halfRad > 1.5533f) halfRad = 1.5533f;   // 89 deg; keep tan() finite
    return 2.0f * std::atan(std::tan(halfRad) * s) * 57.2957795f;
}

// THE FRUSTUM THIS EYE IS ACTUALLY RENDERED AT, as tangent bounds.
//
// THE single definition, and everything that has to agree about the shape of a
// frame reads it: PROJ LOCK for the matrix it writes, the submission for the
// angles it declares, the warp for its intrinsics, and DIBR shift for the
// offset between the two eyes. They used to derive their own from half-angles,
// which was fine while every frustum was symmetric and is exactly the kind of
// agreement that rots the moment one is not.
//
// Tangents rather than angles because that is the space all four work in, and
// because it is the space composited_fov_scale() multiplies in -- see
// render_hfov_deg() for why that matters.
//
// Symmetric unless off-centre projection is on, in which case it is the
// runtime's own per-eye frustum. False when no frustum has been reported yet.
bool render_eye_frustum_tan(int eye, float& tl, float& tr, float& tu, float& td)
{
    if (eye < 0 || eye > 1) return false;

    if (offcenter_projection()) {
        const XrFovf& f = g.lastView[eye].fov;
        if (std::fabs(f.angleLeft) + std::fabs(f.angleRight) > 0.01f) {
            const float s = composited_fov_scale();
            tl = std::tan(f.angleLeft)  * s;
            tr = std::tan(f.angleRight) * s;
            tu = std::tan(f.angleUp)    * s;
            td = std::tan(f.angleDown)  * s;
            return (tr - tl) > 1.0e-4f && (tu - td) > 1.0e-4f;
        }
        // No frustum yet: fall through to the symmetric answer rather than
        // report failure, so a caller cannot end up with no projection at all.
    }

    float hh = 0.0f, vh = 0.0f;
    eye_frustum_half_angles((uint32_t)eye, hh, vh);   // already scaled
    if (!(hh > 1.0e-4f) || !(vh > 1.0e-4f)) return false;
    tr = std::tan(hh); tl = -tr;
    tu = std::tan(vh); td = -tu;
    return true;
}

// Which eye this Present is rendering for real -- the other one is synthesized.
// Needed by the projection builder, which has to know whose frustum to write.
int render_eye() { return present_route_eye(); }
float debug_eye_cant_deg() { return g_debugCantDeg.load(); }

void set_debug_eye_cant_deg(float deg)
{
    if (deg < -15.0f) deg = -15.0f;
    if (deg >  15.0f) deg =  15.0f;
    g_debugCantDeg.store(deg);
}

// THE HEADSET'S OWN VERTICAL, by the same rule as the horizontal below.
//
// It exists because the vertical was never asked of the runtime at all: the
// render's vertical half-angle came from the CANVAS ASPECT, so a square canvas
// declared vh == hh whatever the panel actually shows. That is wrong in both
// directions and the error grows with the FOV:
//
//   Pimax 8KX, wide     H half 70.14, V half 57.78  -> 43% of the vertical
//                       tangent span rendered and never displayed
//   Pimax 8KX, narrow   H half 50.14, V half 57.78  -> the opposite; the top
//                       and bottom of the panel have no content at all
//
// Reported as the widest half x2, the same enclosing rule headset_hfov_deg()
// documents below -- so a headset with an asymmetric vertical (most have more
// panel below the optical axis than above) is covered rather than clipped.
float headset_vfov_deg()
{
    float widestHalf = 0.0f;
    int   valid = 0;
    for (int e = 0; e < 2; ++e) {
        const float up   = std::fabs(g.lastView[e].fov.angleUp);
        const float down = std::fabs(g.lastView[e].fov.angleDown);
        if (up + down > 0.01f) {
            widestHalf = (std::max)(widestHalf, (std::max)(up, down));
            ++valid;
        }
    }
    if (valid == 0) return 0.0f;
    return widestHalf * 2.0f * 57.2957795f;
}

float headset_hfov_deg()
{
    // WIDEST HALF-ANGLE x2, not the sum of the two halves.
    //
    // We can only render a SYMMETRIC frustum (PROJ LOCK writes p00/p11 with no
    // off-centre terms), but a headset's per-eye frustum is ASYMMETRIC -- the
    // nose side and temple side differ. Reporting |angleLeft| + |angleRight|
    // gives the correct TOTAL, but a symmetric frustum built from half of that
    // is too narrow on the wide side and too wide on the narrow side: it leaves
    // an uncovered wedge at one edge of each eye, which is a black bar at the
    // outer edge of both eyes once the submitted FOV is declared honestly.
    //
    // Taking the maximum instead makes the symmetric frustum ENCLOSE the real
    // one, so the display is fully covered and the surplus on the narrow side
    // simply falls outside it. Slightly more world rendered than strictly
    // needed; that is the price of symmetric content on asymmetric optics.
    //
    // Both the render (PROJ LOCK) and the submitted window
    // (eye_frustum_half_angles) derive from this one function, so they stay in
    // agreement whichever way it is defined.
    float widestHalf = 0.0f;
    int   valid = 0;
    for (int e = 0; e < 2; ++e) {
        float left  = std::fabs(g.lastView[e].fov.angleLeft);
        float right = std::fabs(g.lastView[e].fov.angleRight);
        if (left + right > 0.01f) {
            widestHalf = (std::max)(widestHalf, (std::max)(left, right));
            ++valid;
        }
    }
    const float total = widestHalf * 2.0f * (float)valid;   // /valid below
    // No runtime, no frustum, no answer. Callers treat 0 as "not up yet" --
    // detour_projbuild's `render_hfov_deg() > 1` guard is the one that matters,
    // and it correctly declines to lock a projection we cannot size.
    if (valid == 0) return 0.0f;
    constexpr float kRad2Deg = 180.0f / 3.14159265358979323846f;
    return (total / (float)valid) * kRad2Deg;
}

float render_hfov_deg()
{
    float hfov = headset_hfov_deg();
    if (hfov <= 0.0f) return 0.0f;

    // The "Final FOV" angle trim that used to sit here was REMOVED 2026-08-18:
    // with the automatic sizing correct it was only ever left at 1.0, and a
    // knob that is always 1.0 is one more thing that can silently be wrong.
    // render_fov_scale() (the render-size slider) remains -- it works in
    // TANGENT space, see below.
    const float trimmed = hfov;

    // composited_fov_scale() must be applied in TANGENT space, because that is
    // how eye_frustum_half_angles() applies the very same factor when building
    // the window we submit to the compositor:
    //     vh = atan(tan(base) * composited_fov_scale())
    // This used to scale the ANGLE here instead, which silently disagreed with
    // the submission for any scale != 1. With the map window (since removed)
    // at 0.5 on a ~100.7 deg headset that meant declaring a 62.1 deg window
    // over a 50.3 deg render -- the compositor stretched the difference and the
    // map view read as ~1.23x too zoomed. (Measured: the Final-FOV value that
    // cancelled it by hand was ~1.23, matching 62.1/100.67/0.5 to two
    // decimals.) The two are now the same computation, so no view needs FOV
    // compensation at all.
    const float s = composited_fov_scale();
    if (s == 1.0f) return trimmed;   // gameplay path: bit-identical to before

    float halfRad = trimmed * 0.5f * 0.0174532925f;
    if (halfRad > 1.5533f) halfRad = 1.5533f;   // 89 deg; keep tan() finite
    return 2.0f * std::atan(std::tan(halfRad) * s) * 57.2957795f;
}

void request_recenter()
{
    // THE explicit recenter: position, yaw to wherever the head is looking, and
    // the UI plane with it. Pitch and roll are untouched, so the horizon stays
    // world-locked through it.
    g_recenterRequest.fetch_or(kRecenterPos | kRecenterYawToView);
}

void request_position_recenter()
{
    // Camera changes and teleports. Re-centres the LEAN origin only: the head's
    // offset from centre is fed to the game as a lean, so leaving it stale after
    // the player has shifted in their chair slides the view permanently. Yaw is
    // deliberately left alone -- swinging the world round because someone got
    // into a different truck is exactly what this used to do wrong.
    g_recenterRequest.fetch_or(kRecenterPos);
}

// No-OpenXR fallback: still run the full AER pipeline (default 62mm IPD) so the
// camera alternates eyes, and drive the single-eye desktop mirror. Lets the
// stereo pipeline run/develop without a headset connected.
void aer_only_present(IDXGISwapChain* swapchain)
{
    g_presents.fetch_add(1);
    if (!g.device) {
        ComPtr<ID3D11Device> dev;
        if (FAILED(swapchain->GetDevice(__uuidof(ID3D11Device), (void**)&dev))) return;
        g.device = dev; dev->GetImmediateContext(&g.ctx);
    }
    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) return;
    ensure_desktop_save(bb.Get());
    // One eye on the monitor -- except under DLSS, which renders the flat
    // screen mono (below) and is judged on it: holding every other frame would
    // halve the rate it is being judged at.
    if (g.desktopSave && dlss::mode() == dlss::kOff) {
        if (present_route_eye() == g_desktopEye.load()) g.ctx->CopyResource(g.desktopSave.Get(), bb.Get());
        else                                            g.ctx->CopyResource(bb.Get(), g.desktopSave.Get());
    }
    // No headset -> assume the default IPD, so the stereo pipeline keeps running
    // for development. Except under DLSS, which is tested exactly here: a camera
    // jumping 62 mm sideways every frame is not something any temporal AA can
    // accumulate, so the flat screen gets one eye at the head's centre.
    g_ipd.store(dlss::mode() != dlss::kOff ? 0.0f : 0.062f);
    g_newFrame.store(true);    // first camera build next frame advances the parity
}


// A LIVE RESIZE REACHES THE HEADSET HERE, at the top of the Present after the
// engine's resize, where no frame is open, no image is acquired and the previous
// xrEndFrame has finished. Nothing happens if only the render size moved: a DLSS
// mode change keeps the eye images.
//
// The new images are made before the old ones go, so a size the runtime refuses
// costs nothing: the blit scales whatever it is given into the images there are.
//
// They start empty, so they count as never rendered, and no layer is submitted
// until both have something in them. That is normally this very frame: the eye
// not rendered now is warped from its retained frame, which knows nothing of the
// swapchain, into the rect its last blit left -- scaled here, because the warp
// reuses that rect as it stands, and an empty one would draw nothing at all.
static void follow_game_frame()
{
    if (!g_frameResized.exchange(false) || g.session == XR_NULL_HANDLE || !g.device) return;

    uint32_t w = 0, h = 0;
    eye_size_for_frame(g.bbWidth, g.bbHeight, w, h);
    if (w == g.width && h == g.height) return;

    XrSwapchain sc[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::vector<ComPtr<ID3D11RenderTargetView>> rtvs[2];
    if (!create_eye_images(g.device.Get(), w, h, sc, rtvs)) {
        destroy_eye_images(sc, rtvs);
        VRLOG("XR canvas: could not make %ux%u eye images -- staying at %ux%u", w, h, g.width,
              g.height);
        return;
    }
    destroy_eye_images(g.swapchain, g.rtvs);

    const float sx = (float)w / (float)g.width, sy = (float)h / (float)g.height;
    for (int e = 0; e < 2; ++e) {
        g.swapchain[e] = sc[e];
        g.rtvs[e] = std::move(rtvs[e]);
        g.eyeFresh[e] = false;
        g.eyeEverRendered[e] = false;
        XrRect2Di& r = g.blitRect[e];
        r.offset.x = (int32_t)((float)r.offset.x * sx);
        r.offset.y = (int32_t)((float)r.offset.y * sy);
        r.extent.width  = (int32_t)((float)r.extent.width * sx);
        r.extent.height = (int32_t)((float)r.extent.height * sy);
        g.ssPendingValid[e] = false;
    }
    g.ssCommitted = false;
    g_ssReset.store(true);

    VRLOG("XR canvas: eye images %ux%u -> %ux%u, %u+%u images", g.width, g.height, w, h,
          (uint32_t)g.rtvs[0].size(), (uint32_t)g.rtvs[1].size());
    g.width = w;
    g.height = h;
}

void mirror_on_present(IDXGISwapChain* swapchain)
{
    // Which camera build the image in the backbuffer was drawn with, before
    // anything below routes it to an eye or declares a pose for it.
    update_on_screen();

    if (g_failed) { hooks::set_frame_freeze(false); aer_only_present(swapchain); return; }
    if (!g_inited) {
        g_inited = true;
        if (!init(swapchain)) {
            g_failed = true;
            hooks::set_frame_freeze(false);
            VRLOG("no OpenXR -> AER-only fallback (default 62mm IPD)");
            aer_only_present(swapchain);
            return;
        }
    }

    update_world_cursor_hotkey();

    // Fallback parity advance: normally ONLY the camera hooks (cockpit/orbit)
    // advance the AER eye -- required for correct cockpit behavior and looks
    // better for orbit too, so gameplay must keep driving it exclusively.
    // During a menu or loading screen there's no 3D camera at all, so that
    // never fires -- the eye would never toggle and one XR swapchain image
    // goes stale (menu shows in only one eye). Detect that generically: if no
    // camera hook has run recently, advance from here instead. Covers loading
    // screens AND the pause menu without needing to specifically identify
    // either state.
    if (GetTickCount() - g_lastCameraActivity.load() > 250) advance_aer_parity();

    // Ctrl+PageUp (frame-dump burst) is the only diagnostic binding left here.
    // Plain PageUp/PageDown now calibrate the headset cursor; see
    // render/cursor_overlay.cpp.
    // that used to live at this call site has moved into the in-game settings
    // UI (menu_hook.cpp), which is strictly more capable than a keybind --
    // it shows the current state instead of cycling blind:
    //   - T (render mode) and F11 (hole fill) were both combo boxes there before
    //     the render-mode selector itself was removed. F11 in particular was a
    //     second control path for state the combo box already owns, and
    //     blind-cycling through the two diagnostic fills (magenta, disparity) to
    //     get back to a playable one is worse than picking one.
    //   - O/P (frustum trim) and Y/U (vertical shift) are gone: they collided
    //     with in-game controls (P is the handbrake) and their values are now
    //     settled defaults -- see g_vertOffset above.
    //
    // PageUp survives because a burst has to be triggered WHILE something is
    // happening on screen, and reaching for the menu to start one changes what
    // you were trying to capture. The Advanced tab has the same button for when
    // that does not matter.
    static bool pCtrlPgUp=false;
    const bool ctrlPgUp = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 &&
                          (GetAsyncKeyState(VK_PRIOR) & 0x8000) != 0;
    const bool ctrlPgUpEdge = ctrlPgUp && !pCtrlPgUp;
    pCtrlPgUp = ctrlPgUp;
    if (ctrlPgUpEdge) framedump::request(20);

    // BEFORE render_frame, which is where both eyes composite it, and after the
    // game's own drawing for this frame -- so a capture-eye frame shows its own
    // markers rather than the previous one's. Outside the `g.running` test on
    // purpose: the capture buffer has to be cleared every Present whether or not
    // an XR frame was built, or the redirected draws would pile up in it.
    if (g.ctx) {
        winchlayer::latch(g.ctx.Get(), present_route_eye() == kWinchCaptureEye);
    }

    // The previous frame's xrEndFrame may still be on the worker (Wine only).
    // Waited for HERE, ahead of poll_events() as well as render_frame(), so no
    // call on the session -- an xrEndSession included -- ever overlaps it.
    end_frame_wait();
    follow_game_frame();
    poll_events();
    // Every Present, running or not: an unfocused session publishes a neutral
    // pad, so a button held when the headset came off is not held forever.
    xrinput::poll(g.session, g.running && g.state == XR_SESSION_STATE_FOCUSED);

    // Synched Sequential's freeze runs only while there is a session to show it:
    // on the desktop alone, frames in pairs are just half the frame rate. Any
    // change of state drops a half-built pair.
    {
        const bool want = g_renderMethod.load() == kRenderSynchedSequential && g.running;
        static bool s_was = false;
        hooks::set_frame_freeze(want);
        const bool on = hooks::frame_freeze_on();
        if (on != s_was) { s_was = on; g_ssReset.store(true); }
    }
    if (g.running) render_frame(swapchain);
}

// CLOSE THE SESSION BEFORE THE PROCESS GOES. Called from the ExitProcess hook
// (proxy/dllmain.cpp), on the thread that is exiting, before any DLL unloads.
//
// Without it the session simply outlived the game: ExitProcess unloaded the
// DLLs in reverse order, Virtual Desktop's runtime tore its swapchains down
// from its own DLL_PROCESS_DETACH -- by then the game's renderer was gone --
// and crashed inside d3d11 on every quit. Measured with the 64-frame crash
// walk, 2026-10-26: SnowRunner.exe -> ucrtbase!exit -> ExitProcess -> ntdll's
// loader shutdown -> virtualdesktop-openxr.dll -> VD's LibOVR -> d3d11 ->
// fault; this DLL nowhere on the stack. Closed here, everything the runtime
// holds is released while the device it lives on is still intact, and the
// runtime DLL itself is unloaded by xrDestroyInstance rather than by the
// exiting loader.
//
// Children first: swapchains (our views on their images released before
// them), spaces, the action set, the session, the instance. Idempotent, and
// leaves the module in its no-XR state so nothing afterwards touches a handle.
void shutdown_xr()
{
    static std::atomic<bool> s_done{false};
    if (s_done.exchange(true)) return;
    if (g.instance == XR_NULL_HANDLE) return;

    end_frame_wait();
    g.running = false;
    g_failed = true;

    for (auto& v : g.rtvs) v.clear();
    g.menuRtvs.clear();
    g.uiRtvs.clear();
    for (XrSwapchain& sc : g.swapchain)
        if (sc != XR_NULL_HANDLE) { xrDestroySwapchain(sc); sc = XR_NULL_HANDLE; }
    if (g.menuSwapchain != XR_NULL_HANDLE) { xrDestroySwapchain(g.menuSwapchain); g.menuSwapchain = XR_NULL_HANDLE; }
    if (g.uiSwapchain != XR_NULL_HANDLE)   { xrDestroySwapchain(g.uiSwapchain);   g.uiSwapchain   = XR_NULL_HANDLE; }
    if (g.viewSpace != XR_NULL_HANDLE) { xrDestroySpace(g.viewSpace); g.viewSpace = XR_NULL_HANDLE; }
    if (g.space != XR_NULL_HANDLE)     { xrDestroySpace(g.space);     g.space     = XR_NULL_HANDLE; }
    xrinput::shutdown();
    // "xrDestroySession can be called when the session is in any session
    // state" (spec) -- so no xrRequestExitSession round trip at exit.
    if (g.session != XR_NULL_HANDLE) { xrDestroySession(g.session); g.session = XR_NULL_HANDLE; }
    xrDestroyInstance(g.instance);
    g.instance = XR_NULL_HANDLE;
    VRLOG("xr: session and instance closed before exit");
}

void mirror_on_resize()
{
    // Backbuffer size changes: drop staging (rebuilt lazily). The XR swapchain
    // keeps its size unless the mod asked for this one (on_game_resized); the
    // blit samples with clamp so aspect is preserved.
    g.staging.Reset();
    g.stagingSrv.Reset();
}

void before_game_resize() { end_frame_wait(); }

void on_game_resized(uint32_t w, uint32_t h)
{
    g.bbWidth  = w;
    g.bbHeight = h;
    g_frameResized.store(true);
}

} // namespace xr
