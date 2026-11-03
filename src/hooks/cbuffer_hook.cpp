#include "hooks/cbuffer_hook.h"
#include "hooks/viewbuild_hook.h"   // map_by_pose()
#include "hooks/camera_hook.h"
#include "hooks/shader_cull.h"      // the shader-fingerprint gate
#include "xr/xr_mirror.h"
#include "common/log.h"

#include <d3d11_1.h>
#include <wrl/client.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_set>
#include <vector>

#include <MinHook.h>

using Microsoft::WRL::ComPtr;

namespace hooks {
namespace {

constexpr int kIdxMap                    = 14;  // ID3D11DeviceContext vtable
constexpr int kIdxUnmap                  = 15;
constexpr int kIdxUpdateSubresource      = 48;
constexpr int kIdxUpdateSubresource1 = 116; // ID3D11DeviceContext1 vtable (verified against
                                             // the SDK header: FinishCommandList=114,
                                             // CopySubresourceRegion1=115, UpdateSubresource1=116.
                                             // Was 109 (actually CSGetConstantBuffers on the base
                                             // ID3D11DeviceContext vtable) -- silently hooked the
                                             // wrong, rarely-called method, so no crash but also no
                                             // effect; UpdateSubresource1 writes were invisible to
                                             // the small-CB/vertex-buffer catalogs until this fix.

// CB_GLOBAL_CAMERA layout (decoded from the object dump, 352 bytes, row-major
// storage / column-vector math M*v). The full decode, not just the fields this
// file writes -- kOffViewDir and kOffViewProjPrev are here so the layout reads
// as the struct it is rather than as a list of offsets someone happened to need.
constexpr int kOffEye         = 0x00;
constexpr int kOffViewDir     = 0x10;
constexpr int kOffViewProj    = 0x20;
constexpr int kOffView        = 0x60;
constexpr int kOffViewProjPrev = 0xA0;
constexpr int kCamCBSize      = 352;

std::atomic<bool> g_hooked{false};

// Snapshot of the last main-camera CB (DIBR shift needs the projection to turn
// reverse-Z depth into view-space Z; proj = viewProj * inverse(view)).
SRWLOCK g_camSnapLock = SRWLOCK_INIT;
float g_camView[16] = {};
float g_camViewProj[16] = {};
float g_camViewProjPrev[16] = {};
// The game's OWN previous-frame viewProj (CB offset 0xA0). Deliberately never
std::atomic<bool> g_camSnapValid{false};
// WHICH BUILD the snapshot is (see classify_cam): the build its camera sits on
// (0 = not recognised as one), the newest build when it was uploaded, how it
// was recognised, and a count of snapshots. Under g_camSnapLock.
uint32_t g_camSnapBuild = 0, g_camSnapNewest = 0, g_camSnapSeq = 0;
int      g_camSnapHow = 0;
// Main-camera uploads by how they were recognised, and camera buffers turned
// down, for the periodic line in cbuffer_hook_on_present().
std::atomic<uint32_t> g_camHowCount[6] = {};
std::atomic<uint32_t> g_camRejected{0};
std::atomic<float>    g_camBuildDistMax{0.0f};

using PFN_UpdateSubresource = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*,
    const void*, UINT, UINT);
// Immediate/deferred split -- confirmed necessary: the UI is written on a
// deferred context, so a single "real" pointer from the immediate context
// alone left the marker CB's writes completely unhooked.
PFN_UpdateSubresource real_UpdateSubresource_imm = nullptr;
PFN_UpdateSubresource real_UpdateSubresource_def = nullptr;

// How far a constant buffer's eye may sit from the logic-camera eye and still
// be the player camera. Wide (5 units) on purpose: the engine renders several
// passes per frame with slightly different player-camera eyes (e.g. 266.78 vs
// 268.01) and all of them must be rotated, while a shadow or light eye is
// still well outside this window.
constexpr float kEyeTol = 5.0f;
// True if `data` is a camera CB (352 bytes) whose view matrix at 0x60 is a real
// (orthonormal) view. Self-contained: no dependence on the wandering DRIVE
// logic eye, so every player-camera pass is caught consistently. Each buffer is
// rotated about its own eye/basis, so passes with slightly different eyes stay
// coherent. (Shadow/reflection cameras are also caught here — filtered later if
// they misbehave.)
bool is_main_cam(const void* data, UINT byteWidth)
{
    if (byteWidth != kCamCBSize || !data)
        return false;
    const float* f = reinterpret_cast<const float*>(data);
    const float* v = f + kOffView/4;
    auto unit = [](const float* r) {
        float l = r[0]*r[0] + r[1]*r[1] + r[2]*r[2];
        return std::fabs(l - 1.0f) < 0.02f;
    };
    if (!(unit(v) && unit(v+4) && unit(v+8)))   // must be a real (orthonormal) view
        return false;
    // Identify by EYE match (CB eye @0x00 vs the logic camera eye). Robust to the
    // head rotation now baked into the CB view; excludes shadows/mirrors (far eye).
    float le[3];
    if (hooks::logic_eye(le)) {
        if (std::fabs(f[0]-le[0]) > kEyeTol ||
            std::fabs(f[1]-le[1]) > kEyeTol ||
            std::fabs(f[2]-le[2]) > kEyeTol)
            return false;
    }
    return true;
}

// THE MAIN VIEW'S CAMERA, recognised on every screen -- for the snapshot the
// renderer-side work reads (main_camera_matrices: DLSS's vectors, the FOV
// monitor, the presented-build measure). is_main_cam() above stays what it was
// for the relocation tracker and the depth probe, which read "matches the
// DRIVE_CAMERA" as "gameplay".
//
// is_main_cam() alone could not be used for this. Off gameplay it compares
// against a DRIVE_CAMERA that is parked where the truck stands, while the map,
// garage and menu cameras are elsewhere: from the first drive on, nothing
// matched there and the snapshot froze at the last gameplay frame. Measured
// 2026-10-28: 3000+ camera uploads per 3.75 s turned down on the map and in the
// garage, DLSS jitter constant for 30 s (it is read from this snapshot) and its
// vectors built from a camera that had stopped moving -- the heavy smearing in
// the garage and the menus after a drive. The FOV monitor's "PROJ LOCK MISS
// 103 deg vs 54" on the map was the same stale gameplay camera.
//
// So, in order:
//   1. One of the mod's own camera builds. Every main camera the mod builds --
//      cockpit, orbit, map, garage, menus (viewbuild_hook.cpp, detour_fna) --
//      records the eye it handed the game. A buffer whose eye sits on one, and
//      looks the same way, IS that build: exact (kCamBuild) within 2 cm, which
//      also names the build; near (kCamNearBuild) within 1 m, which only says
//      main. Asked at upload time, so later builds cannot be mistaken for it.
//   2. The DRIVE_CAMERA while it drives, as is_main_cam() has always done.
//   3. Off gameplay, a camera looking where one of the mod's builds looks,
//      wherever it stands (kCamBuildDir) -- for a screen whose camera the game
//      would move after the mod hands it over. Shadow and mirror cameras look
//      elsewhere.
//   4. Nothing to compare with yet (the first menu frames, before any camera
//      was built): any real camera, unverified -- as before.
// And over all four, one rule: while uploads sit on builds exactly, nothing
// less is the main view (classify_cam).
enum CamHow : int { kCamNot = 0, kCamBuild, kCamNearBuild, kCamLogic, kCamBuildDir,
                    kCamUnverified, kCamHowCount };
struct CamMatch { int how = kCamNot; uint32_t build = 0; float dist = 0.0f; };
constexpr float kBuildExactTol = 0.02f;
constexpr float kBuildNearTol  = 1.0f;
// How long the DRIVE_CAMERA may go without ticking and still be driving.
constexpr uint64_t kDriveTickMs = 400;
// When an upload last sat on a build exactly (GetTickCount64), and how long that
// turns the lesser matches away; the ones turned away, for the periodic line.
std::atomic<uint64_t> g_exactBuildTick{0};
constexpr uint64_t kExactBuildFreshMs = 250;
std::atomic<uint32_t> g_camTurnedAway{0};

CamMatch recognise_cam(const void* data, UINT byteWidth)
{
    CamMatch m;
    if (byteWidth != kCamCBSize || !data)
        return m;
    const float* f = reinterpret_cast<const float*>(data);
    const float* v = f + kOffView/4;
    auto unit = [](const float* r) {
        float l = r[0]*r[0] + r[1]*r[1] + r[2]*r[2];
        return std::fabs(l - 1.0f) < 0.02f;
    };
    if (!(unit(v) && unit(v+4) && unit(v+8)))   // must be a real (orthonormal) view
        return m;

    const float fwd[3] = {v[8], v[9], v[10]};   // row 2: the view's forward axis
    uint32_t b = 0;
    float d = 0.0f;
    if (xr::match_built_camera(f, fwd, kBuildNearTol, b, d)) {
        m.how   = (d <= kBuildExactTol) ? kCamBuild : kCamNearBuild;
        m.build = (m.how == kCamBuild) ? b : 0;
        m.dist  = d;
        return m;
    }
    // The drive camera keeps ticking on the map, where it is not the camera
    // being rendered -- so the map is left to the build tests.
    float le[3];
    if (hooks::logic_eye(le) && hooks::logic_camera_idle_ms() <= kDriveTickMs &&
        !hooks::in_map_view()) {
        if (std::fabs(f[0]-le[0]) <= kEyeTol &&
            std::fabs(f[1]-le[1]) <= kEyeTol &&
            std::fabs(f[2]-le[2]) <= kEyeTol)
            m.how = kCamLogic;
        return m;
    }
    if (xr::match_built_camera(f, fwd, 1.0e9f, b, d)) {
        m.how  = kCamBuildDir;
        m.dist = d;
        return m;
    }
    if (!xr::have_built_camera()) m.how = kCamUnverified;
    return m;
}

// recognise_cam(), and then the rule over it: while uploads are sitting on
// builds exactly, a camera that only passed a lesser test is not the main view.
//
// A TRUCK MIRROR'S CAMERA passes the drive-camera test: it stands within
// kEyeTol of the driver's eye. Drawn after the main view's last camera upload
// -- the round mirror, with the head turned towards it -- it was the last
// "main" camera of the frame, and every reader of the snapshot took the mirror
// for the view: the stale-eye warp turned and moved the stale eye by the
// mirror camera's swing between the two eyes' builds (a few degrees and 12 cm
// a frame, the truck parked), DLSS lost its jitter, and the FOV monitor read
// the mirror's 22 deg as a PROJ LOCK MISS (Tim, 2026-11-02). The main view sat
// on its build in those very frames, so the lesser tests had nothing left to
// find but other cameras. They decide again once nothing has sat on a build
// for kExactBuildFreshMs -- a game that moves its camera after the mod hands
// it over, or a screen with no builds.
CamMatch classify_cam(const void* data, UINT byteWidth)
{
    CamMatch m = recognise_cam(data, byteWidth);
    const uint64_t now = GetTickCount64();
    if (m.how == kCamBuild) {
        g_exactBuildTick.store(now, std::memory_order_relaxed);
    } else if (m.how != kCamNot &&
               now - g_exactBuildTick.load(std::memory_order_relaxed) <= kExactBuildFreshMs) {
        g_camTurnedAway.fetch_add(1, std::memory_order_relaxed);
        m = CamMatch{};
    }
    return m;
}

// Cache of which resources are 352-byte constant buffers, so the hot Map path
// avoids a QueryInterface/GetDesc on every call. Buffers are created once and
// reused, so this warms up in the first frames.
SRWLOCK g_cacheLock = SRWLOCK_INIT;
std::unordered_set<void*> g_camBufs;   // confirmed 352-byte CBs
std::unordered_set<void*> g_notCam;    // confirmed not

bool is_cam352(ID3D11Resource* res)
{
    if (!res)
        return false;
    AcquireSRWLockShared(&g_cacheLock);
    bool inCam = g_camBufs.count(res) != 0;
    bool inNot = !inCam && g_notCam.count(res) != 0;
    ReleaseSRWLockShared(&g_cacheLock);
    if (inCam) return true;
    if (inNot) return false;

    bool cam = false;
    D3D11_RESOURCE_DIMENSION dim;
    res->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        ComPtr<ID3D11Buffer> buf;
        if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Buffer), (void**)&buf))) {
            D3D11_BUFFER_DESC bd{};
            buf->GetDesc(&bd);
            cam = (bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER) && bd.ByteWidth == kCamCBSize;
        }
    }
    AcquireSRWLockExclusive(&g_cacheLock);
    (cam ? g_camBufs : g_notCam).insert(res);
    ReleaseSRWLockExclusive(&g_cacheLock);
    return cam;
}

