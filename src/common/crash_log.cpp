#include "common/crash_log.h"
#include "common/log.h"

#include <atomic>
#include <cstdio>
#include <cstdint>

namespace vrlog {
namespace {

std::atomic<bool> g_installed{false};

// A crash rarely arrives alone: a fault inside a fault handler, or the same
// fault on several threads at once, arrives as a burst. The first few carry the
// information, and a log that scrolls the first one away is worse than no log
// -- so this stops after a handful rather than trying to be complete.
std::atomic<int> g_logged{0};
constexpr int kMaxLogged = 4;

// Only the codes that mean the process is broken. Everything else is normal
// traffic: 0xE06D7363 is a C++ throw, which this engine does use, 0x406D1388 is
// the thread-naming exception, and the breakpoint codes belong to a debugger.
bool is_fatal(DWORD code)
{
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
            return true;
        default:
            return false;
    }
}

const char* code_name(DWORD code)
{
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:         return "ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT:    return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_ILLEGAL_INSTRUCTION:      return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:            return "IN_PAGE_ERROR";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE";
        case EXCEPTION_PRIV_INSTRUCTION:         return "PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW:           return "STACK_OVERFLOW";
        default:                                 return "?";
    }
}

// "dxgi.dll+0x1E4A0" for an address, or a bare pointer when it belongs to no
// loaded module (a JIT page, or a wild jump).
//
// UNCHANGED_REFCOUNT because taking a reference on a module from inside a crash
// handler is a fine way to turn a crash into a hang.
void describe(const void* addr, char* out, size_t cap)
{
    out[0] = '?'; out[1] = '\0';
    if (!addr) return;

    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(addr), &mod) || !mod) {
        snprintf(out, cap, "%p (no module)", addr);
        return;
    }

    wchar_t wpath[MAX_PATH];
    const DWORD n = GetModuleFileNameW(mod, wpath, MAX_PATH);
    const wchar_t* wname = wpath;
    for (DWORD i = 0; i < n; ++i)
        if (wpath[i] == L'\\' || wpath[i] == L'/') wname = wpath + i + 1;

    char name[128] = "?";
    if (n) WideCharToMultiByte(CP_UTF8, 0, wname, -1, name, sizeof(name), nullptr, nullptr);

    const uintptr_t base = reinterpret_cast<uintptr_t>(mod);
    snprintf(out, cap, "%s+0x%llX", name,
             (unsigned long long)(reinterpret_cast<uintptr_t>(addr) - base));
}

LONG CALLBACK handler(EXCEPTION_POINTERS* info)
{
    if (!info || !info->ExceptionRecord)
        return EXCEPTION_CONTINUE_SEARCH;

    const EXCEPTION_RECORD& er = *info->ExceptionRecord;
    if (!is_fatal(er.ExceptionCode))
        return EXCEPTION_CONTINUE_SEARCH;
    if (g_logged.fetch_add(1) >= kMaxLogged)
        return EXCEPTION_CONTINUE_SEARCH;

    char where[192];
    describe(er.ExceptionAddress, where, sizeof(where));

    VRLOG("=== CRASH: %s (0x%08lX) at %s ===",
          code_name(er.ExceptionCode), (unsigned long)er.ExceptionCode, where);

    // An access violation carries what it was doing and to which address, and
    // that pair is most of the diagnosis: a read of a very small address is a
    // null dereference, a read of a plausible-looking one is a stale or freed
    // pointer, and a write says the same about a write path.
    if (er.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er.NumberParameters >= 2) {
        const ULONG_PTR op = er.ExceptionInformation[0];
        VRLOG("    %s address %p",
              op == 0 ? "reading" : (op == 1 ? "writing" : "executing"),
              (void*)er.ExceptionInformation[1]);
    }

    // Walk the faulting call stack from the exception CONTEXT record.
    // RtlCaptureStackBackTrace(0,...) walks the handler's OWN stack instead,
    // which always shows handler()->ntdll->ntdll regardless of where the fault
    // was. Walking from info->ContextRecord gives the real faulting frames.
    //
    // We use RtlLookupFunctionEntry / RtlVirtualUnwind (available in kernel32
    // on x64 without any dbghelp initialisation) to unwind frame by frame.
    // On x86 this path is not available; fall back to RtlCaptureStackBackTrace.
#if defined(_M_X64)
    if (info->ContextRecord) {
        CONTEXT ctx = *info->ContextRecord;  // local copy; unwind modifies it
        constexpr int kMaxFrames = 24;
        int n = 0;
        while (n < kMaxFrames) {
            // Always log the current RIP, even when it is 0.  A null RIP means
            // the crash happened at address 0 (a call through a null function
            // pointer); RSP already holds the return address (the `call`
            // instruction decremented RSP and wrote it before faulting).
            describe(reinterpret_cast<void*>(ctx.Rip), where, sizeof(where));
            VRLOG("    [%2u] %-46s %016llX", (unsigned)n, where,
                  (unsigned long long)ctx.Rip);
            ++n;

            if (!ctx.Rip) {
                // Recover the caller from RSP so we get the full chain.
                if (!ctx.Rsp) break;
                DWORD64 retAddr = 0;
                __try { retAddr = *reinterpret_cast<DWORD64*>(ctx.Rsp); }
                __except (EXCEPTION_EXECUTE_HANDLER) { break; }
                describe(reinterpret_cast<void*>(retAddr), where, sizeof(where));
                VRLOG("    [%2u] %-46s %016llX  <- caller of null ptr",
                      (unsigned)n, where, (unsigned long long)retAddr);
                ++n;
                ctx.Rip  = retAddr;
                ctx.Rsp += 8;
                if (!ctx.Rip) break;
            }

            // Unwind one frame using the PE exception directory (no dbghelp).
            DWORD64 imageBase = 0;
            RUNTIME_FUNCTION* rf = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
            const DWORD64 prevRip = ctx.Rip;
            if (!rf) {
                // Leaf function (no unwind info): RSP holds the return address.
                if (!ctx.Rsp) break;
                DWORD64 retAddr = 0;
                __try { retAddr = *reinterpret_cast<DWORD64*>(ctx.Rsp); }
                __except (EXCEPTION_EXECUTE_HANDLER) { break; }
                ctx.Rip  = retAddr;
                ctx.Rsp += 8;
            } else {
                void* handlerData = nullptr;
                DWORD64 establisherFrame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, rf,
                                 &ctx, &handlerData, &establisherFrame, nullptr);
            }
            // No progress means corrupt or missing unwind info; stop.
            if (ctx.Rip == prevRip) break;
        }
    }
#else
    {
        void* frames[24] = {};
        const USHORT got = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
        for (USHORT i = 0; i < got; ++i) {
            describe(frames[i], where, sizeof(where));
            VRLOG("    [%2u] %-46s %p", (unsigned)i, where, frames[i]);
        }
    }
#endif
    VRLOG("=== end of crash report (thread %lu) ===", GetCurrentThreadId());

    // NEVER handled. Whatever the game was going to do about this it still
    // does; this only wrote it down on the way past.
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void install_crash_handler()
{
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true))
        return;
    // FIRST in the vectored chain, so a handler the game adds later cannot
    // swallow the exception before this has seen it.
    if (AddVectoredExceptionHandler(1, &handler))
        VRLOG("crash handler installed (logs module+offset, handles nothing)");
    else
        VRLOG("crash handler: AddVectoredExceptionHandler FAILED");
}

} // namespace vrlog
