#include "hooks/input_block.h"
#include "hooks/menu_hook.h"
#include "common/log.h"

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <xinput.h>
#include <MinHook.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <intrin.h>

namespace hooks {
namespace {

// --------------------------------------------------------------------------
// "is this call ours?"
// --------------------------------------------------------------------------
//
// The game and this mod poll the same physical controller through the same
// APIs, so blocking has to be decided per CALLER, not globally: everything
// except our own module gets silenced while the menu is open, and our menu
// navigation keeps reading the real pad. That is also why the XInput detours
// below need no real_/hooked split -- the caller test does that job.
uintptr_t g_selfBase = 0, g_selfEnd = 0;

void init_self_range()
{
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&init_self_range), &self) || !self)
        return;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(self);
    auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS64*>(
        reinterpret_cast<uint8_t*>(self) + dos->e_lfanew);
    g_selfBase = reinterpret_cast<uintptr_t>(self);
    g_selfEnd  = g_selfBase + nt->OptionalHeader.SizeOfImage;
}

bool caller_is_self(void* ret)
{
    // Fails OPEN when our own range is unknown: every call would otherwise
    // look foreign and get silenced, including the menu's own polling, which
    // leaves the mod unusable with no way to back out.
    if (!g_selfBase) return true;
    const uintptr_t a = reinterpret_cast<uintptr_t>(ret);
    return a >= g_selfBase && a < g_selfEnd;
}

void format_caller(void* addr, char* buf, size_t bufSize)
{
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(addr), &owner) || !owner) {
        snprintf(buf, bufSize, "0x%p (unresolved)", addr);
        return;
    }
    const uintptr_t off = reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(owner);
    if (owner == GetModuleHandleW(nullptr)) {
        snprintf(buf, bufSize, "exe+0x%llX", (unsigned long long)off);
        return;
    }
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(owner, path, MAX_PATH);
    wchar_t* name = path;
    for (wchar_t* p = path; *p; ++p) if (*p == L'\\') name = p + 1;
    char name8[MAX_PATH]{};
    WideCharToMultiByte(CP_ACP, 0, name, -1, name8, sizeof(name8), nullptr, nullptr);
    snprintf(buf, bufSize, "%s+0x%llX", name8, (unsigned long long)off);
}

// --------------------------------------------------------------------------
// XInput
// --------------------------------------------------------------------------
//
// The exe imports no XInput at all, yet its buttons keep reaching the game
// with DirectInput fully blocked -- which means it resolves XInput at runtime
// (LoadLibrary + GetProcAddress), a very common way to keep the dependency
// soft. A runtime-loaded DLL is invisible to the import table, so the only way
// to find it is to hook whatever is loaded and look at who calls.
//
// All-zero IS the correct neutral here, unlike DirectInput: XInput sticks are
// signed and centre at 0, triggers rest at 0, buttons are a bitmask. The one
// field that must NOT be zeroed is dwPacketNumber -- games use a change in it
// to mean "new input arrived", so it is frozen at the last real value instead,
// which reads as "nothing has happened since".
using PFN_XInputGetState = DWORD (WINAPI*)(DWORD, XINPUT_STATE*);

struct XiHook { void* target; PFN_XInputGetState orig; };
constexpr int kMaxXi = 8;
XiHook g_xi[kMaxXi]{};
std::atomic<int> g_xiCount{0};
DWORD g_lastPacket[XUSER_MAX_COUNT]{};

// Log each DISTINCT caller once rather than the first N calls: the Steam
// overlay polls XInput every frame, so a plain first-N cap is exhausted by one
// caller before anything more interesting is ever recorded. That is exactly
// what hid the bug below on the first attempt.
constexpr int kMaxLoggedCallers = 8;
char g_loggedCaller[kMaxLoggedCallers][MAX_PATH + 32]{};
std::atomic<int> g_loggedCallerCount{0};

void log_blocked_caller_once(void* ret)
{
    char c[MAX_PATH + 32];
    format_caller(ret, c, sizeof(c));
    const int n = g_loggedCallerCount.load();
    for (int i = 0; i < n && i < kMaxLoggedCallers; ++i)
        if (strcmp(g_loggedCaller[i], c) == 0) return;
    const int slot = g_loggedCallerCount.fetch_add(1);
    if (slot >= kMaxLoggedCallers) return;
    strncpy_s(g_loggedCaller[slot], c, _TRUNCATE);
    VRLOG("input block: XInput from %s blocked while menu open", c);
}