// Both our own rotations are OFF: we now inject the HMD look into the game's
// native look-around system (camera_hook), so the game does the rotation with
// the correct head pivot and full pass coherence.
//
// ROTATING HERE DOES NOT WORK, tried twice. A "rotation mode 3" that turned the
// GPU view/viewProj in apply_rotation() was added and reverted 2026-08-07: it
// split the world in two, some geometry turning with the head and some not.
// The cause is structural, not a missing hook -- all three upload paths
// (UpdateSubresource, UpdateSubresource1, Map/Unmap) already route through
// here, but every one is gated on is_main_cam, so only the buffer whose eye
// matches the DRIVE_CAMERA logic eye is touched and every other pass with its
// own camera view is left alone. Dropping that gate would rotate shadow and
// reflection cameras too. This is the same wall the original CB approach hit --
// see commit 7d04ae4, "CPU-side view-matrix hook rotates all camera views
// COHERENTLY", which is why the injection moved to the CPU side in the first
// place. Rotate at detour_combine2 or in the projection matrix instead.
//
// THE EYE OFFSET DOES NOT WORK HERE EITHER. apply_rotation() ended up as a lateral
// +-IPD/2 shift of the main camera's final CB, cockpit only, and caused cockpit
// artifacts; it sat switched off until it was deleted on 2026-10-30. Whoever tries
// this again: leave g_tmViewProjPrev (0xA0) alone. Overwriting it corrupts the
// passes that read the previous frame's ViewProj (water, SSR, velocity).

