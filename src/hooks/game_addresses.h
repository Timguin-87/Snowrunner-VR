#pragma once
#include <cstdint>

// EVERY SNOWRUNNER.EXE ADDRESS THE MOD USES -- FOUND, NEVER REMEMBERED.
//
// Each one is located at startup in the running image and checked before
// anything uses it:
//
//   a FUNCTION must match its signature exactly once, and the match must be a
//   function entry in the exe's own exception directory;
//   a CALL SITE (a return address a detour filters on) must be the only call
//   to that function inside the function it belongs to -- found by what calls
//   what, not by how far into the caller it sits, so an edit to the caller's
//   body does not move it;
//   a GLOBAL must be the target of the instruction it was found by, and lie in
//   the exe's writable data.
//
// Anything that fails is left at 0 and logged with the reason, and the feature
// that needed it stays off. Nothing is ever patched at an address carried over
// from an older build. That is exactly how the 2026-10-21 update broke the mod:
// every remembered offset landed mid-function in the new exe, MinHook patched
// them anyway, and head tracking and the FOV lock quietly stopped.
//
// AFTER A GAME UPDATE, read the "game addresses" block near the top of the log.
// If it ends "all N found", nothing needs doing. If not, set DumpGameCode=true
// in the config, start the game once, and re-derive whatever is missing from
// the dump that writes (see hooks::maybe_dump_game_image()).
namespace hooks {

struct GameAddresses {
    // Functions, as exe-relative offsets. 0 = not found in this build.
    uintptr_t cameraCommit = 0;   // combine::DRIVE_CAMERA per-tick commit
    uintptr_t viewBuilder  = 0;   // 4x4 orientation builder (diagnostics only)
    uintptr_t combine1     = 0;   // position + orientation -> camera-to-world
    uintptr_t combine2     = 0;   // final cockpit view (rotation injection)
    uintptr_t projBuild    = 0;   // buildPerspective (FOV lock, lean, stereo)
    uintptr_t fna          = 0;   // fillOrbitView: orbit + menu cameras
    uintptr_t sizeCull     = 0;   // small-object screen-size cull (stones, debris)
    uintptr_t setPerspective = 0; // combine::CAMERA set-perspective
    // The UI's own camera (class ui3D_CAMERA_MR): its Update, vtable slot 1,
    // which points the object at the game's active combine::CAMERA. Every world
    // marker the HUD places is projected through that pointer
    // (hooks/marker_camera.h). Found by CLASS NAME rather than by signature, and
    // accepted only while every slot reading the camera still reads the fields
    // the mod's replacement camera fills in.
    uintptr_t uiCameraUpdate = 0;

    // Return addresses: how each detour picks the camera it wants out of every
    // other caller of the same function.
    uintptr_t viewBuilderRet = 0; // main camera build -> viewBuilder
    uintptr_t combine1Ret    = 0; // main camera build -> combine1
    uintptr_t combine2Ret    = 0; // main camera build -> combine2
    uintptr_t orbitGameRet   = 0; // DRIVE_CAMERA commit -> fna (in-game orbit)
    uintptr_t orbitMenuRet   = 0; // main-menu camera -> fna
    uintptr_t projMainRet    = 0; // combine::CAMERA set-perspective -> projBuild
    uintptr_t mirrorPerspRet = 0; // truck mirror cameras -> set-perspective

    // Data: the global holding the game's settings object, whose FOV pair at
    // +0x60/+0x64 is read (never written) for the FOV monitor.
    uintptr_t settingsPtr = 0;

    // The engine's two per-frame callbacks, DXUT style, both on the main thread
    // and each handed the frame's time explicitly (frame_sync.cpp freezes it):
    //   OnFrameMove(double time, float elapsed, void*)          -- the update
    //   OnFrameRender(void* device, double time, float elapsed, void*) -- the
    //   render, which presents inside itself
    uintptr_t frameMove   = 0;
    uintptr_t frameRender = 0;
    // Data: the flag under which OnFrameMove clamps the elapsed time it hands
    // the game into [1/60 s, 1/20 s]. 0 in every session measured; a set flag
    // would give a frozen frame a sixtieth of a second anyway.
    uintptr_t elapsedClampFlag = 0;

    // The engine's own way of changing its render size (resolution_hook.cpp).
    // settingsApply runs from OnFrameMove on every frame nothing is loading, and
    // is the only caller of displayApply: window, device lost, device reset. A
    // load in progress is why that matters -- device lost waits for it with no
    // timeout, so displayApply is only safe where this guard has already passed.
    uintptr_t settingsApply = 0;
    uintptr_t displayApply  = 0;
    // Data: the settings object as the window apply reads it, and it is what
    // displayApply acts on: requested client size at +0x28/+0x2C, mode at +0x3C
    // (0 = windowed). The same object as settingsPtr in the build this was
    // measured on, but this is the global the size is really read through.
    uintptr_t windowSettings = 0;

    // Data: the globals the game keeps its own XInput function pointers in. It
    // loads xinput9_1_0.dll itself and calls through these, so this is where
    // input_block.cpp puts the VR controllers in -- in front of anything that
    // intercepts XInput, the Steam overlay included. Every such global, since
    // there is more than one loader (two in build 0x6AB52F5D); 0 = unused.
    static constexpr int kMaxXInputPtrs = 4;
    uintptr_t xinputGetState[kMaxXInputPtrs] = {};
    uintptr_t xinputGetCaps[kMaxXInputPtrs]  = {};
};

// Resolves on the first call, logging one line per address and a summary, and
// returns the same answers ever after. Call only once the game's own code is
// running -- the exe is decrypted by its DRM stub before that, never earlier --
// i.e. from the first CreateDXGIFactory, not from DllMain.
const GameAddresses& game_addresses();

} // namespace hooks