// Set around the mod's own polling (xinput_poll_self) and inherited by every
// nested call it makes. This, NOT the return address, is what identifies our
// traffic.
//
// The return address cannot do that job here. The Steam overlay hooks
// XInputGetState as well, and hooked it AFTER we did, so it owns the function
// entry and we sit on its trampoline: the game's calls and ours both reach
// this detour from gameoverlayrenderer64.dll+0xD0696, indistinguishable. The
// evidence was a log with exactly one distinct blocked caller ever recorded --
// that address -- and never the exe or this DLL, even while both were
// demonstrably being silenced.
//
// It also covers the nesting case it was originally added for:
// xinput1_4!XInputGetState calls XInputGetStateEx internally and we hook both,
// so a pass-through decision has to survive into the inner call. Blocking
// never calls through, so a blocked call can never nest.
thread_local int t_passThrough = 0;

template <int N>
DWORD WINAPI Detour_XInputGetState(DWORD userIndex, XINPUT_STATE* state)
{
    void* ret = _ReturnAddress();
    PFN_XInputGetState orig = g_xi[N].orig;
    if (!orig)
        return ERROR_DEVICE_NOT_CONNECTED;

    if (t_passThrough || !is_menu_open() || caller_is_self(ret)) {
        ++t_passThrough;
        DWORD r = orig(userIndex, state);
        --t_passThrough;
        if (r == ERROR_SUCCESS && state && userIndex < XUSER_MAX_COUNT)
            g_lastPacket[userIndex] = state->dwPacketNumber;
        return r;
    }

    log_blocked_caller_once(ret);
    if (state) {
        *state = XINPUT_STATE{};
        state->dwPacketNumber = (userIndex < XUSER_MAX_COUNT) ? g_lastPacket[userIndex] : 0;
    }
    return ERROR_SUCCESS;
}

const PFN_XInputGetState kXiDetours[kMaxXi] = {
    &Detour_XInputGetState<0>, &Detour_XInputGetState<1>,
    &Detour_XInputGetState<2>, &Detour_XInputGetState<3>,
    &Detour_XInputGetState<4>, &Detour_XInputGetState<5>,
    &Detour_XInputGetState<6>, &Detour_XInputGetState<7>,
};

bool xi_already_hooked(void* target)
{
    const int n = g_xiCount.load();
    for (int i = 0; i < n && i < kMaxXi; ++i)
        if (g_xi[i].target == target) return true;
    return false;
}

void hook_one_xinput(HMODULE mod, const char* procName, WORD ordinal, const wchar_t* dllName)
{
    void* target = reinterpret_cast<void*>(
        procName ? GetProcAddress(mod, procName)
                 : GetProcAddress(mod, reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(ordinal))));
    if (!target || xi_already_hooked(target))
        return;

    const int slot = g_xiCount.load();
    if (slot >= kMaxXi)
        return;

    void* orig = nullptr;
    if (MH_CreateHook(target, reinterpret_cast<void*>(kXiDetours[slot]), &orig) != MH_OK)
        return;
    // Published BEFORE enabling: once the patch is live a call can arrive on
    // any thread, and a detour that cannot find its trampoline has nothing
    // safe to do.
    g_xi[slot].target = target;
    g_xi[slot].orig   = reinterpret_cast<PFN_XInputGetState>(orig);
    g_xiCount.store(slot + 1);
    if (MH_EnableHook(target) != MH_OK) {
        g_xi[slot].orig = nullptr;
        VRLOG("input block: FAILED to enable %ls!%s hook", dllName,
              procName ? procName : "XInputGetStateEx(ord.100)");
        return;
    }
    VRLOG("input block: hooked %ls!%s", dllName, procName ? procName : "XInputGetStateEx(ord.100)");
}

