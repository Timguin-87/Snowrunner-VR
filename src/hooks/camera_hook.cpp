#include "hooks/camera_hook.h"
#include "hooks/pattern_scan.h"
#include "xr/xr_mirror.h"
#include "common/log.h"

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <atomic>

#include <MinHook.h>

namespace hooks {
namespace {

// --- recon: docs/recon.md, camera commit function ---
const char* kSig =
    "48 8B C4 F3 0F 11 50 18 48 89 50 10 55 53 56 57 41 55 41 56 "
    "48 8D A8 ?? ?? FF FF 48 81 EC ?? ?? 00 00 0F 29 70 A8 48 8D B1 84 01 00 00";

// Offsets within combine::DRIVE_CAMERA (see recon).
constexpr int kStageEye    = 0x184;  // 3 floats
constexpr int kStageTarget = 0x190;  // 3 floats
constexpr int kModeScale   = 0x1A0;  // 1.0 in cockpit, 0.5 exterior

// THIS HOOK NO LONGER WRITES THE CAMERA. It reads three fields and publishes
// them; head look goes in downstream, at detour_combine2 / detour_projbuild.
// The sign conventions, the positional-vs-rotation choice and the forced-yaw
// diagnostic that used to live here as constants went with that write -- Phase
// 2 scaffolding, unread since. The signs that survived are at their real
// injection sites in viewbuild_hook.cpp, where they are runtime-tunable rather
// than compiled in.

struct V3 { float x, y, z; };
V3   sub(V3 a, V3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }

float* fptr(void* cam, int off) { return reinterpret_cast<float*>(reinterpret_cast<char*>(cam) + off); }
V3     readV3(void* cam, int off) { float* p = fptr(cam, off); return {p[0], p[1], p[2]}; }

using PFN_Cam = void*(*)(void* cam, void* rdx, float dt);
PFN_Cam real_cam = nullptr;

// Published logic-camera eye and fwd for the constant-buffer hook.
SRWLOCK g_eyeLock = SRWLOCK_INIT;
V3   g_logicEye{};
V3   g_logicFwd{};
bool g_haveEye = false;

// Current view-mode discriminator (+0x1A0): 1.0 cockpit, 0.5 exterior. Default to
// exterior so cockpit-gated logic never misfires before the DRIVE_CAMERA runs
// (e.g. the first boot menu).
std::atomic<float> g_logicMode{0.5f};

// combine::DRIVE_CAMERA is the vehicle camera commit function -- it only runs
// while actually piloting the truck (cockpit or exterior/orbit). The map
// screen renders its own separate 3D camera, so DRIVE_CAMERA simply stops
// being called while the map is open (same for pause/garage/other non-drive
// screens). Track how recently it last ran as a cheap, no-recon-required
// proxy for "not in the normal drive camera" -- a real map-camera identity
// would need its own signature/recon pass, but this is buildable today and
// testable immediately by comparing against what's on screen in-headset.
std::atomic<uint64_t> g_lastCamTickMs{0};

// The live DRIVE_CAMERA instance, published for the blend-target probe in
// viewbuild_hook.cpp (the engine's camera blend writes somewhere inside this
// object, and the probe searches it). Read-only for that use.
std::atomic<void*> g_driveCamera{nullptr};

// Camera-mode carry-over across a truck swap; see apply_headlook(). g_modeCam
// is the object the mode was last read from, g_modeFirst what that object
// reported on its first tick, and g_modeAdopted whether it has since written
// something of its own.
std::atomic<void*> g_modeCam{nullptr};

// THE COCKPIT FLAG, MEASURED AND BAKED. Found 2026-08-26 with a capture/compare
// probe (removed with it), and it landed exactly where MudRunner's PDB says it
// should: `bool m_isThirdPerson` at +0x017, immediately beside
// `m_isWantThirdPerson`.
//
// That agreement is why this is worth trusting as a constant rather than a
// setting. The offset was arrived at by measurement, and the measurement then
// landed on the field the engine's own symbols name for the job -- two
// independent routes to the same byte.
//
// POLARITY IS THE NAME'S, NOT THE NICKNAME'S: m_isThirdPerson is 0 in the cab,
// so 0 MEANS COCKPIT. Spelled out here because adopting it the other way round
// inverts every gate that reads it.
constexpr int kCockpitFlagOffset = 0x17;
constexpr int kCockpitFlagIsCockpit = 0;
std::atomic<float> g_modeFirst{0.0f};
std::atomic<bool>  g_modeAdopted{false};

void apply_headlook(void* cam)
{
    g_lastCamTickMs.store(GetTickCount64());
    g_driveCamera.store(cam);
    // Publish the staged eye/fwd every frame so the CB hook can match the main view.
    V3 e0 = readV3(cam, kStageEye);
    V3 t0 = readV3(cam, kStageTarget);
    AcquireSRWLockExclusive(&g_eyeLock);
    g_logicEye = e0; 
    g_logicFwd = sub(t0, e0);
    g_haveEye = true;
    ReleaseSRWLockExclusive(&g_eyeLock);
    // CAMERA MODE CARRIES ACROSS A TRUCK SWAP.
    //
    // MEASURED 2026-08-19: switching to another truck on the same level gives a
    // NEW DRIVE_CAMERA object, and its mode field reads 0.50 -- "not cockpit" --
    // even though the game is rendering the cockpit. The hook is ticking on the
    // right object (idle=0ms, the pointer changed), the field simply never got
    // written, because the game only writes it on a camera-mode TRANSITION and
    // swapping trucks is not one. It stays wrong until you go to orbit and back.
    //
    // That killed the cockpit levelling lock and the game-rotation warp, both of
    // which gate on logic_mode() >= 0.75.
    //
    // Since the game preserves the camera MODE across the swap -- switch trucks
    // in the cab and you are still in the cab -- the right reading is the one we
    // already had. So on a new object, hold the previous mode and adopt the new
    // object's value the moment it reports something different from what it
    // first reported, i.e. as soon as the game actually writes the field.
    {
        const float m = *fptr(cam, kModeScale);
        void* const prev = g_modeCam.exchange(cam);
        if (prev != cam) {
            g_modeFirst.store(m);
            // First camera of the session: nothing to carry over, take it as-is.
            g_modeAdopted.store(prev == nullptr);
        }
        if (g_modeAdopted.load()) {
            g_logicMode.store(m);
        } else if (m != g_modeFirst.load()) {
            g_modeAdopted.store(true);      // the game wrote it -- it is real now
            g_logicMode.store(m);
        }
    }

    // CAMERA POSITION SMOOTHING IS LEFT ALONE. [cam+0x170] is a second,
    // independent switch for the same exponential blend viewbuild_hook.cpp's
    // opcode patch controls, and this function used to zero it every frame --
    // which silently defeated that toggle, since re-enabling the opcode did
    // nothing while this line wrote the flag straight back to "off". The write
    // is gone rather than left switched off: the jitter it was added for came
    // from the AER offset living at combine1, which it no longer does.

    // The logic camera is deliberately left untouched. Head look is applied
    // downstream (detour_combine2 / detour_projbuild), and an experiment that
    // ALSO turned this camera -- on the premise that DRIVE_CAMERA is logic-only
    // (commit 7a0e740) and would steer camera-generated geometry -- was tried
    // 2026-08-07 and made no difference to the zone ribbon. Removed rather than
    // left as a dead switch; see docs/cockpit_camera.md.
}

void* detour_cam(void* cam, void* rdx, float dt)
{
    if (cam)
        apply_headlook(cam);
    return real_cam(cam, rdx, dt);
}

} // namespace

bool install_camera_hook()
{
    HMODULE exe = GetModuleHandleW(nullptr);
    uint8_t* fn = pattern_scan(exe, kSig);
    if (!fn) {
        VRLOG("camera signature not found — head tracking disabled (unknown game version)");
        return false;
    }
    VRLOG("camera commit fn at %p (exe+0x%llX)", fn,
          (unsigned long long)(fn - reinterpret_cast<uint8_t*>(exe)));

    if (MH_CreateHook(fn, reinterpret_cast<void*>(&detour_cam),
                      reinterpret_cast<void**>(&real_cam)) != MH_OK ||
        MH_EnableHook(fn) != MH_OK) {
        VRLOG("camera hook install FAILED");
        return false;
    }
    VRLOG("camera hook installed");
    return true;
}

bool logic_eye(float out[3])
{
    AcquireSRWLockShared(&g_eyeLock);
    bool have = g_haveEye;
    if (have) { out[0] = g_logicEye.x; out[1] = g_logicEye.y; out[2] = g_logicEye.z; }
    ReleaseSRWLockShared(&g_eyeLock);
    return have;
}

float logic_mode()
{
    // THE BOOL WINS WHEN WE HAVE IT. Everything here reports the same thing --
    // cockpit or not -- but the bool is state the game keeps current, while the
    // float is a blend it only writes on a transition. See the note at
    // drive_camera_cockpit(): that difference is the truck-swap staleness, and
    // the carry-over heuristic below only exists to survive it.
    const int c = drive_camera_cockpit();
    if (c >= 0) return c ? 1.0f : 0.5f;
    return g_logicMode.load();
}


uint64_t logic_camera_idle_ms()
{
    const uint64_t last = g_lastCamTickMs.load();
    if (last == 0) return 0;   // never ticked yet -- see logic_camera_ever_ticked()
    const uint64_t now = GetTickCount64();
    return now > last ? now - last : 0;
}

bool logic_camera_ever_ticked() { return g_lastCamTickMs.load() != 0; }

// --- DRIVE_CAMERA mode field ---------------------------------------------
// See the long note in camera_hook.h for what this is and why the offset has to
// be found rather than assumed.

// IS THE OBJECT WE HOLD A POINTER TO STILL ALIVE.
//
// g_driveCamera is whatever the hook saw last, and the hook stops running the
// moment the drive camera does -- in a menu, on the map, and possibly in the
// garage. From then on the pointer refers to an object the game may have freed
// and reallocated, so anything read through it is not stale data, it is somebody
// else's data. The SEH guards below catch an unmapped page; they cannot catch a
// reused allocation that happens to hold a 2.
//
// So every read through that pointer is gated on the camera having ticked very
// recently. A frame is ~5-10 ms, so this is many frames of slack and only
// expires when the camera has genuinely stopped.
constexpr uint64_t kDriveLiveMs = 200;

bool drive_camera_live()
{
    return logic_camera_ever_ticked() && logic_camera_idle_ms() <= kDriveLiveMs;
}

int drive_camera_cockpit()
{
    void* const cam = g_driveCamera.load(std::memory_order_relaxed);
    if (!cam || !drive_camera_live()) return -1;
    unsigned char v = 0xFF;
    __try {
        v = *(reinterpret_cast<const unsigned char*>(cam) + kCockpitFlagOffset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    // Anything but a clean 0/1 means we are not looking at the flag -- a freed
    // object, or a build that moved it. Say so rather than returning a number
    // that reads like an answer.
    if (v > 1) return -1;
    return ((int)v == kCockpitFlagIsCockpit) ? 1 : 0;
}

} // namespace hooks
