#include "hooks/marker_camera.h"
#include "hooks/game_addresses.h"
#include "hooks/cbuffer_hook.h"   // in_map_view(), for the status line
#include "render/ui_layer.hpp"
#include "xr/xr_mirror.h"
#include "common/log.h"

#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <MinHook.h>

#pragma intrinsic(_ReturnAddress)

namespace hooks {
namespace {

// combine::CAMERA as the UI camera reads it. Every offset is checked in the
// slot that reads it before this file is allowed to run (game_addresses.cpp,
// kUiCameraSlots).
constexpr int kCamView     = 0x00;   // 4x4 row-vector view: p_view = p_world * V
constexpr int kCamProj     = 0x40;   // 4x4 row-vector projection, w = view z
constexpr int kCamViewInv  = 0x80;   // its inverse; a first dword of -1 = invert on demand
constexpr int kCamViewProj = 0xC0;
constexpr int kCamFov      = 0x108;  // radians; the 3D markers take it as the VERTICAL FOV
constexpr int kCamAspect   = 0x10C;  // width / height
constexpr int kCamSize     = 0x118;
// ui3D_CAMERA_MR: the camera pointer its Update refreshes.
constexpr int kUiCamPointer = 0x18;

// A plane this close to the eye along its own normal, or behind it, has no
// projection onto it worth the name.
constexpr float kMinPlaneDepthM = 0.05f;

using PFN_Update = void* (*)(void* self);
PFN_Update real_update = nullptr;
std::atomic<bool> g_installed{false};
uintptr_t g_exeBase = 0;

// String literals only, so the menu can read it from any thread.
std::atomic<const char*> g_status{"not installed"};

// ONE REPLACEMENT CAMERA PER UI CAMERA OBJECT, so two of them updated on
// different threads never write one block. STATIC, and that is not a detail:
// camera_fov.cpp sweeps the heap for objects whose viewProj is view * proj and
// writes the culling FOV into every one it finds, and a block of ours on the
// heap would be one of them. The sweep skips the DLL's own image.
//
// A block is never freed, so an object still pointing at one after the mode is
// switched off reads a valid camera until its next Update puts the game's back.
struct alignas(16) Slot {
    void*     owner = nullptr;
    ULONGLONG lastUse = 0;
    float     cam[kCamSize / 4] = {};
};
constexpr int kSlots = 8;
Slot    g_slots[kSlots];
SRWLOCK g_slotLock = SRWLOCK_INIT;

Slot& slot_for(void* owner)
{
    AcquireSRWLockExclusive(&g_slotLock);
    Slot* s = nullptr;
    Slot* oldest = &g_slots[0];
    for (Slot& x : g_slots) {
        if (x.owner == owner) { s = &x; break; }
        if (x.lastUse < oldest->lastUse) oldest = &x;
    }
    if (!s) { s = oldest; s->owner = owner; }
    s->lastUse = GetTickCount64();
    ReleaseSRWLockExclusive(&g_slotLock);
    return *s;
}

// What it is doing, logged when that changes -- capped, because a camera that
// drops in and out of matching a build would otherwise write a line a frame.
std::atomic<int> g_statusLines{0};
constexpr int kMaxStatusLines = 40;

void set_status(const char* s)
{
    if (g_status.exchange(s) == s) return;
    const int n = g_statusLines.fetch_add(1);
    if (n < kMaxStatusLines)       VRLOG("marker camera: %s", s);
    else if (n == kMaxStatusLines) VRLOG("marker camera: (further changes not logged)");
}

// Every distinct place Update is called from, once each: which of the game's UI
// modules keep a camera of their own. The screen markers' call returns to
// exe+0x1044DF5 in build 0x6AB52F5D.
constexpr int kMaxCallers = 8;
std::atomic<uintptr_t> g_callers[kMaxCallers];

void note_caller(void* ret)
{
    const uintptr_t r = reinterpret_cast<uintptr_t>(ret) - g_exeBase;
    for (std::atomic<uintptr_t>& c : g_callers) {
        uintptr_t cur = c.load(std::memory_order_relaxed);
        if (cur == r) return;
        if (cur) continue;
        if (c.compare_exchange_strong(cur, r)) {
            VRLOG("marker camera: the UI camera is updated from exe+0x%llX", (unsigned long long)r);
            return;
        }
        if (cur == r) return;
    }
}

float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// The game's camera, before anything is taken from it: an orthonormal view, and
// a projection that divides by view z, row-vector D3D, as the engine builds it.
bool usable(const float* cam)
{
    const float* v = cam + kCamView / 4;
    for (int r = 0; r < 3; ++r) {
        const float l = dot3(v + r * 4, v + r * 4);
        if (!(l > 0.96f && l < 1.04f)) return false;
    }
    const float* p = cam + kCamProj / 4;
    return std::fabs(p[11] - 1.0f) < 0.01f && std::fabs(p[15]) < 1.0e-4f;
}

// p_view = p * R + t, so the eye is where that is zero -- -t * R^T -- and the
// forward axis is R's third column.
void eye_and_forward(const float* v, float eye[3], float fwd[3])
{
    for (int i = 0; i < 3; ++i)
        eye[i] = -(v[12] * v[i * 4] + v[13] * v[i * 4 + 1] + v[14] * v[i * 4 + 2]);
    fwd[0] = v[2];
    fwd[1] = v[6];
    fwd[2] = v[10];
}

// THE REPLACEMENT CAMERA: a copy of the game's, with these replaced.
//
//   VIEW        moved to the middle of the eyes (pl.origin) and turned to look
//               along the plane's normal -- the view's axes times the plane's,
//               so view z is depth along that normal. The markers' "behind the
//               camera" test is view z against the near plane, which then means
//               "on the plane's side of the eyes": exactly the points whose ray
//               can reach it.
//   PROJECTION  the plane's edges at NDC +-1. A point at (x, y, z) in that view
//               crosses the plane, at depth d, at (x, y) * d / z; less the
//               plane's centre (cx, cy), over its half-size, is the NDC -- the
//               off-centre perspective D3DXMatrixPerspectiveOffCenterLH builds,
//               row-vector, w = z. Depth stays the game's.
//   INVERSE     rebuilt rather than marked for the game to invert, so nothing of
//               the game's ever writes into this block.
//   FOV, ASPECT the plane's, for the 3D markers' perspective, which the game
//               builds as PerspectiveFovLH(m_fFOV, m_fAspect): vertical.
bool build_camera(const float* real, const xr::UiPlaneInCamera& pl, float* out)
{
    const float* r = pl.right;
    const float* u = pl.up;
    const float* n = pl.normal;
    const float cx = dot3(pl.center, r), cy = dot3(pl.center, u), d = dot3(pl.center, n);
    if (!(d > kMinPlaneDepthM)) return false;

    std::memcpy(out, real, kCamSize);

    const float* V = real + kCamView / 4;
    float eye[3], fwd[3];
    eye_and_forward(V, eye, fwd);
    const float org[3] = {eye[0] + pl.origin[0], eye[1] + pl.origin[1], eye[2] + pl.origin[2]};
    float* v = out + kCamView / 4;
    for (int i = 0; i < 3; ++i) {
        const float* row = V + i * 4;
        v[i * 4 + 0] = dot3(row, r);
        v[i * 4 + 1] = dot3(row, u);
        v[i * 4 + 2] = dot3(row, n);
        v[i * 4 + 3] = 0.0f;
    }
    // p_view = (p - org) * R, so the translation row is -org * R.
    for (int j = 0; j < 3; ++j)
        v[12 + j] = -(org[0] * v[j] + org[1] * v[4 + j] + org[2] * v[8 + j]);
    v[15] = 1.0f;

    const float* P = real + kCamProj / 4;
    float* p = out + kCamProj / 4;
    std::memset(p, 0, 16 * sizeof(float));
    p[0]  = 2.0f * d / pl.width;
    p[5]  = 2.0f * d / pl.height;
    p[8]  = -2.0f * cx / pl.width;
    p[9]  = -2.0f * cy / pl.height;
    p[10] = P[10];
    p[11] = 1.0f;
    p[14] = P[14];

    float* inv = out + kCamViewInv / 4;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) inv[i * 4 + j] = v[j * 4 + i];
        inv[i * 4 + 3] = 0.0f;
        inv[12 + i] = org[i];
    }
    inv[15] = 1.0f;

