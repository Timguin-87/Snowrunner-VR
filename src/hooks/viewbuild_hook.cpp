#include "hooks/viewbuild_hook.h"
#include "hooks/camera_fov.h"
#include "hooks/camera_hook.h"
#include "hooks/cbuffer_hook.h"
#include "xr/xr_mirror.h"
#include "common/log.h"

#include <windows.h>
#include <intrin.h>
#include <cmath>
#include <cstdio>
#include <atomic>
#include <algorithm>

#include <MinHook.h>

namespace hooks {
namespace {

// View-matrix builder + the MAIN RENDER camera's build call site (found via
// call-site cycling: exe+0xA179FB). Addresses verified against SnowRunner
// 1.886173.SNOW_DLC_18 (exe SHA-256 FBD3F36C...B4E683).
constexpr uintptr_t kBuilderOffset = 0x1153390;
constexpr uintptr_t kMainCallSite  = 0xA179FB;

// Module base, set at install. Defined here rather than beside the camera code
// because the screen-state detour below needs it to make a return address
// exe-relative, and that is the first use in the file.
uintptr_t g_moduleBase = 0;

// SCREEN STATE (menu/gameplay/map/garage) -- REMOVED 2026-08-24, all of it: the
// hook on exe+0x16AE3C0, the candidate table, the declaration UI, the automatic
// narrowing, and the gating that had been moved onto it.
//
// The FIELD was real and its values were confirmed twice (0=menu, 1=garage,
// 2=map, 3=gameplay at [object+0x10]). What never worked was knowing WHICH
// object holds it. That function is a generic utility called for thousands of
// objects and nearly every one has a small integer at +0x10, so the search was
// always separating near-identical candidates -- and it could be narrowed to a
// single survivor that still reported GARAGE while the map was open. A marker
// that is usually right is worse than no marker: it moves the failure from
// "the classifier is wrong in a way you can predict" to "the mod changed
// behaviour and nothing says why".
//
// Nothing about the object survives a restart either -- seven routes were closed
// by measurement (static pointer path, call site, class vtable, names via
// pointers, position in the block, content signature, inline names). It is one
// slot in a table of engine variables, every slot the same class, so no
// structural test can single it out.
//
// map_by_pose() and the cbuffer relocation classifier are what decide the screen
// again, as they did before. They have a known failure -- a gameplay camera
// pitched -45 deg near the origin reads as the map -- which is a bounded,
// nameable bug rather than an unpredictable one.
//
// Do not rebuild this on the strength of being able to narrow to one candidate.
// That part always worked.

// The camera COMBINE fn: combine(out=rcx, position=rdx float3, orientation=r8),
// called for the main camera at +0xA17DF0 (return addr +0xA17DF5). rdx is the
// eye POSITION — the injection point for stereo/positional translation.
constexpr uintptr_t kCombineOffset = 0x11528D0;
constexpr uintptr_t kCombineRet    = 0xA17BC5;

// FnA = fillOrbitView(viewObj=rcx, eye=rdx, target=r8, up=r9). Called for BOTH the
// orbit CAMERA and the sun/shadow view — so we hook FnA and filter by ITS caller
// to transform only the camera view (else head-look drags the shadows).
constexpr uintptr_t kFnAOffset    = 0xDA1D20;
constexpr uintptr_t kOrbitCamGame = 0x860EB9;   // in-game orbit camera caller
constexpr uintptr_t kOrbitCamMenu = 0x8845AD;   // main-menu orbit camera caller

// Game render FOV: settings obj ptr at [exe+0x2AF4570], FOV percent multipliers
// at +0x60/+0x64. We scale them each frame to widen the render to fill the HMD.
constexpr uintptr_t kSettingsPtr = 0x2AF4570;
constexpr int kFovOff0 = 0x60, kFovOff1 = 0x64;

// Live 3D-viewport width/height (CE-confirmed, static, NOT behind kSettingsPtr's
// pointer -- direct module-relative addresses). Crops the 3D scene from origin
// within the backbuffer, read every frame; the HUD is unaffected (separate,
// full-window pass), which is how we know this is a viewport, not the window/
// backbuffer size itself. Paired with forcing the backbuffer bigger at
// CreateSwapChain/ResizeBuffers time (swapchain_hook.cpp) -- that alone caused
// a corner-crop because these fields stayed at the small config value; now
// re-asserted here every frame so they track the forced backbuffer size.
constexpr uintptr_t kViewportW = 0x2AA1808;
constexpr uintptr_t kViewportH = 0x2AA180C;
// THE GAME'S OWN FOV FIELDS (settings+0x60/+0x64) ARE NEVER WRITTEN.
//
// They used to be, every Present, with a declared culling FOV scaled by a
// slider. Removed 2026-08-26: that value had to reach ~200 deg before geometry
// stopped vanishing at the periphery, and from ~150 up it began culling small
// objects DEAD AHEAD -- so the setting that fixed the edges broke the centre and
// no single number satisfied both. The replacement writes combine::CAMERA's own
// m_fFOV instead (hooks/camera_fov.h): the field the engine genuinely builds its
// cull frustum from, which clears the periphery at 1.00x with what forward
// culling remains merely pushed further out.
//
// game_fov_fields() below still READS these two for the FOV-stability monitor.
// Reading them is more useful now than it was, because they hold the game's own
// untouched value rather than an echo of ours.

// PERSPECTIVE PROJECTION BUILDER (CE-confirmed, this game build):
//   buildPerspective(rcx = out 4x4, ...) -- calls ucrtbase.tanf, then writes
//   p00 at [rcx+0x00], p11 at [rcx+0x14], 1.0f at [rcx+0x2C], ret at +0x1154264.
// This is the authoritative FOV: writing settings+0x60 only influences an INPUT,
// which is why the controller-look FOV change kept overriding us. Overriding the
// OUTPUT here cannot be overridden by anything upstream.
//
// It also builds shadow/reflection projections, so it must be filtered by caller.
// Log distinct callers first (kProjOverrideCaller = 0), then pin the main-camera
// one. Found alongside: the per-frame camera struct has projection at +0x00 and a
// camera-to-world matrix at +0x40 whose translation row is +0x70 (the eye
// position), stride 0x140; the struct is copied by camera code at +0x8554C2.
constexpr uintptr_t kProjBuildOffset = 0x11544D0;

// Camera-struct consumer (CE: entry +0x855310, prologue mov rax,rsp / push
// rbp,r12..r15 / sub rsp,0x300, loops with r14d over globals at exe+0x2B1F778 and
// +0x2B1F780). The copy at +0x8554C2 that reads our struct lives in here, so this
// runs AFTER the camera is finalised -- which makes it the place to apply the AER
// eye offset: late enough that nothing recomputes it afterwards.
//
// Because the projection is at struct+0x00, the projection builder's rcx IS the
// camera struct base, giving us the pointer for free. Observe-only for now: log
// which argument register carries the struct and confirm the layout at runtime
// before writing anything.
constexpr uintptr_t kCamConsumerOffset = 0x855310;
// DISABLED. +0x85537C does `movaps xmm15,xmm0`, so this function takes a float in
// xmm0 -- probably an interpolation factor. A detour declared with pointer-only
// parameters does not forward it, so hooking it at all corrupted the camera
// (menu and cockpit went jittery even with no writes). Do not re-enable without
// establishing the FULL signature, floats included.
constexpr bool kHookCamConsumer = false;
// Main-camera projection call site (observed: produced hfov 107.40, matching the
// measured rendered FOV; sits in the same function region as the orbit camera
// setup at 0xDA1C20/0xDA1D2B). The other two callers are a 45-deg view and an
// asymmetric 31.3x11.6 shadow projection -- both must be left alone.
std::atomic<uintptr_t> g_projOverrideCaller{0xDA1F4E};

// --- per-caller FOV probe ---------------------------------------------------
// Every projection call site EXCEPT the locked one keeps the FOV the game gave
// it -- shadow views, reflections, the mirror pass. When geometry goes missing
// from one of those (the mirror losing the cab front and the terrain edge),
// the question is which frustum starved it, and the only way to tell them apart
// is to widen them one at a time and watch.
//
// The multiplier is applied in TANGENT space (p00 /= mul), which is what
// "widen the frustum by this much" actually means -- scaling the angle would
// behave wildly differently near 180 degrees. Applied AFTER the FOV lock, so it
// composes even when the selected site is the locked one.
//
// Diagnostic only: nothing here is saved, and index -1 is off.
constexpr int kMaxProjSites = 16;
std::atomic<uintptr_t> g_projSite[kMaxProjSites];
std::atomic<float>     g_projSiteFov[kMaxProjSites];
std::atomic<int>       g_projSiteCount{0};

// --- which projection at the locked caller is the MAIN VIEW? ----------------
// Narrowing the locked site's FOV was found to fix the truck mirrors, which
// means the mirror pass goes through the SAME call site and inherits our
// headset FOV -- the return address cannot separate them. Something else has
// to.
//
// BY THE SHAPE OF THE FRUSTUM, not by the order it was built in.
//
// p11/p00 is tan(hfov/2)/tan(vfov/2), i.e. the width:height ratio of the target
// the projection was built for. The main view is built for the backbuffer; a
// truck mirror is built for its own, differently shaped texture. So the one
// projection whose aspect matches xr::render_canvas_wh() is the main view, and
// it stays the main view during a cinematic, on any screen, at any point in a
// transition -- none of which the previous rule could survive.
//
// WHAT THE PREVIOUS RULE WAS, and why it is gone. It counted invocations of the
// site within each Present and locked invocation 1, having measured in steady
// gameplay that the mirror is built first and the main view second. It armed
// only after 30 consecutive frames of that two-invocation structure, precisely
// so that screens without it would fall back to locking everything.
//
// That fallback is what made it look safe, and it is also what hid the failure:
// arming needs the structure to HOLD, and during a map-transfer fly-in with a
// mirror in view it holds perfectly -- 2 invocations every frame, streak armed,
// invocation 1 locked. But a cinematic camera has no reason to keep gameplay's
// ordering, and here it does not: the main view is invocation 0, so the lock
// lands on the wrong projection and the fly-in renders at the game's own narrow
// FOV inside a frustum we submit at the headset's, i.e. magnified. Turning your
// head away from the mirror dropped the site to one invocation, broke the
// streak, and the fail-safe made the FOV correct again -- which is what a
// head-direction-dependent zoom during fly-ins turned out to be.
//
// The fail-safe is KEPT, and is the same one: lock every invocation. Its cost is
// the truck mirrors rendering at our FOV, which is what they did before any of
// this existed. The cost of the other direction is a magnified world. It engages
// whenever the aspect cannot decide -- no canvas size yet, or a whole frame in
// which nothing matched -- so the main view can never be left unlocked because
// the rule failed to recognise it.
constexpr float kAspectTol = 0.02f;   // 2% relative; mirrors are not near-misses

// Did ANY projection at this site match the canvas aspect. Split across two
// frames because the decision is made per-invocation as they stream in, and
// "did anything match" is only knowable once the frame is over. Starts false so
// the first frames lock everything and the gate only tightens once it has seen
// the main view at least once.
std::atomic<bool> g_aspectSeenLastFrame{false};
std::atomic<bool> g_aspectSeenThisFrame{false};

std::atomic<unsigned> g_projInvocation{0};      // reset each Present; census only
std::atomic<unsigned> g_projFrameNo{0};         // Presents, for rate-limiting the census

bool proj_lock_should_lock(float p00, float p11)
{
    uint32_t cw = 0, ch = 0;
    const bool haveCanvas = xr::render_canvas_wh(cw, ch) && cw > 0 && ch > 0;

    // Evaluated even when the answer is going to be overridden below, because
    // this is also the bookkeeping that decides whether the NEXT frame can
    // trust the rule. Skipping it while in fail-safe would latch fail-safe on
    // permanently.
    bool match = false;
    if (haveCanvas && p00 > 1.0e-4f && p11 > 1.0e-4f) {
        const float want = (float)cw / (float)ch;
        const float have = p11 / p00;
        match = std::fabs(have - want) <= want * kAspectTol;
        if (match) g_aspectSeenThisFrame.store(true, std::memory_order_relaxed);
    }

    if (!haveCanvas || !g_aspectSeenLastFrame.load(std::memory_order_relaxed))
        return true;                              // fail-safe: lock everything
    return match;
}

// WHAT THE INVOCATION COUNTER WENT THROUGH BEFORE THE ASPECT RULE, so none of
// it gets proposed again:
//
//   `hooks::in_gameplay() ? 1 : -1`. A proxy -- it asks "does this look like
//   gameplay" when the question is "does this screen render this site twice".
//   They agree until the classifier is wrong, and it is wrong in one specific,
//   reachable place: the MAIN MENU returned to from gameplay, which does not
//   relocate the main camera far enough to unlatch the classifier. The menu's
//   only invocation was left unlocked and rendered zoomed until the next
//   restart. Signature: correct on a fresh start, wrong after any visit to
//   gameplay.
//
//   THE RAW COUNT. Measured every frame and self-correcting, but it is LAST
//   frame's: ask for invocation 1 on a frame that renders the site once and
//   nothing is locked at all, which is where any screen with a changing
//   structure spends its time.
//
//   A 30-FRAME ARMING STREAK on top of the count, so the invocation being asked
//   for is one the frame will actually render. This fixed the transitions and
//   the screens -- the menu, map and garage build the site once, so the streak
//   never builds there -- and it is the version the aspect rule replaces. What
//   it could not fix is ordering: arming needs the structure to HOLD, a fly-in
//   with a mirror in view holds it perfectly, and in a cinematic the main view
//   is invocation 0. See the note above proj_lock_should_lock().
//
// The through-line is that all three asked WHEN or HOW MANY, and the question
// was always WHICH.

// Second, MAP/GARAGE-only projection site to hang the AER eye offset on.
//
// Those screens get no eye separation when opened from the COCKPIT, but do when
// opened from orbit -- and the map-scoped caller log proves the projection path
// is identical either way, so the projection is not what differs. What differs
// is the supplier: from orbit, logic_mode() < 0.75 lets detour_fna claim the
// in-game orbit camera and it applies the offset (and advances AER parity)
// there. From the cockpit logic_mode still reads cockpit, that builder is not
// running, and the projection's own offset is locked to g_projOverrideCaller --
// a site the map camera does not use. So nobody supplies an offset, parity never
// advances, and both eyes get the same image.
//
// 0 = off. Set it to whichever "PROJ caller (MAP/GARAGE)" the log names as the
// real map render and the offset (NOT the FOV lock) is applied there instead.
// Settable live from the recon UI so the candidates can be tried without a
// rebuild -- there is more than one, and only one of them is the scene.
std::atomic<uintptr_t> g_mapProjCaller{0};

// RE-MEASURED 2026-08-12, and it flipped back to +1.
//
// The -1 was correct while these screens took their eye offset from the
// PROJECTION. They now take it at fna with every other camera, and that site's
// side convention is the opposite one -- the same opposition that used to make
// the two suppliers cancel to flat stereo when both were active. So the sign
// that corrected for the projection is now the sign that breaks it, and the
// eyes came out reversed until this went positive.
//
// Worth reading as more than a sign fix: map, garage, menu and gameplay orbit
// all want the SAME eye sign here. The per-screen flip existed only because
// different STAGES disagreed about handedness, not because the cameras did.
// Putting everything on one stage made the correction unnecessary rather than
// merely smaller, which is the clearest evidence the unification is right.
//
// The eye side for map / garage / menu, BAKED. Every other sign is uniform --
// measured 2026-08-12, with all the per-screen flips forced off the lean came
// out correct on every screen, so the side AXIS really is the same for every
// camera at this builder.
//
// This one survives because it is not an axis disagreement. The lean rides
// baseRight (pre-rotation) and the eye offset rides camRight (post-rotation),
// and the map camera looks straight DOWN, where "right" is ill-defined -- the
// degenerate case baseOk exists a few lines below for exactly that geometry. So
// the two can differ on these screens and nowhere else.
//
// (The original 2026-08-09 finding still holds for what it measured: with the
// offset applied exactly once at the PROJECTION, those screens needed -1. It was
// also established only after the double-supply was fixed -- while two suppliers
// were fighting, flipping this turned "no depth" into "double depth", which said
// nothing about handedness.)
constexpr float kMapEyeSign = -1.0f;

// The GAME's own camera pose, published from fna BEFORE any head transform.
//
// The camera constant buffer is downstream of this hook, so anything read from
// it carries our injected rotation -- your head moves it. A screen classifier
// has to look at what the GAME is doing, which only exists here, before we
// touch it.
//
// Published on every claimed camera, not only relocated ones, so it is
// available from the first frame instead of waiting for a relocation streak to
// arm -- that gate needs logic_eye(), which does not exist until something has
// been driven, which is exactly the menu->garage->map startup that could not be
// classified.
std::atomic<float> g_gameCamFwd[3];
std::atomic<float> g_gameCamEye[3];
std::atomic<bool>  g_gameCamValid{false};

// --- MAP screen, from the game's own camera pose ----------------------------
// MEASURED: the map camera is pitched exactly -45 deg and its eye sits near the
// ORIGIN -- (1.7, 5.0, -2.1) against gameplay's (270, 19.8, 425). Two
// independent properties, and the second is what makes this safe: an orbit
// camera can be held at 45 degrees, but not while also standing at the origin.
//
// Read from the pose published ABOVE the head transform, so it is the game's
// own camera and your head cannot move it.
//
// Hysteresis on both edges: a few consecutive agreeing samples before latching
// either way, so a camera sweeping through the angle cannot flicker the state.
// TIGHTENED after a false positive in gameplay: at one spot on one map the
// orbit camera satisfied both tests at once and the screen was classified as
// the map mid-drive.
//
// That is not a cosmetic misclassification. The map/garage screens hang their
// AER eye offset on a SEPARATE projection site carrying the opposite sign
// (kMapEyeSign, below), so a false map latch flips the offset actually
// rendered while xr::applied_eye() -- recorded at camera build -- keeps
// reporting the gameplay sign. DIBR shift then warps the synthesized eye the WRONG
// WAY, landing it a full 2x disparity from where that eye belongs. Measured
// from a frame dump: each eye oscillating frame to frame by an amount
// proportional to the local disparity (zero in the distant fog and treeline,
// peak-to-peak ~2x disparity on near ground), which reads in the headset as
// wildly exaggerated stereo -- a miniature world -- that jitters.
//
// Both bounds were generous against the pose they came from. The map camera
// measured (1.7, 5.0, -2.1), i.e. r ~ 5.7, against a 60.0 radius; and its pitch
// is not merely near -45 but FIXED there by the screen itself, so a 4 deg
// window was allowing a whole band of ordinary orbit angles. A gameplay camera
// now has to be within 20 units of the world origin AND within 1 deg of the
// exact map pitch -- still 3x the measured radius, but no longer a corridor
// that normal driving can wander into.
constexpr float kMapPitchDeg    = -45.0f;
constexpr float kMapPitchTolDeg = 1.0f;
constexpr float kMapEyeRadius   = 20.0f;
// Asymmetric: ENTERING the map state is the direction that can break gameplay,
// so it needs more agreeing samples than leaving it does. Opening the map is a
// deliberate act and a few extra frames there is invisible; a false latch
// during a drive is not.
constexpr int   kMapConfirmEnter = 10;
constexpr int   kMapConfirmLeave = 4;
std::atomic<bool> g_mapByPose{false};

void update_map_by_pose()
{
    if (!g_gameCamValid.load()) return;
    const float fy = g_gameCamFwd[1].load();
    const float c  = fy < -1.0f ? -1.0f : (fy > 1.0f ? 1.0f : fy);
    const float pitchDeg = std::asin(c) * 57.2957795f;
    const float ex = g_gameCamEye[0].load();
    const float ey = g_gameCamEye[1].load();
    const float ez = g_gameCamEye[2].load();
    const float r  = std::sqrt(ex*ex + ey*ey + ez*ez);

    const bool looksMap = std::fabs(pitchDeg - kMapPitchDeg) <= kMapPitchTolDeg &&
                          r <= kMapEyeRadius;

    static int agree = 0;
    const bool cur = g_mapByPose.load();
    if (looksMap == cur) { agree = 0; return; }
    if (++agree >= (looksMap ? kMapConfirmEnter : kMapConfirmLeave)) {
        agree = 0;
        g_mapByPose.store(looksMap);
        // Logged because a flip during gameplay is a BUG, and this is the line
        // that names it: the pitch and radius that convinced the classifier.
        VRLOG("MAP-BY-POSE -> %s (pitch %.2f deg, eye r=%.1f)",
              looksMap ? "MAP" : "not map", pitchDeg, r);
    }
}
// Extra trim on the locked FOV (multiplies tan, so it composes correctly).
constexpr float kProjFovMul = 1.0f;

// AER EYE OFFSET IN THE PROJECTION MATRIX.
// A lateral eye translation t is EXACTLY expressible here, not approximated:
// with the camera moved by t, clip.x = (x - t)*p00 + ... , i.e. clip.x gains the
// CONSTANT -t*p00 while w is untouched. Adding that constant to the projection's
// x-translation term yields identical clip coords for every vertex, so the
// rasterised image -- including occlusion and depth order -- matches a real
// camera translation.
// Layout: the builder writes 1.0f at +0x2C (float 11 = m[2][3]), i.e. DirectX
// row-vector convention (clip = v * M), so the x-translation term is m[3][0] =
// float index 12.
// Why here: this is the only stage we have found that (a) reaches the render and
// (b) is not overwritten downstream. Caveat: culling/LOD and any shader that uses
// the camera POSITION (specular, SSR, fog) still see the un-offset eye. At +-3 cm
// that is a minor, static error -- not the per-frame collapse we have now.
constexpr bool  kProjEyeOffset = true;
constexpr float kProjEyeSign   = -1.0f;   // flipped 2026-07-30: eyes were inverted at +1.0

// Parity moves here too. The offset VALUE only flipped 60x/s (combine1's sim
// tick) while frames are routed to eyes ~106x/s, so consecutive presents carried
// the SAME baked eye while the router alternated anyway -- which is why the dump
// measured a near-nothing uniform difference instead of stereo. advance_aer_parity
// CASes on g_newFrame, so it flips exactly once per Present even though this hook
// runs ~3x per frame. Latched so combine1 keeps advancing until this has fired,
// and parity can never be orphaned (that froze the headset once already).
std::atomic<bool> g_projOwnsParity{false};

// The engine's exponential camera blend (exe+0xA17F70) is left ALONE.
// Patching it out was tried twice and measured both times: without it the
// camera snaps between 60 Hz sim positions while we present ~106/s, so motion
// reads as 60 fps no matter the frame rate. The damping it applies to head
// rotation is avoided by injecting rotation downstream instead (g_rotateMode
// kRotProj), which costs nothing here. The toggle, its Y-key binding and its
// settings-UI checkbox were removed 2026-08-07 rather than left as a trap.

// Orbit-only: overwrite the stick's elevation with head pitch instead of
// adding to it (see detour_fna). H toggles it live; off falls back to the
// original additive behaviour. Also force-disabled in map/garage (any
// confirmed main-camera relocation, hooks::in_map_view()) regardless of this
// toggle -- those screens aren't a normal orbit-around-the-truck view.
std::atomic<bool> g_horizonLockOn{true};

// The camera builder (exe+0xDA1C20) is the ONLY injection point: it takes the
// head rotation, the head lean and the AER +-IPD/2, for every camera -- cockpit,
// orbit, map, garage and menu alike. Nothing is written at combine2 and nothing
// in the projection matrix.
//
// There were six other modes here, injecting at combine2 and/or the projection
// in various splits, plus lag compensation, an un-damp ring, a world-yaw split,
// a restamp publisher and a cone follower. All of it existed to work around one
// belief: that the camera stage is sim-rate and damped. That is true of
// combine2 -- and was never true of this builder, which nobody had measured.
//
// MEASURED 2026-08-12: it runs ONCE PER RENDERED FRAME (~200/s; the 60/s figure
// that drove the whole effort was g_buildSeq counting combine2, a different
// stage), and its output reaches the render UNDAMPED -- a 24 degree step applied
// there appears in the camera constant buffer one frame later with zero error,
// in the cockpit and in orbit alike.
//
// So there is nothing to work around, and every mechanism those modes
// introduced became unnecessary rather than merely switched off. The camera is
// genuinely rotated, so culling, LOD, view-space effects and the ground markers
// read a correct camera; there is no blend to cancel, no basis to restamp, no
// constant-buffer read and no frame correspondence to get wrong.
// Mode 6 ("Camera builder (fna)") -- the measurement every other mode was
// missing.
//
// Modes 0-5 all exist to work around one belief: that the camera stage is slow
// and damped, so a rotation put there arrives late and quantised. That is true
// of combine2. It is NOT true of exe+0xDA1C20, the look-at builder BOTH the
// cockpit and orbit cameras go through -- which nobody had measured until
// 2026-08-12. It runs ONCE PER RENDERED FRAME (~200/s, not the 60/s that was
// really g_buildSeq counting combine2), and its output reaches the render
// UNDAMPED: a 24 degree step applied there shows up in the camera constant
// buffer one frame later with zero error, in both cameras.
//
// So there is nothing to work around. This mode writes the head rotation AND
// the head translation -- lean and the AER +-IPD/2 -- at fna, and writes nothing
// at combine2 and nothing in the projection. One injection point.
//
// What that buys, all from the same property: the camera is genuinely rotated,
// so culling, LOD, view-space effects and the markers read a correct camera
// instead of a stale or un-rotated one. It is undamped, so there is no blend to
// cancel, no basis to restamp, no constant-buffer read, and no frame
// correspondence to get wrong. It is full rate, so no cone, no yaw window and no
// lag compensation are needed. Every mechanism modes 0-5 introduced becomes
// unnecessary rather than merely switched off.
//
// The eye offset belongs here for a reason that took a wrong answer first: AER
// renders ONE EYE PER FRAME and fna runs ONCE PER FRAME, so each call builds the
// camera for that frame's eye and advances parity in the same breath. Orbit has
// always done exactly this. And an offset applied here moves the real camera, so
// culling and reflections see the true per-eye position -- a projection-matrix
// shear only moves the image.
//
// The truck locks and the stick-look roll removal were briefly lost here and are
// RESTORED. They cannot run at combine2 in this mode -- that stage is AFTER fna,
// so it would be conditioning an already-rotated basis and would flatten the
// head's contribution along with the stick's. They run at fna instead, on the
// game's own look, before the head rotation: the ordering they always needed.
//
// The obstacle looked structural and was not. The levelling reads the truck
// from combine2's r8, which fna is never handed -- but combine2 still runs, so it
// publishes the frame (see truck_frame()) and fna reads the previous build's.
// One sim tick stale, which is nothing for a chassis.
//
// The other half was the reference UP. This builder rebuilds the basis against
// WORLD up, which is orbit's horizon lock and is correct there; for the cockpit
// it discarded the truck's roll and pitch outright. The cockpit now takes the
// game's own r9 up instead, which already carries the truck's tilt.

// Orbit head LEAN through the projection matrix instead of through detour_fna's
// eye/target rewrite. MOVES it, not duplicates -- fna's lean terms are skipped
// while this is on, or the two would double up. The AER +-IPD/2 stays at fna
// either way: that one demonstrably works there (stereo is correct in orbit),
// and it is only the positional lean that appears not to survive.
//
// Hunch worth testing: fna hands the game a modified eye/target pair, and the
// orbit camera may re-derive its eye from its own arm/collision logic, keeping
// our target but discarding our eye -- which would explain rotation working
// (that rides the target) while lean does not (that rides the eye). The
// projection matrix is downstream of all of that and cannot be re-derived away,
// which is exactly why the cockpit lean lives there.

std::atomic<bool> g_truckLevelLock{true};    // config is authoritative
std::atomic<bool> g_truckYawLock{false};


// Head LEAN through the same matrix, so it can be used as a live check that this
// injection point works: lean should move the view immediately.
// ALL THREE AXES are exact here. X and Y are constant clip offsets with w
// untouched. Z was initially (wrongly) assumed inexpressible because w = vz
// changes per vertex -- but that is precisely what the matrix's FOURTH ROW does.
// With m23 = 1.0 (the 3F800000 written at +0x2C) and m33 = 0 (the zeroed edi at
// +0x3C), a camera moved tz along view-Z gives:
//     clip.z' = vz*m22 + (m32 - tz*m22)   ->  m[14] -= tz * m[10]
//     clip.w' = vz     + (-tz)            ->  m[15] -= tz
// Near/far clipping follows correctly too, since w is what gets tested.
constexpr bool  kLeanAtProj     = true;
// Hooked and enabled at startup. (The Scroll Lock runtime toggle that let a
// clean baseline and the hooked state be compared in one session is gone --
// that comparison is long settled, and the toggle was the only thing that
// could ever turn the hook off.)
// FIFTH argument is a float on the STACK ([rsp+0x70] at +0x115421F resolves to
// entry+0x28). A 4-parameter detour left that slot as garbage from its own frame,
// which is why the camera broke even with no writes.
void* g_projTarget = nullptr;

// Downstream sibling view-builder: builds the FINAL view (up rebuilt here,
// after look-angles are folded in). Called at +0xA17F20 (return +0xA17F25).
// Candidate injection point for ROLL, which the earlier stages discard.
constexpr uintptr_t kCombine2Offset = 0x1152AC0;
constexpr uintptr_t kCombine2Ret    = 0xA17CF5;

// TEST: while true, the builder rotation is off and we only test translating
// the eye position at the combine fn.
constexpr bool kTestCombine = true;

// When true, drive the combine from the real HMD pose instead of the T-cycle
// sway test. Rotation is applied as one composed matrix (yaw*pitch*roll) so
// roll rides along with yaw/pitch; the eye is auto-pivoted (C = -eyePos).
constexpr bool  kConnectHead = true;   // true = live HMD; false = flat T-cycle probe
constexpr bool  kUseQuatRotation = true;  // drive cockpit rotation from the full HMD quaternion
constexpr bool  kQuatConj = false;        // conjugate (invert) if rotation comes out reversed
constexpr float kHeadPosScale = 1.0f;   // metres of head motion -> game units

// DIAGNOSTIC: cycle yaw -> pitch -> horizontal-translate sway (flat-visible).
// Set false for live head tracking.
constexpr bool kForceTest = true;

// --- 4x4 (column-vector M*v, memory row-major m[r][c]=f[r*4+c]) ---
struct Mat4 { float m[4][4]; };
Mat4 load4(const float* f){ Mat4 M; for(int r=0;r<4;++r)for(int c=0;c<4;++c)M.m[r][c]=f[r*4+c]; return M; }
void store4(float* f, const Mat4& M){ for(int r=0;r<4;++r)for(int c=0;c<4;++c)f[r*4+c]=M.m[r][c]; }
Mat4 mul4(const Mat4& A, const Mat4& B){ Mat4 C{}; for(int r=0;r<4;++r)for(int c=0;c<4;++c){ float s=0; for(int k=0;k<4;++k)s+=A.m[r][k]*B.m[k][c]; C.m[r][c]=s;} return C; }

float dot3(const float a[3], const float b[3]){ return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
bool  unit3(const float a[3]){ float l=dot3(a,a); return std::fabs(l-1.0f)<0.02f; }

// Full HMD head rotation as a view-space 4x4 (no Euler): quaternion -> matrix,
// then basis-change OpenXR axes into the game's view space (view_x=-o_z,
// view_y=o_y, view_z=o_x, matching the yaw=Y / pitch=Z / roll=X findings).
void build_head_R(Mat4& R){
    float q[4]; xr::head_orientation(q);
    // Negate x,z to reverse pitch (and roll — discarded downstream) while keeping
    // yaw: headset-confirmed the game's pitch is inverted vs the naive mapping.
    q[0]=-q[0]; q[2]=-q[2];
    if (kQuatConj) { q[0]=-q[0]; q[1]=-q[1]; q[2]=-q[2]; }
    float x=q[0],y=q[1],z=q[2],w=q[3];
    float Ro[3][3]={
        {1-2*(y*y+z*z), 2*(x*y-w*z),   2*(x*z+w*y)},
        {2*(x*y+w*z),   1-2*(x*x+z*z), 2*(y*z-w*x)},
        {2*(x*z-w*y),   2*(y*z+w*x),   1-2*(x*x+y*y)}};
    static const float P[3][3]={{0,0,-1},{0,1,0},{1,0,0}};
    float PR[3][3], Rv[3][3];
    for(int i=0;i<3;++i)for(int j=0;j<3;++j){float s=0;for(int k=0;k<3;++k)s+=P[i][k]*Ro[k][j];PR[i][j]=s;}
    for(int i=0;i<3;++i)for(int j=0;j<3;++j){float s=0;for(int k=0;k<3;++k)s+=PR[i][k]*P[j][k];Rv[i][j]=s;}
    R = Mat4{}; for(int r=0;r<3;++r)for(int c=0;c<3;++c)R.m[r][c]=Rv[r][c]; R.m[3][3]=1.0f;
}

void cross3(const float a[3], const float b[3], float o[3]){
    o[0]=a[1]*b[2]-a[2]*b[1]; o[1]=a[2]*b[0]-a[0]*b[2]; o[2]=a[0]*b[1]-a[1]*b[0];
}
void norm3(float v[3]){ float l=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); if(l>1e-6f){v[0]/=l;v[1]/=l;v[2]/=l;} }

void rodrigues(const float a[3], float ang, float R[3][3]){
    float x=a[0],y=a[1],z=a[2],c=std::cos(ang),s=std::sin(ang),t=1-c;
    R[0][0]=c+x*x*t;   R[0][1]=x*y*t-z*s; R[0][2]=x*z*t+y*s;
    R[1][0]=y*x*t+z*s; R[1][1]=c+y*y*t;   R[1][2]=y*z*t-x*s;
    R[2][0]=z*x*t-y*s; R[2][1]=z*y*t+x*s; R[2][2]=c+z*z*t;
}

// Rotate a 3-vector about a unit axis.
void rot_vec(float v[3], const float ax[3], float ang){
    float R[3][3]; rodrigues(ax, ang, R);
    float x=v[0],y=v[1],z=v[2];
    v[0]=R[0][0]*x+R[0][1]*y+R[0][2]*z;
    v[1]=R[1][0]*x+R[1][1]*y+R[1][2]*z;
    v[2]=R[2][0]*x+R[2][1]*y+R[2][2]*z;
}

// eye=0 camera-relative view (axis-swap): rows orthonormal, bottom (0,0,0,1).
bool is_view(const float* f){
    float r0[3]={f[0],f[1],f[2]}, r1[3]={f[4],f[5],f[6]}, r2[3]={f[8],f[9],f[10]};
    return unit3(r0)&&unit3(r1)&&unit3(r2) &&
           std::fabs(f[12])<0.01f && std::fabs(f[13])<0.01f &&
           std::fabs(f[14])<0.01f && std::fabs(f[15]-1.0f)<0.01f;
}

// Live-tunable state. Head look-around is applied in CAMERA-RELATIVE space
// (before the view's axis-swap): the camera-relative axes are X=side, Y=forward
// (depth), Z=up. Yaw about Z, pitch about X. We rotate about `pivot` = the head
// offset from the render origin, tuned live onto the eye.
std::atomic<float> g_yawSign{-1.0f};
std::atomic<float> g_pitchSign{-1.0f};  // game pitch inverted (headset-confirmed)
std::atomic<float> g_rollSign{-1.0f};  // headset-confirmed (roll was reversed)
// Flipped 2026-07-30 alongside the (since-removed) present-routing pipeline
// delay defaulting 1->0. The eye parity is a strict 2-state alternation
// (L,R,L,R...), so an odd->even delay change swapped which physical eye each
// swapchain slot received, GLOBALLY (present_route_eye is shared routing for
// every mode). Cockpit's polarity happened to stay correct
// because kProjEyeSign was ALSO flipped that session (two flips cancel), but this
// sign's only other live call sites (line ~620/628/735) are currently dead code
// for cockpit (gated off by kLeanAtProj/kProjEyeOffset/kIpdAtCombine2), so it is
// now effectively an ORBIT/MENU-ONLY lever -- flipping it here does not touch
// cockpit. If cockpit's injection point ever changes to make those sites live
// again, re-check this comment's premise before trusting the "orbit-only" framing.
std::atomic<float> g_sideSign{1.0f};
std::atomic<float> g_pivotSide{0.0f};  // camrel X
std::atomic<float> g_pivotFwd {0.0f};  // camrel Y (depth/forward)
std::atomic<float> g_pivotUp  {0.0f};  // camrel Z (up)

// combine1's `pos` is NOT world-space XYZ -- it's a vector in the CAB's own
// local basis (index0=forward,1=up,2=side; confirmed live via the U/I/K/L/O/P
// probe), which combine1's downstream transforms by the cab's *base* (pre-head-
// rotation) orientation into world space. combine2 separately rotates the final
// view basis (fwd/up) by head yaw/pitch/roll -- but that rotation NEVER touches
// the position/eye row (see combine2: only out[0..2]/out[12..14] are written).
// So a fixed pos[2] (side) offset does NOT track head yaw: it stays pinned to
// the cab's own right, while the view direction rotates independently -> the
// AER separation axis and the actual gaze diverge as soon as you turn your
// head, which is exactly the "only correct dead-ahead" bug.
//
// Fix: replicate combine2's EXACT rotation sequence (same signs, same order),
// seeded with the CANONICAL local basis (fwd0=(1,0,0), up0=(0,1,0) in pos-index
// space) instead of the real world fwd/up. The result is "where the view's
// right axis ends up, expressed in pos' own local coordinates" -- so adding it
// to pos carries the SAME head rotation combine2 applies to the basis, without
// ever leaving pos' local space (no world/local basis mismatch).
// The cockpit AER offset used to be applied at combine1, in pos' local space,
// with the gaze-right axis RECONSTRUCTED from the head angles alone
// (local_gaze_right below). That reconstruction is incomplete: combine2 applies
// our head rotation on top of the basis the GAME hands it, and that basis
// already carries the game's own stick-driven cockpit look (the "rail" the view
// slides along). So the reconstructed axis is short by exactly the game's look
// rotation, and the eye separation stops being perpendicular to the actual gaze
// as soon as the stick is deflected -> the eyes pick up a vertical disparity
// component and stop fusing. Applying the offset at combine2 instead, along the
// right axis derived from the FINAL basis, needs no reconstruction at all: it
// picks up the game's look and our head rotation together, in world space.
// Also falsified, same reason: combine2's output basis is consumed for rotation;
// its position row does not reach the render camera.
constexpr bool kIpdAtCombine2 = false;
// Flip if L/R comes out reversed in the headset: combine2's world basis and
// pos' local index space are not guaranteed to share handedness.
constexpr float kIpdC2Sign = 1.0f;

void local_gaze_right(const xr::HeadLook& h, float out[3]){
    float fwd[3]={1,0,0}, up[3]={0,1,0};
    if (h.valid) {
        rot_vec(fwd, up, -g_yawSign.load()*h.yaw);
        float right[3]; cross3(up, fwd, right); norm3(right);
        float pitch=g_pitchSign.load()*h.pitch;
        rot_vec(fwd, right, pitch); rot_vec(up, right, pitch);
        rot_vec(up, fwd, g_rollSign.load()*h.roll);
    }
    cross3(fwd, up, out); norm3(out);
}

// Re-express a head POSITION offset, given in play-space axes (X right, Y up,
// Z forward), in the FINAL view frame -- i.e. after the head rotation has been
// applied. In-place.
//
// Why this is needed: for the cockpit the rotation and the lean are injected at
// DIFFERENT stages. detour_combine2 rotates the view basis; detour_projbuild
// then adds the lean as a translation in VIEW space -- which by that point is
// the ROTATED frame. So the composed camera lands at
//     E + (R_game * R_head) * lean
// when the correct VR camera is
//     E + R_game * lean
// with R_head affecting orientation only. The head's position is reported in
// play space (h.ox/oy/oz are relative to the recenter ORIGIN, not to wherever
// the head currently points), so letting R_head rotate it too swings the pivot
// away from the viewer: lean right, then look left, and the view orbits a point
// off to the side instead of turning about your own head.
//
// Applying R_head^-1 here cancels exactly the rotation the projection stage is
// about to re-apply. R_head^-1 * v is just the components of v in the rotated
// basis, so this rebuilds that basis with the SAME rot_vec sequence and sign
// constants detour_combine2 uses (yaw about up, pitch about the new right, roll
// about the new forward) and takes three dot products. Sharing the construction
// is what stops the two from drifting apart.
//
// POSITION ONLY. The AER +-IPD/2 offset is added to the same tx separately and
// must NOT be counter-rotated -- an eye genuinely does sit left/right of the
// head in the HEAD's frame, so that one is correct to ride the head rotation.
// Orbit needs no equivalent call: it still has its pre-rotation basis vectors
// in scope and projects the lean onto those directly (see detour_fna).


// Fills yaw, pitch (radians) and transX (camrel-X translation, metres).
void get_transform(float& yaw, float& pitch, float& transX){
    yaw = pitch = transX = 0.0f;
    if (kForceTest) {
        DWORD ms = GetTickCount();
        float s = std::sin(ms * 0.0016f);
        int phase = (ms / 4000) % 3;     // 4s per phase
        if      (phase == 0) yaw    = 0.4f * s;
        else if (phase == 1) pitch  = 0.3f * s;
        else                 transX = 3.0f * s;   // big, to check if it shows at all
        static std::atomic<int> last{-1};
        if (last.exchange(phase) != phase)
            VRLOG("force phase %d = %s", phase, phase==0?"YAW":phase==1?"PITCH":"H-TRANSLATE");
        return;
    }
    xr::HeadLook h = xr::head_look();
    if (!h.valid) return;
    yaw   = g_yawSign.load()   * h.yaw;
    pitch = g_pitchSign.load() * h.pitch;
}

// W = T(pivot) * [pitch about camrel-X, yaw about camrel-Z] * T(-pivot),
// in camera-relative space. newView = V * W.
Mat4 camrel_W(const float pivot[3], float yaw, float pitch){
    const float axZ[3]={0,0,1}, axX[3]={1,0,0};
    float Ry[3][3], Rp[3][3], R[3][3];
    rodrigues(axZ, yaw, Ry);
    rodrigues(axX, pitch, Rp);
    for(int r=0;r<3;++r)for(int c=0;c<3;++c){ float s=0; for(int k=0;k<3;++k)s+=Rp[r][k]*Ry[k][c]; R[r][c]=s; }
    Mat4 W{};
    for(int r=0;r<3;++r){ for(int c=0;c<3;++c) W.m[r][c]=R[r][c];
        W.m[r][3]=pivot[r]-(R[r][0]*pivot[0]+R[r][1]*pivot[1]+R[r][2]*pivot[2]); }
    W.m[3][3]=1.0f;
    return W;
}

using PFN = void*(*)(void*, void*, void*, void*);
PFN real_builder = nullptr;

// Every unrecognised eye-origin view-build caller, logged once each. That log
// line is the part that has repeatedly been worth having; the SELECTOR that
// used to claim one of these sites is gone, and its answer is worth not
// re-deriving:
//
// TESTED 2026-08-20 -- the map, garage and menu cameras are NOT among these. A
// settings-UI selector was wired to this and every discovered site was claimed
// in turn while sitting in the garage; none changed the view, and one visibly
// swung the shadows. So the injection genuinely writes at a claimed site, and
// simply none of them builds those screens' camera. Combined with isCam being a
// tautology for kOrbitCamGame below (its three conditions are exhaustive),
// those screens are already injected, at the right site, on every frame.
//
// Which places the residual drag they show DOWNSTREAM of our write -- the
// engine consuming that camera late or damped on those screens, the one thing
// the 2026-08-12 "undamped, once per rendered frame" measurement never covered
// (it was cockpit and orbit only). Left there: the reference-pose fix in
// xr_mirror.cpp reduced it to a small residual and the search was called off.
// rendered_cam_rot() vs the forward written here would measure what is left.
uintptr_t g_sites[64]; std::atomic<int> g_nSites{0};

// Per-call-site rate census for the view builder. combine runs at exactly 60/s
// in cockpit while the view matrix reaching the GPU changes ~95-120/s, so some
// stage downstream of combine recomputes the camera every render frame. This
// finds it: the site whose call rate tracks the present rate (and whose output is
// camera-relative, i.e. eye at origin) is the per-frame stage, and therefore
// where the AER eye offset has to be applied so it is not interpolated away
// between two sim ticks carrying opposite offsets.
// AER at the PER-RENDER-FRAME camera stage.
//
// Census result (cockpit, presents=106/s, renderCamMoved=102/s): the main camera
// site +0xA17C2B runs at exactly 60/s (the sim tick), while +0xABE7C5 and
// +0xD6B25B run at 106/s -- the present rate -- and both emit camera-relative
// views (eye at origin), which is the main render view's signature. So the engine
// recomputes the render camera per frame downstream of combine.
//
// Injecting ±IPD/2 at combine was therefore wrong twice over:
//   1. the offset is baked at 60 Hz and then interpolated, so a render frame
//      lands between two sim ticks carrying OPPOSITE offsets -- often near zero,
//      collapsing stereo separation and making it wobble frame to frame;
//   2. parity is advanced there too, so g_eyeSide only flips 60 times/s while
//      ~106 frames are rendered -- consecutive frames reuse the same eye.
// Both have to happen at the per-frame stage instead. The offset is a view-space
// X translation (T_view * V), which is exactly the transX path already here.
// FALSIFIED: translation at the per-frame builder sites has NO effect. Those
// outputs are ROTATION ONLY -- is_view() requires a (0,0,0,1) bottom row and the
// recon notes record the main render view as eye-at-origin with the eye
// "combined downstream at render time". The engine reads only the rotation, so a
// baked-in translation is discarded. Same reason translating at combine2 produced
// identical eyes. Verified by moving head LEAN here too: lean did nothing either.
// Left in place (disabled) because the census data behind it is still valid --
// the per-frame stage is real, it just is not where the eye POSITION lives.
constexpr bool      kEyeOffsetAtBuilder = false;
constexpr uintptr_t kEyeOffsetSite      = 0xABE7C5;   // other candidate: 0xD6B25B
constexpr float     kEyeBuilderSign     = 1.0f;       // flip if L/R is reversed

// Did the injection actually fire, and with what offset? "Both eyes identical"
// is exactly what a NEVER-MATCHING site looks like (no parity advance -> no eye
// flip -> eye_side_offset() stays 0), so this has to be measured, not assumed.
std::atomic<int>   g_eyeInjects{0};
std::atomic<float> g_lastEyeOff{0.0f};

// Site probe: applies a big CONSTANT view-space offset at one candidate site at a
// time, cycling every 2.5 s and logging which is active. Whichever site makes the
// desktop image visibly jump sideways is the one that actually feeds the render
// camera. Same call-site-cycling technique that originally found kMainCallSite.
// Set false once the site is known.
constexpr bool  kSiteProbe     = false;  // cycling probe: destructive, off
constexpr float kSiteProbeOff  = 0.35f;   // metres; deliberately unmissable
std::atomic<uintptr_t> g_probeSite{0};

// Head LEAN is applied at the same site, and on the same axes, as the IPD -- so
// leaning left/right exercises exactly the axis the eye separation uses. If lean
// moves the view, this site and this axis are load-bearing for stereo too; if it
// does not, the site is wrong and no amount of IPD tuning will help. Flip a sign
// if an axis moves the wrong way.
constexpr float kLeanSignX = 1.0f;   // head right  -> view-space X (the IPD axis)
constexpr float kLeanSignY = 1.0f;   // head up
constexpr float kLeanSignZ = 1.0f;   // head back

// Set once the builder site has actually fired. Until then combine keeps
// advancing parity, so a site that never matches can no longer orphan the eye
// flip -- which is what froze the headset (buildCalls dropped to 0/s and every
// Present was skipped as stale).
std::atomic<bool> g_builderOwnsParity{false};

constexpr bool kBuilderCensus = false;  // data gathered; off (was ~90k bumps/s)
constexpr int  kMaxSites = 24;
struct SiteRec { std::atomic<uintptr_t> site{0}; std::atomic<int> calls{0}, views{0}; };
SiteRec g_siteRec[kMaxSites];

void census(uintptr_t site, bool isViewMat)
{
    for (int i = 0; i < kMaxSites; ++i) {
        uintptr_t s = g_siteRec[i].site.load(std::memory_order_relaxed);
        if (s == site) {
            g_siteRec[i].calls.fetch_add(1, std::memory_order_relaxed);
            if (isViewMat) g_siteRec[i].views.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (s == 0) {
            uintptr_t expect = 0;
            if (g_siteRec[i].site.compare_exchange_strong(expect, site)) {
                g_siteRec[i].calls.fetch_add(1, std::memory_order_relaxed);
                if (isViewMat) g_siteRec[i].views.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            --i;   // someone else claimed it; re-examine this slot
        }
    }
}

void* detour_builder(void* rcx, void* rdx, void* r8, void* r9){
    void* raw_ret = _ReturnAddress();
    void* ret = real_builder(rcx, rdx, r8, r9);
    __try {
        float* m = reinterpret_cast<float*>(rcx);
        if (kBuilderCensus && m)
            census(reinterpret_cast<uintptr_t>(raw_ret) - g_moduleBase, is_view(m));
        if (m && is_view(m)) {
            uintptr_t site = reinterpret_cast<uintptr_t>(raw_ret) - g_moduleBase;

            // Per-render-frame AER: advance parity AND apply the eye offset here,
            // downstream of the interpolation, so the offset survives intact and
            // the eye alternates once per rendered frame rather than per sim tick.
            if (kSiteProbe && site == g_probeSite.load()) {
                Mat4 V = load4(m);
                Mat4 T{}; for (int i = 0; i < 4; ++i) T.m[i][i] = 1.0f;
                T.m[0][3] = kSiteProbeOff;
                store4(m, mul4(T, V));
                g_eyeInjects.fetch_add(1);
            }
            if (!kSiteProbe && kEyeOffsetAtBuilder && kConnectHead && site == kEyeOffsetSite) {
                g_builderOwnsParity.store(true);
                xr::advance_aer_parity();   // first call per Present flips the eye
                xr::note_camera_active();
                const float off = kEyeBuilderSign * xr::eye_side_offset() * kHeadPosScale;
                xr::record_applied_eye_offset(off, xr::kEyeAppliedAsCameraMove);
                g_eyeInjects.fetch_add(1);
                g_lastEyeOff.store(off);

                // Eye separation AND head lean, same site, same axes.
                float tx = off, ty = 0.0f, tz = 0.0f;
                xr::HeadLook h = xr::head_look();
                if (h.valid) {
                    tx += kLeanSignX * h.ox * kHeadPosScale;
                    ty += kLeanSignY * h.oy * kHeadPosScale;
                    tz += kLeanSignZ * h.oz * kHeadPosScale;
                }
                if (tx != 0.0f || ty != 0.0f || tz != 0.0f) {
                    Mat4 V = load4(m);
                    Mat4 T{}; for (int i = 0; i < 4; ++i) T.m[i][i] = 1.0f;
                    T.m[0][3] = tx; T.m[1][3] = ty; T.m[2][3] = tz;
                    store4(m, mul4(T, V));
                }
            }

            if (site == kMainCallSite && !kTestCombine) {
                float yaw, pitch, transX; get_transform(yaw, pitch, transX);
                if (transX != 0.0f) {
                    // Stereo/positional shift in VIEW space: newView = T_view * V.
                    Mat4 V = load4(m);
                    Mat4 T{}; for(int i=0;i<4;++i) T.m[i][i]=1.0f; T.m[0][3]=transX;
                    store4(m, mul4(T, V));
                } else if (yaw != 0.0f || pitch != 0.0f) {
                    float pivot[3] = {g_pivotSide.load(), g_pivotFwd.load(), g_pivotUp.load()};
                    store4(m, mul4(load4(m), camrel_W(pivot, yaw, pitch)));  // V * W
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return ret;
}

// --- camera combine hook: rotate the orientation (r8) about the eye and/or
// translate the eye position (rdx). This is where both live. ---
using PFN4 = void*(*)(void*, void*, void*, void*);
PFN4 real_combine = nullptr;

// Test mode cycled with T: 0=yaw 1=pitch 2=roll 3=slideFwd 4=slideUp 5=slideSide.
std::atomic<int> g_mode{0};

// Tunable eye-position offset (pos axes: [0]=forward,[1]=up,[2]=side) to move
// the eye/pivot onto the head. U/I forward, K/L up, O/P side.
std::atomic<float> g_posFwd{0.0f}, g_posUp{0.0f}, g_posSide{0.0f};

// Roll applied to the OUTPUT view matrix (about row0, the view-space forward;
// rows 0 and 3 rotate). The input orientation's roll is re-levelled downstream,
// so roll has to ride on the output -- headset-confirmed.
std::atomic<float> g_rollPending{0.0f};

void* detour_combine(void* rcx, void* rdx, void* r8, void* r9){
    void* raw_ret = _ReturnAddress();
    bool  isMain = false;
    float rollOut = 0.0f;   // roll (rad) to apply to the OUTPUT view after combine
    __try {
        isMain = (reinterpret_cast<uintptr_t>(raw_ret) - g_moduleBase == kCombineRet);
        if (isMain) {
            // Parity now advances at the per-frame builder stage (see
            // kEyeOffsetAtBuilder); advancing here too would let whichever ran
            // first consume the flag, reintroducing the 60 Hz eye rate.
            if ((!kEyeOffsetAtBuilder || !g_builderOwnsParity.load()) &&
                !(kProjEyeOffset && g_projOwnsParity.load()))
                xr::advance_aer_parity();
            xr::note_camera_active();   // lets Present detect "no camera" (menu/loading)
            float* pos = reinterpret_cast<float*>(rdx);
            if (kConnectHead) {
                xr::HeadLook h = xr::head_look();
                if (r8 && pos) {
                    // Rotation is at combine2; eye POSITION (lean + AER ±IPD/2) here.
                    // NOTE: the game lerps this eye downstream -> the alternating
                    // ±IPD/2 shakes (lean is slow so it's fine). Interim baseline;
                    // clean fix TBD (post-lerp injection or disabling the eye lerp).
                    float sc=kHeadPosScale;
                    if (h.valid && !kEyeOffsetAtBuilder) {
                        if (!kLeanAtProj) {   // all three axes now go to the projection
                            pos[0] += -h.oz*sc;
                            pos[1] += h.oy*sc;
                            pos[2] += g_sideSign.load()*h.ox*sc;
                        }
                    }
                    // AER +-IPD/2 along the head-rotated right axis, expressed in
                    // pos' OWN local (fwd,up,side) space via local_gaze_right() —
                    // see its comment for why a plain pos[2] offset (or a world-
                    // space axis) breaks off-centre. -g_sideSign keeps L/R
                    // handedness consistent with orbit (fna).
                    float eyeOff = -g_sideSign.load()*xr::eye_side_offset()*sc;
                    // Ground truth for which eye THIS frame is being built for.
                    // Recorded at the builder stage instead when the offset is
                    // applied there -- eye identity must travel with the frame
                    // that actually carries the offset.
                    if (!kEyeOffsetAtBuilder && !kProjEyeOffset)
                        xr::record_applied_eye_offset(eyeOff, xr::kEyeAppliedAsCameraMove);
                    if (!kIpdAtCombine2 && !kEyeOffsetAtBuilder && !kProjEyeOffset) {
                        float rgt[3]; local_gaze_right(h, rgt);
                        pos[0] += rgt[0]*eyeOff; pos[1] += rgt[1]*eyeOff; pos[2] += rgt[2]*eyeOff;
                    }
                    (void)rollOut;
                }
            } else {
                int mode = g_mode.load();
                float ramp = (GetTickCount() % 3000) / 3000.0f;   // sawtooth 0->1
                if (mode == 2) {              // ROLL test -> output (post-combine)
                    rollOut = 0.6f*ramp;
                } else if (mode < 2 && r8) {  // yaw / pitch on input orientation
                    const float axY[3]={0,1,0}, axZ[3]={0,0,1};
                    float R3[3][3];
                    rodrigues(mode==0?axY:axZ, 0.6f*ramp, R3);
                    Mat4 R{}; for(int r=0;r<3;++r)for(int c=0;c<3;++c)R.m[r][c]=R3[r][c]; R.m[3][3]=1.0f;
                    float* mat = reinterpret_cast<float*>(r8);
                    store4(mat, mul4(R, load4(mat)));
                    if (pos) {
                        float C[3]={ -pos[0]+g_posFwd.load(), -pos[1]+g_posUp.load(), -pos[2]+g_posSide.load() };
                        for(int i=0;i<3;++i)
                            pos[i] += C[i] - (R3[i][0]*C[0]+R3[i][1]*C[1]+R3[i][2]*C[2]);
                    }
                } else if (mode >= 3 && pos) {
                    pos[mode-3] += 2.0f * ramp;   // 3=fwd,4=up,5=side
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    // Roll can't be applied here (up is rebuilt downstream); carry it to the
    // final view-builder hook (detour_combine2).
    if (isMain) g_rollPending.store(rollOut);
    return real_combine(rcx, rdx, r8, r9);
}


// Truck-relative look locks, applied to the final cockpit view basis before the
// head rotation goes on top.
//
// r8 ([rsp+70], the stage-1 orientation) is the VEHICLE's own basis, before the
// stick look is folded in. Confirmed empirically 2026-08-06 with a read-only
// probe (docs/cockpit_camera.md records the numbers):
//   row0 = truck forward, row1 = truck up, row2 = truck right, row3 = position
// Driving over a hill with the stick centred left dot(viewFwd, row0..2) EXACTLY
// constant (0.9988 / -0.0499 / -0.0000) while the world-space view direction
// swung ~25 deg -- i.e. the basis rotates rigidly with the truck. Moving the
// look stick on flat ground moved those same dots -- i.e. the stick look is NOT
// in this basis. Row identity from the same run: dot(row1) went +0.1495 looking
// up and -0.3545 looking down while dot(row2) stayed ~0.
//
// Two optional locks plus one unconditional correction, all expressed by
// decomposing the view direction into that basis (a = forward, b = up,
// c = right components) and rebuilding it:
//
//   ROLL       -- always removed. The stick look carries a slight roll with it;
//                 rebuilding up from the truck's own up drops it. The truck's
//                 real body roll survives, because that roll IS tUp. Head roll
//                 is added afterwards by the caller, so it still works.
//   level lock -- drop the truck-UP component    (stick pitch stops contributing)
//   yaw  lock  -- collapse the horizontal direction onto truck-FORWARD, keeping
//                 its magnitude (stick yaw stops contributing)
//
// The yaw lock is NOT a plain "project out the right component". That was the
// first implementation and it snapped the view backwards once the hidden stick
// yaw passed 90 deg: projecting onto the plane perpendicular to truck-right
// leaves a vector that may point along EITHER +forward or -forward, and past
// 90 deg it is the latter. Taking a = sqrt(a*a + c*c) keeps the horizontal
// magnitude (cos of the pitch) while forcing it onto +forward, so the result
// is continuous through the full 360 deg of stick yaw.
//
// Consequence: unlike the level lock, the yaw lock DOES depend on row0's sign.
// The probe pinned it -- dot(viewFwd, row0) = 0.9988 at rest, i.e. row0 points
// forward. If a future game build flipped that, the yaw lock would face the
// view permanently backwards (and nothing else here would change).
//
// NOTE: the stick-centred resting value measured -0.0499 (~2.9 deg nose-down),
// not 0 -- so the level lock also removes whatever fixed downward tilt the
// cockpit view rests at, exactly as orbit's horizon lock fully overwrites
// elevation rather than merely re-centring it.
// --- cockpit roll diagnostic ------------------------------------------------
//
// The stick's cockpit look adds a slight roll at the outer positions.
// apply_truck_frame_basis() removes it unconditionally, and the image still
// rolls -- in EVERY rotation mode, which is what makes it worth measuring rather
// than guessing at. Three numbers locate where it survives:
//
//   game      the roll the engine's own look had, measured before removal
//   written   the roll of the basis we hand back at combine2
//   rendered  the roll actually in the camera constant buffer
//
// written ~= rendered  -> combine2's up row DOES reach the render, so the roll
//                         we see is coming from somewhere we are not looking
//   game  ~= rendered
//   with written ~= 0  -> the render ignores combine2's up row and derives its
//                         own; the roll is applied downstream of us and has to
//                         be cancelled at the projection instead
//
// All three are measured about the same forward against the same reference (the
// truck's up), so they are directly comparable.
SRWLOCK g_rollLock = SRWLOCK_INIT;
float   g_truckUpW[3]   = {0.0f, 1.0f, 0.0f};
float   g_writtenUpW[3] = {0.0f, 1.0f, 0.0f};
bool    g_truckUpValid  = false;
bool    g_writtenValid  = false;
std::atomic<float> g_rollGameDeg{0.0f};
std::atomic<float> g_rollDerolledDeg{0.0f};


// Signed roll of `up` about `fwd`, measured from the reference up `ref`.
// Positive is the right-handed sense about fwd, matching rot_vec(). False if
// either vector is degenerate against fwd, in which case there is no roll to
// speak of and the caller should leave its last reading alone.
bool roll_about(const float fwd[3], const float up[3], const float ref[3], float& outRad)
{
    float r[3] = {ref[0], ref[1], ref[2]};
    const float d = dot3(r, fwd);
    for (int i = 0; i < 3; ++i) r[i] -= fwd[i]*d;
    const float rl = std::sqrt(dot3(r, r));
    if (rl < 1.0e-3f) return false;
    for (int i = 0; i < 3; ++i) r[i] /= rl;

    float u[3] = {up[0], up[1], up[2]};
    const float du = dot3(u, fwd);
    for (int i = 0; i < 3; ++i) u[i] -= fwd[i]*du;
    const float ul = std::sqrt(dot3(u, u));
    if (ul < 1.0e-3f) return false;
    for (int i = 0; i < 3; ++i) u[i] /= ul;

    float cr[3]; cross3(r, u, cr);
    outRad = std::atan2(dot3(cr, fwd), dot3(r, u));
    return true;
}

// The truck's own frame, published by combine2 so detour_fna can use it.
//
// combine2 runs AFTER fna within a camera update, so fna reads the PREVIOUS
// build's frame -- one sim tick stale. For a truck's orientation that is
// nothing (it is the head rotation that moves fast, not the chassis), and the
// alternative is not having it at all: fna is handed eye/target/up and no truck
// pointer, which is what made this look unsolvable when mode 6 was first built.
SRWLOCK g_truckFrameLock = SRWLOCK_INIT;
float   g_truckFrame[3][3] = {};      // rows: forward, up, right
bool    g_truckFrameValid  = false;

bool truck_frame(float f[3], float u[3], float r[3])
{
    bool ok = false;
    AcquireSRWLockShared(&g_truckFrameLock);
    if (g_truckFrameValid) {
        for (int i = 0; i < 3; ++i) {
            f[i] = g_truckFrame[0][i];
            u[i] = g_truckFrame[1][i];
            r[i] = g_truckFrame[2][i];
        }
        ok = true;
    }
    ReleaseSRWLockShared(&g_truckFrameLock);
    return ok;
}

// Condition the game's own look against the truck: strip the stick-look roll
// always, and drop the stick's pitch and/or yaw when their locks are on so the
// HEAD becomes the only source on that axis. The truck's real tilt on hills is
// preserved throughout -- that is the whole point of levelling against the
// TRUCK rather than the world.
//
// Takes the truck's basis directly rather than a combine2 pointer, so mode 6
// can call it at fna where the frame arrives via truck_frame(). Must run
// BEFORE the head rotation at whichever stage calls it, or it would flatten the
// head's contribution along with the stick's.
void apply_truck_frame_basis(const float tFwd[3], const float tUp[3],
                             const float tRgt[3], float fwd[3], float up[3])
{
    const bool lockLevel = g_truckLevelLock.load();
    const bool lockYaw   = g_truckYawLock.load();

    // Diagnostic: the roll the engine's own look arrived with. Taken here, on
    // the INCOMING basis, so an early return below leaves derolled == game --
    // which is itself the reading that says the removal never ran.
    float gameRoll;
    if (roll_about(fwd, up, tUp, gameRoll)) {
        g_rollGameDeg.store(gameRoll * 57.2957795f);
        g_rollDerolledDeg.store(gameRoll * 57.2957795f);
    }
    AcquireSRWLockExclusive(&g_rollLock);
    for (int i = 0; i < 3; ++i) g_truckUpW[i] = tUp[i];
    g_truckUpValid = true;
    ReleaseSRWLockExclusive(&g_rollLock);

    float a = fwd[0]*tFwd[0] + fwd[1]*tFwd[1] + fwd[2]*tFwd[2];
    float b = fwd[0]*tUp [0] + fwd[1]*tUp [1] + fwd[2]*tUp [2];
    float c = fwd[0]*tRgt[0] + fwd[1]*tRgt[1] + fwd[2]*tRgt[2];

    if (lockYaw)   { a = std::sqrt(a*a + c*c); c = 0.0f; }   // see the 90-deg note above
    if (lockLevel) { b = 0.0f; }

    float f[3];
    for (int i = 0; i < 3; ++i) f[i] = a*tFwd[i] + b*tUp[i] + c*tRgt[i];
    const float fl = std::sqrt(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);
    if (fl < 1.0e-3f) return;   // view lay entirely along a removed axis; leave it alone
    for (int i = 0; i < 3; ++i) f[i] /= fl;

    // Roll removal (unconditional): up = the truck's up, re-orthogonalised
    // against the final forward. With the level lock on, f is already
    // perpendicular to tUp and this returns tUp itself; with only the yaw lock,
    // it returns the correctly pitched up; with neither, it strips the stick's
    // roll from an otherwise untouched view direction.
    float u[3] = {tUp[0], tUp[1], tUp[2]};
    const float du = u[0]*f[0] + u[1]*f[1] + u[2]*f[2];
    for (int i = 0; i < 3; ++i) u[i] -= f[i]*du;
    const float ul = std::sqrt(u[0]*u[0] + u[1]*u[1] + u[2]*u[2]);
    if (ul < 1.0e-3f) return;
    for (int i = 0; i < 3; ++i) u[i] /= ul;

    // What the removal actually achieved. ~0 means it ran and did its job; a
    // value tracking `game` means it bailed out above and the up was never
    // touched.
    float derolled;
    if (roll_about(f, u, tUp, derolled))
        g_rollDerolledDeg.store(derolled * 57.2957795f);

    for (int i = 0; i < 3; ++i) { fwd[i] = f[i]; up[i] = u[i]; }
}

// Extract the truck frame from combine2's r8 and publish it. Called on EVERY
// cockpit build, including mode 6's -- that mode does no conditioning here, but
// fna still needs the frame, and this is the only place it is available.
bool publish_truck_frame(const void* r8, float tFwd[3], float tUp[3], float tRgt[3])
{
    if (!r8) return false;
    const float* m = reinterpret_cast<const float*>(r8);
    tFwd[0]=m[0];  tFwd[1]=m[1];  tFwd[2]=m[2];    // row0 = truck forward
    tUp [0]=m[4];  tUp [1]=m[5];  tUp [2]=m[6];    // row1 = truck up
    tRgt[0]=m[8];  tRgt[1]=m[9];  tRgt[2]=m[10];   // row2 = truck right
    norm3(tFwd); norm3(tUp); norm3(tRgt);
    AcquireSRWLockExclusive(&g_truckFrameLock);
    for (int i = 0; i < 3; ++i) {
        g_truckFrame[0][i] = tFwd[i];
        g_truckFrame[1][i] = tUp[i];
        g_truckFrame[2][i] = tRgt[i];
    }
    g_truckFrameValid = true;
    ReleaseSRWLockExclusive(&g_truckFrameLock);
    return true;
}

// (A blend-object stack hunter lived here: it walked the caller's frame looking
// for the object exe+0xA17F76's camera blend writes to, since `rbx` there is NOT
// the DRIVE_CAMERA camera_hook publishes -- contradicting docs/cockpit_camera.md.
// Its finding is recorded; the tool itself went with its UI toggle and is not
// worth carrying into a release.)


// Downstream final view-builder (+0x11527A0). Its output has the rebuilt up, so
// rolling its basis here should survive to the render.
PFN4 real_combine2 = nullptr;
void* detour_combine2(void* rcx, void* rdx, void* r8, void* r9){
    void* raw_ret = _ReturnAddress();
    bool isMain = false;
    __try { isMain = (reinterpret_cast<uintptr_t>(raw_ret) - g_moduleBase == kCombine2Ret); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    // (Reasserting the FOV here was tried and measurably did NOT help -- the
    // rendered FOV still swung while settings+0x60 held our value, because that
    // field is only an INPUT to the projection builder. Removed rather than left
    // as a per-build write on the camera thread.)

    void* ret = real_combine2(rcx, rdx, r8, r9);

    __try {
        if (isMain && rcx) {
            // FULL cockpit head rotation applied here on the final view basis.
            // OUT layout: row0=forward, row1=position(eye), row2=params, row3=up.
            // Rotate forward/up intrinsically about the current axes (yaw about up,
            // pitch about right, roll about forward) — pivots about the eye (row1
            // untouched); renderer derives right from forward x up.
            float* out = reinterpret_cast<float*>(rcx);

            xr::HeadLook h = xr::head_look();

            // The truck frame is deliberately NOT gated on h.valid. It is a
            // camera-behaviour setting in its own right, not a head-tracking
            // feature -- and orbit's horizon lock is already outside its own
            // h.valid check in detour_fna, so gating this would make the
            // cockpit behave differently from its sibling. It also made the
            // feature impossible to test on the desktop without a headset,
            // which is how it gets checked first.
            // Cockpit only: orbit has its own equivalent at detour_fna, and
            // both running would fight.
            const bool cockpit = hooks::logic_mode() >= 0.75f;

            if (h.valid || cockpit) {
                float fwd[3]={out[0],out[1],out[2]}, up[3]={out[12],out[13],out[14]};
                norm3(fwd); norm3(up);

                // BEFORE the head rotation, so head pitch/yaw becomes the only
                // source on whichever axis is locked (same absolute-not-
                // additive semantics as orbit's horizon lock) and head roll is
                // added on top of a de-rolled base. Runs unconditionally in
                // cockpit because roll removal is not optional.
                // Mode 6 does its conditioning at fna instead -- BEFORE the head
                // rotation, which is the ordering the truck locks need and which
                // this stage can no longer provide once fna has already rotated
                // the camera. The frame is still published from here either way:
                // combine2's r8 is the only place it is available.
                // Publish only. The conditioning happens at fna, before the
                // head rotation -- combine2 runs after it, so by here the basis
                // already carries the rotation and flattening anything would
                // take the head's contribution with the stick's.
                if (cockpit) {
                    float tf[3], tu[3], tr[3];
                    publish_truck_frame(r8, tf, tu, tr);
                }

                // Skipped entirely when the rotation is being folded into the
                // projection instead (g_rotateAtProj) -- doing both would rotate
                // the view twice. The truck frame above still runs either way:
                // it conditions the GAME's own basis and has nothing to do with
                // where the head rotation is injected.
                // NOTHING is written here. Mode 6 puts the head rotation at the
                // camera builder, which runs before this stage -- so by the time
                // combine2 sees the basis it already carries the rotation, and
                // adding one here would double it.
                //
                // Modes 0-5 all wrote here and all fought the same thing: the
                // engine's camera blend damps whatever this stage produces, at
                // the sim rate. Their entire machinery -- lag compensation, the
                // un-damp ring, the world-yaw split, the restamp, the cone --
                // existed to work around that, and none of it is needed once the
                // rotation goes in one stage earlier. See kRotFna.
                // Publish the up we are about to hand over, in world space, so
                // the report can compare it against the camera buffer's own up.
                // That comparison is HEAD-INDEPENDENT -- both vectors carry the
                // same head rotation, so it stays valid whatever the head is
                // doing, which a roll measured against the truck does not.
                if (cockpit) {
                    AcquireSRWLockExclusive(&g_rollLock);
                    for (int i = 0; i < 3; ++i) g_writtenUpW[i] = up[i];
                    g_writtenValid = true;
                    ReleaseSRWLockExclusive(&g_rollLock);
                }

                out[0]=fwd[0]; out[1]=fwd[1]; out[2]=fwd[2];
                out[12]=up[0]; out[13]=up[1]; out[14]=up[2];

                // Poke IN-FRAME as well as at Present. The Present hook fires
                // at the end of the frame, by which time the game has long
                // since recomputed its camera -- a write there is overwritten
                // before anything can read it. Here we are inside the camera
                // build, so a poked value has a chance of being consumed.
            


                // AER ±IPD/2 along the FINAL gaze-right axis (see kIpdAtCombine2).
                // fwd/up here are post-rotation and already include the game's own
                // cockpit look, so this axis is correct at any stick deflection.
                // cross order matches local_gaze_right() so the offset sign carries
                // over unchanged. Row1 (out[4..6]) is the eye position; rotation
                // above pivots about it, so translating it here is independent.
                if (kIpdAtCombine2 && !kEyeOffsetAtBuilder) {
                    float rgtW[3]; cross3(fwd, up, rgtW); norm3(rgtW);
                    float eyeOff = kIpdC2Sign * -g_sideSign.load()
                                 * xr::eye_side_offset() * kHeadPosScale;
                    out[4] += rgtW[0]*eyeOff;
                    out[5] += rgtW[1]*eyeOff;
                    out[6] += rgtW[2]*eyeOff;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return ret;
}

// Projection-builder hook. Observe-only until the main-camera caller is known.
// Signature read off the disassembly: rcx = out 4x4, xmm1 = fovY (halved at
// +0x11541BA), xmm2 = aspect, xmm3 = near. Declaring the floats is MANDATORY --
// with an all-pointer signature MSVC never forwards xmm1..xmm3 and the game gets
// whatever the detour left in them. That is not "observe-only", it is corruption.
using PFN_PROJ = void*(*)(void*, float, float, float, float);
PFN_PROJ real_projbuild = nullptr;
void* detour_projbuild(void* rcx, float fovY, float aspect, float nearZ, float farZ){
    void* raw_ret = _ReturnAddress();
    void* ret = real_projbuild(rcx, fovY, aspect, nearZ, farZ);
    __try {
        if (rcx) {
            float* m = reinterpret_cast<float*>(rcx);
            const uintptr_t site = reinterpret_cast<uintptr_t>(raw_ret) - g_moduleBase;
            const float p00 = m[0], p11 = m[5];   // +0x00 and +0x14

            // Log each distinct caller once, with the FOV it produced, so the
            // main camera can be told apart from shadow/reflection projections.
            bool known = false;
            const int n = g_projSiteCount.load();
            for (int i = 0; i < n && i < kMaxProjSites; ++i)
                if (g_projSite[i].load() == site) { known = true; break; }
            if (!known && n < kMaxProjSites && p00 > 0.0001f) {
                g_projSite[n].store(site);
                g_projSiteFov[n].store(2.0f*std::atan(1.0f/p00)*57.2957795f);
                g_projSiteCount.store(n + 1);
                VRLOG("PROJ caller[%d] exe+0x%llX -> p00=%.6f p11=%.6f "
                      "(hfov=%.2f vfov=%.2f deg)",
                      n, (unsigned long long)site, p00, p11,
                      2.0f*std::atan(1.0f/p00)*57.2957795f,
                      2.0f*std::atan(1.0f/p11)*57.2957795f);
            }

            // The list above logs each site once for the whole session, so a
            // site first seen during startup never reappears -- which is why
            // opening the map produced no new line even though its camera has
            // to be built by SOMETHING. Keep a second list, scoped to
            // in_map_view(), so the map/garage builder is named even when it is
            // a site we already know from gameplay.
            if (hooks::in_map_view() && p00 > 0.0001f) {
                static std::atomic<uintptr_t> seenMap[16];
                static std::atomic<int> nSeenMap{0};
                bool knownMap = false;
                const int nm = nSeenMap.load();
                for (int i = 0; i < nm && i < 16; ++i)
                    if (seenMap[i].load() == site) { knownMap = true; break; }
                if (!knownMap && nm < 16) {
                    seenMap[nm].store(site);
                    nSeenMap.store(nm + 1);
                    VRLOG("PROJ caller (MAP/GARAGE)[%d] exe+0x%llX -> hfov=%.2f "
                          "vfov=%.2f deg   %s",
                          nm, (unsigned long long)site,
                          2.0f*std::atan(1.0f/p00)*57.2957795f,
                          2.0f*std::atan(1.0f/p11)*57.2957795f,
                          site == g_projOverrideCaller.load()
                              ? "== PROJ LOCK site (eye offset IS applied here)"
                              : "!= PROJ LOCK site (no eye offset -- mono)");
                }
            }

            // Lock the main camera's projection to the frustum we actually submit
            // to the compositor. This is the authoritative FOV: settings+0x60 is
            // only an INPUT to this builder, which is why the controller-look
            // modifier kept overriding it. Overriding the OUTPUT cannot be
            // overridden by anything upstream, so the FOV stops swinging (measured
            // 94.5 -> 128.9 deg) during the cockpit look animation.
            //
            // Use xr::render_hfov_deg() directly -- this IS the source of
            // truth the whole file computes from, not a second copy of it.
            // MAP/GARAGE eye separation. Deliberately NOT the PROJ LOCK block
            // below: that one also overwrites the FOV, and the map camera's
            // frustum is the game's own business. Only the lateral eye offset
            // and the AER bookkeeping go here -- exactly what detour_fna
            // supplies for the same screens when they are opened from orbit.
            {
                // MEASURED 2026-08-09: the map is built at exe+0xDA1E4E, which
                // IS the PROJ LOCK site -- so pointing this at it applied a
                // SECOND offset on top of the one below, using the m[0] from
                // before the FOV lock rewrites it. That is what read as doubled
                // and reversed. The site guard makes it impossible; this block
                // survives only for the case where the garage differs.
                const uintptr_t mp = g_mapProjCaller.load();
                if (mp && site == mp && site != g_projOverrideCaller.load() &&
                    hooks::in_map_view() && p00 > 1.0e-4f) {
                    xr::advance_aer_parity();
                    xr::note_camera_active();
                    const float eo = kMapEyeSign * kProjEyeSign *
                                     xr::eye_side_offset();
                    if (eo != 0.0f) {
                        m[12] += -eo * m[0];
                        // A PROJECTION SHEAR, not a camera move: it displaces
                        // every depth by the same amount. And it carries
                        // kMapEyeSign. Either fact alone keeps the depth
                        // reprojection off these screens.
                        xr::record_applied_eye_offset(
                            eo, xr::kEyeAppliedAsProjShift | xr::kEyeAppliedMapSign);
                    }
                }
            }

            const uintptr_t ov = g_projOverrideCaller.load();
            if (ov && site == ov) {
                // Which invocation of this site we are, within this Present.
                // Kept for the census below only -- the LOCK no longer depends
                // on it. See proj_lock_should_lock().
                const unsigned invoc = g_projInvocation.fetch_add(1);
                const bool lockThis = proj_lock_should_lock(p00, p11);

                // CENSUS, once every 600 Presents. The aspect rule assumes a
                // truck mirror is not shaped like the backbuffer, which is a
                // reasonable assumption and not a proven one -- and a square
                // render resolution (2688x2688 here) shrinks the gap it relies
                // on. If a mirror ever matches, this is the line that says so,
                // instead of leaving a magnified view nobody can account for.
                if ((g_projFrameNo.load(std::memory_order_relaxed) % 600u) == 0u) {
                    const float aspect = (p00 > 1.0e-4f && p11 > 1.0e-4f) ? p11 / p00 : 0.0f;
                    const float nativeHfov = (p00 > 1.0e-4f)
                        ? 2.0f * std::atan(1.0f / p00) * 57.2957795f : 0.0f;
                    VRLOG("PROJ LOCK census: invocation %u  aspect %.4f  native hfov "
                          "%.2f deg  -> %s", invoc, aspect, nativeHfov,
                          lockThis ? "LOCKED to our FOV" : "left at the game's FOV");
                }

                // RECON probe. Deliberately ABOVE the FOV-lock gate below.
                //
                // Inside it, the probe silently did nothing at a desk: with no
                // headset and no debug head pose, render_hfov_deg() has nothing
                // to report, so `hfovDeg > 1.0f` fails and the WHOLE locked
                // block is skipped. Holding J switched the debug pose on, the
                // gate started passing, and the probe appeared to spring to life
                // -- which reads like an intermittent bug and is really a
                // precondition. A recon tool has to be independent of the thing
                // being investigated, and desk testing is exactly where it gets
                // used.
                const float hfovDeg = xr::render_hfov_deg();
                if (lockThis && hfovDeg > 1.0f && p00 > 1.0e-4f && p11 > 1.0e-4f) {
                    const float aspect = p11 / p00;          // keep the builder's aspect
                    const float t = std::tan(hfovDeg * 0.5f * 0.0174532925f) * kProjFovMul;
                    if (t > 1.0e-4f) {
                        m[0] = 1.0f / t;

                        // THE VERTICAL. By default it keeps the builder's own
                        // aspect, which is what a game rendering to a monitor
                        // means by it -- and on a SQUARE canvas that is 1.0, so
                        // the frame is rendered as tall as it is wide whatever
                        // the panel shows. On a 140 deg-wide headset with a
                        // 115 deg-tall panel that is 43% of the vertical
                        // tangent span drawn and thrown away; on a narrow-FOV
                        // setting where the panel is TALLER than it is wide it
                        // is the reverse, and the top and bottom go empty.
                        //
                        // Asking the headset instead fixes both. It MUST be
                        // matched by eye_frustum_half_angles() -- rendering one
                        // vertical and declaring another is a stretch, and the
                        // two branches there mirror the two here.
                        const float vdeg = xr::match_headset_vfov()
                                               ? xr::render_vfov_deg() : 0.0f;
                        const float tv = (vdeg > 1.0f)
                            ? std::tan(vdeg * 0.5f * 0.0174532925f) * kProjFovMul
                            : 0.0f;
                        m[5] = (tv > 1.0e-4f) ? (1.0f / tv) : (m[0] * aspect);

                        // OFF-CENTRE, when asked: build the whole projection
                        // from the runtime's own bounds for the eye being
                        // rendered, shear included, rather than a symmetric one
                        // that merely encloses it.
                        //
                        // This matrix is ROW-VECTOR D3D (m[11] = 1, m[15] = 0 --
                        // see the lean note above), so the off-centre terms are
                        // m[8]/m[9], the same slots D3DXMatrixPerspectiveOffCenterLH
                        // puts (l+r)/(l-r) and (t+b)/(b-t) in.
                        //
                        // ONLY WRITTEN ON THIS PATH. An earlier version zeroed
                        // them on the symmetric path as well, to stop a shear
                        // outliving the setting -- but the game writes those
                        // slots itself for its own off-centre projections
                        // (shadow cascades, and a TAA jitter is a sub-pixel
                        // value in exactly these two), and the fail-safe locks
                        // every invocation. Clearing them was destroying the
                        // game's own values on every projection we touch. The
                        // shear we write is unconditional per frame, so there
                        // is nothing of ours to inherit anyway.
                        bool offc = false;
                        if (xr::offcenter_projection()) {
                            float tl, tr, tup, tdn;
                            if (xr::render_eye_frustum_tan(xr::render_eye(),
                                                           tl, tr, tup, tdn)) {
                                const float dx = tr - tl, dy = tup - tdn;
                                if (dx > 1.0e-4f && dy > 1.0e-4f) {
                                    m[0] = 2.0f / dx;
                                    m[5] = 2.0f / dy;
                                    m[8] = (tl + tr) / (tl - tr);
                                    m[9] = (tdn + tup) / (tdn - tup);
                                    offc = true;
                                }
                            }
                        }

                        static std::atomic<bool> once{false};
                        bool e2 = false;
                        if (once.compare_exchange_strong(e2, true))
                            VRLOG("PROJ LOCK active: was hfov=%.2f -> now %.2f deg "
                                  "(vertical %s, aspect %.4f)",
                                  2.0f*std::atan(1.0f/p00)*57.2957795f, hfovDeg,
                                  offc ? "off-centre, per eye"
                                       : ((tv > 1.0e-4f) ? "from the headset"
                                                         : "from the builder aspect"),
                                  aspect);

                        // Nothing is injected here any more. Mode 6 puts the
                        // head rotation, the lean and the AER +-IPD/2 at the
                        // camera builder (exe+0xDA1C20), which is full-rate and
                        // undamped, so this stage does only the FOV lock above.
                        //
                        // What used to live here -- a rotation folded in as
                        // M = A*P, a lean as a row-3 edit, and a per-eye offset --
                        // existed because the camera stage was believed to be
                        // sim-rate and damped. That was true of combine2 and
                        // never of the builder; see kRotFna.
                        // m_fFOV is an INPUT to the camera build, so the earlier
                        // in the frame it lands the sooner it takes effect.
                        hooks::camera_fov_reassert();


                        // (Ordering check done and FALSIFIED: rcx+0x40 read as
                        // NaN/inf with a zero position, so rcx is a scratch
                        // projection buffer, NOT the camera struct base. The
                        // struct's proj at +0x00 is the copy DESTINATION. The eye
                        // offset cannot ride this hook.)
                    }
                }
            }

            // (A per-caller FOV probe used to sit here -- a dropdown of the
            // observed call sites plus a multiplier, to find which frustum was
            // starving the truck mirrors by widening them one at a time. It
            // never was the answer: the invocation gate above fixes the mirrors
            // on its own and the probe made no difference at any setting.
            // Removed with its UI 2026-08-15.)
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return ret;
}

// Does this pointer look like the camera struct we decoded? proj at +0x00 with
// [+0x2C] == 1.0f (the builder writes that constant), plus a plausible world
// position in the camera-to-world translation row at +0x70.
bool looks_like_cam_struct(const void* p, float outPos[3])
{
    if (!p) return false;
    bool ok = false;
    __try {
        const float* f = reinterpret_cast<const float*>(p);
        const float one = f[11];              // +0x2C
        const float px = f[28], py = f[29], pz = f[30];   // +0x70/74/78
        if (std::fabs(one - 1.0f) < 0.001f &&
            std::fabs(px) < 1.0e6f && std::fabs(py) < 1.0e6f && std::fabs(pz) < 1.0e6f &&
            f[0] > 0.0001f && f[0] < 100.0f) {
            outPos[0] = px; outPos[1] = py; outPos[2] = pz;
            ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return ok;
}

// Camera-consumer hook. Observe-only: identifies which argument carries the
// camera struct and confirms the +0x00 / +0x40 / +0x70 layout live.
PFN4 real_camconsumer = nullptr;
void* detour_camconsumer(void* rcx, void* rdx, void* r8, void* r9){
    void* raw_ret = _ReturnAddress();
    void* ret = real_camconsumer(rcx, rdx, r8, r9);
    __try {
        static std::atomic<int> logged{0};
        if (logged.load() < 6) {
            void* args[4] = {rcx, rdx, r8, r9};
            const char* names[4] = {"rcx", "rdx", "r8", "r9"};
            char line[400]; int n = 0;
            n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE,
                             "CAMCONSUMER ret=exe+0x%llX:",
                             (unsigned long long)(reinterpret_cast<uintptr_t>(raw_ret) - g_moduleBase));
            bool any = false;
            for (int i = 0; i < 4 && n < (int)sizeof(line) - 80; ++i) {
                float pos[3] = {0,0,0};
                if (looks_like_cam_struct(args[i], pos)) {
                    any = true;
                    n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE,
                                     " %s=CAMSTRUCT pos=(%.2f,%.2f,%.2f)",
                                     names[i], pos[0], pos[1], pos[2]);
                } else {
                    n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE,
                                     " %s=%p", names[i], args[i]);
                }
            }
            if (any) { logged.fetch_add(1); VRLOG("%s", line); }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return ret;
}

// Orbit view hook (FnA): inject head-look by rewriting COPIES of eye/target,
// filtered to the CAMERA caller (not the sun/shadow view). Unrecognised
// callers are logged once each (see g_sites).
PFN4 real_fna = nullptr;
void* detour_fna(void* rcx, void* rdx, void* r8, void* r9){
    void* raw_ret = _ReturnAddress();
    void* useRdx = rdx, *useR8 = r8, *useR9 = r9;
    float eyeL[4], tgtL[4], upL[4];   // stack copies; survive to the real call below
    __try {
        uintptr_t caller = reinterpret_cast<uintptr_t>(raw_ret) - g_moduleBase;
        // In-game orbit only in EXTERIOR mode (the cockpit is still computed via
        // this caller in cockpit mode; transforming it there corrupts the cockpit).
        // Menu camera is unconditional.
        const bool isMenuCam = (caller==kOrbitCamMenu);
        // The logic_mode test excludes the in-game orbit builder while the
        // cockpit is the active camera mode -- correct during gameplay, wrong
        // for the MAP or GARAGE opened FROM the cockpit. logic_mode still reads
        // cockpit there, so this hook declined the camera and the projection's
        // own eye offset is gated to a single call site the map camera may not
        // use, leaving those screens with no eye separation at all. Entering the
        // same screens from the main menu works because they run on the MENU
        // camera instead, which is unconditional above.
        //
        // in_map_view() is the debounced main-camera-relocation classifier, so
        // this only widens while map/garage is actually confirmed.
        // Mode 6 claims the cockpit's use of this same caller and does
        // The cockpit is claimed here too, and everything is done here for it:
        // rotation, lean and the AER +-IPD/2. See the note on this builder above.
        const bool fnaCockpit = hooks::logic_mode() >= 0.75f &&
                                !hooks::in_map_view();
        bool isCam = isMenuCam ||
                     (caller==kOrbitCamGame &&
                      (hooks::logic_mode() < 0.75f || hooks::in_map_view() ||
                       fnaCockpit));
        if (!isCam) {   // log any other orbit-ish caller, once each
            int n=g_nSites.load(); bool found=false;
            for(int i=0;i<n;++i) if(g_sites[i]==caller){found=true;break;}
            if(!found && n<64 && rdx){ g_sites[n]=caller; g_nSites.store(n+1);
                float* e=reinterpret_cast<float*>(rdx);
                VRLOG("fna caller[%d] exe+0x%llX eye=(%.1f,%.1f,%.1f)", n,
                      (unsigned long long)caller, e[0],e[1],e[2]); }
        }
        if (isCam && rdx && r8) {
            xr::advance_aer_parity();   // first main build per frame flips the AER eye
            xr::note_camera_active();   // lets Present detect "no camera" (menu/loading)
            static bool loggedR9=false;
            if(!loggedR9 && r9){ loggedR9=true; float* u=reinterpret_cast<float*>(r9);
                VRLOG("fna r9(up?)=(%.3f,%.3f,%.3f)", u[0],u[1],u[2]); }
            float* eye = reinterpret_cast<float*>(rdx);
            float* tgt = reinterpret_cast<float*>(r8);
            for(int i=0;i<4;++i){ eyeL[i]=eye[i]; tgtL[i]=tgt[i]; }
            float fwd[3]={tgtL[0]-eyeL[0], tgtL[1]-eyeL[1], tgtL[2]-eyeL[2]};
            float dist=std::sqrt(fwd[0]*fwd[0]+fwd[1]*fwd[1]+fwd[2]*fwd[2]);
            if (dist>1e-4f){
                norm3(fwd);

                // Pre-transform: this is the game's own look, before the head
                // rotation, the lean and the truck locks below.
                for (int i = 0; i < 3; ++i) {
                    g_gameCamFwd[i].store(fwd[i]);
                    g_gameCamEye[i].store(eyeL[i]);
                }
                g_gameCamValid.store(true);


                // The reference up. WORLD up for orbit -- that is its horizon
                // lock, and levelling the chase camera is the point of it.
                //
                // For the COCKPIT that is exactly wrong, and it is what made the
                // cab stop rolling with the truck when mode 6 moved the cockpit
                // onto this builder: rebuilding the basis against world up
                // discards whatever roll and pitch the truck had, and we then
                // hand our levelled version back as r9. The game passes the real
                // camera up in r9 -- which already carries the truck's tilt AND
                // the stick look -- so the cockpit takes that instead.
                float up[3] = {0.0f, 1.0f, 0.0f};
                if (fnaCockpit && r9) {
                    const float* gu = reinterpret_cast<const float*>(r9);
                    float u[3] = {gu[0], gu[1], gu[2]};
                    if (dot3(u, u) > 1.0e-6f) {
                        norm3(u);
                        up[0]=u[0]; up[1]=u[1]; up[2]=u[2];
                    }
                }

                // Truck locks and the stick-look roll removal, restored for mode
                // 6. Applied HERE, to the game's own look, before any head
                // rotation -- the same ordering this had at combine2, which
                // is what the locks require. The frame comes
                // from combine2's previous build; see truck_frame().
                if (fnaCockpit) {
                    float tf[3], tu[3], tr[3];
                    if (truck_frame(tf, tu, tr))
                        apply_truck_frame_basis(tf, tu, tr, fwd, up);
                }
                // tF/tU/tS = head POSITION (lean), projected onto the BASE
                // (pre-head-rotation) axes. ipdS = AER +-IPD/2, projected onto
                // the head-ROTATED axis. See the translation at the end.
                float yaw=0,pitch=0,roll=0,tF=0,tU=0,tS=0,ipdS=0;
                // Orbit's right axis is handed opposite to the cockpit's side, so
                // negate the side term to keep head-tracking directions aligned.
                // Orbit's yaw and side axes are handed opposite to the cockpit, so
                // negate both to keep head-tracking directions aligned across cameras.
                // Skipped entirely while the cockpit probe is running: combine2
                // is applying the head rotation already, and applying it here
                // too would double it -- which would look like corruption and
                // measure nothing.
                if (kConnectHead){
                    xr::HeadLook h=xr::head_look();
                    // The lean goes in HERE for every camera. It used to be
                    // switchable to the projection for orbit; that option is
                    // gone, along with the rest of the projection injection.
                    if(h.valid){ yaw=-g_yawSign.load()*h.yaw; pitch=g_pitchSign.load()*h.pitch;
                                 roll=g_rollSign.load()*h.roll;
                                 tF=-h.oz; tU=h.oy; }
                    // Stereo ±IPD/2 always (default-IPD fallback runs AER w/o headset).
                    // The main-menu orbit camera is handed a basis mirrored
                    // relative to the in-game one, so the same side offset comes
                    // out with the eyes reversed there. Same hook, same code --
                    // only the sign differs, hence the per-caller flip.

                    // The lean's side term is applied with the OPPOSITE sign to
                    // the eye offset below, and that is not a mistake.
                    //
                    // MEASURED 2026-08-12 with a synthetic mouse-driven lean
                    // (since removed): left/right came out mirrored at this site
                    // in the cockpit AND, once leanAtProj was turned off, in
                    // orbit -- so the fault is in the LEAN's sign for
                    // every camera, not in a cockpit-vs-orbit handedness
                    // difference. Orbit's +-IPD/2 has been going in here all
                    // along and is correct, so the two genuinely disagree.
                    //
                    // They can, because they are different quantities: h.ox is a
                    // head position in play space and eye_side_offset() is a
                    // per-eye AER offset, each with its own convention. An
                    // earlier version of this tied them to one constant on the
                    // argument that both mean "which way is right" -- that
                    // argument was wrong, and acting on it would have inverted
                    // orbit's working stereo.
                    // Head LEAN and the AER +-IPD/2 used to share one tS term.
                    // They are now split because they belong on DIFFERENT bases:
                    // the lean is a play-space position and must not follow the
                    // head rotation, while the eye offset must. See the two
                    // translation terms at the end of this block.
                    tS = g_sideSign.load()*h.ox;   // 0 when !h.valid
                    // MAP/GARAGE takes its eye offset HERE too, as of
                    // 2026-08-12. It used to defer to the projection, because
                    // supplying it at both sites gave two offsets of equal
                    // magnitude and OPPOSITE sign, cancelling to exactly no
                    // stereo -- and doubling to ~2x IPD when either sign was
                    // flipped. Flat and "reversed" are different symptoms, and
                    // flat is what a cancellation looks like.
                    //
                    // That was a DOUBLE-APPLICATION, not a reason this site
                    // cannot serve those screens. With every camera's offset on
                    // this one stage and the projection supplying none, there is
                    // no second contributor left to cancel against.
                    //
                    // Map/garage/menu take the opposite eye side; see
                    // kMapEyeSign for why that one asymmetry is real while every
                    // other per-screen sign turned out not to be.
                    {
                        const bool mapSide = (isMenuCam || hooks::in_map_view());
                        const float mapFlip = mapSide ? kMapEyeSign : 1.0f;
                        ipdS=-mapFlip*g_sideSign.load()*xr::eye_side_offset();
                        // TELL THE REPROJECTION WHICH SIGN CONVENTION THIS IS.
                        // The flip is a correction for how these screens render,
                        // not a statement about where the camera sat, so a model
                        // built on camera geometry must not inherit it -- see
                        // kEyeAppliedMapSign. A LEVEL FLY-IN reaches here with
                        // mapSide true via isMenuCam -- the CALL SITE, which no
                        // screen-gate setting affects -- while the marker gate
                        // calls the same frame a garage and leaves the
                        // reprojection on. That is what left the synthesized eye
                        // on the wrong side, measured at -1.1x true disparity.
                        xr::record_applied_eye_offset(
                            ipdS, xr::kEyeAppliedAsCameraMove |
                                  (mapSide ? xr::kEyeAppliedMapSign : 0));
                    }
                } else {
                    int mode=g_mode.load(); float ramp=(GetTickCount()%3000)/3000.0f;
                    if(mode==0)yaw=-0.6f*ramp; else if(mode==1)pitch=0.6f*ramp;
                    else if(mode==2)roll=0.6f*ramp;
                    else if(mode==3)tF=2.0f*ramp; else if(mode==4)tU=2.0f*ramp; else if(mode==5)tS=-2.0f*ramp;
                }
                // Overwrite the stick's elevation entirely instead of adding
                // head pitch on top of it: flatten fwd to purely horizontal
                // first (discarding whatever vertical component the game's
                // own stick-driven aim had), THEN apply the exact same
                // rot_vec/sign convention as before. Since the base is now
                // always horizontal, what used to be an ADDITIVE rotation
                // becomes an absolute one -- head pitch is the only source
                // of elevation. Eye position/orbit distance (game's own eye)
                // and the head-position translation below are untouched;
                // only the look direction's vertical component changes.
                // H toggles this (g_horizonLockOn); also force-off in
                // map/garage (hooks::in_map_view()) -- those aren't a normal
                // orbit-around-the-truck view, so overwriting elevation there
                // isn't meaningful and the plain additive fallback runs instead.
                //
                // Moved AHEAD of the head rotation (it used to sit between the
                // yaw and the pitch). Under the old world-up yaw the two orders
                // were equivalent -- a rotation about world Y commutes with
                // discarding the vertical component -- but the yaw axis below
                // is now derived FROM fwd, so the flatten has to happen before
                // that axis is built or the lock would have no effect on it.
                // ORBIT ONLY. The cockpit has its own truck-relative locks
                // above, levelled against the TRUCK so hills are preserved;
                // flattening to the world horizon here as well would undo them
                // and pin the cab level whatever the terrain did.
                if (g_horizonLockOn.load() && !hooks::in_map_view() && !fnaCockpit) {
                    float horiz[3] = {fwd[0], 0.0f, fwd[2]};
                    norm3(horiz);
                    if (horiz[0] != 0.0f || horiz[2] != 0.0f) {
                        fwd[0] = horiz[0]; fwd[1] = 0.0f; fwd[2] = horiz[2];
                    }
                }

                // Head rotation composed in the GAME CAMERA'S OWN frame, not
                // the world frame -- the same intrinsic yaw/pitch/roll the
                // cockpit path has always used (see the FULL cockpit head
                // rotation block above), which is why cockpit never showed
                // this problem.
                //
                // This used to yaw about world up ({0,1,0}) and hand the game a
                // hardcoded world-up vector. That keeps the image horizon level
                // at all times, which sounds desirable and is actually the bug:
                // with the orbit camera elevated (looking down at the truck) and
                // horizon lock off, holding the horizon level as the head yaws
                // means the world RE-TILTS to follow you instead of staying put.
                // Symptom: head yaw appeared to under-track, by cos(elevation) --
                // ~23 deg of visible rotation for a 30 deg head turn at 40 deg
                // camera pitch. Composing in the camera's frame makes the whole
                // view basis a rigid rotation of the head's, so with the stick
                // untouched the world stays nailed where it is. The horizon then
                // DOES roll as you look around -- that is veridical, not an
                // artifact: the virtual world is genuinely tilted relative to the
                // real one at that point, and rolling is what a tilted world does
                // when you turn your head in it.
                //
                // Degenerate only if the camera looks straight up/down, where
                // "the camera's own right axis" is undefined; falls back to the
                // previous world-up basis there.
                float baseUp[3]  = {0.0f, 1.0f, 0.0f};
                float baseRight[3]; cross3(up, fwd, baseRight);
                const float brLen = std::sqrt(baseRight[0]*baseRight[0] +
                                              baseRight[1]*baseRight[1] +
                                              baseRight[2]*baseRight[2]);
                const bool baseOk = brLen > 1.0e-3f;
                if (baseOk) {
                    baseRight[0]/=brLen; baseRight[1]/=brLen; baseRight[2]/=brLen;
                    // Reciprocal of the file's right = cross(up, fwd) convention.
                    // Reduces to exactly {0,1,0} whenever fwd is horizontal, so
                    // the horizon-locked path stays numerically as it was.
                    cross3(fwd, baseRight, baseUp); norm3(baseUp);
                }

                // Snapshot the base forward before the head rotation moves it;
                // baseRight/baseUp above are already pre-rotation. Together these
                // three are the frame the head's POSITION offset lives in.
                const float baseFwd[3] = {fwd[0], fwd[1], fwd[2]};

                rot_vec(fwd, baseUp, yaw);             // yaw about the CAMERA's up
                float right[3]; cross3(baseUp, fwd, right); norm3(right);

                // Head roll has to rotate the eye-separation axis, not just the
                // image. Rolling only the up vector handed to the game left the
                // ±IPD/2 offset (and the positional offsets) on a world-horizontal
                // axis while the view itself rolled, so the two eyes picked up a
                // vertical disparity component and stopped fusing. Build the
                // rolled basis ONCE and use it for both the game's up vector and
                // the translation, so they can never disagree again.
                //
                // camUp now starts from the camera's own up and is carried
                // through the pitch with fwd (it used to be pinned to world up
                // and only ever picked up the roll), so the basis handed to the
                // game is orthonormal at any elevation instead of relying on the
                // lookAt to re-orthogonalise a world-up vector against a pitched
                // forward.
                float camUp[3] = {baseUp[0], baseUp[1], baseUp[2]};
                rot_vec(fwd, right, pitch); rot_vec(camUp, right, pitch);
                if (roll != 0.0f) rot_vec(camUp, fwd, roll);
                // Same cross order as before (sign conventions depend on it) --
                // only the up vector fed into it now carries the roll.
                float camRight[3]; cross3(camUp, fwd, camRight); norm3(camRight);

                // LEAN on the base axes, +-IPD/2 on the head-rotated axis.
                // Both used to ride camRight/camUp/fwd (all post-rotation), which
                // made the head rotation pivot about where the head would have
                // been WITHOUT the lean: lean right, look left, and the view
                // orbits a point off to the side. The head's position is reported
                // in play space and does not follow where the head points, so it
                // belongs on the pre-rotation basis. The eye offset genuinely
                // does sit left/right of the head in the HEAD's frame, so it
                // stays on camRight. Identical to the old behaviour whenever the
                // head rotation is zero. Cockpit has the same split, done by
                // counter-rotation instead -- see the camRight note above.
                // In the degenerate case above baseRight was never normalised
                // (it is a near-zero cross product), so fall back to the
                // post-rotation basis there rather than collapsing the lean to
                // nothing. A camera looking straight up/down has no well-defined
                // right axis at all, so any choice is arbitrary -- but this one
                // is at least unit-length and continuous.
                const float* lr = baseOk ? baseRight : camRight;
                const float* lu = baseOk ? baseUp    : camUp;
                const float* lf = baseOk ? baseFwd   : fwd;
                for(int i=0;i<3;++i)
                    eyeL[i] += lr[i]*tS + lu[i]*tU + lf[i]*tF + camRight[i]*ipdS;
                for(int i=0;i<3;++i) tgtL[i]= eyeL[i]+fwd[i]*dist;
                useRdx=eyeL; useR8=tgtL;
                upL[0]=camUp[0]; upL[1]=camUp[1]; upL[2]=camUp[2]; upL[3]=0.0f;
                useR9 = upL;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return real_fna(rcx, useRdx, useR8, useR9);
}

} // namespace

// Read-only. Nothing writes these any more, so what they report IS the game's
// own idea of the FOV -- worth logging beside what we actually render.
bool game_fov_fields(float& f0, float& f1)
{
    bool ok = false;
    __try {
        const char* obj = *reinterpret_cast<char* const*>(g_moduleBase + kSettingsPtr);
        if (obj) {
            f0 = *reinterpret_cast<const float*>(obj + kFovOff0);
            f1 = *reinterpret_cast<const float*>(obj + kFovOff1);
            ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return ok;
}


// Orbit's horizon lock: head pitch overwrites the stick's elevation instead of
// adding to it. ORBIT ONLY -- the cockpit levels against the TRUCK instead, so
// hills are preserved; see apply_truck_frame_basis().
bool horizon_lock_enabled() { return g_horizonLockOn.load(); }
void set_horizon_lock_enabled(bool on)
{
    g_horizonLockOn.store(on);
    VRLOG("HORIZON LOCK -> %s", on ? "ON" : "OFF");
}

bool map_by_pose() { return g_mapByPose.load(); }


bool truck_level_lock_enabled() { return g_truckLevelLock.load(); }
bool truck_yaw_lock_enabled()   { return g_truckYawLock.load(); }
void set_truck_level_lock_enabled(bool on)
{
    g_truckLevelLock.store(on);
    VRLOG("TRUCK LEVEL LOCK -> %s", on ? "ON" : "OFF");
}
void set_truck_yaw_lock_enabled(bool on)
{
    g_truckYawLock.store(on);
    VRLOG("TRUCK YAW LOCK -> %s", on ? "ON" : "OFF");
}

// basis rather than assuming.
void viewbuild_on_present(){
    update_map_by_pose();
    // Per-Present reset for the locked site's invocation counter
    // (g_projInvocation). The per-frame total it keeps only feeds the
    // once-a-second log line now that the invocation selector UI is gone.
    const unsigned invocs = g_projInvocation.exchange(0);
    g_projFrameNo.fetch_add(1);

    // Roll the aspect gate's two-frame window -- see proj_lock_should_lock().
    const bool seen = g_aspectSeenThisFrame.exchange(false);
    const bool prev = g_aspectSeenLastFrame.exchange(seen);
    if (prev != seen)
        VRLOG("PROJ LOCK aspect gate -> %s (site built %u projection%s last frame)",
              seen ? "main view identified by canvas aspect; mirrors keep the game's FOV"
                   : "ALL (fail-safe: nothing matched the canvas aspect -- correct FOV, "
                     "mirrors inherit it)",
              invocs, invocs == 1 ? "" : "s");

    auto edge=[](int vk,bool& prev)->bool{ bool k=(GetAsyncKeyState(vk)&0x8000)!=0; bool e=k&&!prev; prev=k; return e; };
    // HOME is the only key bound here. What used to be:
    //   - [/] cockpit FOV, I/K orbit FOV: collided with in-game controls, and
    //     both FOVs auto-initialise from the headset's own VFOV on the first
    //     valid XR frame, so there is nothing left to tune by hand.
    //   - H horizon lock: moved to the settings UI's checkbox (menu_hook.cpp),
    //     which shows the current state instead of toggling blind.
    //   - Scroll Lock (projection-hook toggle), PageDown (sentinel probe) and
    //     Delete (shear probe): Cheat Engine recon aids, removed with the
    //     probes themselves. Both questions they existed to answer are settled
    //     and recorded where the probes were declared -- the shear measurement
    //     showed the engine reads only the projection's diagonal, and the
    //     sentinel found the per-frame camera stage. Two of them wrote
    //     deliberately corrupt values into the live camera, so they were also
    //     the only bindings here that could wreck a normal session by mistake.
    //   - J-drag / K-drag: a synthetic head pose from the mouse -- hold a key,
    //     move the mouse, and the injection paths ran as if a headset were
    //     rotating or leaning. It existed to work on the camera paths at a desk
    //     with nothing plugged in, which was worth having while those paths
    //     were being found and not worth two more keys once they worked.
    //
    // HOME has no in-game conflict and recenter is a real VR feature, so it
    // stays -- and the settings menu has the same button, which is the version
    // most people will use.
    static bool pHome=0;

    if (edge(VK_HOME, pHome)) {
        xr::request_recenter();
        VRLOG("HOME -> recenter");
    }

    // Builder call-site census, every 2 s. Compare each site's calls/s against
    // the present rate in the AER rate line: a site at ~present rate is the
    // per-render-frame camera stage we are looking for; sites at ~60/s are on the
    // sim tick like combine.
    if (kBuilderCensus) {
        static DWORD t0 = GetTickCount();
        const DWORD now = GetTickCount();
        const int dt = (int)(now - t0);
        if (dt >= 2000) {
            char line[512]; int n = 0;
            n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, "builder sites/s:");
            for (int i = 0; i < kMaxSites && n < (int)sizeof(line) - 48; ++i) {
                const uintptr_t s = g_siteRec[i].site.load();
                if (!s) continue;
                const int c = g_siteRec[i].calls.exchange(0);
                const int v = g_siteRec[i].views.exchange(0);
                if (!c) continue;
                n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE,
                                 " [+0x%llX %.0f/s%s]", (unsigned long long)s,
                                 c * 1000.0 / dt, v ? " VIEW" : "");
            }
            VRLOG("%s", line);
            VRLOG("   eye inject: %d/s applied, last off=%.4f, eye_side_offset=%.4f",
                  (int)(g_eyeInjects.exchange(0) * 1000.0 / dt),
                  g_lastEyeOff.load(), xr::eye_side_offset());

            // Advance the probe to the next site that emitted a view matrix and
            // is called at least ~20/s (per-frame candidates only, not per-draw
            // sites, which would offset every object).
            if (kSiteProbe) {
                static int cursor = 0;
                for (int step = 0; step < kMaxSites; ++step) {
                    const int i = (cursor + step) % kMaxSites;
                    const uintptr_t s2 = g_siteRec[i].site.load();
                    if (!s2) continue;
                    g_probeSite.store(s2);
                    cursor = i + 1;
                    VRLOG("   SITE PROBE -> now offsetting +0x%llX by %.2f "
                          "(watch the desktop image jump sideways)",
                          (unsigned long long)s2, kSiteProbeOff);
                    break;
                }
            }
            t0 = now;
        }
    }

    // Pin the 3D viewport width/height to EXACTLY match the forced backbuffer
    // size (same aspect-preserving W,H the swapchain was actually resized to
    // -- NOT a square edge, which squashed the image), every frame -- see
    // kViewportW/kViewportH above for why.
    __try {
        uint32_t w = 0, h = 0;
        if (xr::get_forced_render_wh(w, h)) {
            int32_t* vw = reinterpret_cast<int32_t*>(g_moduleBase + kViewportW);
            int32_t* vh = reinterpret_cast<int32_t*>(g_moduleBase + kViewportH);
            *vw = (int32_t)w;
            *vh = (int32_t)h;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    // Camera mode change (cockpit <-> exterior): POSITION only. Yaw stays put
    // deliberately -- swinging the world round to face wherever the head
    // happened to be pointing at the moment of a camera switch is disorienting,
    // and it also used to drag a world-pinned UI plane with it. The lean origin
    // does still need re-taking, since the cockpit and exterior views put the
    // player's head somewhere different.
    { static float prevMode=-1.0f; float m=hooks::logic_mode();
      if (prevMode>=0.0f && std::fabs(m-prevMode)>0.1f) { xr::request_position_recenter(); VRLOG("auto-recenter POSITION (camera mode change)"); }
      prevMode=m; }
    // Auto-recenter on level load / fast-travel: the world camera eye teleports.
    { float e[3];
      if (hooks::logic_eye(e)) {
          static float prev[3]={0,0,0}; static bool have=false;
          if (have) { float dx=e[0]-prev[0], dy=e[1]-prev[1], dz=e[2]-prev[2];
              if (dx*dx+dy*dy+dz*dz > 2500.0f) { xr::request_position_recenter(); VRLOG("auto-recenter POSITION (camera teleport)"); } }
          prev[0]=e[0]; prev[1]=e[1]; prev[2]=e[2]; have=true;
      } }
}


bool install_viewbuild_hook(){
    HMODULE base = GetModuleHandleW(nullptr);
    if (!base) return false;
    g_moduleBase = reinterpret_cast<uintptr_t>(base);

    void* target = reinterpret_cast<void*>(g_moduleBase + kBuilderOffset);
    if (MH_CreateHook(target, &detour_builder, reinterpret_cast<void**>(&real_builder)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        VRLOG("viewbuild hook FAILED at %p", target);
        return false;
    }
    VRLOG("viewbuild hook installed at %p (exe+0x%llX)", target, (unsigned long long)kBuilderOffset);

    void* fna = reinterpret_cast<void*>(g_moduleBase + kFnAOffset);
    if (MH_CreateHook(fna, &detour_fna, reinterpret_cast<void**>(&real_fna)) == MH_OK &&
        MH_EnableHook(fna) == MH_OK)
        VRLOG("fna (orbit view) hook installed at exe+0x%llX", (unsigned long long)kFnAOffset);
    else
        VRLOG("fna hook FAILED");

    if (kHookCamConsumer) {
        void* camc = reinterpret_cast<void*>(g_moduleBase + kCamConsumerOffset);
        if (MH_CreateHook(camc, &detour_camconsumer, reinterpret_cast<void**>(&real_camconsumer)) == MH_OK &&
            MH_EnableHook(camc) == MH_OK)
            VRLOG("camera consumer hook installed at exe+0x%llX", (unsigned long long)kCamConsumerOffset);
        else
            VRLOG("camera consumer hook FAILED");
    }

    g_projTarget = reinterpret_cast<void*>(g_moduleBase + kProjBuildOffset);
    if (MH_CreateHook(g_projTarget, &detour_projbuild,
                      reinterpret_cast<void**>(&real_projbuild)) == MH_OK &&
        MH_EnableHook(g_projTarget) == MH_OK)
        VRLOG("projection builder hook installed at exe+0x%llX (FOV lock ON)",
              (unsigned long long)kProjBuildOffset);
    else
        VRLOG("projection builder hook create FAILED");

    void* combine2 = reinterpret_cast<void*>(g_moduleBase + kCombine2Offset);
    if (MH_CreateHook(combine2, &detour_combine2, reinterpret_cast<void**>(&real_combine2)) == MH_OK &&
        MH_EnableHook(combine2) == MH_OK)
        VRLOG("combine2 hook installed at exe+0x%llX", (unsigned long long)kCombine2Offset);
    else
        VRLOG("combine2 hook FAILED");

    void* combine = reinterpret_cast<void*>(g_moduleBase + kCombineOffset);
    if (MH_CreateHook(combine, &detour_combine, reinterpret_cast<void**>(&real_combine)) == MH_OK &&
        MH_EnableHook(combine) == MH_OK)
        VRLOG("combine hook installed at exe+0x%llX", (unsigned long long)kCombineOffset);
    else
        VRLOG("combine hook FAILED");

    // (The scattered-prop hooks at exe+0xC06C80 / +0xC06F60 that mode 5 used
    // were removed with it -- see the note at kRotRestamp. They ran ~150 times a
    // second through a detour for nothing.)

    return true;
}

} // namespace hooks
