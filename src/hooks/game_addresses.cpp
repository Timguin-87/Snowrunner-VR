#include "hooks/game_addresses.h"
#include "hooks/pattern_scan.h"
#include "common/log.h"

#include <windows.h>
#include <cstdio>
#include <mutex>
#include <vector>

namespace hooks {
namespace {

// --- signatures -------------------------------------------------------------
//
// Written from build 0x6AB52F5D (the 2026-10-21 update) and checked against a
// dump of it: each matches exactly once. Wildcarded, because each moves on its
// own whenever the code around it changes:
//   - call targets and RIP-relative displacements: move with every relink;
//   - STACK OFFSETS: the frame size in sub rsp, and every [rsp+x], [rax+x] or
//     [r11+x] that is a local, a saved XMM register or a nonvolatile parked in
//     a home slot. The frame is laid out per function, so one local more or
//     less anywhere in it shifts all of these while the code stays the same.
//     (An argument spilled to ITS OWN home slot is fixed by the calling
//     convention and stays in.)
// What is left is the instruction stream itself, and for these functions that
// has not changed in any build the mod has run on -- the 2026-10-21 update moved
// every one of them and edited none. Checked against more than one build where
// one exists: kSigMainCamera also matches a fragment of the August build, and
// kSigCameraCommit is the July pattern (docs/recon.md) with its stack offset
// wildcarded. tools/sigcheck.py takes any number of dumps; keep one per build.
//
// Struct offsets are deliberately LEFT IN where the mod depends on them. If
// combine::CAMERA's FOV ever moves off +0x108, the set-perspective signature
// should fail and say so, rather than match and leave camera_fov.cpp writing
// into whatever now lives there.

// combine::DRIVE_CAMERA's per-tick commit (rcx = the camera). Ends on
// lea rsi,[rcx+0x184]: the staged eye camera_hook reads.
constexpr const char* kSigCameraCommit =
    "48 8B C4 F3 0F 11 50 18 48 89 50 10 55 53 56 57 41 55 41 56 "
    "48 8D A8 ?? ?? FF FF 48 81 EC ?? ?? 00 00 0F 29 70 ?? 48 8D B1 84 01 00 00";

// The main render camera's build (docs/cockpit_camera.md): the function that
// calls the view builder, combine #1 and combine #2, in that order. Not hooked
// itself; it is where the three main-camera call sites are looked for.
constexpr const char* kSigMainCamera =
    "48 8B C4 48 89 58 ?? 48 89 78 ?? 55 48 8D A8 ?? ?? FF FF 48 81 EC ?? ?? 00 00 "
    "48 8B D9 48 8B FA 48 8B 89 38 01 00 00 48 85 C9 0F 84 ?? ?? ?? ?? "
    "0F 10 83 BC 01 00 00 48 81 C1 70 01 00 00";

// The view builder: a 4x4 transpose (rcx = out, rdx = in).
constexpr const char* kSigViewBuilder =
    "48 83 EC ?? 0F 10 02 4C 8D 41 08 0F 10 4A 10 0F 29 04 24 0F 10 42 20 "
    "0F 29 4C 24 ?? 0F 10 4A 30 33 D2";

// combine #1 (out = rcx, position = rdx float3, orientation = r8).
constexpr const char* kSigCombine1 =
    "48 83 EC ?? F3 0F 10 62 04 48 8B C1 F3 0F 10 6A 08 0F 29 74 24 ?? "
    "0F 28 CD F3 0F 10 32 F3 41 0F 59 48 20";

// combine #2, the final cockpit view (out = rcx, r8 = orientation).
constexpr const char* kSigCombine2 =
    "48 83 EC ?? F2 0F 10 02 8B 42 08 0F 28 E8 F2 0F 11 04 24 F3 0F 10 24 24 "
    "0F C6 ED 55 0F 28 C4 F3 41 0F 59 00";

// buildPerspective(rcx = out, xmm1 = fovY, xmm2 = aspect, xmm3 = near,
// far on the stack). Long on purpose: it runs through the fovY * 0.5, the tanf
// call and the 1.0f stored at +0x2C that makes it a D3D row-vector perspective
// -- everything detour_projbuild relies on.
constexpr const char* kSigProjBuild =
    "48 89 5C 24 ?? 57 48 83 EC ?? F3 0F 59 0D ?? ?? ?? ?? 33 FF 48 8B D9 "
    "48 89 79 30 0F 29 74 24 ?? 0F 28 F2 0F 29 7C 24 ?? 0F 28 FB 0F 28 C1 "
    "48 89 79 20 48 89 79 18 48 89 79 0C 48 89 79 04 E8 ?? ?? ?? ?? "
    "F3 0F 10 1D ?? ?? ?? ?? 0F 28 D0 F3 0F 59 D6 0F 28 CB 48 8B C3 "
    "C7 43 2C 00 00 80 3F";

// combine::CAMERA set-perspective: stores m_fFOV (+0x108) and the aspect
// (+0x10C), then builds the projection. Its one call to buildPerspective is
// the main camera's -- the PROJ LOCK site.
constexpr const char* kSigSetPerspective =
    "48 89 5C 24 ?? 57 48 81 EC ?? ?? 00 00 0F 57 C0 48 8B F9 0F 2F C8 76 08 "
    "F3 0F 11 89 08 01 00 00 0F 2F D0 76 08 F3 0F 11 91 0C 01 00 00";

// fillOrbitView(rcx = view, rdx = eye, r8 = target, r9 = up). Opens on
// target - eye.
constexpr const char* kSigFnA =
    "48 8B C4 48 89 58 ?? 48 89 68 ?? 48 89 70 ?? 57 48 83 EC ?? "
    "F3 41 0F 10 00 48 8B EA F3 0F 5C 02 F3 41 0F 10 48 04 48 8B F9 F3 0F 5C 4A 04";

// The main-menu camera, whose one call to fillOrbitView is the menu view.
constexpr const char* kSigMenuCamera =
    "4C 8B DC 49 89 7B ?? 55 49 8D AB ?? ?? FF FF 48 81 EC ?? ?? 00 00 "
    "83 3D ?? ?? ?? ?? 00 48 8B F9 0F 85 ?? ?? ?? ?? 48 8B 41 28 49 89 5B ??";
// The same call found from its own site instead, for when the function's
// prologue changes and its body does not. The E8 is at +6. With its stack
// offset wildcarded this also matches the set-perspective call a few lines
// further on, so the site is the one match whose E8 calls fillOrbitView.
constexpr const char* kSigMenuCameraCall =
    "F3 0F 11 4C 24 ?? E8 ?? ?? ?? ?? 48 8D 4F 28 E8";
constexpr int kMenuCameraCallE8 = 6;

// mov rax,[settings] / movss xmm1,[rax+0x64] / movss xmm2,[rax+0x60]: the
// game reading its own FOV pair, inside the DRIVE_CAMERA commit.
constexpr const char* kSigSettingsRead =
    "48 8B 05 ?? ?? ?? ?? F3 0F 10 48 64 F3 0F 10 50 60";

// --- resolution ---------------------------------------------------------------

HMODULE  g_exe  = nullptr;
uint8_t* g_base = nullptr;
GameAddresses g_addr;
int g_found = 0, g_wanted = 0;

uintptr_t rva_of(const void* p)
{
    return p ? static_cast<uintptr_t>(static_cast<const uint8_t*>(p) - g_base) : 0;
}

// The entry of the function containing `p`, as an offset. 0 if unknown.
uintptr_t owner_of(const void* p)
{
    uint8_t* b = nullptr;
    uint8_t* e = nullptr;
    return function_range(p, b, e) ? rva_of(b) : 0;
}

// One summary line per address the mod actually uses. Callers and other
// intermediate steps log only when they fail, so a healthy log stays short.
uintptr_t report(const char* name, uintptr_t rva, const char* how)
{
    ++g_wanted;
    if (rva) {
        ++g_found;
        VRLOG("  %-22s exe+0x%llX  (%s)", name, (unsigned long long)rva, how);
    }
    return rva;
}

// A function by its signature: exactly one match, and that match a function
// entry. Two matches is a failure, not "take the first": it means the
// signature no longer says which one it meant.
uintptr_t by_signature(const char* name, const char* sig)
{
    uint8_t* hit = nullptr;
    const int n = pattern_scan_all(g_exe, sig, &hit, 1);
    if (n != 1) {
        VRLOG("  %-22s NOT FOUND -- signature matches %d place(s)%s", name, n,
              n > 1 ? "; ambiguous, so none is used" : "");
        return 0;
    }
    if (!is_function_entry(hit)) {
        VRLOG("  %-22s REJECTED -- signature matched exe+0x%llX, which is not "
              "a function entry", name, (unsigned long long)rva_of(hit));
        return 0;
    }
    return rva_of(hit);
}

// Every call from the function entered at `caller` to the one entered at
// `callee`, chained fragments of the caller included.
std::vector<uint8_t*> calls_from(uintptr_t caller, uintptr_t callee)
{
    std::vector<uint8_t*> out;
    if (!caller || !callee) return out;
    for (uint8_t* site : find_calls_to(g_exe, g_base + callee))
        if (owner_of(site) == caller)
            out.push_back(site);
    return out;
}

// The return address of the ONE call from `caller` to `callee`.
uintptr_t unique_call(const char* name, uintptr_t caller, uintptr_t callee)
{
    if (!caller || !callee) {
        VRLOG("  %-22s NOT FOUND -- needs both the caller and the callee", name);
        return 0;
    }
    const std::vector<uint8_t*> c = calls_from(caller, callee);
    if (c.size() != 1) {
        VRLOG("  %-22s NOT FOUND -- exe+0x%llX calls exe+0x%llX %zu time(s), "
              "expected exactly once", name, (unsigned long long)caller,
              (unsigned long long)callee, c.size());
        return 0;
    }
    return rva_of(c[0] + 5);
}

// In the exe's own writable data: where a global has to be.
bool exe_data(const void* p)
{
    MEMORY_BASIC_INFORMATION mbi{};
    return VirtualQuery(p, &mbi, sizeof(mbi)) && mbi.AllocationBase == g_exe &&
           (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY));
}

// Every global the game stores GetProcAddress(`name`) in. Found from the name
// string, which a relink cannot change: lea rdx,[name], then a call through the
// exe's own GetProcAddress import slot, then mov [global],rax, each within a
// few instructions of the last. Build 0x6AB52F5D, for the record:
//
//   lea  rdx,["XInputGetState"]      lea  rdx,["XInputGetState"]
//   mov  rcx,rax                     mov  rcx,[rsi+1A8h]
//   call [GetProcAddress]            call [GetProcAddress]
//   lea  rdx,["XInputGet...ities"]   mov  [global],rax
//   mov  rcx,rdi
//   mov  [global],rax
//
// The windows between the steps are searched byte-wise, not disassembled, so
// each step is held to exactly what it has to be -- the call through that one
// import slot, the store into the exe's writable data, 8-aligned -- and a step
// that meets the NEXT one's marker first (rdx reloaded before the call, another
// import call before the store) drops the site rather than take what belongs
// to someone else. `sites` counts the lookups found, kept or not.
int procaddress_globals(const char* name, uintptr_t* out, int cap, int& sites)
{
    sites = 0;
    void** gpa = import_slot(g_exe, nullptr, "GetProcAddress");
    if (!gpa) return 0;
    constexpr int kWindow = 32;
    static const uint8_t kLeaRdx[] = {0x48, 0x8D, 0x15};
    int n = 0;
    for (uint8_t* str : find_cstrings(g_exe, name)) {
        for (uint8_t* lea : find_rip_refs(g_exe, kLeaRdx, 3, str)) {
            ++sites;
            uint8_t* call = nullptr;
            for (uint8_t* p = lea + 7; p < lea + 7 + kWindow; ++p) {
                if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x15) break;
                if (p[0] == 0xFF && p[1] == 0x15) {
                    if (rel32_target(p, 2, 6) == reinterpret_cast<uint8_t*>(gpa)) call = p;
                    break;
                }
            }
            if (!call) continue;
            uint8_t* glob = nullptr;
            for (uint8_t* p = call + 6; p < call + 6 + kWindow; ++p) {
                if (p[0] == 0xFF && p[1] == 0x15) break;
                if (p[0] == 0x48 && p[1] == 0x89 && p[2] == 0x05) {
                    glob = rel32_target(p, 3, 7);
                    break;
                }
            }
            if (!glob || (reinterpret_cast<uintptr_t>(glob) & 7) || !exe_data(glob)) continue;
            const uintptr_t r = rva_of(glob);
            bool dup = false;
            for (int i = 0; i < n; ++i) dup = dup || out[i] == r;
            if (!dup && n < cap) out[n++] = r;
        }
    }
    return n;
}