// Rescanned until something lands, because the game loads its XInput DLL
// whenever it feels like it -- possibly long after our DllMain.
void scan_xinput()
{
    static const wchar_t* kDlls[] = {
        L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll", L"xinput1_2.dll"};
    for (const wchar_t* name : kDlls) {
        HMODULE m = GetModuleHandleW(name);
        if (!m) continue;
        hook_one_xinput(m, "XInputGetState", 0, name);
        // Ordinal 100 is the undocumented XInputGetStateEx, identical
        // signature, used by anything that wants the Guide button.
        hook_one_xinput(m, nullptr, 100, name);
    }
}

// --------------------------------------------------------------------------
// DirectInput 8
// --------------------------------------------------------------------------

constexpr int kIdxCreateDevice   = 3;   // IDirectInput8
constexpr int kIdxGetProperty    = 5;   // IDirectInputDevice8
constexpr int kIdxGetDeviceState = 9;
constexpr int kIdxGetDeviceData  = 10;
constexpr int kIdxSetDataFormat  = 11;

using PFN_DirectInput8Create = HRESULT (WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using PFN_CreateDevice   = HRESULT (STDMETHODCALLTYPE*)(IDirectInput8W*, REFGUID,
                                                        LPDIRECTINPUTDEVICE8W*, LPUNKNOWN);
using PFN_GetProperty    = HRESULT (STDMETHODCALLTYPE*)(IDirectInputDevice8W*, REFGUID, LPDIPROPHEADER);
using PFN_GetDeviceState = HRESULT (STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPVOID);
using PFN_GetDeviceData  = HRESULT (STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD,
                                                        LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
using PFN_SetDataFormat  = HRESULT (STDMETHODCALLTYPE*)(IDirectInputDevice8W*, LPCDIDATAFORMAT);

PFN_DirectInput8Create real_DirectInput8Create = nullptr;

void** vtable_of(void* obj) { return *reinterpret_cast<void***>(obj); }

// MinHook patches the FUNCTION BODY, not the vtable slot, so a device's vtable
// still holds the original address and identifies which implementation a call
// belongs to. dinput8 does not share one vtable across keyboard, mouse and
// joystick, so each distinct implementation is a separate target needing its
// own trampoline, while all instances of a given type share one.
struct Tramp { void* target; void* orig; };
constexpr int kMaxTramps = 6;

struct TrampTable {
    Tramp e[kMaxTramps]{};
    std::atomic<int> n{0};

    void* find(void* target) const
    {
        const int count = n.load();
        for (int i = 0; i < count && i < kMaxTramps; ++i)
            if (e[i].target == target) return e[i].orig;
        return nullptr;
    }
    bool has(void* target) const { return find(target) != nullptr; }
    void add(void* target, void* orig)
    {
        const int slot = n.load();
        if (slot >= kMaxTramps) return;
        e[slot].target = target;
        e[slot].orig   = orig;
        n.store(slot + 1);
    }
};

TrampTable g_tCreateDevice, g_tGetState, g_tGetData, g_tSetFormat;

// The game chooses the memory layout GetDeviceState fills, via SetDataFormat.
// To hand it a believable "nothing is being touched" buffer we have to know
// which byte offsets are which kind of control. Zeroing the whole thing is
// WRONG in two ways that both look exactly like input that is stuck on: a POV
// hat reads 0 as "pressed up", and an absolute axis whose range the game set
// to 0..65535 reads 0 as full deflection rather than centre. The latter is
// what made the camera zoom run away while the menu was open.
//
// Buttons and RELATIVE axes (mouse deltas) are correctly zero, so only
// absolute axes and POVs are recorded.
struct DevRec {
    IDirectInputDevice8W* dev = nullptr;
    DWORD absOfs[24]{};
    LONG  absCentre[24]{};
    bool  absKnown[24]{};
    int   absCount = 0;
    DWORD povOfs[8]{};
    int   povCount = 0;
    bool  centresResolved = false;
    // When this record was last looked up. A device being polled is looked up
    // every frame, so this separates the live ones from the released ones
    // without having to ask DirectInput anything. See rec_for().
    DWORD lastUsed = 0;
};

// A CACHE, not a fixed pool. The game creates and releases keyboard/mouse
// device pairs repeatedly -- a dozen of them in the first thirty seconds, and
// it visibly recycles the addresses (the same pointer shows up later as a
// different device type). A pool that only ever grew ran out after eight, and
// every device created past that had no record, which made GetDeviceState fall
// through to "pass the real state" -- i.e. DirectInput silently stopped being
// blocked at all a few seconds into the session.
//
// EVICT THE COLDEST, not the oldest, and that distinction is the whole bug.
//
// Round-robin eviction only asks which record was WRITTEN longest ago, so a
// device created once and kept forever is guaranteed to be thrown out by
// churn -- it just takes kMaxDevices creations instead of eight. The gamepad
// is exactly that device: the game makes it once and holds it, while the
// keyboard and mouse pair is recreated over and over. Once its record went, so
// did the only thing that knew where its axes were, and GetDeviceState fell
// into the same "pass the real state through" branch as before. The menu stayed
// open and the pad stayed live.
//
// Least-recently-USED cannot do that. A device being polled is looked up on
// every one of its own GetDeviceState calls, so a live one is never the coldest
// entry; a released one goes cold the moment the game stops asking about it.
//
// Unsynchronised, as it always was: input arrives on more than one thread, and
// the worst case is two creations picking the same victim, which costs one
// re-capture. A lock on this path would be a lock in the game's input loop.
constexpr int kMaxDevices = 32;
DevRec g_dev[kMaxDevices]{};

DevRec* rec_for(IDirectInputDevice8W* dev, bool create)
{
    const DWORD now = GetTickCount();
    for (int i = 0; i < kMaxDevices; ++i)
        if (g_dev[i].dev == dev) { g_dev[i].lastUsed = now; return &g_dev[i]; }
    if (!create) return nullptr;

    // An empty slot first, then the one nobody has asked about for longest.
    // Recycled addresses never reach here -- the scan above already found them.
    int slot = 0;
    for (int i = 0; i < kMaxDevices; ++i) {
        if (!g_dev[i].dev) { slot = i; break; }
        if ((DWORD)(now - g_dev[i].lastUsed) > (DWORD)(now - g_dev[slot].lastUsed))
            slot = i;
    }
    g_dev[slot] = DevRec{};
    g_dev[slot].dev = dev;
    g_dev[slot].lastUsed = now;
    return &g_dev[slot];
}

// Centre of each absolute axis, straight from the range the game itself set.
// Deferred to the first block rather than done at SetDataFormat time: games
// routinely call SetProperty(DIPROP_RANGE) after SetDataFormat, so asking too
// early would cache the default range instead of the real one.
void resolve_centres(IDirectInputDevice8W* dev, DevRec& r)
{
    if (r.centresResolved) return;
    r.centresResolved = true;

    auto getProp = reinterpret_cast<PFN_GetProperty>(vtable_of(dev)[kIdxGetProperty]);

    // Which axes are RELATIVE is a property of the device, not of the data
    // format: a mouse's format declares its axes as plain DIDFT_AXIS, which
    // matches the DIDFT_ABSAXIS mask, so they arrive here looking absolute.
    // Centring a relative axis is actively harmful -- the "centre" of a
    // relative axis's range is a nonzero delta, i.e. constant movement every
    // frame, which for a mouse wheel reads as the camera zooming forever. For
    // relative axes 0 (no movement) is the correct neutral.
    DIPROPDWORD mode{};
    mode.diph.dwSize       = sizeof(mode);
    mode.diph.dwHeaderSize = sizeof(mode.diph);
    mode.diph.dwObj        = 0;
    mode.diph.dwHow        = DIPH_DEVICE;
    const bool relative = SUCCEEDED(getProp(dev, DIPROP_AXISMODE, &mode.diph)) &&
                          mode.dwData == DIPROPAXISMODE_REL;

    int known = 0;
    for (int i = 0; i < r.absCount; ++i) {
        if (relative) {
            r.absCentre[i] = 0;
            r.absKnown[i]  = true;
            ++known;
            continue;
        }
        DIPROPRANGE pr{};
        pr.diph.dwSize       = sizeof(pr);
        pr.diph.dwHeaderSize = sizeof(pr.diph);
        pr.diph.dwObj        = r.absOfs[i];
        pr.diph.dwHow        = DIPH_BYOFFSET;
        if (SUCCEEDED(getProp(dev, DIPROP_RANGE, &pr.diph))) {
            r.absCentre[i] = pr.lMin + (pr.lMax - pr.lMin) / 2;
            r.absKnown[i]  = true;
            ++known;
        }
    }
    VRLOG("input block: device %p neutral resolved -- %s axes, %d of them (%d with a "
          "known neutral), %d POVs", (void*)dev, relative ? "RELATIVE" : "absolute",
          r.absCount, known, r.povCount);
}

// --------------------------------------------------------------------------
// detours
// --------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE Detour_SetDataFormat(IDirectInputDevice8W* dev, LPCDIDATAFORMAT fmt)
{
    auto orig = reinterpret_cast<PFN_SetDataFormat>(g_tSetFormat.find(vtable_of(dev)[kIdxSetDataFormat]));
    if (!orig)
        return DIERR_NOTINITIALIZED;
    HRESULT hr = orig(dev, fmt);
    if (FAILED(hr) || !fmt || !fmt->rgodf)
        return hr;

    DevRec* r = rec_for(dev, true);
    if (!r) return hr;
    r->absCount = 0;
    r->povCount = 0;
    r->centresResolved = false;
    for (DWORD i = 0; i < fmt->dwNumObjs; ++i) {
        const DIOBJECTDATAFORMAT& o = fmt->rgodf[i];
        if ((o.dwType & DIDFT_ABSAXIS) && r->absCount < 24)
            r->absOfs[r->absCount++] = o.dwOfs;
        else if ((o.dwType & DIDFT_POV) && r->povCount < 8)
            r->povOfs[r->povCount++] = o.dwOfs;
    }
    VRLOG("input block: device %p data format captured -- %lu objects, %lu bytes, "
          "%d abs axes, %d POVs", (void*)dev, fmt->dwNumObjs, fmt->dwDataSize,
          r->absCount, r->povCount);
    return hr;
}

// --- the keyboard, via the EXE's import table ------------------------------
//
// WHY THIS EXISTS AT ALL. Blocking XInput and DirectInput covers the pad only
// while the pad arrives as a pad. Steam Input does not have to deliver it that
// way: a Steam Controller has no native XInput, and a common mapping turns its
// buttons into KEYSTROKES. The game then reads them the way it reads the
// keyboard -- GetAsyncKeyState -- and neither of the other two blocks sees a
// thing. Measured 2026-09-20: XInput blocked, DirectInput neutralised, no
// joystick device in the session at all, and the pad still drove the game.
//
// AN IAT HOOK, NOT AN INLINE PATCH, and that is the whole point. Detouring
// GetAsyncKeyState with MinHook was tried three times and crashed the game on
// startup every time -- silently, with MinHook reporting success, which points
// at a conflict with another inline patch on the same very hot, very
// heavily-instrumented export (RTSS is confirmed present in this process).
// Rewriting the EXE's import slot touches no code anywhere: user32 is left
// exactly as it is, every other hooker's patch is left exactly as it is, and
// nothing but this one binary's own calls are redirected.
//
// IT ALSO REMOVES A PROBLEM RATHER THAN ADDING ONE. Because only the exe's
// imports are redirected, the mod's own ::GetAsyncKeyState calls -- the menu
// toggle, the rebind capture, HOME, PageUp -- resolve through OUR import table
// and are untouched by construction. No return-address test, no thread-local
// flag, no way for the menu to silence itself.
//
// INSTALLED AT THE FIRST PRESENT, not from DllMain. This DLL is loaded while
// the exe's own imports are still being resolved, and the Steam DRM unpacks
// after that -- so a slot written too early is a slot that gets overwritten.
using PFN_GetAsyncKeyState = SHORT(WINAPI*)(int);
using PFN_GetKeyState      = SHORT(WINAPI*)(int);
PFN_GetAsyncKeyState real_GetAsyncKeyState = nullptr;
PFN_GetKeyState      real_GetKeyState      = nullptr;

SHORT WINAPI Detour_GetAsyncKeyState(int vk)
{
    if (is_menu_open()) return 0;
    return real_GetAsyncKeyState ? real_GetAsyncKeyState(vk) : ::GetAsyncKeyState(vk);
}

SHORT WINAPI Detour_GetKeyState(int vk)
{
    // Paired with the one above because they are two faces of one question and
    // a game that finds one silenced will happily use the other.
    if (is_menu_open()) return 0;
    return real_GetKeyState ? real_GetKeyState(vk) : ::GetKeyState(vk);
}

// Redirects one named import of one DLL in `module`'s import table. Returns the
// original through `orig`. Walks by NAME rather than by address so it is
// immune to the export already being detoured by somebody else -- which it
// very likely is.
bool hook_iat(HMODULE module, const char* dllName, const char* fnName,
              void* detour, void** orig)
{
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const IMAGE_DATA_DIRECTORY& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return false;

    auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; imp->Name; ++imp) {
        const char* dll = reinterpret_cast<const char*>(base + imp->Name);
        if (_stricmp(dll, dllName) != 0) continue;

        // OriginalFirstThunk holds the NAMES, FirstThunk holds the live
        // pointers. A bound import has no OFT, in which case the names are not
        // recoverable and this import is left alone rather than guessed at.
        if (!imp->OriginalFirstThunk) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->FirstThunk);

        for (int i = 0; names[i].u1.AddressOfData; ++i) {
            if (IMAGE_SNAP_BY_ORDINAL64(names[i].u1.Ordinal)) continue;
            auto* imn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                base + names[i].u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(imn->Name), fnName) != 0) continue;

            void** slot = reinterpret_cast<void**>(&slots[i].u1.Function);
            DWORD old = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
                return false;
            if (orig) *orig = *slot;
            *slot = detour;
            VirtualProtect(slot, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}

void install_keyboard_block()
{
    static std::atomic<bool> s_done{false};
    bool expected = false;
    if (!s_done.compare_exchange_strong(expected, true))
        return;

    HMODULE exe = GetModuleHandleW(nullptr);
    if (!exe) { s_done.store(false); return; }

    const bool a = hook_iat(exe, "USER32.dll", "GetAsyncKeyState",
                            reinterpret_cast<void*>(&Detour_GetAsyncKeyState),
                            reinterpret_cast<void**>(&real_GetAsyncKeyState));
    const bool k = hook_iat(exe, "USER32.dll", "GetKeyState",
                            reinterpret_cast<void*>(&Detour_GetKeyState),
                            reinterpret_cast<void**>(&real_GetKeyState));
    VRLOG("input block: keyboard IAT %s (GetAsyncKeyState %s, GetKeyState %s) -- "
          "this is what silences a Steam Input pad mapped to keys",
          (a || k) ? "hooked" : "NOT hooked", a ? "ok" : "absent", k ? "ok" : "absent");
}

std::atomic<bool> g_loggedFirstDiBlock{false};

HRESULT STDMETHODCALLTYPE Detour_GetDeviceState(IDirectInputDevice8W* dev, DWORD cbData, LPVOID lpvData)
{
    auto orig = reinterpret_cast<PFN_GetDeviceState>(g_tGetState.find(vtable_of(dev)[kIdxGetDeviceState]));
    if (!orig)
        return DIERR_NOTINITIALIZED;

    if (!is_menu_open() || !lpvData || !cbData)
        return orig(dev, cbData, lpvData);

    // Read the real state first, so an axis whose range we could not determine
    // can keep its true value instead of being forced to a fabricated one.
    // Getting that wrong is strictly worse than not blocking: a wrong constant
    // is indistinguishable from the stick being held at full deflection, which
    // is exactly the runaway-zoom bug this replaced.
    BYTE realState[1024];
    const bool haveReal = (cbData <= sizeof(realState)) &&
                          SUCCEEDED(orig(dev, cbData, realState));

    memset(lpvData, 0, cbData);
    if (DevRec* r = rec_for(dev, false)) {
        resolve_centres(dev, *r);
        auto* bytes = static_cast<BYTE*>(lpvData);
        for (int i = 0; i < r->absCount; ++i) {
            if (r->absOfs[i] + sizeof(LONG) > cbData) continue;
            LONG v = 0;
            if (r->absKnown[i])
                v = r->absCentre[i];
            else if (haveReal)
                memcpy(&v, realState + r->absOfs[i], sizeof(LONG));
            *reinterpret_cast<LONG*>(bytes + r->absOfs[i]) = v;
        }
        for (int i = 0; i < r->povCount; ++i)
            if (r->povOfs[i] + sizeof(DWORD) <= cbData)
                *reinterpret_cast<DWORD*>(bytes + r->povOfs[i]) = 0xFFFFFFFFu;  // centred
    } else if (haveReal) {
        // No data format seen for this device, so nothing can be neutralised
        // safely -- pass the real state through rather than invent one.
        //
        // LOGGED, because this is the one branch where input genuinely reaches
        // the game with the menu open, and it was silent. It should now be
        // unreachable for any live device (see rec_for), so a line here means
        // either a device whose SetDataFormat we never saw, or the cache
        // thrashing again -- and either way it names the device to look at.
        static std::atomic<int> s_loggedPassThrough{0};
        if (s_loggedPassThrough.fetch_add(1) < 4)
            VRLOG("input block: device %p has NO captured data format -- its real "
                  "state PASSES THROUGH while the menu is open", (void*)dev);
        memcpy(lpvData, realState, cbData);
    }

    // Deliberately a SUCCESSFUL call reporting a neutral device, not an error.
    // DIERR_NOTACQUIRED/DIERR_INPUTLOST would be the tidier lie, but a common
    // game idiom is "while (GetDeviceState fails) Acquire();" -- against a
    // failure we return unconditionally that spins forever.
    if (!g_loggedFirstDiBlock.exchange(true))
        VRLOG("input block: menu open -- DirectInput state neutralised for the game");
    return DI_OK;
}

HRESULT STDMETHODCALLTYPE Detour_GetDeviceData(IDirectInputDevice8W* dev, DWORD cbObjectData,
                                               LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut,
                                               DWORD dwFlags)
{
    auto orig = reinterpret_cast<PFN_GetDeviceData>(g_tGetData.find(vtable_of(dev)[kIdxGetDeviceData]));
    if (!orig)
        return DIERR_NOTINITIALIZED;

    if (!is_menu_open())
        return orig(dev, cbObjectData, rgdod, pdwInOut, dwFlags);

    // Buffered mode: report an empty event queue. Events that piled up while
    // the menu was open would otherwise all be delivered at once on close, so
    // drain rather than peek -- except under DIGDD_PEEK, which by contract
    // must leave the queue alone.
    if (!(dwFlags & DIGDD_PEEK)) {
        DWORD discard = INFINITE;   // rgdod == NULL + INFINITE: flush the buffer
        orig(dev, cbObjectData, nullptr, &discard, 0);
    }
    if (pdwInOut) *pdwInOut = 0;
    return DI_OK;
}

// Hooks the three device methods the first time a device of a given
// implementation appears. All later instances share the vtable, so this runs
// at most a couple of times per process.
void hook_device_methods(IDirectInputDevice8W* dev)
{
    struct Slot { int index; TrampTable* table; void* detour; const char* name; };
    const Slot slots[] = {
        { kIdxGetDeviceState, &g_tGetState,  reinterpret_cast<void*>(&Detour_GetDeviceState), "GetDeviceState" },
        { kIdxGetDeviceData,  &g_tGetData,   reinterpret_cast<void*>(&Detour_GetDeviceData),  "GetDeviceData"  },
        { kIdxSetDataFormat,  &g_tSetFormat, reinterpret_cast<void*>(&Detour_SetDataFormat),  "SetDataFormat"  },
    };
    for (const Slot& s : slots) {
        void* target = vtable_of(dev)[s.index];
        if (!target || s.table->has(target))
            continue;
        void* orig = nullptr;
        if (MH_CreateHook(target, s.detour, &orig) != MH_OK) {
            VRLOG("input block: FAILED to create IDirectInputDevice8::%s hook", s.name);
            continue;
        }
        s.table->add(target, orig);   // published before enabling -- see hook_one_xinput()
        if (MH_EnableHook(target) != MH_OK)
            VRLOG("input block: FAILED to enable IDirectInputDevice8::%s hook", s.name);
        else
            VRLOG("input block: hooked IDirectInputDevice8::%s", s.name);
    }
}

const char* device_kind(REFGUID rguid)
{
    if (IsEqualGUID(rguid, GUID_SysKeyboard)) return "keyboard";
    if (IsEqualGUID(rguid, GUID_SysMouse))    return "mouse";
    return "joystick/other";
}

HRESULT STDMETHODCALLTYPE Detour_CreateDevice(IDirectInput8W* di, REFGUID rguid,
                                              LPDIRECTINPUTDEVICE8W* out, LPUNKNOWN outer)
{
    auto orig = reinterpret_cast<PFN_CreateDevice>(g_tCreateDevice.find(vtable_of(di)[kIdxCreateDevice]));
    if (!orig)
        return DIERR_NOTINITIALIZED;
    HRESULT hr = orig(di, rguid, out, outer);
    if (SUCCEEDED(hr) && out && *out) {
        VRLOG("input block: game created a DirectInput %s device (%p)",
              device_kind(rguid), (void*)*out);
        hook_device_methods(*out);
    }
    return hr;
}

HRESULT WINAPI Detour_DirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid,
                                         LPVOID* out, LPUNKNOWN outer)
{
    HRESULT hr = real_DirectInput8Create(inst, version, riid, out, outer);
    if (FAILED(hr) || !out || !*out)
        return hr;

    // The device methods are the real targets; this interface only exists to
    // reach them, since CreateDevice is the only way a device object is handed
    // out. IDirectInput8A and IDirectInput8W share a vtable layout, so the
    // cast is safe regardless of which riid was asked for.
    auto* di = static_cast<IDirectInput8W*>(*out);
    void* target = vtable_of(di)[kIdxCreateDevice];
    if (target && !g_tCreateDevice.has(target)) {
        void* orig = nullptr;
        if (MH_CreateHook(target, reinterpret_cast<void*>(&Detour_CreateDevice), &orig) == MH_OK) {
            g_tCreateDevice.add(target, orig);
            if (MH_EnableHook(target) == MH_OK)
                VRLOG("input block: hooked IDirectInput8::CreateDevice");
            else
                VRLOG("input block: FAILED to enable IDirectInput8::CreateDevice hook");
        } else {
            VRLOG("input block: FAILED to create IDirectInput8::CreateDevice hook");
        }
    }
    return hr;
}

} // namespace