    float* vp = out + kCamViewProj / 4;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += v[i * 4 + k] * p[k * 4 + j];
            vp[i * 4 + j] = s;
        }

    out[kCamFov / 4]    = 2.0f * std::atan(0.5f * pl.height / d);
    out[kCamAspect / 4] = pl.width / pl.height;
    return true;
}

// WHEN IT APPLIES: null, or why not.
//   A 2D screen, or a runtime that takes one quad: the UI is on the panel it
//     belongs to (and ui_plane_in_camera() has no plane to give on a 2D screen).
//   Every other screen, the garage and the menus included since 2026-11-05.
//     They were left out because their cameras come from the menu builder,
//     whose conventions had not been checked -- but the map is built by that
//     same builder whichever way it is opened, and its markers were placed
//     right there from the start. The map's icons are taken to be screen
//     markers like the HUD's (the game keeps one marker manager, and it runs
//     there); unconfirmed.
const char* not_applying()
{
    if (!uilayer::active() || xr::ui_stays_in_frame()) return "idle -- the UI is not on its own plane";
    return nullptr;
}

void log_first(const xr::UiPlaneInCamera& pl)
{
    static std::atomic<bool> s_once{false};
    if (s_once.exchange(true)) return;
    VRLOG("marker camera: first replacement -- build %u (eye %d, camera within %.3f m of it), "
          "%s plane %.2f x %.2f m, centre (%.2f, %.2f, %.2f) m in the camera's frame, normal "
          "(%.3f, %.3f, %.3f)", pl.build, pl.eye, pl.matchM, pl.pinned ? "pinned" : "head-locked",
          pl.width, pl.height, pl.center[0], pl.center[1], pl.center[2],
          pl.normal[0], pl.normal[1], pl.normal[2]);
}