// Instead of guessing at a near-origin eye or some magic radius,
// watch the SPECIFIC buffer that's normally the main view
// camera and see if IT STOPS matching for several consecutive frames --
// i.e. the main camera relocated, whatever its new eye is. Debounced
// (kMainCamMismatchThreshold) to ignore the one-frame miss seen at level
// load (is_main_cam fails once before logic_eye() is first published, then
// self-corrects the very next frame -- confirmed in the log, NOT a
// relocation). Only reacts to writes to the SAME buffer pointer that most
// recently matched, so shadow/reflection cameras (which never match at all)
// are naturally ignored rather than triggering noise every frame.
std::atomic<void*> g_lastMainCamBuf{nullptr};
std::atomic<int>   g_mainCamMismatchStreak{0};
constexpr int kMainCamMismatchThreshold = 5;
// Matches the idle threshold ui_hook.cpp's own CAMERA: DRIVE_CAMERA
// idle/resumed log already uses.
constexpr uint64_t kCamIdleThresholdMs = 400;

enum class CamLocation { kNormal, kMap, kGarage };

// Starts RELOCATED, not kNormal. Before gameplay this classifier structurally
// cannot fire: the mismatch branch below returns early unless the buffer is the
// tracked one, and g_lastMainCamBuf stays null until a main camera has matched
// at least once -- which only happens once the game is actually rendering
// gameplay. So the whole pre-gameplay session (main menu, and the garage or map
// reached from it) ran as kNormal, then flipped to kMap for the identical
// screens after the first drive. Same screen, two different behaviours,
// depending only on whether you had played yet.
//
// Seeding kMap makes those screens behave the same from the first frame. The
// first gameplay frame exchanges it back to kNormal and logs "MATCHED again
// (was MAP)", which is a real transition rather than a spurious one.
std::atomic<int> g_camLocation{(int)CamLocation::kMap};