void install_input_block()
{
    static std::atomic<bool> s_done{false};
    bool expected = false;
    if (!s_done.compare_exchange_strong(expected, true))
        return;

    init_self_range();

    void* target = nullptr;
    if (MH_CreateHookApiEx(L"dinput8", "DirectInput8Create",
                           reinterpret_cast<void*>(&Detour_DirectInput8Create),
                           reinterpret_cast<void**>(&real_DirectInput8Create),
                           &target) == MH_OK &&
        MH_EnableHook(target) == MH_OK) {
        VRLOG("input block: DirectInput8Create hooked");
    } else {
        VRLOG("input block: DirectInput8Create hook FAILED");
    }

    scan_xinput();
}

DWORD xinput_poll_self(DWORD userIndex, XINPUT_STATE* state)
{
    // Goes through the hooked function deliberately -- including the Steam
    // overlay's own hook, which sits in front of ours and must stay in the
    // chain -- with the flag set so our detour lets it past.
    ++t_passThrough;
    const DWORD r = ::XInputGetState(userIndex, state);
    --t_passThrough;

    // CAN THE MOD SEE A PAD AT ALL. Logged on transition only, so it costs
    // nothing per frame and still catches a pad appearing or vanishing.
    //
    // This is the line that separates the two failures that look identical
    // from the outside. "The gamepad does not work in the menu" can mean the
    // BLOCK is leaking (the game gets input it should not), or that the mod
    // cannot READ the pad (the menu gets nothing to navigate with). They call
    // for opposite fixes, and only this says which one is happening.
    //
    // Steam Input is the reason it can be the second one: it captures the
    // physical pad and re-presents it, and what it presents -- a virtual pad,
    // keystrokes, or nothing at all to callers other than the game -- is a
    // per-controller setting on Steam's side, not something visible from here.
    static std::atomic<DWORD> s_lastResult{0xFFFFFFFFu};
    if (userIndex == 0 && s_lastResult.exchange(r) != r)
        VRLOG("input block: our own pad poll -> %s (XInputGetState returned %lu)",
              r == ERROR_SUCCESS ? "PAD PRESENT, menu navigable"
                                 : "NO PAD -- the menu has nothing to read",
              (unsigned long)r);
    return r;
}

void input_block_on_present()
{
    install_keyboard_block();

    // The game resolves XInput at runtime, so the DLL it uses may not have
    // been loaded yet at DllMain. Keep looking until at least one hook lands,
    // then stop -- this runs per frame and must cost nothing once satisfied.
    if (g_xiCount.load() > 0)
        return;
    scan_xinput();
}

} // namespace hooks
