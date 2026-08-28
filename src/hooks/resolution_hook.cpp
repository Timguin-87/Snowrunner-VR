#include "hooks/resolution_hook.h"
#include "hooks/swapchain_hook.h"
#include "common/log.h"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <intrin.h>

namespace hooks {
namespace {

// --------------------------------------------------------------------------
// state
// --------------------------------------------------------------------------

std::atomic<int>  g_forceW{0}, g_forceH{0};
std::atomic<bool> g_installed{false};

uintptr_t g_exeBase = 0;
uintptr_t g_exeEnd  = 0;

void init_exe_range()
{
    HMODULE exe = GetModuleHandleW(nullptr);
    if (!exe) return;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(exe);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(
        reinterpret_cast<uint8_t*>(exe) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    g_exeBase = reinterpret_cast<uintptr_t>(exe);
    g_exeEnd  = g_exeBase + nt->OptionalHeader.SizeOfImage;
}

// Both hooks below sit on APIs that every module in the process uses. Acting
// only on calls made by game code keeps our own overlay, the D3D runtime and
// anything else that creates a window entirely out of it.
bool caller_is_exe(void* ret)
{
    const uintptr_t a = reinterpret_cast<uintptr_t>(ret);
    return g_exeBase && a >= g_exeBase && a < g_exeEnd;
}

// The window-manager ceiling has to clear the requested client size plus the
// window frame; Windows applies its limit to the WINDOW size, so an exact-fit
// ceiling would still clip by the border width.
bool ceiling_size(LONG& cx, LONG& cy)
{
    const int w = g_forceW.load(), h = g_forceH.load();
    if (w <= 0 || h <= 0) return false;
    cx = (LONG)w + 512;
    cy = (LONG)h + 512;
    return true;
}

// --------------------------------------------------------------------------
// hooks
// --------------------------------------------------------------------------

using PFN_CreateWindowExW = HWND (WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int,
                                           HWND, HMENU, HINSTANCE, LPVOID);
using PFN_RegisterClassW  = ATOM (WINAPI*)(const WNDCLASSW*);

PFN_CreateWindowExW real_CreateWindowExW = nullptr;
PFN_RegisterClassW  real_RegisterClassW = nullptr;

// --------------------------------------------------------------------------
// WM_GETMINMAXINFO ceiling
// --------------------------------------------------------------------------
//
// Forcing the size is only half the job: Windows independently refuses to size
// a window beyond MINMAXINFO::ptMaxTrackSize, which defaults to the virtual
// screen plus the frame. Measured on a 1920x1080 desktop at 125% scaling:
// asking for a 2688x2688 client (2706x2733 window) produced a 1942x1102
// window -- exactly that default -- and hence a 1924x1057 swapchain. Raising
// the ceiling is what lets the forced size actually stick, and it is also why
// the old 3000x3000 virtual-display workaround worked: a larger virtual screen
// lifts this limit as a side effect.
//
// The clamp lands INSIDE CreateWindowExW -- the game creates its window at its
// final size in one shot and never calls SetWindowPos or MoveWindow during
// startup at all. Subclassing after CreateWindowExW returns is therefore one
// step too late: WM_GETMINMAXINFO has already been answered, with the default
// ceiling, by the class's own proc. The ceiling has to exist BEFORE the window
// does, so the window procedure is substituted at RegisterClassW time.
struct Subclassed { HWND hwnd; WNDPROC orig; };
constexpr int kMaxSubclassed = 32;
Subclassed g_sub[kMaxSubclassed]{};
std::atomic<int> g_subCount{0};

// Classes whose lpfnWndProc we substituted at registration.
struct ClassEntry { ATOM atom; WNDPROC orig; };
constexpr int kMaxClasses = 16;
ClassEntry g_cls[kMaxClasses]{};
std::atomic<int> g_clsCount{0};

WNDPROC class_orig_for_atom(ATOM a)
{
    if (!a) return nullptr;
    for (int i = 0; i < kMaxClasses; ++i)
        if (g_cls[i].atom == a) return g_cls[i].orig;
    return nullptr;
}

// Scans the whole table rather than up to g_subCount, and the writer fills
// `hwnd` last: an entry becomes findable only once its `orig` is already
// there, so a message arriving mid-install can never route to a half-built
// entry and fall through to DefWindowProc (which would eat it).
WNDPROC orig_proc_for(HWND h)
{
    for (int i = 0; i < kMaxSubclassed; ++i)
        if (g_sub[i].hwnd == h) return g_sub[i].orig;
    return nullptr;
}

void remember_window(HWND h, WNDPROC orig)
{
    const int slot = g_subCount.fetch_add(1);
    if (slot >= kMaxSubclassed) return;   // table full: correct, just uncached
    g_sub[slot].orig = orig;
    g_sub[slot].hwnd = h;                 // published last -- see orig_proc_for()
}

LRESULT CALLBACK Detour_WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    // Per-window table first (one array scan, no syscall). Windows born from a
    // class we patched are not in it until their first message, so fall back
    // to the class table via the window's atom and cache the answer.
    WNDPROC orig = orig_proc_for(h);
    if (!orig) {
        orig = class_orig_for_atom(static_cast<ATOM>(GetClassWord(h, GCW_ATOM)));
        if (orig) remember_window(h, orig);
    }
    LRESULT r = orig ? CallWindowProcW(orig, h, msg, wp, lp)
                     : DefWindowProcW(h, msg, wp, lp);

    // AFTER the real handler: DefWindowProc is what fills MINMAXINFO in, so
    // overriding before it runs would simply be overwritten.
    if (msg == WM_GETMINMAXINFO && lp) {
        LONG cx = 0, cy = 0;
        if (ceiling_size(cx, cy)) {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            if (mmi->ptMaxTrackSize.x < cx) mmi->ptMaxTrackSize.x = cx;
            if (mmi->ptMaxTrackSize.y < cy) mmi->ptMaxTrackSize.y = cy;
            if (mmi->ptMaxSize.x < cx)      mmi->ptMaxSize.x = cx;
            if (mmi->ptMaxSize.y < cy)      mmi->ptMaxSize.y = cy;
        }
    }

    // Drop the entry before the HWND value can be recycled by a later window,
    // which would otherwise inherit this one's original proc.
    if (msg == WM_NCDESTROY) {
        for (int i = 0; i < kMaxSubclassed; ++i)
            if (g_sub[i].hwnd == h) { g_sub[i].hwnd = nullptr; break; }
    }
    return r;
}

// The fix for the "one step too late" problem above: substitute the window
// procedure at class-registration time, so it is already in place when
// CreateWindowExW sends the very first WM_GETMINMAXINFO.
ATOM WINAPI Detour_RegisterClassW(const WNDCLASSW* wc)
{
    void* ret = _ReturnAddress();
    if (!wc || !wc->lpfnWndProc || !caller_is_exe(ret))
        return real_RegisterClassW(wc);

    // Reserve the slot BEFORE substituting: a class registered with our proc
    // but no recorded original would route every message to DefWindowProc,
    // which silently breaks the window rather than failing loudly.
    const int slot = g_clsCount.fetch_add(1);
    if (slot >= kMaxClasses)
        return real_RegisterClassW(wc);

    WNDCLASSW copy = *wc;
    copy.lpfnWndProc = &Detour_WndProc;
    ATOM atom = real_RegisterClassW(&copy);
    if (!atom)
        return real_RegisterClassW(wc);   // retry unmodified rather than fail the game

    g_cls[slot].orig = wc->lpfnWndProc;
    g_cls[slot].atom = atom;              // published last
    VRLOG("resolution: hooked window class atom=0x%04X at registration "
          "(WM_GETMINMAXINFO ceiling active before any window exists)", atom);
    return atom;
}

HWND WINAPI Detour_CreateWindowExW(DWORD exStyle, LPCWSTR cls, LPCWSTR name, DWORD style,
                                   int x, int y, int w, int h, HWND parent, HMENU menu,
                                   HINSTANCE inst, LPVOID param)
{
    void* ret = _ReturnAddress();

    // Replace the size the game decided on with ours, before the window is
    // ever created. This is the cleanest injection point available -- the
    // engine sizes its swapchain from GetClientRect on this window immediately
    // afterwards (exe+0x13C2929), so the window IS the source of truth for
    // render resolution. The game's own settings object keeps whatever
    // video.dat said and does not need to agree: it already disagreed for
    // weeks while the clamp was active (settings 2688x2688, actual 1902x977)
    // with no ill effect.
    //
    // Only the first top-level, captioned, explicitly-sized window created by
    // game code is touched -- that is the main window, and the log line below
    // makes it obvious if this ever picks the wrong one.
    static std::atomic<bool> s_forced{false};
    const int fw = g_forceW.load(), fh = g_forceH.load();
    if (fw > 0 && fh > 0 && caller_is_exe(ret) && !(style & WS_CHILD) &&
        (style & WS_CAPTION) && w > 0 && h > 0) {
        bool expected = false;
        if (s_forced.compare_exchange_strong(expected, true)) {
            RECT want{ 0, 0, fw, fh };
            AdjustWindowRectEx(&want, style, menu != nullptr, exStyle);
            const int newW = want.right - want.left, newH = want.bottom - want.top;
            VRLOG("resolution: overriding main window %dx%d -> %dx%d (client %dx%d + frame %dx%d)",
                  w, h, newW, newH, fw, fh, newW - fw, newH - fh);
            w = newW;
            h = newH;
        }
    }

    HWND hwnd = real_CreateWindowExW(exStyle, cls, name, style, x, y, w, h,
                                     parent, menu, inst, param);
    if (!hwnd || !caller_is_exe(ret) || (style & WS_CHILD))
        return hwnd;

    // Only subclass windows whose class we did NOT already patch at
    // registration -- otherwise Detour_WndProc would end up chained to itself
    // and every message would run our WM_GETMINMAXINFO block twice.
    const bool classHooked = class_orig_for_atom(
        static_cast<ATOM>(GetClassWord(hwnd, GCW_ATOM))) != nullptr;
    if (!classHooked) {
        // Reserve the slot BEFORE redirecting the window proc: a successful
        // SetWindowLongPtrW with nowhere to store the original would leave the
        // window pointed at a detour that can only fall back to DefWindowProc,
        // silently breaking every message the game handles itself.
        const int slot = g_subCount.fetch_add(1);
        if (slot >= kMaxSubclassed)
            return hwnd;
        auto prev = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Detour_WndProc)));
        if (!prev)
            return hwnd;
        g_sub[slot].orig = prev;
        g_sub[slot].hwnd = hwnd;      // published last -- see orig_proc_for()
    }

    RECT wr{};
    GetWindowRect(hwnd, &wr);
    const int gotW = wr.right - wr.left, gotH = wr.bottom - wr.top;
    if (gotW != w || gotH != h)
        VRLOG("resolution: game window 0x%p asked %dx%d, got %dx%d  [class hook %s]",
              (void*)hwnd, w, h, gotW, gotH,
              classHooked ? "ACTIVE" : "missed -- subclassed after the fact");

    // Fallback for the case the class hook did not cover: the window was born
    // clamped, but our proc is on it now, so ask again. Safe to do from here
    // even though the game is still inside its own CreateWindowExW call -- the
    // window is WS_VISIBLE, so its proc has already handled a WM_SIZE from
    // creation and a second one is nothing new. The game reads the result back
    // with GetClientRect immediately after this returns, so fixing the size
    // here is enough to carry through to the swapchain.
    if (w > 0 && h > 0 && (gotW < w || gotH < h) && fw > 0 && !(style & WS_POPUP)) {
        SetWindowPos(hwnd, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        RECT after{};
        GetWindowRect(hwnd, &after);
        int nowW = after.right - after.left, nowH = after.bottom - after.top;
        if (nowW < w || nowH < h) {
            // SWP_NOSENDCHANGING skips the WM_WINDOWPOSCHANGING round trip,
            // which is where DefWindowProc applies the tracking limits at all.
            SetWindowPos(hwnd, nullptr, 0, 0, w, h,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
            GetWindowRect(hwnd, &after);
            nowW = after.right - after.left; nowH = after.bottom - after.top;
        }
        VRLOG("resolution: corrective resize to %dx%d -> %dx%d %s", w, h, nowW, nowH,
              (nowW >= w && nowH >= h) ? "OK" : "STILL CLAMPED");
    }
    return hwnd;
}

template <typename Fn>
bool hook_api(const wchar_t* mod, const char* name, Fn detour, Fn* original)
{
    void* target = nullptr;
    if (MH_CreateHookApiEx(mod, name, reinterpret_cast<void*>(detour),
                           reinterpret_cast<void**>(original), &target) != MH_OK)
        return false;
    return MH_EnableHook(target) == MH_OK;
}

} // namespace