// (A "left gameplay" rule keyed on the DRIVE_CAMERA going idle lived here and
// was REVERTED 2026-08-09. It did latch map mode when returning to the main
// menu, but it also fires on any PAUSE that freezes gameplay -- and there the
// two halves of map mode come apart: the HUD-shrink bypass takes effect at once
// because it is evaluated per draw, while the reduced render FOV can only reach
// the image through PROJ LOCK, which a frozen game never rebuilds. The result
// was a shrunken compositor window around an unshrunken render, arriving 1.5 s
// after pausing. Fixing a cosmetic inconsistency was not worth that.
//
// Anything that tries this again has to establish that the game is still
// BUILDING projections, not merely that gameplay is not running.)

void track_main_cam_relocation(ID3D11Resource* dst, bool mainCam, const float eye[3])
{
    void* key = (void*)dst;
    if (mainCam) {
        // is_main_cam() compares the buffer's eye against the DRIVE_CAMERA's
        // ONLY when there is a DRIVE_CAMERA. With none -- the main menu, before
        // anything has been driven -- that test is skipped and every orthonormal
        // 352-byte camera buffer passes, so "main camera matched" carried no
        // information at all and released map mode unconditionally.
        //
        // MEASURED: the release at startup logged haveLogic=0 with a plausible
        // eye, i.e. exactly this path. An unverified match is not evidence of
        // gameplay, so it changes nothing here -- not the state, not the tracked
        // buffer, not the streak. Once a DRIVE_CAMERA exists the comparison is
        // real again and everything below behaves as it always did.
        //
        // Note this needs no screen_state(): that field logged -1 at the same
        // moment and is not usable from this call site.
        float le[3];
        if (!hooks::logic_eye(le)) return;

        g_mainCamMismatchStreak.store(0);
        g_lastMainCamBuf.store(key);

        // A matching main camera is NOT enough to say we are back in gameplay:
        // the main menu renders its own 3D scene with one, which used to drop
        // the seeded kMap straight to kNormal before the player had played at
        // all. The identical screen then behaved one way before the first drive
        // and another way after -- the map's FOV shrink applied or not,
        // depending only on session history.
        //
        // REVERTED 2026-08-09. Two attempts at gating this on screen_state()
        // (only state 3 = gameplay may release the relocated state) both failed,
        // in opposite directions: releasing on the unknown value -1 never
        // engaged at all, and holding on it never released, so gameplay itself
        // stayed stuck in map mode at the reduced FOV.
        //
        // Both outcomes say the same thing -- screen_state() is not delivering
        // a usable value here. It is surfaced in the settings UI now; that
        // reading is the prerequisite for trying this again, and guessing at the
        // gate without it has cost two rounds already.
        //
        // The kMap seed above stays: it costs nothing and is what makes the
        // pre-gameplay screens behave like the post-gameplay ones.
        const CamLocation prev = (CamLocation)g_camLocation.exchange((int)CamLocation::kNormal);
        if (prev != CamLocation::kNormal) {
            // Everything the decision rested on, logged at the moment it is
            // made. is_main_cam() only compares the CB eye against the
            // DRIVE_CAMERA eye WHEN THERE IS ONE -- with no logic camera it
            // accepts any orthonormal 352-byte camera buffer, so "haveLogic=0"
            // on this line means the match was unconditional and the main menu
            // releasing map mode is explained outright.
            float le[3] = {0, 0, 0};
            const bool haveLogic = hooks::logic_eye(le);
            VRLOG("CAMERA: main view camera MATCHED again (was %s) "
                  "eye=(%.1f,%.1f,%.1f) haveLogic=%d logicEye=(%.1f,%.1f,%.1f) "
                  "logicMode=%.2f",
                  prev == CamLocation::kMap ? "MAP" : "GARAGE",
                  eye[0], eye[1], eye[2], (int)haveLogic, le[0], le[1], le[2],
                  hooks::logic_mode());
        }
        return;
    }
    if (key != g_lastMainCamBuf.load()) return;   // not the tracked buffer -- ignore (shadows etc.)
    const int streak = g_mainCamMismatchStreak.fetch_add(1) + 1;
    if (streak < kMainCamMismatchThreshold) return;   // still debouncing the initial relocation

    // Re-evaluate EVERY frame once relocated is confirmed (not just once at
    // the threshold crossing): confirmed via headset test that GARAGE<->MAP
    // can flip later WITHOUT the main cam ever matching again in between
    // (e.g. opening the garage from within the map) -- DRIVE_CAMERA's
    // idle/active state is what actually flips at that moment, so only
    // logging once at the 5-frame mark missed those transitions entirely.
    // Cheap (one atomic load) and only logs on an actual change.
    const bool driveIdle = hooks::logic_camera_idle_ms() > kCamIdleThresholdMs;
    const CamLocation now = driveIdle ? CamLocation::kGarage : CamLocation::kMap;

    const CamLocation prev = (CamLocation)g_camLocation.exchange((int)now);
    if (now != prev)
        VRLOG("CAMERA: you are probably in the %s eye=(%.1f,%.1f,%.1f) (streak=%d)",
              now == CamLocation::kMap ? "MAP" : "GARAGE", eye[0], eye[1], eye[2], streak);
}

