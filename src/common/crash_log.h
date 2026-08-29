#pragma once
#include <windows.h>

// WHOSE CRASH IS IT.
//
// Without this, a crash inside the mod and a crash inside SnowRunner look
// exactly alike from the outside: the log simply stops. That is not a small gap
// when the mod is a DLL injected into somebody else's process and the reports
// arrive from machines nobody here can reproduce on -- the first question every
// time is "is this us", and nothing in the log answered it.
//
// So a vectored handler logs the fatal ones and gets out of the way. What it
// writes is the faulting MODULE + OFFSET, not a symbol: offsets survive the
// trip through someone else's log file, need no PDB on the reporting machine,
// and a Release map file turns dxgi.dll+0x1E4A0 back into a function here. If
// the faulting module is SnowRunner.exe the crash is the game's -- possibly
// provoked by us, but not inside our code, which is a different search.
namespace vrlog {

// Installs a vectored exception handler. Call as early as possible in DllMain,
// right after init(), so it also covers the mod's own startup. Idempotent.
//
// VECTORED, not SetUnhandledExceptionFilter: the game installs its own filter
// (it has a crash reporter) and whoever calls that API last wins. A vectored
// handler runs regardless and cannot be displaced.
//
// It NEVER handles anything -- always EXCEPTION_CONTINUE_SEARCH -- so the
// game's own handling is bit-for-bit unchanged, including any __try/__except it
// uses deliberately. It also ignores non-fatal codes (C++ throws, RPC, the
// thread-naming exception, debugger breakpoints), which are normal traffic in a
// running game and would otherwise fill the log with noise.
void install_crash_handler();

} // namespace vrlog
