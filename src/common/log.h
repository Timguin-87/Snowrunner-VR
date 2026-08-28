#pragma once
#include <windows.h>

namespace vrlog {

// Opens <dll directory>\snowrunner_vr.log (truncated each launch).
void init(HMODULE self);
void write(const char* fmt, ...);

} // namespace vrlog

#define VRLOG(...) ::vrlog::write(__VA_ARGS__)