void retarget(void* self)
{
    __try {
        void** ptr = reinterpret_cast<void**>(static_cast<uint8_t*>(self) + kUiCamPointer);
        const float* real = static_cast<const float*>(*ptr);
        if (!real) { set_status("idle -- the game has no active camera"); return; }
        if (const char* why = not_applying()) { set_status(why); return; }
        if (!usable(real)) { set_status("idle -- the game's camera is not one this understands"); return; }

        float eye[3], fwd[3];
        eye_and_forward(real + kCamView / 4, eye, fwd);
        xr::UiPlaneInCamera pl;
        if (!xr::ui_plane_in_camera(eye, fwd, pl)) {
            set_status("idle -- no plane yet, or the camera matches no recent build");
            return;
        }
        Slot& s = slot_for(self);
        if (!build_camera(real, pl, s.cam)) {
            set_status("idle -- the plane is not in front of the eye");
            return;
        }
        *ptr = s.cam;
        set_status(in_map_view() ? "placing the map's markers"
                   : pl.pinned   ? "placing markers on the pinned plane"
                                 : "placing markers on the head-locked plane");
        log_first(pl);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        set_status("idle -- the camera could not be read");
    }
}

void* detour_update(void* self)
{
    void* ret = real_update(self);
    note_caller(_ReturnAddress());
    retarget(self);
    return ret;
}

} // namespace

void install_marker_camera()
{
    static std::atomic<bool> s_done{false};
    if (s_done.exchange(true)) return;
    g_exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const uintptr_t rva = game_addresses().uiCameraUpdate;
    if (!rva) {
        g_status.store("unavailable -- the UI camera is not in this game build");
        VRLOG("marker camera: NOT installed -- the UI camera's Update was not found");
        return;
    }
    void* target = reinterpret_cast<void*>(g_exeBase + rva);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&detour_update),
                      reinterpret_cast<void**>(&real_update)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        g_status.store("unavailable -- hooking the UI camera failed");
        VRLOG("marker camera: hook FAILED at exe+0x%llX", (unsigned long long)rva);
        return;
    }
    g_installed.store(true);
    g_status.store("waiting for the UI camera");
    VRLOG("marker camera: UI camera Update hooked at exe+0x%llX", (unsigned long long)rva);
}

bool marker_camera_available() { return g_installed.load(); }

const char* marker_camera_status() { return g_status.load(); }

} // namespace hooks