// Records the un-modified main-camera matrices for DIBR shift. Taken from the source
// data before any of our edits, so it reflects what the game intends.
void snapshot_camera(const float* f, const CamMatch& m)
{
    const uint32_t newest = xr::current_build();
    AcquireSRWLockExclusive(&g_camSnapLock);
    std::memcpy(g_camView,     f + kOffView/4,     sizeof(g_camView));
    std::memcpy(g_camViewProj, f + kOffViewProj/4, sizeof(g_camViewProj));
    std::memcpy(g_camViewProjPrev, f + kOffViewProjPrev/4, sizeof(g_camViewProjPrev));
    g_camSnapBuild  = m.build;
    g_camSnapNewest = newest;
    g_camSnapHow    = m.how;
    ++g_camSnapSeq;
    ReleaseSRWLockExclusive(&g_camSnapLock);
    g_camSnapValid.store(true);
}

// One camera upload: counted, and snapshotted when it is the main view's.
void observe_camera(const void* src)
{
    const CamMatch m = classify_cam(src, kCamCBSize);
    if (m.how == kCamNot) { g_camRejected.fetch_add(1, std::memory_order_relaxed); return; }
    g_camHowCount[m.how].fetch_add(1, std::memory_order_relaxed);
    if (m.how == kCamBuild || m.how == kCamNearBuild) {
        float mx = g_camBuildDistMax.load(std::memory_order_relaxed);
        while (m.dist > mx && !g_camBuildDistMax.compare_exchange_weak(mx, m.dist)) {}
    }
    snapshot_camera(static_cast<const float*>(src), m);
}

// Shrink amount for ui_hook.cpp's reactive, PS-shader-identity-gated HUD
// viewport shrink (ui_hook.cpp's shrink_ui_draw).
// Started life as a multiplier for a direct CB_DYNAMIC_UI content rewrite
// (that approach -- rewriting the buffer's own scale/translate floats -- was
// removed: confirmed dead end for gameplay HUD, and superseded by shrinking
// the viewport itself instead), but the tuned factor is still shared here
// since [ / ] already drive both mechanisms via the same accessor.

// WHOSE CAMERA THE IMMEDIATE CONTEXT LAST COMMITTED, for depth_probe.
//
// The scene-depth capture accumulates every scene-sized depth unbind in a frame
// with max(), which is only sound while every one of them was drawn from the
// SAME viewpoint. It already guards against two eyes landing in one window, and
// against the camera moving between captures -- but that second test reads
// main_camera_matrices(), i.e. the last MAIN camera, so a pass rendered from
// some other camera entirely reports zero movement and is folded straight in.
//
// A reflection pass is exactly that: its own camera, its own depth, the same
// dimensions as the scene if the engine sizes it that way. max() then keeps
// whichever surface is nearer, so the reflected view's geometry lands on scene
// pixels and the shift moves them by a disparity belonging to another camera.
//
// This is the discriminator that was missing: is_main_cam() already decides,
// per commit, whether a camera CB is the main one. All that was needed was to
// remember the answer.
//
// FAILS OPEN. The flag starts true and only ever goes false when a NON-main
// camera is committed on the IMMEDIATE context. If this engine commits its
// camera buffers on deferred contexts only, nothing here ever fires and the
// capture behaves exactly as it did -- which is the safe direction, since the
// cost of being wrong is a lost frame of depth rather than a wrong one.
std::atomic<bool>     g_immCamMain{true};
std::atomic<uint32_t> g_nonMainCommits{0};
std::atomic<uint32_t> g_immCamCommits{0};

