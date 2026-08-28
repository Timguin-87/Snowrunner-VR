#include "hooks/shader_cull.h"
#include "common/log.h"
#include "common/config.h"
#include "hooks/camera_hook.h"      // the DRIVE_CAMERA garage/gameplay field

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <string>

#include <MinHook.h>

using Microsoft::WRL::ComPtr;

namespace hooks {
namespace {

// Verified against the SDK header (10.0.26100.0) this session, the same way
// ui_hook.cpp's constants were: ID3D11Device slot 15 is CreatePixelShader
// (QueryInterface/AddRef/Release, CreateBuffer, three CreateTextureND, SRV,
// UAV, RTV, DSV, CreateInputLayout, VS, GS, GSWithStreamOutput, PS), and
// ID3D11DeviceContext slot 9 is PSSetShader. Slot 9 is untouched by every
// other hook in this codebase, so this module can own it outright -- unlike
// the draw slots, which ui_hook.cpp already holds and which MinHook will not
// let a second detour share.
constexpr int kIdxCreatePixelShader = 15;
constexpr int kIdxPSSetShader       = 9;

using PFN_CreatePixelShader = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
using PFN_PSSetShader = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);

PFN_CreatePixelShader real_CreatePixelShader = nullptr;
// Immediate and deferred contexts have DISTINCT vtables (see the long note in
// ui_hook.cpp), so each needs its own saved original.
PFN_PSSetShader real_PSSetShader_imm = nullptr, real_PSSetShader_def = nullptr;

bool is_imm(ID3D11DeviceContext* ctx) { return ctx->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE; }

// --- shader identity -----------------------------------------------------

// The first 8 bytes of the DXBC container checksum, which sits immediately
// after the 'DXBC' fourcc. It is the compiler's own hash of the bytecode, so
// it is identical every run and identical to what fxc and RenderDoc report --
// which is the whole reason a shader found once can be written to the config
// file and still be the right shader next launch. The FNV fallback exists
// only so a blob that somehow isn't a DXBC container still gets a stable key
// rather than colliding with every other such blob at 0.
uint64_t dxbc_hash(const void* bytecode, SIZE_T len)
{
    const uint8_t* p = static_cast<const uint8_t*>(bytecode);
    if (!p || len < 20) return 0;
    if (p[0] == 'D' && p[1] == 'X' && p[2] == 'B' && p[3] == 'C') {
        uint64_t h = 0;
        for (int i = 0; i < 8; ++i) h |= (uint64_t)p[4 + i] << (i * 8);
        return h;
    }
    uint64_t h = 1469598103934665603ull;
    for (SIZE_T i = 0; i < len; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// --- pointer -> {hash, list index} --------------------------------------
// Open-addressed, linear-probed, lock-free. Written from whichever thread
// creates or first binds a shader (this engine does both from worker
// threads), read from the draw path, so every field is atomic. `usedPlus1`
// is biased by one so that zero-initialised storage means "not in the
// candidate list yet" without an init pass.
constexpr int kSlots = 16384;   // power of two
struct Slot {
    std::atomic<void*>    ps{nullptr};
    std::atomic<uint64_t> hash{0};
    std::atomic<int>      usedPlus1{0};
    // Created by the mod itself (mirror blit, stale-eye warp, ImGui) rather
    // than by the game. Belt-and-braces alongside the draw-time self-render
    // bracket: our shaders are all created lazily from inside the Present
    // hook, so this catches them at the source and they are never registered
    // as candidates at all -- no search step can reach them however the
    // thread-local flag happens to be sitting.
    std::atomic<bool>     exempt{false};
};
Slot g_slots[kSlots];
std::atomic<int> g_slotCount{0};
std::atomic<bool> g_slotsFullLogged{false};

inline size_t slot_of(void* p)
{
    uint64_t x = reinterpret_cast<uint64_t>(p) >> 4;   // COM objects are at least 16-byte aligned
    x *= 0x9E3779B97F4A7C15ull;
    return (size_t)(x >> 32) & (kSlots - 1);
}

Slot* find_slot(void* ps)
{
    size_t i = slot_of(ps);
    for (int n = 0; n < kSlots; ++n, i = (i + 1) & (kSlots - 1)) {
        void* k = g_slots[i].ps.load(std::memory_order_acquire);
        if (k == ps) return &g_slots[i];
        if (!k) return nullptr;
    }
    return nullptr;
}

// Returns the slot now owned by `ps`, claiming a free one if needed. Null
// only when the table is full.
Slot* claim_slot(void* ps)
{
    size_t i = slot_of(ps);
    for (int n = 0; n < kSlots; ++n, i = (i + 1) & (kSlots - 1)) {
        void* k = g_slots[i].ps.load(std::memory_order_acquire);
        if (k == ps) return &g_slots[i];
        if (!k) {
            void* expected = nullptr;
            if (g_slots[i].ps.compare_exchange_strong(expected, ps,
                                                      std::memory_order_acq_rel)) {
                g_slotCount.fetch_add(1, std::memory_order_relaxed);
                return &g_slots[i];
            }
            // Lost the race: either to this same pointer (fine, use it) or to
            // an unrelated one (fine, keep probing).
            if (expected == ps) return &g_slots[i];
        }
    }
    if (!g_slotsFullLogged.exchange(true))
        VRLOG("SHADER-CULL: pointer table full (%d slots) -- shaders seen from "
              "here on cannot be identified or culled", kSlots);
    return nullptr;
}

// --- the candidate list (shaders actually seen bound) --------------------
// Deliberately populated at first BIND, not at creation: the game compiles
// far more shaders than any one scene uses, and a list of everything ever
// created would be useless to search through.
constexpr int kMaxUsed = 2048;
struct Used {
    void*                 ps = nullptr;
    uint64_t              hash = 0;
    std::atomic<uint32_t> draws{0};
    uint32_t              drawsLast = 0;
    std::atomic<uint32_t> maxElems{0};
    bool                  culled = false;    // user/config intent
    bool                  inSearch = false;  // this search step
    int                   role = -1;         // CullRole this shader's hash names
    // The three sources folded into one atomic so the bind path reads a single
    // flag instead of testing membership of an arbitrary index set. Recomputed
    // only when something actually changes, which is never on a hot path.
    std::atomic<bool>     suppress{false};
    // Frames of grace left since this shader last drew, for the "drawing now"
    // readout. A plain drawsLast test flickers: plenty of shaders draw every
    // other frame, and a count that bounces between 11 and 12 is unreadable at
    // exactly the moment you are trying to judge whether a set holds.
    std::atomic<uint8_t>  recent{0};
    // Index into g_markers, or -1. Reassigned by rebuild_markers() whenever the
    // evidence changes, since the marker set is now derived from the probe
    // rather than baked -- written and read on the Present thread only.
    int                   markerId = -1;
};
constexpr uint8_t kRecentFrames = 30;   // ~0.3 s of grace
Used g_used[kMaxUsed];
std::atomic<int> g_usedCount{0};

// --- the fingerprint gate ------------------------------------------------
// THE MAP'S MARKER SET, BAKED. Collected with the screen probe over several
// sessions and written in here 2026-08-26, at which point the probe and its tab
// were removed -- the working-out is not worth carrying once the answer is
// known, and a hash is the same number tomorrow, on another machine, after a
// reinstall. That property is the whole reason a shader hash was worth probing
// where a heap object was not.
//
// THE MAP IS THE ONLY SCREEN WITH A SET, and the only one that can be claimed.
//
//   MENU     had one exclusive marker, and it was withdrawn 2026-08-26: the
//            shader also draws inside a level, so the list went live mid-drive
//            and claimed MENU while driving. A positive claim is taken at face
//            value, so that is worse than no claim at all.
//   GARAGE   never had one. It shares its interface with the menus and its
//            world with gameplay, so nothing draws only there -- and the
//            engine's own DRIVE_CAMERA mode enum, which would have settled it,
//            is unreadable in the garage because that screen does not tick the
//            drive camera at all (see camera_hook.h).
//   GAMEPLAY never had one either, for the mirror-image reason: it shares its
//            world with everything. It is the COMPLEMENT instead -- see
//            in_gameplay().
//
// Both are answered one step further down by hooks::logic_camera_idle_ms(),
// which measures the drive camera having stopped. That is not a proxy for those
// screens; it is the same fact read directly, and it needs nothing discovered.
//
// EVERY MEMBER MUST BE DRAWING for the screen to be claimed. Not a quorum: the
// whole list. A quorum of two was tried and it is the wrong shape -- it makes a
// list of five no stronger than a list of two, and lets the screen be claimed
// by whichever pair happens to be alive at that instant. Requiring all of them
// turns the list's LENGTH into the strength of the claim.
//
// BY HASH, not by shader object. The same bytecode can be created as two
// separate ID3D11PixelShader objects, and with an all-must-be-seen rule a
// duplicate that never draws would hold the screen down permanently.
constexpr uint64_t kMapMarkers[] = {
    0x706EA8364F9B532Eull,
    0xBC0C12146C44EE5Aull,
    0xE541E77E9DF0ADCAull,
    0x7214F7C17A621620ull,
    0x5BFB281560215A3Cull,
};
constexpr int kMapMarkerCount = (int)(sizeof(kMapMarkers) / sizeof(kMapMarkers[0]));

constexpr int kMaxMarkers = 256;
struct Marker { uint64_t hash; int screen; };

const char* const kScreenNames[kScreenProbeCount] = { "MENU", "GARAGE", "MAP", "GAMEPLAY" };

// Present-thread state. Rebuilt only when the evidence changes, which is at a
// pass end, a reset, or the first bind of a shader restored from the file.
Marker g_markers[kMaxMarkers];
int    g_markerCount = 0;
int    g_markerTotal[kScreenProbeCount] = {};
std::atomic<bool> g_markersDirty{true};

// Frames of agreement before the answer changes. The per-marker liveness
// already carries ~0.3 s of grace (kRecentFrames), so this only guards against
// a genuine one-frame disagreement during a transition, where two screens are
// briefly drawing at once.
constexpr int kScreenConfirmFrames = 8;

std::atomic<bool> g_gateDirect{false};
std::atomic<int>  g_screenNow{kScreenGarage};
std::atomic<int>  g_markersLive[kScreenProbeCount];
// Whether the current answer was POSITIVELY identified -- a marker list
// satisfied, or the drive camera's own mode field read -- as opposed to being
// the garage fallback, which is only ever "nothing claimed you". Callers that
// switch the render mode must be able to tell those apart; that distinction is
// what took DIBR shift off mid-drive when it was missing.
std::atomic<bool> g_screenPositive{false};

// Present thread only, which is why none of the state above is atomic: it is
// written here and read by the counting loop, all on that thread.
//
// The table itself is fixed; what changes between calls is which entries of
// g_used have been bound yet, since a shader has no hash until the game creates
// it. So this re-links rather than re-derives.
void rebuild_markers()
{
    g_markerCount = kMapMarkerCount;
    for (int s = 0; s < kScreenProbeCount; ++s) g_markerTotal[s] = 0;
    g_markerTotal[kScreenMap] = kMapMarkerCount;
    for (int k = 0; k < kMapMarkerCount; ++k) {
        g_markers[k].hash   = kMapMarkers[k];
        g_markers[k].screen = kScreenMap;
    }

    const int n = g_usedCount.load(std::memory_order_acquire);
    for (int i = 0; i < n && i < kMaxUsed; ++i) {
        g_used[i].markerId = -1;
        const uint64_t h = g_used[i].hash;
        if (!h) continue;               // pre-hook: no identity, cannot be a marker
        for (int k = 0; k < kMapMarkerCount; ++k)
            if (kMapMarkers[k] == h) { g_used[i].markerId = k; break; }
    }
}


void update_direct_screen(const bool markerLive[kMaxMarkers])
{
    int live[kScreenProbeCount] = {};
    for (int i = 0; i < g_markerCount; ++i)
        if (markerLive[i]) ++live[g_markers[i].screen];
    for (int s = 0; s < kScreenProbeCount; ++s)
        g_markersLive[s].store(live[s], std::memory_order_relaxed);

    // A screen is claimed only when its WHOLE list is drawing. Only the map has
    // a list, so there is nothing to arbitrate between -- and a screen with no
    // markers can never be claimed, which is how the garage stays the fallback
    // without needing a special case for it.
    int want = kScreenGarage;
    bool positive = false;
    if (g_markerTotal[kScreenMap] > 0 &&
        live[kScreenMap] == g_markerTotal[kScreenMap]) {
        want = kScreenMap; positive = true;
    }

    // GARAGE IS NEVER CLAIMED HERE. It has no marker list, because it shares
    // its interface with the menus and its world with gameplay, and reading the
    // engine's own DRIVE_CAMERA mode enum was tried and does not work either --
    // SnowRunner's garage does not tick that camera at all, so the field is
    // unreadable exactly where it would be needed. See the note in
    // camera_hook.h.
    //
    // So garage stays the FALLBACK: what you get when nothing claimed you.
    // Callers separate it from gameplay with logic_camera_idle_ms(), which
    // measures the drive camera having stopped -- a direct reading of the same
    // fact, and one that needs nothing discovered first.
    g_screenPositive.store(positive, std::memory_order_relaxed);

    static int agree = 0;
    const int cur = g_screenNow.load(std::memory_order_relaxed);
    if (want == cur) { agree = 0; return; }
    if (++agree < kScreenConfirmFrames) return;
    agree = 0;
    g_screenNow.store(want, std::memory_order_relaxed);
    // Logged whether or not the gate is switched on, so the answer it WOULD
    // give stays visible while running on legacy. The x/y pair is how many of
    // the map's markers are drawing out of how many there are: on the map it
    // has to read all of them, since anything less claims nothing.
    VRLOG("DIRECT SCREEN -> %s%s (map %d/%d)%s", kScreenNames[want],
          g_screenPositive.load(std::memory_order_relaxed) ? "" : " (fallback)",
          live[kScreenMap], g_markerTotal[kScreenMap],
          g_gateDirect.load(std::memory_order_relaxed) ? "" : "  [gate is on legacy]");
}

// --- cull state ----------------------------------------------------------
std::atomic<bool> g_enabled{false};
std::atomic<uint32_t> g_suppressed{0}, g_suppressedLast{0};

CullSearch g_search;   // touched only from the Present thread (settings UI)

// The filter defaults: 6 elements is exactly a quad's worth of indices, so
// "> 6" is the smallest threshold that still excludes every fullscreen pass,
// and 64 draws/frame leaves generous room above the handful a windscreen
// takes while excluding the bulk-geometry shaders.
std::atomic<unsigned> g_minElems{7};
std::atomic<unsigned> g_maxDrawsFrame{64};

// The named shaders, found once by the search and baked in -- see the CullRole
// note in the header. Both default OFF: removing geometry the game drew is not
// something to do to someone who never asked for it.
std::atomic<uint64_t> g_roleHash[kCullRoleCount][kMaxRoleHashes] = {
    { std::atomic<uint64_t>{0x5BB24059AAE20F12ull} },   // kCullWindows
    // Rain, mud and snow on the glass are separate shaders, and more turn up
    // as new weather is seen -- the last two were found 2026-08-18.
    { std::atomic<uint64_t>{0x5B9A9AF3C67E30C6ull},
      std::atomic<uint64_t>{0x86EEF755B3BE854Bull},
      std::atomic<uint64_t>{0xF2A7D0285C830F6Aull} },  // kCullWindowSmudge
    // Confirmed by direct testing to cover the HUD, the pause menu, the map,
    // the garage and the main menu -- i.e. all of it, not just gameplay.
    { std::atomic<uint64_t>{0x3412C7480D335085ull},
      std::atomic<uint64_t>{0x182934D7084AB9D8ull},
      std::atomic<uint64_t>{0x08CAF81F835F85CEull},
      std::atomic<uint64_t>{0xC8C30095AB7B056Dull},
      std::atomic<uint64_t>{0xC901FED400BE074Bull},
      std::atomic<uint64_t>{0x4B3885B96F72F43Full},
      std::atomic<uint64_t>{0xC90D1842A7C52213ull} },  // kCullUi
    { std::atomic<uint64_t>{0x2833C7477B5978A5ull} },   // kCullMirror
    // kCullRigid -- everything that moves with the camera. Mapped by hand
    // 2026-08-20: truck bodies, cab interiors, trailers and loads.
    //
    // Three entries were dropped again 2026-08-23 (F61254DE9334A35F,
    // 0660CCF817D82F52, 26EDBC7192686F63) after the per-hash mute showed they
    // are not camera-rigid -- which is what that toggle exists for.
    {
      std::atomic<uint64_t>{0xE6DB0E5E6F58B024ull}, std::atomic<uint64_t>{0xBDD66BC3D02FDC27ull}, std::atomic<uint64_t>{0x1534DC93BB346F1Dull},
      std::atomic<uint64_t>{0x0FB8F035C5053790ull}, std::atomic<uint64_t>{0xC27D7DC55CF1FD64ull}, std::atomic<uint64_t>{0x02A9C2F49B602680ull},
      std::atomic<uint64_t>{0x1816BD5E3E6396C9ull}, std::atomic<uint64_t>{0xE1AEA3A01FAA0870ull}, std::atomic<uint64_t>{0x3302D76884486983ull},
      std::atomic<uint64_t>{0xA6F669696901DF83ull}, std::atomic<uint64_t>{0x533CD8FEF33FD71Bull}, std::atomic<uint64_t>{0xC25EB9A11CBCAA07ull},
      std::atomic<uint64_t>{0xAE574A258855FE09ull}, std::atomic<uint64_t>{0xC0791E40EF340C1Aull}, std::atomic<uint64_t>{0xBFE3B083F3D13C17ull},
      std::atomic<uint64_t>{0x3E1ACA5FFB821AA8ull}, std::atomic<uint64_t>{0x72EA0DAA9C7CF2F0ull}, std::atomic<uint64_t>{0x6A5712B177C7C4B9ull},
      std::atomic<uint64_t>{0xE9E9ED092F074357ull}, std::atomic<uint64_t>{0x13CA83236C2D6554ull}, std::atomic<uint64_t>{0x77E51B0EAFF6ACCEull},
      std::atomic<uint64_t>{0x439EAF120B961DE0ull}, std::atomic<uint64_t>{0xE1B2BDC69C9AEA91ull}, std::atomic<uint64_t>{0x175B9776B90C06F6ull},
      std::atomic<uint64_t>{0xD2513876A218EF24ull}, std::atomic<uint64_t>{0x9E8A36D84DFCC821ull}, std::atomic<uint64_t>{0x960F127C9D9B91A9ull},
      std::atomic<uint64_t>{0x0307F05139389031ull}, std::atomic<uint64_t>{0x9648E9CCFD38F1FFull}, std::atomic<uint64_t>{0xCFC50754AEDBD177ull},
      std::atomic<uint64_t>{0x2C927396BC7302E5ull}, std::atomic<uint64_t>{0xF85A3FDD7D4E56BEull}, std::atomic<uint64_t>{0x753BFFEEC3E668C5ull},
      std::atomic<uint64_t>{0xF28210030C88F1ABull}, std::atomic<uint64_t>{0x0E49C3580168E555ull}, std::atomic<uint64_t>{0x191E78C9C1F70281ull},
      std::atomic<uint64_t>{0x25100671E4796E53ull},
      // Found 2026-08-22.
      std::atomic<uint64_t>{0xCE6C573FD0DF0047ull}, std::atomic<uint64_t>{0x7906F60E109E3754ull}, std::atomic<uint64_t>{0xB3FF8E703D6ED095ull},
      std::atomic<uint64_t>{0x59227199E16281FBull}, std::atomic<uint64_t>{0x6FDB33A4B053B7E9ull}, std::atomic<uint64_t>{0xF58361D3D9E58250ull},
      std::atomic<uint64_t>{0xA07F3D0CF3E5BC7Aull}, std::atomic<uint64_t>{0x1AA4A12889BC310Eull}, std::atomic<uint64_t>{0x4889AAEC0EBBFA28ull},
      std::atomic<uint64_t>{0x91192E764448F522ull}, std::atomic<uint64_t>{0x91B3F4E2399911E4ull}, std::atomic<uint64_t>{0xD588FF50B32D5FABull},
      std::atomic<uint64_t>{0xD4B017437156ED48ull}, std::atomic<uint64_t>{0x19DF2400316B4A60ull}, std::atomic<uint64_t>{0xD7134DEB98E308E3ull},
      std::atomic<uint64_t>{0x016F559673E00281ull}, std::atomic<uint64_t>{0x4A2E2058CC4A409Bull}, std::atomic<uint64_t>{0xCB01E5F99A85EFCCull},
      std::atomic<uint64_t>{0x67073445ACE0A5C2ull}, std::atomic<uint64_t>{0x320FFA766E024358ull},
      // Found 2026-08-27.
      std::atomic<uint64_t>{0xA7C40B5170D6A30Full}, std::atomic<uint64_t>{0xCF4137497EF07A1Bull}, std::atomic<uint64_t>{0x422181240232A738ull},
      std::atomic<uint64_t>{0x5B40595B1ABDDA73ull}, std::atomic<uint64_t>{0x00902DC70B5CB150ull}, std::atomic<uint64_t>{0x9A6DDB4A1D592793ull},
      std::atomic<uint64_t>{0x24F466B2EBC03458ull}, std::atomic<uint64_t>{0xAA11C50599F72059ull}, std::atomic<uint64_t>{0x9B5B633B013DFF9Cull}
    },
    // kCullWinchMarker -- the winch anchor-point icons, found 2026-08-24.
    { std::atomic<uint64_t>{0xEDF24FDD6650E8FDull},
      std::atomic<uint64_t>{0xB7DE76615CE84794ull},
      std::atomic<uint64_t>{0xA8B2A29B050BD59Cull},
      std::atomic<uint64_t>{0x5DFB8A19059E7928ull},
      std::atomic<uint64_t>{0x12C37259E013EDB0ull} },
};
// HOW MANY OF EACH ROLE SHIP IN THE BUILD, and the only place that number is
// written down. g_roleHashCount starts here and grows as the search adds to an
// extensible role, so the difference between the two is exactly "what this
// install found that the build did not know about" -- which is what the config
// file stores, instead of a copy of the built-in set that would go stale the
// next time these are updated.
constexpr int kRoleBuiltinCount[kCullRoleCount] = { 1, 3, 7, 1, 66, 5 };
std::atomic<int>  g_roleHashCount[kCullRoleCount] = {
    std::atomic<int>{kRoleBuiltinCount[0]}, std::atomic<int>{kRoleBuiltinCount[1]},
    std::atomic<int>{kRoleBuiltinCount[2]}, std::atomic<int>{kRoleBuiltinCount[3]},
    std::atomic<int>{kRoleBuiltinCount[4]}, std::atomic<int>{kRoleBuiltinCount[5]},
};
std::atomic<bool> g_roleOn[kCullRoleCount] = {};

// PER-HASH MUTE, session only and deliberately not persisted.
//
// A role is a list of shaders someone decided belong together, and the rigid
// list in particular is a hand-built map of every vehicle in the game -- so a
// wrong entry there is not hypothetical, it is the expected failure. Muting one
// takes it out of role_of_hash() entirely, which removes it from the 6-DoF
// reprojection's rigid mask AND from the hide-rigid switch in one move, because
// both read the same role. Toggle, look, decide: that is the whole point, and a
// value that survived into the config would turn an experiment into a setting.
std::atomic<bool> g_roleHashMuted[kCullRoleCount][kMaxRoleHashes] = {};

int role_of_hash(uint64_t h)
{
    if (!h) return -1;
    for (int r = 0; r < kCullRoleCount; ++r) {
        const int n = g_roleHashCount[r].load(std::memory_order_acquire);
        for (int i = 0; i < n && i < kMaxRoleHashes; ++i) {
            if (g_roleHash[r][i].load(std::memory_order_relaxed) != h) continue;
            // Muted: the hash keeps its place in the list (so the UI can show
            // it and put it back) but stops answering to the role, which is
            // what every consumer actually reads.
            if (g_roleHashMuted[r][i].load(std::memory_order_relaxed)) return -1;
            return r;
        }
    }
    return -1;
}

void refresh_suppress(int i)
{
    const int r = g_used[i].role;
    const bool byRole = (r >= 0) && g_roleOn[r].load(std::memory_order_relaxed);
    // The master switch gates only the search and the hand-ticked list; a
    // named toggle is a normal setting and answers to nothing but itself.
    const bool byList = g_enabled.load(std::memory_order_relaxed) &&
                        (g_used[i].culled || g_used[i].inSearch);
    g_used[i].suppress.store(byRole || byList, std::memory_order_relaxed);
}

void refresh_all_suppress()
{
    const int n = g_usedCount.load(std::memory_order_acquire);
    for (int i = 0; i < n && i < kMaxUsed; ++i) refresh_suppress(i);
}

// The persisted set, small by nature -- this is "which shaders does this game
// draw the windscreen with", not a general blocklist.
constexpr int kMaxCulledHashes = 32;
std::atomic<uint64_t> g_culledHash[kMaxCulledHashes] = {};
std::atomic<int>      g_culledHashCount{0};

bool hash_is_culled(uint64_t h)
{
    if (!h) return false;
    const int n = g_culledHashCount.load(std::memory_order_acquire);
    for (int i = 0; i < n && i < kMaxCulledHashes; ++i)
        if (g_culledHash[i].load(std::memory_order_relaxed) == h) return true;
    return false;
}

// --- per-thread bound-shader cache --------------------------------------
// One entry per thread rather than per context: a deferred context is
// recorded by exactly one thread at a time and the immediate context lives on
// the render thread, so "the last pixel shader this thread bound" and "the
// pixel shader the next draw on this thread will use" are the same thing.
thread_local int  t_idx  = -1;
thread_local bool t_cull = false;
thread_local bool t_selfRender = false;

std::atomic<int>  g_unknownHashDraws{0};
std::atomic<bool> g_firstCullLogged{false};

void note_ps_bound(ID3D11PixelShader* ps)
{
    if (t_selfRender || !ps) { t_idx = -1; t_cull = false; return; }

    Slot* s = find_slot((void*)ps);
    if (!s) {
        // Created before this hook was installed, so there is no recorded
        // bytecode hash. Still worth listing (it can be culled for the rest
        // of the session), it just cannot be saved to the config file.
        s = claim_slot((void*)ps);
        if (!s) { t_idx = -1; t_cull = false; return; }
        g_unknownHashDraws.fetch_add(1, std::memory_order_relaxed);
    }
    if (s->exempt.load(std::memory_order_relaxed)) { t_idx = -1; t_cull = false; return; }

    int idx = s->usedPlus1.load(std::memory_order_acquire) - 1;
    if (idx < 0) {
        if (g_usedCount.load(std::memory_order_relaxed) >= kMaxUsed) {
            t_idx = -1; t_cull = false; return;
        }
        const int mine = g_usedCount.fetch_add(1, std::memory_order_relaxed);
        if (mine >= kMaxUsed) { t_idx = -1; t_cull = false; return; }
        g_used[mine].ps     = (void*)ps;
        g_used[mine].hash   = s->hash.load(std::memory_order_relaxed);
        g_used[mine].culled = hash_is_culled(g_used[mine].hash);
        g_used[mine].role   = role_of_hash(g_used[mine].hash);
        // A shader only gets its hash here, and the marker table is linked by
        // hash -- so a newly bound shader may be one of the map's markers and
        // the links have to be redone before the next decision.
        g_markersDirty.store(true, std::memory_order_relaxed);
        refresh_suppress(mine);
        // Published last: another thread that reaches this slot before the
        // store simply reads -1 and treats the shader as unregistered for one
        // bind, which costs nothing.
        s->usedPlus1.store(mine + 1, std::memory_order_release);
        idx = mine;
    }

    t_idx  = idx;
    t_cull = g_used[idx].suppress.load(std::memory_order_relaxed);
}

// --- detours -------------------------------------------------------------

HRESULT STDMETHODCALLTYPE Detour_CreatePixelShader(
    ID3D11Device* dev, const void* bytecode, SIZE_T len,
    ID3D11ClassLinkage* linkage, ID3D11PixelShader** out)
{
    const HRESULT hr = real_CreatePixelShader(dev, bytecode, len, linkage, out);
    if (SUCCEEDED(hr) && out && *out) {
        if (Slot* s = claim_slot((void*)*out)) {
            s->hash.store(dxbc_hash(bytecode, len), std::memory_order_relaxed);
            // Every shader the mod compiles for itself is built lazily from
            // inside the Present hook, so this is a reliable "ours, hands off"
            // test -- including ImGui's, which is created by backend code we
            // do not otherwise touch.
            if (t_selfRender) s->exempt.store(true, std::memory_order_relaxed);
        }
    }
    return hr;
}

void STDMETHODCALLTYPE Detour_PSSetShader(
    ID3D11DeviceContext* ctx, ID3D11PixelShader* ps,
    ID3D11ClassInstance* const* insts, UINT numInsts)
{
    note_ps_bound(ps);
    (is_imm(ctx) ? real_PSSetShader_imm : real_PSSetShader_def)(ctx, ps, insts, numInsts);
}

} // namespace

// --- hot path ------------------------------------------------------------

// Reads the same thread-local the cull decision uses, so it costs nothing on
// top of the PSSetShader bookkeeping already being done. -1 for our own
// rendering (self-render clears the index) and for anything unregistered.
int current_draw_role()
{
    return (t_idx >= 0) ? g_used[t_idx].role : -1;
}

uint64_t current_draw_hash()
{
    return (t_idx >= 0) ? g_used[t_idx].hash : 0ull;
}

bool cull_current_draw(unsigned elemCount)
{
    if (t_idx < 0) return false;

    g_used[t_idx].draws.fetch_add(1, std::memory_order_relaxed);
    // Plain load/store rather than a CAS loop: a lost race costs one sample of
    // a figure that only feeds a filter and a table column.
    if (elemCount > g_used[t_idx].maxElems.load(std::memory_order_relaxed))
        g_used[t_idx].maxElems.store(elemCount, std::memory_order_relaxed);

    if (!t_cull) return false;
    g_suppressed.fetch_add(1, std::memory_order_relaxed);
    if (!g_firstCullLogged.exchange(true))
        VRLOG("SHADER-CULL: first draw suppressed (candidate #%d hash=%016llX, "
              "%u elements) -- the cull reaches live geometry", t_idx,
              (unsigned long long)g_used[t_idx].hash, elemCount);
    return true;
}

void note_ps_state_lost() { t_idx = -1; t_cull = false; }

void set_self_render(bool on)
{
    t_selfRender = on;
    if (on) { t_idx = -1; t_cull = false; }
}

// --- install -------------------------------------------------------------

bool install_shader_cull(IDXGISwapChain* swapchain)
{
    static std::atomic<bool> installed{false};
    bool expected = false;
    if (!installed.compare_exchange_strong(expected, true))
        return true;

    ComPtr<ID3D11Device> dev;
    if (!swapchain || FAILED(swapchain->GetDevice(__uuidof(ID3D11Device), (void**)&dev))) {
        installed = false; return false;
    }
    ComPtr<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(&ctx);
    if (!ctx) { installed = false; return false; }
    ComPtr<ID3D11DeviceContext> defProbe;
    dev->CreateDeferredContext(0, &defProbe);

    void** vtDev = *reinterpret_cast<void***>(dev.Get());
    void** vtImm = *reinterpret_cast<void***>(ctx.Get());
    void** vtDef = defProbe ? *reinterpret_cast<void***>(defProbe.Get()) : nullptr;

    // Same idempotent wrapper as ui_hook.cpp: ALREADY_CREATED/ENABLED mean a
    // previous call installed this exact address successfully, which is a
    // working hook, not a failure to retry.
    auto hookOn = [](void** vtbl, int idx, void* detour, void** orig) -> bool {
        MH_STATUS c = MH_CreateHook(vtbl[idx], detour, orig);
        if (c != MH_OK && c != MH_ERROR_ALREADY_CREATED) return false;
        MH_STATUS e = MH_EnableHook(vtbl[idx]);
        return e == MH_OK || e == MH_ERROR_ENABLED;
    };

    bool ok = hookOn(vtDev, kIdxCreatePixelShader,
                     reinterpret_cast<void*>(&Detour_CreatePixelShader),
                     reinterpret_cast<void**>(&real_CreatePixelShader));
    ok = hookOn(vtImm, kIdxPSSetShader, reinterpret_cast<void*>(&Detour_PSSetShader),
               reinterpret_cast<void**>(&real_PSSetShader_imm)) && ok;
    if (vtDef)
        ok = hookOn(vtDef, kIdxPSSetShader, reinterpret_cast<void*>(&Detour_PSSetShader),
                   reinterpret_cast<void**>(&real_PSSetShader_def)) && ok;

    VRLOG("Shader cull %s -- CreatePixelShader + PSSetShader (imm%s) hooked",
          ok ? "installed" : "FAILED", vtDef ? "+def" : ", NO deferred");
    if (!ok) installed = false;
    return ok;
}

void shader_cull_on_present()
{
    if (g_markersDirty.exchange(false, std::memory_order_relaxed)) rebuild_markers();

    bool markerLive[kMaxMarkers] = {};
    const int n = g_usedCount.load(std::memory_order_acquire);
    for (int i = 0; i < n && i < kMaxUsed; ++i) {
        g_used[i].drawsLast = g_used[i].draws.exchange(0, std::memory_order_relaxed);
        if (g_used[i].drawsLast) {
            g_used[i].recent.store(kRecentFrames, std::memory_order_relaxed);
        } else {
            const uint8_t r = g_used[i].recent.load(std::memory_order_relaxed);
            if (r) g_used[i].recent.store((uint8_t)(r - 1), std::memory_order_relaxed);
        }
        // By marker index, not by table entry -- two shader objects sharing one
        // bytecode are one marker, and under an all-must-be-seen rule a
        // duplicate that never draws would hold its screen down for good.
        const int mk = g_used[i].markerId;
        if (mk >= 0 && g_used[i].recent.load(std::memory_order_relaxed))
            markerLive[mk] = true;
    }
    update_direct_screen(markerLive);
    g_suppressedLast.store(g_suppressed.exchange(0, std::memory_order_relaxed),
                           std::memory_order_relaxed);

    // One line, once, if the CreatePixelShader hook turned out to be too late
    // to see the shaders that matter -- otherwise the config-file side of this
    // feature would silently never work and the reason would not be obvious.
    static bool warned = false;
    if (!warned && g_unknownHashDraws.load() > 0 && n > 0) {
        warned = true;
        VRLOG("SHADER-CULL: %d shader(s) were already created before the hook "
              "installed -- those can be culled this session but have no hash "
              "to persist", g_unknownHashDraws.load());
    }
}

// --- master switch -------------------------------------------------------

bool shader_cull_enabled() { return g_enabled.load(std::memory_order_relaxed); }

void set_shader_cull_enabled(bool on)
{
    g_enabled.store(on, std::memory_order_relaxed);
    refresh_all_suppress();
    VRLOG("SHADER-CULL: experimental list %s", on ? "enabled" : "disabled");
}

// --- named shaders -------------------------------------------------------

bool cull_role_enabled(CullRole role)
{
    return (role >= 0 && role < kCullRoleCount) &&
           g_roleOn[role].load(std::memory_order_relaxed);
}

void set_cull_role_enabled(CullRole role, bool on)
{
    if (role < 0 || role >= kCullRoleCount) return;
    g_roleOn[role].store(on, std::memory_order_relaxed);
    refresh_all_suppress();
}

int cull_role_hash_count(CullRole role)
{
    if (role < 0 || role >= kCullRoleCount) return 0;
    const int n = g_roleHashCount[role].load(std::memory_order_acquire);
    return (n > kMaxRoleHashes) ? kMaxRoleHashes : n;
}

uint64_t cull_role_hash_at(CullRole role, int index)
{
    if (role < 0 || role >= kCullRoleCount) return 0;
    if (index < 0 || index >= cull_role_hash_count(role)) return 0;
    return g_roleHash[role][index].load(std::memory_order_relaxed);
}

namespace {
// Anything already registered was tagged against the OLD role sets, so every
// tag has to be redone -- otherwise a hash added now would only take effect
// for shaders first seen after it.
void retag_roles()
{
    const int n = cull_shader_count();
    for (int i = 0; i < n; ++i) {
        g_used[i].role = role_of_hash(g_used[i].hash);
        refresh_suppress(i);
    }
}
} // namespace

bool cull_role_hash_muted(CullRole role, int index)
{
    if (role < 0 || role >= kCullRoleCount) return false;
    if (index < 0 || index >= cull_role_hash_count(role)) return false;
    return g_roleHashMuted[role][index].load(std::memory_order_relaxed);
}

void set_cull_role_hash_muted(CullRole role, int index, bool muted)
{
    if (role < 0 || role >= kCullRoleCount) return;
    if (index < 0 || index >= cull_role_hash_count(role)) return;
    g_roleHashMuted[role][index].store(muted, std::memory_order_relaxed);
    // Shaders already seen carry their own copy of the answer, so the change
    // only reaches the draw path once they are re-tagged.
    retag_roles();
}

bool cull_role_hash_seen(CullRole role, int index)
{
    if (role < 0 || role >= kCullRoleCount) return false;
    if (index < 0 || index >= cull_role_hash_count(role)) return false;
    // By HASH, not by role -- a muted entry has no role any more, and "has this
    // one ever been drawn" is exactly the question being asked while deciding
    // whether to mute it.
    const uint64_t h = g_roleHash[role][index].load(std::memory_order_relaxed);
    const int n = cull_shader_count();
    for (int i = 0; i < n && i < kMaxUsed; ++i)
        if (g_used[i].hash == h) return true;
    return false;
}

int cull_role_builtin_count(CullRole role)
{
    if (role < 0 || role >= kCullRoleCount) return 0;
    return kRoleBuiltinCount[role];
}

bool cull_role_extensible(CullRole role)
{
    // Only the RIGID role. The other four name a small, closed set of effects
    // -- the glass, the muck on it, the UI, the mirrors -- and those were found
    // once and are done, so leaving them open to a search was a way to break a
    // working install and nothing else. Rigid is a map of every vehicle in the
    // game and genuinely is not finished, so it stays open.
    return role == kCullRigid;
}

void add_cull_role_hash(CullRole role, uint64_t hash)
{
    if (role < 0 || role >= kCullRoleCount || !hash) return;
    if (!cull_role_extensible(role)) {
        VRLOG("SHADER-CULL: '%s' is a fixed set -- %016llX not added",
              cull_role_key(role), (unsigned long long)hash);
        return;
    }
    if (role_of_hash(hash) == (int)role) return;          // already ours
    const int n = g_roleHashCount[role].load(std::memory_order_acquire);
    if (n >= kMaxRoleHashes) {
        VRLOG("SHADER-CULL: '%s' already holds %d shaders -- %016llX not added",
              cull_role_key(role), kMaxRoleHashes, (unsigned long long)hash);
        return;
    }
    g_roleHash[role][n].store(hash, std::memory_order_relaxed);
    g_roleHashCount[role].store(n + 1, std::memory_order_release);
    retag_roles();
    VRLOG("SHADER-CULL: '%s' now covers %d shader(s) (added %016llX)",
          cull_role_key(role), n + 1, (unsigned long long)hash);
}

int cull_role_seen_count(CullRole role)
{
    if (role < 0 || role >= kCullRoleCount) return 0;
    // Counted from what has actually been bound rather than tracked with a
    // flag, so it stays honest when the hash set changes mid-session.
    const int n = cull_shader_count();
    int seen = 0;
    for (int i = 0; i < n; ++i) if (g_used[i].role == (int)role) ++seen;
    return seen;
}

const char* screen_name(int screen)
{
    return (screen >= 0 && screen < kScreenProbeCount) ? kScreenNames[screen] : "?";
}

bool screen_gate_direct() { return g_gateDirect.load(std::memory_order_relaxed); }

void set_screen_gate_direct(bool on)
{
    if (g_gateDirect.exchange(on, std::memory_order_relaxed) == on) return;
    VRLOG("SCREEN GATE -> %s", on ? "direct markers" : "legacy (camera pose)");
}

int direct_screen() { return g_screenNow.load(std::memory_order_relaxed); }

bool direct_screen_positive() { return g_screenPositive.load(std::memory_order_relaxed); }

const char* cull_role_key(CullRole role)
{
    switch (role) {
        case kCullWindows:      return "windows";
        case kCullWindowSmudge: return "window smudge";
        case kCullUi:           return "ui";
        case kCullMirror:       return "mirror";
        case kCullRigid:        return "rigid (moves with camera)";
        case kCullWinchMarker:  return "winch markers";
        default:                return "?";
    }
}

// --- browsing ------------------------------------------------------------

// Clamped: the counter is bumped by a racing thread before the capacity check
// can reject it, so it can briefly read past the array.
int cull_shader_count()
{
    const int n = g_usedCount.load(std::memory_order_acquire);
    return (n > kMaxUsed) ? kMaxUsed : n;
}

namespace {
// A shader is worth showing the user only if it could plausibly BE the
// windscreen. See set_search_filter's note in the header for why these two
// tests specifically.
bool passes_filter(int i)
{
    const uint32_t elems = g_used[i].maxElems.load(std::memory_order_relaxed);
    if (elems == 0) return false;                       // never actually drew
    if (elems < g_minElems.load(std::memory_order_relaxed)) return false;
    const uint32_t draws = g_used[i].drawsLast;
    return draws > 0 && draws <= g_maxDrawsFrame.load(std::memory_order_relaxed);
}
} // namespace

bool cull_shader_info(int index, CullShaderInfo& out)
{
    const int n = cull_shader_count();
    if (index < 0 || index >= n || index >= kMaxUsed) return false;
    out.ps            = g_used[index].ps;
    out.hash          = g_used[index].hash;
    out.drawsPerFrame = g_used[index].drawsLast;
    out.maxElems      = g_used[index].maxElems.load(std::memory_order_relaxed);
    out.culled        = g_used[index].culled;
    out.inSearchRange = g_used[index].inSearch;
    out.isCandidate   = passes_filter(index);
    return true;
}

void set_search_filter(unsigned minElems, unsigned maxDrawsFrame)
{
    g_minElems.store(minElems, std::memory_order_relaxed);
    g_maxDrawsFrame.store(maxDrawsFrame, std::memory_order_relaxed);
}

unsigned search_min_elems()       { return g_minElems.load(std::memory_order_relaxed); }
unsigned search_max_draws_frame() { return g_maxDrawsFrame.load(std::memory_order_relaxed); }

int search_candidate_count()
{
    const int n = cull_shader_count();
    int c = 0;
    for (int i = 0; i < n; ++i) if (passes_filter(i)) ++c;
    return c;
}

uint32_t suppressed_draws_last_frame() { return g_suppressedLast.load(std::memory_order_relaxed); }

void set_cull_shader_culled(int index, bool culled)
{
    const int n = cull_shader_count();
    if (index < 0 || index >= n || index >= kMaxUsed) return;
    g_used[index].culled = culled;
    refresh_suppress(index);

    const uint64_t h = g_used[index].hash;
    if (!h) return;                     // session-only; nothing to persist

    // Unticking a row whose shader is one of the named ones has to turn the
    // NAMED setting off, or the row would stay suppressed and the tick would
    // look broken. add_culled_hash() handles the other direction.
    const int r = role_of_hash(h);
    if (r >= 0) { set_cull_role_enabled((CullRole)r, culled); return; }
    if (culled) {
        add_culled_hash(h);
    } else {
        // Rebuild without it: the list is at most 32 entries and only changes
        // when a box is clicked, so a copy is cheaper than any bookkeeping.
        uint64_t keep[kMaxCulledHashes];
        int kept = 0;
        const int c = g_culledHashCount.load(std::memory_order_acquire);
        for (int i = 0; i < c && i < kMaxCulledHashes; ++i) {
            const uint64_t v = g_culledHash[i].load(std::memory_order_relaxed);
            if (v != h) keep[kept++] = v;
        }
        clear_culled_hashes();
        for (int i = 0; i < kept; ++i) add_culled_hash(keep[i]);
    }
}

// --- bisect search -------------------------------------------------------

namespace {
// The candidate list, snapshotted at cull_search_begin(). Holding indices
// into g_used rather than a contiguous range is what lets the filter exclude
// fullscreen passes from the middle of the list without the search having to
// know they exist.
int g_cand[kMaxUsed];
int g_candCount = 0;

// Applies whatever the current range implies. Once it is down to a single
// candidate that one stays suppressed, so the result can be eyeballed before
// it is saved.
void apply_search_step()
{
    for (int i = 0; i < g_candCount; ++i) {
        const bool on = (i >= g_search.lo) &&
                        (i < ((g_search.hi - g_search.lo <= 1)
                                  ? g_search.lo + 1
                                  : g_search.lo + (g_search.hi - g_search.lo) / 2));
        g_used[g_cand[i]].inSearch = on;
        refresh_suppress(g_cand[i]);
    }
    g_search.testCount = (g_search.hi - g_search.lo <= 1)
                             ? 1 : (g_search.hi - g_search.lo) / 2;

    if (g_search.hi - g_search.lo <= 1) {
        g_search.found      = true;
        g_search.foundIndex = (g_search.lo < g_candCount) ? g_cand[g_search.lo] : -1;
        g_search.foundHash  = (g_search.foundIndex >= 0)
                                  ? g_used[g_search.foundIndex].hash : 0;
    }
}

void clear_search_flags()
{
    for (int i = 0; i < g_candCount; ++i) {
        g_used[g_cand[i]].inSearch = false;
        refresh_suppress(g_cand[i]);
    }
    g_candCount = 0;
}
} // namespace

CullSearch cull_search_state() { return g_search; }

void cull_search_begin()
{
    clear_search_flags();
    const int n = cull_shader_count();
    for (int i = 0; i < n && g_candCount < kMaxUsed; ++i)
        if (passes_filter(i)) g_cand[g_candCount++] = i;

    if (g_candCount <= 0) {
        VRLOG("SHADER-CULL: no candidates pass the filter (minElems=%u, "
              "maxDraws/frame=%u) -- widen it or get into the cockpit first",
              search_min_elems(), search_max_draws_frame());
        return;
    }
    g_search = CullSearch{};
    g_search.active = true;
    g_search.total  = g_candCount;   // frozen: later arrivals would renumber it
    g_search.lo = 0;
    g_search.hi = g_candCount;
    apply_search_step();
    VRLOG("SHADER-CULL: search started over %d candidates (of %d shaders seen)",
          g_candCount, n);
}

void cull_search_answer(bool windscreenGone)
{
    if (!g_search.active || g_search.found) return;
    const int mid = g_search.lo + (g_search.hi - g_search.lo) / 2;
    if (windscreenGone) g_search.hi = mid; else g_search.lo = mid;
    ++g_search.step;
    apply_search_step();
    if (g_search.found)
        VRLOG("SHADER-CULL: search converged after %d answers -- shader #%d "
              "hash=%016llX", g_search.step, g_search.foundIndex,
              (unsigned long long)g_search.foundHash);
}

void cull_search_cancel()
{
    clear_search_flags();
    g_search = CullSearch{};
}

// --- persisted set -------------------------------------------------------

int culled_hash_count() { return g_culledHashCount.load(std::memory_order_acquire); }

uint64_t culled_hash_at(int index)
{
    const int n = culled_hash_count();
    if (index < 0 || index >= n || index >= kMaxCulledHashes) return 0;
    return g_culledHash[index].load(std::memory_order_relaxed);
}

void add_culled_hash(uint64_t hash)
{
    if (!hash) return;

    // A hash that a role already names is absorbed BY that role rather than
    // added to the anonymous list. That keeps one shader from being suppressed
    // by two independent mechanisms, where turning the named checkbox off
    // would appear to do nothing. It also migrates a config written by the
    // search before these shaders had names -- which is how the two of them
    // were found in the first place.
    const int r = role_of_hash(hash);
    if (r >= 0) {
        if (!g_roleOn[r].load(std::memory_order_relaxed)) {
            g_roleOn[r].store(true, std::memory_order_relaxed);
            refresh_all_suppress();
            VRLOG("SHADER-CULL: hash %016llX belongs to '%s' -- enabling that "
                  "setting instead of listing it",
                  (unsigned long long)hash, cull_role_key((CullRole)r));
        }
        return;
    }

    if (hash_is_culled(hash)) return;
    const int n = g_culledHashCount.load(std::memory_order_acquire);
    if (n >= kMaxCulledHashes) return;
    g_culledHash[n].store(hash, std::memory_order_relaxed);
    g_culledHashCount.store(n + 1, std::memory_order_release);

    // Anything already in the candidate list keeps its own copy of the
    // decision, so it has to be told.
    const int used = cull_shader_count();
    for (int i = 0; i < used && i < kMaxUsed; ++i)
        if (g_used[i].hash == hash) { g_used[i].culled = true; refresh_suppress(i); }
}

void clear_culled_hashes() { g_culledHashCount.store(0, std::memory_order_release); }

} // namespace hooks
