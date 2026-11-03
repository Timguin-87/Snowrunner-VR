#include "hooks/frame_sync.h"
#include "hooks/game_addresses.h"
#include "hooks/pattern_scan.h"
#include "common/log.h"

#include <windows.h>
#include <MinHook.h>

#include <atomic>

namespace hooks {
namespace {

// Every argument declared, floats included. A hook on OnFrameMove once
// corrupted the cameras (then mistaken for a "camera consumer", July build) for
// exactly the want of the two time registers -- xmm0 and xmm1 here, xmm1 and
// xmm2 on the render callback, which takes the device first.
using PFN_FrameMove   = void (*)(double time, float elapsed, void* user);
using PFN_FrameRender = void (*)(void* device, double time, float elapsed, void* user);
PFN_FrameMove   real_FrameMove   = nullptr;
PFN_FrameRender real_FrameRender = nullptr;

std::atomic<bool>     g_ready{false};
std::atomic<bool>     g_freeze{false};
std::atomic<uint32_t> g_update{0};
std::atomic<int>      g_lag{0};
volatile int*         g_clampFlag = nullptr;

// The recent updates' kind, readable from any thread: the camera hooks that
// ask run on the engine's render workers. Packed as (index << 1) | frozen, so
// a slot that has moved on to a newer update is told apart from the one asked
// for.
constexpr uint32_t kRing = 32;
std::atomic<uint64_t> g_kind[kRing]{};

// The time each recent update was handed, for the render that shows it. Main
// thread only -- both callbacks run there.
double g_time[kRing]{};
float  g_elapsed[kRing]{};

// The alternation (main thread only). Negative last-real time = not started.
double g_lastRealT = -1.0;
std::atomic<bool> g_nextFrozen{false};

void det_FrameMove(double t, float dt, void* user)
{
    const uint32_t idx = g_update.load() + 1;
    double tt = t;
    float  dd = dt;
    bool frozen = false;
    if (g_freeze.load()) {
        if (g_lastRealT < 0.0) {           // starting: the first one is real
            g_lastRealT = t - dt;
            g_nextFrozen.store(false);
        }
        frozen = g_nextFrozen.load();
        g_nextFrozen.store(!frozen);
        if (frozen) {
            tt = g_lastRealT;
            dd = 0.0f;
        } else {
            dd = (float)(t - g_lastRealT);
            g_lastRealT = t;
        }
    } else {
        g_lastRealT = -1.0;
    }
    g_time[idx % kRing] = tt;
    g_elapsed[idx % kRing] = dd;
    g_kind[idx % kRing].store(((uint64_t)idx << 1) | (frozen ? 1u : 0u));
    g_update.store(idx);

    // OnFrameMove clamps the elapsed time it hands the game into [1/60, 1/20]
    // while this flag is set, which would give a frozen update a sixtieth of a
    // second anyway. It read 0 in every session measured; held off for the
    // length of a frozen update in case it ever is not.
    if (frozen && g_clampFlag && *g_clampFlag) {
        const int saved = *g_clampFlag;
        *g_clampFlag = 0;
        real_FrameMove(tt, dd, user);
        *g_clampFlag = saved;
        return;
    }
    real_FrameMove(tt, dd, user);
}

void det_FrameRender(void* device, double t, float dt, void* user)
{
    // The time of the update this render shows, not of the one just run --
    // in-game those are an update apart.
    if (g_freeze.load() && g_lastRealT >= 0.0) {
        const uint32_t shown = g_update.load() - (uint32_t)g_lag.load();
        const uint64_t k = g_kind[shown % kRing].load();
        if ((uint32_t)(k >> 1) == shown) {
            t = g_time[shown % kRing];
            dt = g_elapsed[shown % kRing];
        }
    }
    real_FrameRender(device, t, dt, user);
}

template <typename T>
bool hook_callback(const char* name, uintptr_t rva, void* detour, T** real)
{
    if (!rva) {
        VRLOG("frame sync: %s not found in this game build -- Synched Sequential "
              "unavailable", name);
        return false;
    }
    void* target = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr)) + rva;
    if (!is_function_entry(target)) {
        VRLOG("frame sync: %s at exe+0x%llX is not a function entry -- refused",
              name, (unsigned long long)rva);
        return false;
    }
    if (MH_CreateHook(target, detour, reinterpret_cast<void**>(real)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        VRLOG("frame sync: %s hook FAILED", name);
        return false;
    }
    return true;
}

} // namespace

void install_frame_sync()
{
    static std::atomic<bool> s_done{false};
    if (s_done.exchange(true)) return;
    const GameAddresses& a = game_addresses();
    if (a.elapsedClampFlag)
        g_clampFlag = reinterpret_cast<volatile int*>(
            reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr)) + a.elapsedClampFlag);
    const bool m = hook_callback("OnFrameMove", a.frameMove,
                                 reinterpret_cast<void*>(&det_FrameMove), &real_FrameMove);
    const bool r = m && hook_callback("OnFrameRender", a.frameRender,
                                      reinterpret_cast<void*>(&det_FrameRender), &real_FrameRender);
    g_ready.store(m && r);
    if (m && r)
        VRLOG("frame sync: OnFrameMove and OnFrameRender hooked -- Synched Sequential available");
}

bool frame_sync_ready() { return g_ready.load(); }

void set_frame_freeze(bool on)
{
    if (on && !g_ready.load()) return;
    if (g_freeze.exchange(on) != on)
        VRLOG("frame sync: every second update frozen -> %s", on ? "ON" : "OFF");
}

bool frame_freeze_on() { return g_freeze.load(); }

uint32_t frame_update_index() { return g_update.load(); }

bool frame_update_frozen(uint32_t index)
{
    const uint64_t k = g_kind[index % kRing].load();
    return (uint32_t)(k >> 1) == index && (k & 1u) != 0;
}

bool frame_next_update_frozen() { return g_freeze.load() && g_nextFrozen.load(); }

void set_frame_render_lag(int lag) { g_lag.store(lag < 0 ? 0 : (lag > 2 ? 2 : lag)); }

} // namespace hooks