void note_camera_commit(ID3D11DeviceContext* ctx, bool mainCam)
{
    if (!ctx || ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
    g_immCamCommits.fetch_add(1, std::memory_order_relaxed);
    g_immCamMain.store(mainCam, std::memory_order_relaxed);
    if (!mainCam) g_nonMainCommits.fetch_add(1, std::memory_order_relaxed);
}

// --- UpdateSubresource path ---
void STDMETHODCALLTYPE Detour_UpdateSubresource(
    ID3D11DeviceContext* ctx, ID3D11Resource* dst, UINT sub, const D3D11_BOX* box,
    const void* src, UINT rowPitch, UINT depthPitch)
{
    PFN_UpdateSubresource real = (ctx->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE)
        ? real_UpdateSubresource_imm
        : (real_UpdateSubresource_def ? real_UpdateSubresource_def : real_UpdateSubresource_imm);

    if (is_cam352(dst) && src) {
        const bool mainCam = is_main_cam(src, kCamCBSize);
        note_camera_commit(ctx, mainCam);
        track_main_cam_relocation(dst, mainCam, static_cast<const float*>(src));
        observe_camera(src);
    }

    real(ctx, dst, sub, box, src, rowPitch, depthPitch);
}

// --- UpdateSubresource1 (ID3D11DeviceContext1) — the engine uses this heavily ---
using PFN_US1 = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3D11Resource*, UINT,
                                         const D3D11_BOX*, const void*, UINT, UINT, UINT);
PFN_US1 real_US1 = nullptr;

void STDMETHODCALLTYPE Detour_US1(ID3D11DeviceContext1* ctx, ID3D11Resource* dst, UINT sub,
                                  const D3D11_BOX* box, const void* src, UINT rp, UINT dp, UINT cf)
{
    if (is_cam352(dst) && src) {
        const bool mainCam = is_main_cam(src, kCamCBSize);
        note_camera_commit(ctx, mainCam);
        track_main_cam_relocation(dst, mainCam, static_cast<const float*>(src));
        observe_camera(src);
    }
    real_US1(ctx, dst, sub, box, src, rp, dp, cf);
}

// --- Map/Unmap path (dynamic CBs, e.g. water) ---
using PFN_Map = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT,
    D3D11_MAPPED_SUBRESOURCE*);
using PFN_Unmap = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
PFN_Map   real_Map = nullptr;
PFN_Unmap real_Unmap = nullptr;

// Per-thread record of an in-flight map of the camera constant buffer.
thread_local ID3D11Resource* t_mapRes = nullptr;
thread_local void*           t_mapPtr = nullptr;

HRESULT STDMETHODCALLTYPE Detour_Map(
    ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub, D3D11_MAP type,
    UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped)
{
    HRESULT hr = real_Map(ctx, res, sub, type, flags, mapped);
    t_mapRes = nullptr; t_mapPtr = nullptr;
    if (SUCCEEDED(hr) && sub == 0 && mapped && is_cam352(res)) {
        t_mapRes = res; t_mapPtr = mapped->pData;
    }
    return hr;
}

void STDMETHODCALLTYPE Detour_Unmap(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub)
{
    if (res == t_mapRes && t_mapPtr && is_cam352(res)) {
        const bool mainCam = is_main_cam(t_mapPtr, kCamCBSize);
        note_camera_commit(ctx, mainCam);
        track_main_cam_relocation(res, mainCam, reinterpret_cast<const float*>(t_mapPtr));
        observe_camera(t_mapPtr);
    }
    t_mapRes = nullptr; t_mapPtr = nullptr;
    real_Unmap(ctx, res, sub);
}

} // namespace

bool main_camera_matrices(float view[16], float viewProj[16])
{
    if (!g_camSnapValid.load()) return false;
    AcquireSRWLockShared(&g_camSnapLock);
    std::memcpy(view,     g_camView,     sizeof(g_camView));
    std::memcpy(viewProj, g_camViewProj, sizeof(g_camViewProj));
    ReleaseSRWLockShared(&g_camSnapLock);
    return true;
}

bool main_camera_matrices(float view[16], float viewProj[16], float viewProjPrev[16])
{
    if (!g_camSnapValid.load()) return false;
    AcquireSRWLockShared(&g_camSnapLock);
    std::memcpy(view,         g_camView,         sizeof(g_camView));
    std::memcpy(viewProj,     g_camViewProj,     sizeof(g_camViewProj));
    std::memcpy(viewProjPrev, g_camViewProjPrev, sizeof(g_camViewProjPrev));
    ReleaseSRWLockShared(&g_camSnapLock);
    return true;
}

