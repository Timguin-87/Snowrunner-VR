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
//   body does not move it.
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

    // Return addresses: how each detour picks the camera it wants out of every
    // other caller of the same function.
    uintptr_t viewBuilderRet = 0; // main camera build -> viewBuilder
    uintptr_t combine1Ret    = 0; // main camera build -> combine1
    uintptr_t combine2Ret    = 0; // main camera build -> combine2
    uintptr_t orbitGameRet   = 0; // DRIVE_CAMERA commit -> fna (in-game orbit)
    uintptr_t orbitMenuRet   = 0; // main-menu camera -> fna
    uintptr_t projMainRet    = 0; // combine::CAMERA set-perspective -> projBuild

    // Data: the global holding the game's settings object, whose FOV pair at
    // +0x60/+0x64 is read (never written) for the FOV monitor.
    uintptr_t settingsPtr = 0;
};

// Resolves on the first call, logging one line per address and a summary, and
// returns the same answers ever after. Call only once the game's own code is
// running -- the exe is decrypted by its DRM stub before that, never earlier --
// i.e. from the first CreateDXGIFactory, not from DllMain.
const GameAddresses& game_addresses();

} // namespace hooks