void install_resolution_hooks()
{
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true))
        return;

    init_exe_range();

    const int w = g_forceW.load(), h = g_forceH.load();
    if (w <= 0 || h <= 0) {
        // Nothing to force: leave the game's window handling completely alone
        // rather than installing pass-through detours on it.
        VRLOG("resolution: not installed (ForceGameResolution=off)");
        return;
    }

    const bool okCreate = hook_api(L"user32", "CreateWindowExW",
                                   &Detour_CreateWindowExW, &real_CreateWindowExW);
    // Without this one the forced size is created and then immediately clamped
    // back by the window manager -- see the WM_GETMINMAXINFO comment above.
    const bool okClass  = hook_api(L"user32", "RegisterClassW",
                                   &Detour_RegisterClassW, &real_RegisterClassW);
    VRLOG("resolution: forcing %dx%d -- CreateWindowExW hook %s, RegisterClassW hook %s",
          w, h, okCreate ? "ok" : "FAILED", okClass ? "ok" : "FAILED");
}

// REMOVED 2026-08-24: apply_resolution_now(), the live-resize path behind the
// settings UI's "Apply resolution now" button.
//
// It drove IDXGISwapChain::ResizeTarget -> ResizeBuffers, the same pair the
// game's own video settings and its fullscreen toggle use, and both calls
// succeeded -- which is why it looked promising. It still did not work: the
// engine does not re-derive its render size from a swapchain it did not resize
// itself, so the picture was resized without being re-rendered. The window is
// the source of truth only at CreateWindowExW, which is where
// set_force_resolution() applies and why a restart is the honest answer.
//
// Do not rebuild this on the evidence that the calls succeed. They always did.
//
// find_main_window() went with it -- an EnumWindows search for the process's
// largest visible window, which existed because the live path had to locate a
// window install_resolution_hooks() may never have seen (it no-ops when no size
// is set, so there is no remembered HWND). Nothing else ever needed one.

void set_force_resolution(int w, int h) { g_forceW.store(w); g_forceH.store(h); }
int  force_resolution_w()               { return g_forceW.load(); }
int  force_resolution_h()               { return g_forceH.load(); }

} // namespace hooks
