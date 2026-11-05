#include "hooks/resolution_hook.h"
#include "hooks/game_addresses.h"
#include "hooks/swapchain_hook.h"
#include "render/dlss.hpp"
#include "xr/xr_mirror.h"
#include "common/config.h"
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

// The window's client size: the forced size, or -- under a DLSS upscaling mode --
// that mode's render size, with the forced size then describing the headset image
// instead. Set at install time for the CreateWindowExW detour, and by each live
// apply after that, so moving the slider alone changes neither.
std::atomic<int>  g_winW{0}, g_winH{0};
std::atomic<int>  g_upMode{0};            // dlss::Mode the window is sized for; 0 = off
std::atomic<int>  g_upOutW{0}, g_upOutH{0};
std::atomic<bool> g_upApplied{false};     // ...and it really is that size

std::atomic<bool> g_windowHooks{false};   // the hooks below are in (a size set, or 'auto')
std::atomic<HWND> g_mainWindow{nullptr};  // the game's main window, as they found it

// The live apply, see below.
std::atomic<bool>      g_applyWanted{false};
std::atomic<ULONGLONG> g_applyAskedAt{0};

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
    //
    // FOUND EVEN WITH NO SIZE TO FORCE. A first launch (ForceGameResolution=auto)
    // creates the window at the game's own size and only learns the headset's
    // once a runtime is up; the live apply then resizes this window, so it has to
    // know which one it is.
    static std::atomic<bool> s_found{false};
    const int fw = g_winW.load(), fh = g_winH.load();
    bool isMain = false;
    if (caller_is_exe(ret) && !(style & WS_CHILD) && (style & WS_CAPTION) && w > 0 && h > 0) {
        bool expected = false;
        isMain = s_found.compare_exchange_strong(expected, true);
    }
    if (isMain && fw > 0 && fh > 0) {
        RECT want{ 0, 0, fw, fh };
        AdjustWindowRectEx(&want, style, menu != nullptr, exStyle);
        const int newW = want.right - want.left, newH = want.bottom - want.top;
        VRLOG("resolution: overriding main window %dx%d -> %dx%d (client %dx%d + frame %dx%d)",
              w, h, newW, newH, fw, fh, newW - fw, newH - fh);
        w = newW;
        h = newH;
        if (g_upMode.load() != dlss::kOff) g_upApplied.store(true);
    } else if (isMain) {
        VRLOG("resolution: main window created at the game's own %dx%d -- no size set yet", w, h);
    }

    HWND hwnd = real_CreateWindowExW(exStyle, cls, name, style, x, y, w, h,
                                     parent, menu, inst, param);
    if (isMain) g_mainWindow.store(hwnd);
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

// --------------------------------------------------------------------------
// the live apply
// --------------------------------------------------------------------------
//
// THE ENGINE RESIZES ITSELF, IF ASKED IN ITS OWN TERMS. Its settings apply calls
// its display apply when the requested size changes, and that does the whole job:
// window, device lost, device reset, swapchain, every render target. So the size
// goes where the display apply reads it, and the display apply runs.
//
// FROM A HOOK ON THE SETTINGS APPLY AND NOWHERE ELSE. Device lost waits with no
// timeout on the load job in progress, so the display apply hangs the game during
// a load -- the first version called it from OnFrameMove and did exactly that.
// OnFrameMove only reaches the settings apply when nothing is loading, so a detour
// there inherits the whole guard, and a request made during a load simply waits.
//
// THE REQUEST IS PUT BACK AFTERWARDS, because the settings apply re-reads the
// object from the game's own variables and compares the two: a size left behind
// would read as a change, on whichever frame the player next touched a video
// setting, and resize the window back to what those variables say. Restored, the
// object is exactly as it is after a launch, when the window disagrees with it
// and nothing minds.
//
// THE CLAMP is the two limits of docs/resolution_clamp_strategies.md §2 again. The
// window apply takes the size down to the monitor's work area, so that is enlarged
// for this one call; the window manager's ceiling is WM_GETMINMAXINFO above, which
// follows g_forceW. MoveWindow is the only thing this ever moves past the desktop.
constexpr LONG kSpoofedWorkArea = 16384;   // D3D11's own largest texture
constexpr int  kCfgRequestW = 0x28, kCfgRequestH = 0x2C, kCfgMode = 0x3C;

using PFN_GetMonitorInfoA = BOOL (WINAPI*)(HMONITOR, LPMONITORINFO);
using PFN_SettingsApply   = void (*)(void*);
using PFN_DisplayApply    = void (*)();

PFN_GetMonitorInfoA real_GetMonitorInfoA = nullptr;
PFN_SettingsApply   real_SettingsApply   = nullptr;
PFN_DisplayApply    g_displayApply       = nullptr;
std::atomic<bool>   g_spoofWorkArea{false};
uint8_t**           g_settings = nullptr;

