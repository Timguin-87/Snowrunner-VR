#pragma once
#include <windows.h>
#include <cstdint>

namespace hooks {

// Scans the executable sections of `module` for an IDA-style pattern,
// e.g. "48 8B 05 ?? ?? ?? ?? F3 0F 10 40 ?? C3".
// Returns the address of the first match or nullptr.
// How the camera commit function is located per game version, since the exe is
// Steam-DRM wrapped and has no usable static addresses until it is running.
uint8_t* pattern_scan(HMODULE module, const char* ida_pattern);

} // namespace hooks