// One summary line for a set of globals, counted as one address.
void report_globals(const char* label, const char* name, uintptr_t* out, int cap)
{
    int sites = 0;
    const int n = procaddress_globals(name, out, cap, sites);
    ++g_wanted;
    if (n == 0) {
        VRLOG("  %-22s NOT FOUND -- %d lookup(s) of \"%s\" in the code, none storing "
              "to a global", label, sites, name);
        return;
    }
    ++g_found;
    char list[128] = {};
    int len = 0;
    for (int i = 0; i < n && len < (int)sizeof(list); ++i)
        len += snprintf(list + len, sizeof(list) - len, "%sexe+0x%llX", i ? ", " : "",
                        (unsigned long long)out[i]);
    VRLOG("  %-22s %s  (GetProcAddress(\"%s\"), %d of %d lookup(s))", label, list, name,
          n, sites);
}

// The main camera build found WITHOUT its signature: the one function that
// calls the view builder, combine #1 and combine #2 exactly once each, in that
// order. In build 0x6AB52F5D, 22 functions call both combines and 4 call all
// three; exactly one calls each once, in order.
uintptr_t main_camera_by_calls(uintptr_t vb, uintptr_t c1, uintptr_t c2)
{
    if (!vb || !c1 || !c2) return 0;
    struct Site { uintptr_t fn; uint8_t* at; };
    auto sites = [](uintptr_t callee) {
        std::vector<Site> s;
        for (uint8_t* p : find_calls_to(g_exe, g_base + callee))
            s.push_back({owner_of(p), p});
        return s;
    };
    const std::vector<Site> sv = sites(vb), s1 = sites(c1), s2 = sites(c2);
    auto only = [](const std::vector<Site>& s, uintptr_t fn) -> uint8_t* {
        uint8_t* at = nullptr;
        int n = 0;
        for (const Site& x : s)
            if (x.fn == fn) { at = x.at; ++n; }
        return n == 1 ? at : nullptr;
    };
    uintptr_t found = 0;
    int candidates = 0;
    for (const Site& x : s2) {
        if (!x.fn || x.fn == found) continue;
        uint8_t* a2 = only(s2, x.fn);
        uint8_t* a1 = only(s1, x.fn);
        uint8_t* av = only(sv, x.fn);
        if (a2 && a1 && av && av < a1 && a1 < a2) {
            found = x.fn;
            ++candidates;
        }
    }
    return candidates == 1 ? found : 0;
}