BOOL WINAPI Detour_GetMonitorInfoA(HMONITOR mon, LPMONITORINFO mi)
{
    void* ret = _ReturnAddress();
    const BOOL ok = real_GetMonitorInfoA(mon, mi);
    if (ok && mi && g_spoofWorkArea.load() && caller_is_exe(ret)) {
        mi->rcWork.right  = mi->rcWork.left + kSpoofedWorkArea;
        mi->rcWork.bottom = mi->rcWork.top + kSpoofedWorkArea;
    }
    return ok;
}

void client_size(HWND hwnd, int& w, int& h)
{
    RECT cr{};
    GetClientRect(hwnd, &cr);
    w = cr.right - cr.left;
    h = cr.bottom - cr.top;
}

// The engine's own resize, to a client size of w x h.
void run_display_apply(int w, int h)
{
    uint8_t* cfg = *g_settings;
    int* reqW = reinterpret_cast<int*>(cfg + kCfgRequestW);
    int* reqH = reinterpret_cast<int*>(cfg + kCfgRequestH);
    const int oldW = *reqW, oldH = *reqH;

    xr::before_game_resize();
    *reqW = w;
    *reqH = h;
    g_spoofWorkArea.store(true);
    g_displayApply();
    g_spoofWorkArea.store(false);
    *reqW = oldW;
    *reqH = oldH;
}

void apply_resolution()
{
    const HWND hwnd = g_mainWindow.load();
    const int outW = g_forceW.load(), outH = g_forceH.load();
    if (!live_resize_available() || outW <= 0 || outH <= 0) return;

    // The same rule as install_resolution_hooks().
    const int mode = dlss::mode();
    const int upMode = dlss::compiled_in() && dlss::is_upscaling(mode) ? mode : (int)dlss::kOff;
    int rw = outW, rh = outH;
    dlss::render_size_for(upMode, outW, outH, rw, rh);

    int gotW = 0, gotH = 0;
    client_size(hwnd, gotW, gotH);
    if (gotW != rw || gotH != rh) {
        const ULONGLONG t0 = GetTickCount64();
        run_display_apply(rw, rh);
        const int wasW = gotW, wasH = gotH;
        client_size(hwnd, gotW, gotH);
        VRLOG("resolution: the game resized %dx%d -> %dx%d in %llu ms%s", wasW, wasH, gotW, gotH,
              GetTickCount64() - t0, gotW == rw && gotH == rh ? "" : " -- NOT the size asked for");
    }

    // What the window turned out to be, whatever was asked for: a clamped one is
    // no render size for DLSS, and keeps the 1:1 canvas (xr_mirror.cpp).
    const bool upscaling = upMode != dlss::kOff && gotW == rw && gotH == rh;
    g_winW.store(gotW);
    g_winH.store(gotH);
    g_upOutW.store(outW);
    g_upOutH.store(outH);
    g_upMode.store(upscaling ? upMode : (int)dlss::kOff);
    g_upApplied.store(upscaling);
    if (upscaling) {
        const char* by = dlss::method_name(dlss::method());
        VRLOG("resolution: %s %s -- the game renders at %dx%d, and %s brings each eye up "
              "to %dx%d", by, dlss::mode_name(upMode), gotW, gotH, by, outW, outH);
    }

    dlss::on_resized();
    xr::on_game_resized((uint32_t)gotW, (uint32_t)gotH);
}

void Detour_SettingsApply(void* self)
{
    if (g_applyWanted.exchange(false))
        apply_resolution();
    real_SettingsApply(self);
}

} // namespace