bool main_camera_snap(CamSnapInfo& out)
{
    if (!g_camSnapValid.load()) return false;
    AcquireSRWLockShared(&g_camSnapLock);
    out.seq      = g_camSnapSeq;
    out.build    = g_camSnapBuild;
    out.newestAt = g_camSnapNewest;
    out.how      = g_camSnapHow;
    ReleaseSRWLockShared(&g_camSnapLock);
    return true;
}

// The MAP screen, now identified from the game's own camera POSE -- pitched
// -45 deg with its eye at the origin. See hooks::map_by_pose().
//
// This used to be "the main camera relocated and the drive camera is not idle",
// which needed the relocation tracker to arm first -- and that needs
// logic_eye(), which does not exist until something has been driven. So a
// menu -> garage -> map startup could never be classified, and the workaround
// was to treat map, garage AND menus alike. The pose test has no such
// dependency and separates the map from the other two.
//
// The old classifier still runs, and is kept as in_gameplay() -- a good
// marker in its own right even though nothing depends on it now.
//
// THE POSE CLASSIFIERS DECIDE AGAIN, 2026-08-24. These briefly answered from the
// game's own screen-state field instead; that whole mechanism is gone (see the
// note at the top of viewbuild_hook.cpp). Finding the field was never the
// problem -- knowing which of thousands of near-identical objects holds it was,
// and a marker that is usually right is worse than a heuristic with a known
// failure. The known failure here: map_by_pose() calls the map at any camera
// pitched -45 deg near the origin, which gameplay can reach.
//
// EITHER OF TWO ANSWERS, chosen by the Screen gate setting. Legacy is the pose
// classifier described above. Fingerprint asks what the game is DRAWING
// instead of where the camera is, which is immune to that known failure but
// depends on marker lists that are short and were collected by hand. Both are
// live at once -- only which one is READ changes -- so the log's SCREEN
// FINGERPRINT lines say what the other one would have decided.
bool in_map_view()
{
    if (hooks::screen_gate_direct())
        return hooks::direct_screen() == hooks::kScreenMap;
    return hooks::map_by_pose();
}

// TRUE while the world is actually being played; false on map/garage/menus.
// Still true in the PAUSE menu -- the world is loaded and the camera has not
// relocated. The callers that care about the pause overlay (xr_mirror.cpp) test
// the drive-camera idle timer for it.
//
// UNDER THE FINGERPRINT GATE, GAMEPLAY IS THE COMPLEMENT -- not a positive
// identification. It used to be `fingerprint == kScreenGameplay`, which is
// wrong in a way that only shows up once the marker lists are live: gameplay
// shares its interface with the garage and its world with everything, so it may
// well never have a shader of its own, and in practice its list is empty. A
// test that can never pass silently switched OFF everything downstream of it --
// the windscreen smudge layer captures and composites nothing when this is
// false, and it does not announce that.
//
// So gameplay is now "not a screen we can positively identify as static, and
// the drive camera is actually running". Both halves are needed: the marker
// lists rule out the menu and the map, and the idle timer rules out the garage
// and the pause menu, which have no markers to be identified by. Neither can be
// wrong for seconds at a time the way an empty-list test was.
//
// The idle threshold matches xr_mirror.cpp's kDibrIdleMs deliberately: both are
// asking the same question -- is a world being driven right now -- and two
// numbers for one question is how they drift apart.
constexpr uint64_t kGameplayIdleMs = 400;

bool in_gameplay()
{
    // THE MENU CAMERA ON SCREEN is a positive answer under either gate: the
    // image was rendered by the main-menu camera builder, which only runs on
    // the main menu and the garage and map opened from it (xr_mirror.h,
    // onscreen_camera_builder). Per frame, so the menus no longer wait for the
    // drive camera's idle timer.
    if (xr::onscreen_camera_builder() == xr::kBuilderMenu) return false;
    if (hooks::screen_gate_direct()) {
        const int s = hooks::direct_screen();
        // A POSITIVE answer is taken at face value, in both directions: the
        // drive camera's own mode field can now say gameplay, which it could not
        // when gameplay had to be recognised by a marker list it may never have.
        if (hooks::direct_screen_positive())
            return s == hooks::kScreenGameplay;
        // Otherwise `s` is the garage FALLBACK -- "nothing claimed you" -- which
        // is not evidence of anything. Fall through to the drive camera actually
        // running, which is what carried this before the mode field existed.
        // Only the map can appear here now -- there is no menu marker list
        // any more, so a menu reaches this function as the garage fallback and
        // is answered by the drive camera below, which is right for it.
        if (s == hooks::kScreenMap) return false;
        return hooks::logic_camera_ever_ticked() &&
               hooks::logic_camera_idle_ms() <= kGameplayIdleMs;
    }
    return (CamLocation)g_camLocation.load() == CamLocation::kNormal;
}


bool     imm_camera_is_main()      { return g_immCamMain.load(std::memory_order_relaxed); }
uint32_t non_main_camera_commits() { return g_nonMainCommits.exchange(0, std::memory_order_relaxed); }


