#pragma once
#include <windows.h>

// Loads the real System32 dxgi.dll and resolves every forwarded export.
// Must run before any export of this proxy is called (i.e. in DllMain).
bool dxgi_proxy_init();

// Slot table consumed by the jmp stubs in dxgi_stubs.asm. Order must match
// kForwardNames in dxgi_proxy.cpp and the stub indices in dxgi_stubs.asm.
extern "C" FARPROC g_dxgiForwards[16];
