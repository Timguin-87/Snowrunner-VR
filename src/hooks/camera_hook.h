#pragma once
#include <cstdint>

// THE DRIVE CAMERA, OBSERVED. Pattern-scans SnowRunner.exe for the camera
// commit function (recon: +0x85F4D0, RCX = combine::DRIVE_CAMERA) and
// MinHook-detours it.
//
// IT READS, IT DOES NOT WRITE. The detour used to rewrite the staged eye/target
// (+0x184 / +0x190) from the head pose, on the theory that turning the logic
// camera would turn everything downstream of it. It does not: the engine
// recomputes the render camera per frame, well past this stage, so head look
// goes in at viewbuild_hook.cpp's per-frame sites instead. What is left here is
// the four things only this object can answer -- where the logic camera is
// looking, whether it is in the cockpit, whether it is running at all, and a
// pointer to it for the FOV declaration.
//
// Requires hooks::init() (MinHook) to have run. Safe to call once; returns
// false if the signature is not found (unknown game version), which costs the
// cockpit/garage distinction rather than the whole mod.
namespace hooks {

bool install_camera_hook();

// published each frame by the camera hook so the constant-buffer hook can
// identify the main view. Returns false until the camera hook has run once.
bool logic_eye(float out[3]);

// Current view-mode discriminator: ~1.0 cockpit, ~0.5 exterior/orbit.
//
// Prefers drive_camera_cockpit(), and only falls back to the +0x1A0 blend float
// (with the truck-swap carry-over heuristic) when there is no live drive camera
// to ask -- the garage and the menus. Callers keep testing `>= 0.75` either way.
float logic_mode();


// Milliseconds since combine::DRIVE_CAMERA (the vehicle camera commit
// function) last ran, or 0 if it has never run yet. DRIVE_CAMERA only
// updates while actually piloting the truck -- it stops being called on the
// map screen (and pause/garage/other non-drive screens), so a large value
// here is a cheap proxy for "not in the normal drive camera" without needing
// a dedicated signature/recon pass on the map's own camera.
uint64_t logic_camera_idle_ms();

// THE GARAGE IS NOT A DRIVE_CAMERA MODE IN SNOWRUNNER. Tried and MEASURED
// 2026-08-26; do not rebuild it.
//
// MudRunner's PDB names combine::DRIVE_CAMERA::m_nCurrentMode, an enum
// { MODE_DEFAULT, MODE_TRAILER, MODE_GARAGE }, which said the garage is not a
// screen at all but the drive camera in another mode looking at the same world
// -- neatly explaining why the shader probe never found anything that draws only
// there. It looked like one integer would settle a question no marker list can.
//
// It does not, because SnowRunner's garage DOES NOT TICK DRIVE_CAMERA AT ALL,
// before or after driving. The field is unreadable exactly when it would be
// needed, and reading it through a pointer whose object has stopped updating is
// worse than not reading it (see drive_camera_live()). The whole mechanism --
// offset, config key and probe target -- was removed again rather than left as a
// path that can never fire.
//
// What that leaves is better than it sounds: the drive camera STOPPING is itself
// the signal, and logic_camera_idle_ms() measures it directly. It is not a proxy
// for the garage, it is a direct reading of "the drive camera is not running",
// which is true in the garage, the menus and the map and false in gameplay.
//
// --- cockpit vs exterior --------------------------------------------------
// The OTHER field worth having, and for the same reason. MudRunner's PDB has
// `bool m_isThirdPerson` at +0x017, immediately beside `m_isWantThirdPerson` --
// a discrete state, separate from any blend.
//
// SnowRunner's mod currently reads a FLOAT (+0x1A0, 1.0 cockpit / 0.5 exterior).
// A float with those values is an interpolation parameter, which is precisely
// why the game only writes it on a camera-mode TRANSITION: it is the thing being
// animated. Jump from one truck's cockpit straight into another's and no
// transition happens, so the new DRIVE_CAMERA object's float is never written
// and everything gated on cockpit -- the truck level lock, the yaw lock, the
// game-rotation warp -- stays off until you exit to orbit and come back.
// camera_hook.cpp carries the old value across the swap to paper over that; a
// bool the game keeps up to date needs no such trick.
//
// FOUND AND BAKED IN: +0x017, where 0 means the cockpit. It is MudRunner's
// `m_isThirdPerson`, so the polarity is the opposite of what "cockpit flag"
// suggests -- corrected here rather than left to callers. See the note at
// kCockpitFlagOffset in camera_hook.cpp for how it was arrived at.
//
// Returns -1 = unknown (no live drive camera, or the byte is not a clean 0/1),
// 0 = exterior, 1 = cockpit.
int  drive_camera_cockpit();

// TRUE while combine::DRIVE_CAMERA is actually ticking, i.e. while the pointer
// the hook published still refers to a live object. Every read of the two fields
// above is gated on this: once the drive camera stops, the game is free to
// release that object, and a value read out of a reused allocation is worse than
// no value at all.
//
// It is also why entering the garage straight from the main menu reports no mode:
// nothing has driven, so no drive camera exists to ask. That case is handled
// upstream by logic_camera_ever_ticked(), which treats it as a static screen.
bool drive_camera_live();

// (THE DISCOVERY PROBE lived here: two snapshots of the live DRIVE_CAMERA
// object, narrowed by repeated capture, and a diff listing the fields that
// looked like discrete state in both. Removed 2026-08-26 with its answer taken
// -- see drive_camera_cockpit() above. Worth recording that it WORKED, unlike
// the screen-state variant hunt: it was bounded to one object whose pointer we
// already held, a few hundred bytes of it, and a field whose expected values
// were known in advance.)

// FALSE until the drive camera has ticked even once -- the very first boot
// menu, before anything has been driven.
//
// ASK THIS BEFORE TRUSTING THE TIMER ABOVE. It reports 0 for "never ticked",
// which is indistinguishable from "ticked this instant", so a caller gating on
// movement reads the boot menu as actively driving. That is not hypothetical:
// it made the main menu jitter the moment the idle timer became the sole gate
// for DIBR shift and the 6-DoF warp.
bool logic_camera_ever_ticked();

} // namespace hooks