void install_cbuffer_hook(IDXGISwapChain* swapchain)
{
    bool expected = false;
    if (!g_hooked.compare_exchange_strong(expected, true))
        return;

    ComPtr<ID3D11Device> dev;
    if (!swapchain || FAILED(swapchain->GetDevice(__uuidof(ID3D11Device), (void**)&dev))) {
        g_hooked = false;
        return;
    }
    ComPtr<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(&ctx);
    if (!ctx) { g_hooked = false; return; }

    void** vt = *reinterpret_cast<void***>(ctx.Get());

    // Deferred-context vtable, for UpdateSubresource -- the engine records on
    // deferred contexts too, and a hook on the immediate vtable alone would
    // miss every write made there.
    ComPtr<ID3D11DeviceContext> defProbe;
    dev->CreateDeferredContext(0, &defProbe);
    void** vtDef = defProbe ? *reinterpret_cast<void***>(defProbe.Get()) : nullptr;
    if (!vtDef) VRLOG("cbuffer hook: CreateDeferredContext FAILED -- deferred-side hooks skipped");

    // Idempotent: MH_ERROR_ALREADY_CREATED/MH_ERROR_ENABLED mean a PRIOR call
    // already hooked this address successfully -- still live and working, not
    // a failure. Without this, a retry (install_cbuffer_hook is effectively
    // called every Present) would treat its own earlier success as fresh
    // failure, permanently reset g_hooked, and spam-retry forever.
    auto hook = [&](void** vtable, int idx, void* detour, void** orig, const char* name) -> bool {
        void* target = vtable[idx];
        MH_STATUS c = MH_CreateHook(target, detour, orig);
        if (c != MH_OK && c != MH_ERROR_ALREADY_CREATED) {
            VRLOG("%s hook FAILED", name);
            return false;
        }
        MH_STATUS e = MH_EnableHook(target);
        if (e != MH_OK && e != MH_ERROR_ENABLED) {
            VRLOG("%s hook FAILED", name);
            return false;
        }
        return true;
    };

    bool ok = hook(vt, kIdxUpdateSubresource, &Detour_UpdateSubresource,
                   reinterpret_cast<void**>(&real_UpdateSubresource_imm), "UpdateSubresource(imm)");
    if (vtDef) {
        if (vtDef[kIdxUpdateSubresource] == vt[kIdxUpdateSubresource]) {
            real_UpdateSubresource_def = real_UpdateSubresource_imm;
        } else {
            ok = hook(vtDef, kIdxUpdateSubresource, &Detour_UpdateSubresource,
                      reinterpret_cast<void**>(&real_UpdateSubresource_def), "UpdateSubresource(def)") && ok;
        }
    }
    ok = hook(vt, kIdxMap,   &Detour_Map,   reinterpret_cast<void**>(&real_Map),   "Map")   && ok;
    ok = hook(vt, kIdxUnmap, &Detour_Unmap, reinterpret_cast<void**>(&real_Unmap), "Unmap") && ok;

    // UpdateSubresource1 lives on the ID3D11DeviceContext1 vtable (same object).
    ComPtr<ID3D11DeviceContext1> ctx1;
    if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&ctx1)))
        hook(vt, kIdxUpdateSubresource1, &Detour_US1, reinterpret_cast<void**>(&real_US1), "UpdateSubresource1");

    if (!ok) { g_hooked = false; return; }
    VRLOG("camera CB hooks installed (UpdateSubresource[1] + Map/Unmap)");
}

void cbuffer_hook_on_present()
{
    // How the main view's camera was recognised, every 10 s. "on a build" is
    // the exact match that names the build; a large max distance there, or
    // most uploads only "near a build", would mean the game moves the camera
    // after the mod hands it over -- see classify_cam().
    static ULONGLONG t0 = 0;
    const ULONGLONG now = GetTickCount64();
    if (!t0) t0 = now;
    if (now - t0 >= 10000) {
        t0 = now;
        const uint32_t onBuild = g_camHowCount[kCamBuild].exchange(0);
        const uint32_t nearOne = g_camHowCount[kCamNearBuild].exchange(0);
        const uint32_t drive   = g_camHowCount[kCamLogic].exchange(0);
        const uint32_t dir     = g_camHowCount[kCamBuildDir].exchange(0);
        const uint32_t unver   = g_camHowCount[kCamUnverified].exchange(0);
        const uint32_t rej     = g_camRejected.exchange(0);
        const uint32_t away    = g_camTurnedAway.exchange(0);
        const float    dmax    = g_camBuildDistMax.exchange(0.0f);
        if (onBuild || nearOne || drive || dir || unver)
            VRLOG("camera: main view uploads in 10 s -- on a build %u, near one %u (max "
                  "%.3f m), drive camera %u, a build's direction only %u, unverified %u; "
                  "other cameras %u (%u of them passed a lesser test while the builds "
                  "matched -- mirrors)", onBuild, nearOne, dmax, drive, dir, unver, rej, away);
    }
}

} // namespace hooks