void resolve()
{
    g_exe  = GetModuleHandleW(nullptr);
    g_base = reinterpret_cast<uint8_t*>(g_exe);
    GameAddresses& a = g_addr;
    VRLOG("game addresses for build 0x%08X -- each found by signature, by what "
          "calls it or by the name it looks up, never by a remembered offset:",
          exe_timestamp());

    a.cameraCommit = report("camera commit", by_signature("camera commit", kSigCameraCommit), "signature");
    a.viewBuilder  = report("view builder", by_signature("view builder", kSigViewBuilder), "signature");
    a.combine1     = report("combine1", by_signature("combine1", kSigCombine1), "signature");
    a.combine2     = report("combine2", by_signature("combine2", kSigCombine2), "signature");
    a.projBuild    = report("projection builder", by_signature("projection builder", kSigProjBuild), "signature");
    a.fna          = report("fna (orbit view)", by_signature("fna (orbit view)", kSigFnA), "signature");

    // The main camera build, two independent ways. Both answers must agree
    // when both exist; either alone is enough.
    {
        const uintptr_t bySig   = by_signature("main camera build", kSigMainCamera);
        const uintptr_t byCalls = main_camera_by_calls(a.viewBuilder, a.combine1, a.combine2);
        uintptr_t mainCam = 0;
        if (bySig && byCalls && bySig != byCalls)
            VRLOG("  main camera build      DISAGREE -- signature says exe+0x%llX, the call "
                  "pattern exe+0x%llX; neither is used", (unsigned long long)bySig,
                  (unsigned long long)byCalls);
        else
            mainCam = bySig ? bySig : byCalls;
        a.viewBuilderRet = report("view builder <- main", unique_call("view builder <- main", mainCam, a.viewBuilder),
                                  bySig && byCalls ? "call in main camera build, located 2 ways"
                                                   : "call in main camera build");
        a.combine1Ret    = report("combine1 <- main", unique_call("combine1 <- main", mainCam, a.combine1),
                                  "call in main camera build");
        a.combine2Ret    = report("combine2 <- main", unique_call("combine2 <- main", mainCam, a.combine2),
                                  "call in main camera build");
    }

    a.orbitGameRet = report("fna <- in-game orbit", unique_call("fna <- in-game orbit", a.cameraCommit, a.fna),
                            "call in camera commit");

    {
        const uintptr_t menuCam = by_signature("main-menu camera", kSigMenuCamera);
        uintptr_t ret = menuCam ? unique_call("fna <- main menu", menuCam, a.fna) : 0;
        const char* how = "call in main-menu camera";
        if (!ret && a.fna) {
            // Fallback: the call site's own signature. Still exactly one: of
            // its matches, exactly one may call fillOrbitView.
            constexpr int kCap = 8;
            uint8_t* hits[kCap] = {};
            const int n = pattern_scan_all(g_exe, kSigMenuCameraCall, hits, kCap);
            uint8_t* site = nullptr;
            int calling = 0;
            for (int i = 0; i < n && i < kCap; ++i)
                if (rva_of(rel32_target(hits[i] + kMenuCameraCallE8, 1, 5)) == a.fna) {
                    site = hits[i];
                    ++calling;
                }
            if (n <= kCap && calling == 1) {
                ret = rva_of(site + kMenuCameraCallE8 + 5);
                how = "call-site signature";
            }
        }
        a.orbitMenuRet = report("fna <- main menu", ret, how);
        if (!ret) VRLOG("  %-22s NOT FOUND", "fna <- main menu");
    }

    {
        const uintptr_t setPersp = by_signature("camera set-perspective", kSigSetPerspective);
        a.projMainRet = report("projection <- main", unique_call("projection <- main", setPersp, a.projBuild),
                               "call in camera set-perspective");
    }

    {
        // A RIP-relative load: the global is the instruction's own target, and
        // it has to land in a writable, non-executable section to be data.
        uintptr_t g = 0;
        uint8_t* hit = nullptr;
        const int n = pattern_scan_all(g_exe, kSigSettingsRead, &hit, 1);
        if (n == 1) {
            uint8_t* tgt = rel32_target(hit, 3, 7);
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(tgt, &mbi, sizeof(mbi)) && mbi.AllocationBase == g_exe &&
                (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)))
                g = rva_of(tgt);
        }
        a.settingsPtr = report("settings global", g, "load in camera commit");
        if (!g)
            VRLOG("  %-22s NOT FOUND -- read signature matches %d place(s)%s",
                  "settings global", n, n == 1 ? ", but not a load from writable data" : "");
    }

    report_globals("XInputGetState ptrs", "XInputGetState", a.xinputGetState,
                   GameAddresses::kMaxXInputPtrs);
    report_globals("XInputGetCaps ptrs", "XInputGetCapabilities", a.xinputGetCaps,
                   GameAddresses::kMaxXInputPtrs);

    if (g_found == g_wanted)
        VRLOG("game addresses: all %d found", g_wanted);
    else
        VRLOG("game addresses: %d of %d found -- whatever needs the missing ones stays "
              "OFF. This game build is newer than the mod's signatures: set "
              "DumpGameCode=true in Snowrunner_VR_config.txt, run once, and update them "
              "from the dump.", g_found, g_wanted);
}

} // namespace

const GameAddresses& game_addresses()
{
    static std::once_flag once;
    std::call_once(once, resolve);
    return g_addr;
}

} // namespace hooks
