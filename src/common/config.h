#pragma once
#include <string>

// Snowrunner_VR_config.txt -- plain-text settings persisted next to this
// DLL. Created with defaults if missing. A key that IS present but holds an
// invalid value resets the WHOLE file to defaults (guards against a
// hand-garbled file); a key that's simply ABSENT (e.g. the file predates a
// newer setting) only fills in that one key's default and leaves the rest
// of the file alone, so adding a setting later never nukes an existing
// user's file.
namespace vrcfg {

// Loads (or creates) the config file and applies every setting via its
// owning module's setter (xr::set_warp_type(), hooks::set_horizon_lock_enabled(),
// hooks::set_menu_toggle_vk(), hooks::set_menu_toggle_gamepad_combo()).
// Call once, early -- before anything could act on a stale default.
void init();

// Re-serializes every setting from its current LIVE value (read back via the
// same modules' getters) and overwrites the file. Called after every
// in-game settings-UI edit -- there is no separate "Apply"/"Save" step.
void save();

// TRUE while no render resolution has ever been chosen -- the config had no
// ForceGameResolution line at all (a fresh install) or it read as 'auto'.
//
// It is a question the SETTINGS UI answers, not init(): the right value is the
// headset's own native width, and no OpenXR runtime exists yet when the config
// is loaded. So the state has to survive until something can resolve it, which
// is why 'auto' is written back rather than 'off' -- a first launch with no
// runtime reachable would otherwise record 'off' and the question would never
// be asked again. 'off' stays what it always was: a deliberate 'leave the
// game's own resolution alone'.
//
// Cleared by save() as soon as a real size is set.
bool resolution_unset();

// Shared name<->value tables, also used by the settings UI so a displayed
// binding label and the serialized file can never disagree on what a VK or
// gamepad combo is called.
std::string vk_name(int vk);                                     // VK_INSERT -> "Insert"
bool        vk_from_name(const std::string& name, int& outVk);
std::string gamepad_combo_name(unsigned int xinputButtonMask);   // 0xC0 -> "L3+R3"
bool        gamepad_combo_from_name(const std::string& name, unsigned int& outMask);

} // namespace vrcfg