void install_resolution_hooks()
{
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true))
        return;

    init_exe_range();

    const int w = g_forceW.load(), h = g_forceH.load();
    const bool sized = w > 0 && h > 0;
    if (!sized && !vrcfg::resolution_unset()) {
        // ForceGameResolution=off: leave the game's window handling completely
        // alone rather than installing pass-through detours on it.
        VRLOG("resolution: not installed (ForceGameResolution=off)%s",
              dlss::is_upscaling(dlss::mode())
                  ? " -- DLSS upscaling needs it: it sets the headset image size"
                  : "");
        return;
    }
    // 'auto' -- the first launch -- has no size yet either, but is about to get
    // one: the first-run notice (menu_hook.cpp) reads it from the headset and
    // applies it to the running game. That apply needs what these hooks provide,
    // the window ceiling and the main window's handle, so they go in now with
    // nothing forced: the window is created exactly as it would be without them.

    // DLSS UPSCALING: the game renders at the mode's render size and DLSS
    // brings each eye up to the forced size at Present (render/dlss.hpp). The
    // window is the one place the engine takes its render size from, so this
    // is where a mode the config starts with has to act.
    int cw = w, ch = h;
    const int upMode = dlss::mode();
    if (sized && dlss::compiled_in() && dlss::is_upscaling(upMode)) {
        dlss::render_size_for(upMode, w, h, cw, ch);
        g_upMode.store(upMode);
        g_upOutW.store(w);
        g_upOutH.store(h);
        VRLOG("resolution: DLSS %s -- the game renders at %dx%d, and DLSS brings each "
              "eye up to %dx%d", dlss::mode_name(upMode), cw, ch, w, h);
    }
    g_winW.store(cw);
    g_winH.store(ch);

    const bool okCreate = hook_api(L"user32", "CreateWindowExW",
                                   &Detour_CreateWindowExW, &real_CreateWindowExW);
    // Without this one the forced size is created and then immediately clamped
    // back by the window manager -- see the WM_GETMINMAXINFO comment above.
    const bool okClass  = hook_api(L"user32", "RegisterClassW",
                                   &Detour_RegisterClassW, &real_RegisterClassW);
    g_windowHooks.store(okCreate && okClass);
    if (sized)
        VRLOG("resolution: forcing %dx%d -- CreateWindowExW hook %s, RegisterClassW hook %s",
              cw, ch, okCreate ? "ok" : "FAILED", okClass ? "ok" : "FAILED");
    else
        VRLOG("resolution: no size yet (ForceGameResolution=auto) -- the game's own size until "
              "the headset's is applied; CreateWindowExW hook %s, RegisterClassW hook %s",
              okCreate ? "ok" : "FAILED", okClass ? "ok" : "FAILED");
}

void install_live_resize()
{
    static std::atomic<bool> s_done{false};
    if (s_done.exchange(true)) return;

    const GameAddresses& a = game_addresses();
    if (!a.settingsApply || !a.displayApply || !a.windowSettings) {
        VRLOG("resolution: no live apply -- the engine's addresses were not found, so a new "
              "size or DLSS mode waits for a restart");
        return;
    }
    if (!g_windowHooks.load()) {
        VRLOG("resolution: no live apply this session -- the window hooks are not in "
              "(ForceGameResolution=off, or they failed above), so a new size waits for a "
              "restart");
        return;
    }

    uint8_t* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    g_settings     = reinterpret_cast<uint8_t**>(base + a.windowSettings);
    g_displayApply = reinterpret_cast<PFN_DisplayApply>(base + a.displayApply);

    const bool okMonitor = hook_api(L"user32", "GetMonitorInfoA", &Detour_GetMonitorInfoA,
                                    &real_GetMonitorInfoA);
    void* target = base + a.settingsApply;
    const bool okApply = okMonitor &&
                         MH_CreateHook(target, reinterpret_cast<void*>(&Detour_SettingsApply),
                                       reinterpret_cast<void**>(&real_SettingsApply)) == MH_OK &&
                         MH_EnableHook(target) == MH_OK;
    if (!okApply) real_SettingsApply = nullptr;
    VRLOG("resolution: live apply %s", okApply ? "ready" : "hook FAILED -- changes wait for a restart");
}

bool live_resize_available()
{
    if (!real_SettingsApply || !g_mainWindow.load() || !*g_settings) return false;
    return *reinterpret_cast<const int*>(*g_settings + kCfgMode) == 0;
}

void request_resolution_apply()
{
    if (!live_resize_available()) return;
    g_applyAskedAt.store(GetTickCount64());
    g_applyWanted.store(true);
}

bool resolution_apply_waiting()
{
    constexpr ULONGLONG kOneFrameIsNotWaiting = 250;
    return g_applyWanted.load() && GetTickCount64() - g_applyAskedAt.load() > kOneFrameIsNotWaiting;
}

// A LIVE RESIZE FROM OUTSIDE THE ENGINE DOES NOT WORK, and this is the record of
// why, because it looks as if it does. apply_resolution_now(), removed 2026-08-24,
// sat behind an "Apply resolution now" button and drove IDXGISwapChain::ResizeTarget
// -> ResizeBuffers, the same pair the game's own video settings and its fullscreen
// toggle use. Both calls succeeded. But the engine does not re-derive its render
// size from a swapchain it did not resize itself, so the picture was resized
// without being re-rendered.
//
// Do not rebuild that on the evidence that the calls succeed. They always did. The
// live apply above works because the engine runs its own device reset.

void set_force_resolution(int w, int h) { g_forceW.store(w); g_forceH.store(h); }
int  force_resolution_w()               { return g_forceW.load(); }
int  force_resolution_h()               { return g_forceH.load(); }

bool upscale_session(int& outW, int& outH, int& renderW, int& renderH)
{
    if (!g_upApplied.load()) return false;
    outW = g_upOutW.load();  outH = g_upOutH.load();
    renderW = g_winW.load(); renderH = g_winH.load();
    return outW > 0 && outH > 0 && renderW > 0 && renderH > 0;
}

int upscale_mode_applied() { return g_upApplied.load() ? g_upMode.load() : (int)dlss::kOff; }

} // namespace hooks
