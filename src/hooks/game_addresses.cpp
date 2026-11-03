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
// dump of it and of 0x6AB53420, the Microsoft Store (Game Pass) build of the same
// version: each matches exactly once in both, or has a second form that does.
// That build is the same source compiled differently, so nearly everything still
// matches as it is; where the compiler went another way, a signature ending in
// Store is the same code in its shape. Wildcarded, because each moves on its own
// whenever the code around it changes:
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
// The Store build saves more registers and every XMM up front, and keeps the
// staged eye in r14: same opening, same field, nothing shared past the push list.
constexpr const char* kSigCameraCommitStore =
    "48 8B C4 F3 0F 11 50 18 48 89 50 10 55 53 56 57 41 54 41 55 41 56 41 57 "
    "48 8D A8 ?? ?? FF FF 48 81 EC ?? ?? 00 00 48 C7 85 ?? ?? 00 00 FE FF FF FF "
    "0F 29 70 ?? 0F 29 78 ?? 44 0F 29 40 ?? 44 0F 29 88 ?? ?? FF FF "
    "44 0F 29 90 ?? ?? FF FF 44 0F 29 98 ?? ?? FF FF 44 0F 29 A0 ?? ?? FF FF "
    "44 0F 29 A8 ?? ?? FF FF 44 0F 29 B0 ?? ?? FF FF 44 0F 29 B8 ?? ?? FF FF "
    "48 8B D9 4C 8D B1 84 01 00 00";

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

// The small-object cull: bool(rcx = combine::CAMERA, xmm1 = 1/tan(fov/2),
// r8 = AABB min.xyz max.xyz, xmm3 = threshold). Half the box diagonal times
// xmm1 over the centre's VIEW-SPACE depth, times the camera's aspect (+0x10C)
// where that is larger, against the threshold -- 0.0444 at all three call
// sites. Runs through the AABB loads and the two argument moves the detour
// relies on: xmm3 -> xmm10 (threshold), xmm1 -> xmm11 (the FOV scale).
constexpr const char* kSigSizeCull =
    "48 8B C4 53 48 81 EC ?? ?? 00 00 F3 41 0F 10 20 48 8D 54 24 ?? "
    "F3 41 0F 10 50 04 0F 28 C4 0F 29 70 ?? 48 8B D9 F3 41 0F 10 70 0C "
    "F3 0F 58 C6 0F 29 78 ?? F3 41 0F 10 78 10 F3 0F 5C F4 44 0F 29 40 ?? "
    "F3 45 0F 10 40 14 44 0F 29 48 ?? F3 44 0F 10 0D ?? ?? ?? ?? "
    "F3 41 0F 59 C1 44 0F 29 50 ?? 44 0F 28 D3 44 0F 29 58 ?? 44 0F 28 D9";

// Where the truck mirrors' cameras are built: the one call to set-perspective in
// the loop over a truck's mirrors, found from the call itself, ending on its E8.
// Runs through aspect = item[+0x98] / item[+0x9C] -- each mirror's own -- and the
// camera at item+4 going in as `this`. That aspect is why a mirror cannot be told
// from the main view by shape: a square mirror on a square canvas is a match.
constexpr const char* kSigMirrorCameraCall =
    "F3 42 0F 5E 94 36 9C 00 00 00 0F 47 C1 48 8D 0D ?? ?? ?? ?? "
    "48 89 4C 24 ?? 49 8D 4C 24 04 89 44 24 ?? 0F 28 DF 41 0F 28 C9 "
    "F3 0F 11 74 24 ?? E8";
constexpr int kMirrorCameraCallE8 = 47;

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
// The block holding that call, as a function of its own: the Store build did not
// inline it, so there the call sits in here and neither signature above matches.
// Steam has this function too and never calls it, which is why it comes last.
constexpr const char* kSigMenuViewHelper =
    "40 53 48 81 EC ?? ?? 00 00 48 8B D9 48 BA FF FF FF FF FF FF 00 FF "
    "48 81 C1 38 01 00 00 48 8B 01 48 23 C2";

// mov rax,[settings] / movss xmmN,[rax+0x64]: the game reading its own FOV pair,
// in the DRIVE_CAMERA commit and the function after it. Which register, and what
// comes next, differs between builds and between those two, so a match says
// little on its own -- the check is that every match names one global.
constexpr const char* kSigSettingsRead =
    "48 8B 05 ?? ?? ?? ?? F3 0F 10 ?? 64";

// The DXUT-style callback table being filled -- the one place both frame
// callbacks are named, and in this order:
//   lea rax,[OnFrameRender]  mov [reg+18h],rax
//   lea rax,[OnFrameMove]    mov [reg+10h],rax
//   lea rax,[...]            mov [reg+20h],rax
// The table is in rbx on Steam and rdi in the Store build.
// Not a function entry: each lea's own target is, and both are checked.
constexpr const char* kSigCallbackTable =
    "48 8D 05 ?? ?? ?? ?? 48 89 ?? 18 48 8D 05 ?? ?? ?? ?? 48 89 ?? 10 "
    "48 8D 05 ?? ?? ?? ?? 48 89 ?? 20";
constexpr int kCallbackRenderLea = 0;    // lea rax,[OnFrameRender]
constexpr int kCallbackMoveLea   = 11;   // lea rax,[OnFrameMove]

// Inside OnFrameMove: movss xmmN,[1/20] / cmp dword [flag],0 / je /
// maxss xmmN,[1/60] -- the elapsed-time clamp and the flag it hangs on. The
// registers differ between builds. The cmp is at +9.
constexpr const char* kSigElapsedClampFlag =
    "F3 44 0F 10 ?? ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 00 74 ?? F3";
constexpr int kElapsedClampCmp = 9;

// The settings apply dispatching on what changed. Bit 0 (the display) and bit 7
// (the window size) each call the display apply; between them is bit 8's own
// call. One match gives both functions: the two calls have to agree, and the
// function they sit in is the settings apply.
constexpr const char* kSigSettingsDispatch =
    "A8 01 74 05 E8 ?? ?? ?? ?? 0F BA E3 08 73 05 E8 ?? ?? ?? ?? 84 DB 79 05 E8";
constexpr int kDispatchDisplayA = 4;
constexpr int kDispatchDisplayB = 24;

// The window apply reading the settings object, from the load of its global to
// the two fields a live resize writes: mode at +0x3C, requested size at
// +0x28/+0x2C, where -1 means none. The offsets are the point of it -- if they
// move, this should fail, not leave the mod writing to whatever lives there now.
constexpr const char* kSigWindowSettings =
    "48 8B 0D ?? ?? ?? ?? 33 DB 38 1D ?? ?? ?? ?? B8 00 00 00 10 0F 45 D8 "
    "83 79 3C 00 0F 85 ?? ?? ?? ?? 33 C0 48 89 BC 24 ?? ?? 00 00 "
    "48 89 45 ?? C7 45 ?? 28 00 00 00 48 89 45 ?? 48 89 45 ?? 48 89 45 ?? 48 89 45 ?? "
    "44 8B 41 28 41 83 F8 FF 74 ?? 8B 51 2C 83 FA FF 74";

// THE UI'S CAMERA, ui3D_CAMERA_MR -- by class name (rtti_vtable), because it is
// reached only through its vtable and has no caller to be found by. Every slot
// below is checked AT that slot, not scanned for: each reads one of the
// combine::CAMERA fields the replacement camera in marker_camera.cpp fills in,
// and these are what fail if the engine ever moves one.
constexpr const char* kUiCameraClass = ".?AVui3D_CAMERA_MR@@";
// Update: the camera pointer at +0x18 = the getter's answer.
constexpr int kUiCamSlotUpdate = 1;
constexpr const char* kSigUiCamUpdate =
    "40 53 48 83 EC ?? 48 8B D9 E8 ?? ?? ?? ?? 48 89 43 18 48 83 C4 ?? 5B C3";
constexpr int kUiCamUpdateE8 = 9;
// The getter: the active camera, (*global)->+0x10. Update is its only caller.
constexpr const char* kSigUiCamGetter =
    "48 83 EC ?? E8 ?? ?? ?? ?? 48 8B 00 48 8B 40 10 48 83 C4 ?? C3";
// The eye: viewInv at +0x80, inverted on demand while its first dword is -1,
// row 3 (+0xB0) the position.
constexpr int kUiCamSlotPosition = 4;
constexpr const char* kSigUiCamPosition =
    "48 89 5C 24 ?? 57 48 83 EC ?? 48 8B 59 18 48 8B FA 48 85 DB 74 ?? "
    "48 8D 8B 80 00 00 00 83 39 FF 75 ?? 4C 8B C3 33 D2 E8 ?? ?? ?? ?? "
    "F3 0F 10 8B B8 00 00 00 F3 0F 10 83 B4 00 00 00 8B 83 B0 00 00 00";
// World to view, through the view at the camera itself (+0x00).
constexpr int kUiCamSlotToView = 9;
constexpr const char* kSigUiCamToView =
    "40 53 48 83 EC ?? 49 8B D8 4C 8B 41 18 4D 85 C0 74 ?? F3 0F 10 02 48 8D 4C 24 ?? "
    "F3 0F 10 4A 04 F3 0F 11 44 24 ?? F3 0F 10 42 08 48 8D 54 24 ?? "
    "F3 0F 11 44 24 ?? F3 0F 11 4C 24 ?? E8";
// View to world, through viewInv.
constexpr int kUiCamSlotToWorld = 10;
constexpr const char* kSigUiCamToWorld =
    "48 89 74 24 ?? 57 48 83 EC ?? 49 8B F8 48 8B F2 4C 8B 41 18 4D 85 C0 74 ?? "
    "48 89 5C 24 ?? 49 8D 98 80 00 00 00 83 3B FF";
// View to NDC, through the projection at +0x40: where a 2D marker lands.
constexpr int kUiCamSlotToNdc = 11;
constexpr const char* kSigUiCamToNdc =
    "40 53 48 83 EC ?? 49 8B D8 4C 8B 41 18 4D 85 C0 74 ?? F3 0F 10 02 48 8D 4C 24 ?? "
    "F3 0F 10 4A 04 49 83 C0 40";
// The view and its inverse, copied out -- Scaleform's 3D markers take the view.
constexpr int kUiCamSlotView = 12;
constexpr const char* kSigUiCamView =
    "40 53 48 83 EC ?? 48 8B DA 48 8B 51 18 48 85 D2 74 ?? 48 8D 4C 24 ?? E8";
constexpr int kUiCamSlotViewInv = 13;
constexpr const char* kSigUiCamViewInv =
    "40 57 48 83 EC ?? 4C 8B 41 18 48 8B FA 4D 85 C0 74 ?? 48 89 5C 24 ?? "
    "49 8D 98 80 00 00 00 83 3B FF";
// 1/aspect (+0x10C), near and far (min and max of +0x110/+0x114) and the FOV
// (+0x108, in degrees): the 3D markers' perspective is built from these.
constexpr int kUiCamSlotInvAspect = 14;
constexpr const char* kSigUiCamInvAspect =
    "48 8B 41 18 48 85 C0 74 ?? F3 0F 10 05 ?? ?? ?? ?? F3 0F 5E 80 0C 01 00 00 C3";
constexpr int kUiCamSlotNear = 15;
constexpr const char* kSigUiCamNear =
    "48 8B 41 18 48 85 C0 74 ?? F3 0F 10 80 14 01 00 00 F3 0F 5D 80 10 01 00 00 C3";
constexpr int kUiCamSlotFar = 16;
constexpr const char* kSigUiCamFar =
    "48 8B 41 18 48 85 C0 74 ?? F3 0F 10 80 14 01 00 00 F3 0F 5F 80 10 01 00 00 C3";
constexpr int kUiCamSlotFov = 17;
constexpr const char* kSigUiCamFov =
    "48 8B 41 18 48 85 C0 74 ?? F3 0F 10 80 08 01 00 00 F3 0F 59 05 ?? ?? ?? ?? C3";

struct SlotCheck { int slot; const char* sig; const char* reads; };
constexpr SlotCheck kUiCameraSlots[] = {
    {kUiCamSlotUpdate,    kSigUiCamUpdate,    "the camera pointer at +0x18"},
    {kUiCamSlotPosition,  kSigUiCamPosition,  "viewInv at +0x80"},
    {kUiCamSlotToView,    kSigUiCamToView,    "the view at +0x00"},
    {kUiCamSlotToWorld,   kSigUiCamToWorld,   "viewInv at +0x80"},
    {kUiCamSlotToNdc,     kSigUiCamToNdc,     "the projection at +0x40"},
    {kUiCamSlotView,      kSigUiCamView,      "the view at +0x00"},
    {kUiCamSlotViewInv,   kSigUiCamViewInv,   "viewInv at +0x80"},
    {kUiCamSlotInvAspect, kSigUiCamInvAspect, "the aspect at +0x10C"},
    {kUiCamSlotNear,      kSigUiCamNear,      "near/far at +0x110"},
    {kUiCamSlotFar,       kSigUiCamFar,       "near/far at +0x110"},
    {kUiCamSlotFov,       kSigUiCamFov,       "the FOV at +0x108"},
};

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
// signature no longer says which one it meant. `storeSig`, where there is one,
// is tried only when `sig` matches nowhere at all.
uintptr_t by_signature(const char* name, const char* sig, const char* storeSig = nullptr)
{
    uint8_t* hit = nullptr;
    int n = pattern_scan_all(g_exe, sig, &hit, 1);
    if (n == 0 && storeSig) n = pattern_scan_all(g_exe, storeSig, &hit, 1);
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

    a.cameraCommit = report("camera commit", by_signature("camera commit", kSigCameraCommit,
                                                          kSigCameraCommitStore), "signature");
    a.viewBuilder  = report("view builder", by_signature("view builder", kSigViewBuilder), "signature");
    a.combine1     = report("combine1", by_signature("combine1", kSigCombine1), "signature");
    a.combine2     = report("combine2", by_signature("combine2", kSigCombine2), "signature");
    a.projBuild    = report("projection builder", by_signature("projection builder", kSigProjBuild), "signature");
    a.fna          = report("fna (orbit view)", by_signature("fna (orbit view)", kSigFnA), "signature");
    a.sizeCull     = report("small-object cull", by_signature("small-object cull", kSigSizeCull), "signature");

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
        if (!ret && a.fna) {
            // The Store build: its one call to fillOrbitView is the menu view,
            // whichever of the two paths into the menu camera got there.
            ret = unique_call("fna <- menu view helper",
                              by_signature("menu view helper", kSigMenuViewHelper), a.fna);
            how = "call in menu view helper";
        }
        a.orbitMenuRet = report("fna <- main menu", ret, how);
        if (!ret) VRLOG("  %-22s NOT FOUND", "fna <- main menu");
    }

    {
        const uintptr_t setPersp = by_signature("camera set-perspective", kSigSetPerspective);
        a.projMainRet = report("projection <- main", unique_call("projection <- main", setPersp, a.projBuild),
                               "call in camera set-perspective");

        // The E8 at the end of the signature has to be a call to set-perspective,
        // and the only one in that function.
        a.setPerspective = setPersp;
        uintptr_t mirror = 0;
        uint8_t* hit = nullptr;
        const int n = pattern_scan_all(g_exe, kSigMirrorCameraCall, &hit, 1);
        if (n == 1 && setPersp &&
            rva_of(rel32_target(hit + kMirrorCameraCallE8, 1, 5)) == setPersp)
            mirror = unique_call("perspective <- mirror", owner_of(hit), setPersp);
        a.mirrorPerspRet = report("perspective <- mirror", mirror, "call in mirror cameras");
        if (!mirror)
            VRLOG("  %-22s NOT FOUND -- signature matches %d place(s); mirrors are "
                  "told apart by shape alone, so a square one gets the headset's FOV",
                  "perspective <- mirror", n);
    }

    {
        // A RIP-relative load: the global is the instruction's own target, and
        // it has to land in a writable, non-executable section to be data.
        // Two matches in each build known, and all of them must name it.
        constexpr int kCap = 8;
        uint8_t* hits[kCap] = {};
        const int n = pattern_scan_all(g_exe, kSigSettingsRead, hits, kCap);
        uint8_t* tgt = nullptr;
        bool agree = n >= 1 && n <= kCap;
        for (int i = 0; agree && i < n; ++i) {
            uint8_t* t = rel32_target(hits[i], 3, 7);
            agree = !tgt || t == tgt;
            tgt = t;
        }
        const uintptr_t g = agree && exe_data(tgt) ? rva_of(tgt) : 0;
        a.settingsPtr = report("settings global", g, "load in camera commit");
        if (!g)
            VRLOG("  %-22s NOT FOUND -- read signature matches %d place(s)%s",
                  "settings global", n, agree ? ", but not a load from writable data"
                                              : ", which do not name one global");
    }

    {
        // Both callbacks out of one match of the table being filled, each
        // accepted only as a function entry.
        uintptr_t fm = 0, fr = 0;
        uint8_t* hit = nullptr;
        const int n = pattern_scan_all(g_exe, kSigCallbackTable, &hit, 1);
        if (n == 1) {
            uint8_t* r = rel32_target(hit + kCallbackRenderLea, 3, 7);
            uint8_t* m = rel32_target(hit + kCallbackMoveLea, 3, 7);
            if (is_function_entry(r) && is_function_entry(m)) {
                fr = rva_of(r);
                fm = rva_of(m);
            }
        }
        a.frameMove   = report("OnFrameMove", fm, "callback table");
        a.frameRender = report("OnFrameRender", fr, "callback table");
        if (!fm || !fr)
            VRLOG("  %-22s NOT FOUND -- table signature matches %d place(s)%s",
                  "frame callbacks", n, n == 1 ? ", but not at function entries" : "");
    }

    {
        uintptr_t g = 0;
        uint8_t* hit = nullptr;
        const int n = pattern_scan_all(g_exe, kSigElapsedClampFlag, &hit, 1);
        if (n == 1) {
            uint8_t* tgt = rel32_target(hit + kElapsedClampCmp, 2, 7);
            if (exe_data(tgt) && !(reinterpret_cast<uintptr_t>(tgt) & 3))
                g = rva_of(tgt);
        }
        a.elapsedClampFlag = report("elapsed clamp flag", g, "compare in OnFrameMove");
        if (!g)
            VRLOG("  %-22s NOT FOUND -- signature matches %d place(s)", "elapsed clamp flag", n);
    }

    {
        // The settings apply has to be entered from OnFrameMove and from nowhere
        // else, and the display apply only from it: that is the guard against
        // resizing during a load, which would deadlock (game_addresses.h).
        uintptr_t settings = 0, display = 0;
        uint8_t* hit = nullptr;
        const int n = pattern_scan_all(g_exe, kSigSettingsDispatch, &hit, 1);
        if (n == 1 && a.frameMove) {
            uint8_t* da = rel32_target(hit + kDispatchDisplayA, 1, 5);
            uint8_t* db = rel32_target(hit + kDispatchDisplayB, 1, 5);
            const uintptr_t owner = owner_of(hit);
            const std::vector<uint8_t*> in = find_calls_to(g_exe, g_base + owner);
            bool onlyHere = owner && da == db && is_function_entry(da) && in.size() == 1 &&
                            owner_of(in[0]) == a.frameMove;
            for (uint8_t* site : find_calls_to(g_exe, da))
                onlyHere = onlyHere && owner_of(site) == owner;
            if (onlyHere) {
                settings = owner;
                display  = rva_of(da);
            }
        }
        a.settingsApply = report("settings apply", settings, "dispatch, called by OnFrameMove");
        a.displayApply  = report("display apply", display, "dispatch in settings apply");
        if (!settings)
            VRLOG("  %-22s NOT FOUND -- signature matches %d place(s); a resolution or "
                  "DLSS change waits for a restart", "settings apply", n);

        // Read from the window apply, which the display apply has to call.
        uintptr_t cfg = 0;
        uint8_t* read = nullptr;
        const int m = pattern_scan_all(g_exe, kSigWindowSettings, &read, 1);
        if (m == 1 && display && calls_from(display, owner_of(read)).size() == 1) {
            uint8_t* tgt = rel32_target(read, 3, 7);
            if (exe_data(tgt)) cfg = rva_of(tgt);
        }
        a.windowSettings = report("window settings", cfg, "load in window apply");
        if (!cfg)
            VRLOG("  %-22s NOT FOUND -- signature matches %d place(s)", "window settings", m);
    }

    {
        // Through the class's vtable, and accepted only when every slot the
        // replacement camera has to satisfy still reads the field it did, and
        // the getter Update calls has no other caller -- so the pointer it sets
        // is the UI's alone, and replacing it touches nothing but the UI.
        uintptr_t upd = 0;
        const char* why = nullptr;
        char reason[96];
        void** vt = rtti_vtable(g_exe, kUiCameraClass, &why);
        if (vt) {
            for (const SlotCheck& c : kUiCameraSlots) {
                if (!pattern_matches_at(vt[c.slot], c.sig)) {
                    snprintf(reason, sizeof(reason), "vtable slot %d no longer reads %s",
                             c.slot, c.reads);
                    why = reason;
                    break;
                }
            }
        }
        if (vt && !why) {
            uint8_t* u = static_cast<uint8_t*>(vt[kUiCamSlotUpdate]);
            uint8_t* getter = rel32_target(u + kUiCamUpdateE8, 1, 5);
            if (!is_function_entry(u))
                why = "Update is not a function entry";
            else if (!pattern_matches_at(getter, kSigUiCamGetter))
                why = "Update no longer reads the active camera";
            else if (find_calls_to(g_exe, getter).size() != 1)
                why = "the active-camera read has callers besides Update";
            else
                upd = rva_of(u);
        }
        a.uiCameraUpdate = report("ui camera update", upd, "class ui3D_CAMERA_MR, vtable slot 1");
        if (!upd)
            VRLOG("  %-22s NOT FOUND -- %s; the camera-pointer world marker fix stays off",
                  "ui camera update", why ? why : "unknown");
    }

    report_globals("XInputGetState ptrs", "XInputGetState", a.xinputGetState,
                   GameAddresses::kMaxXInputPtrs);
    report_globals("XInputGetCaps ptrs", "XInputGetCapabilities", a.xinputGetCaps,
                   GameAddresses::kMaxXInputPtrs);

    if (g_found == g_wanted)
        VRLOG("game addresses: all %d found", g_wanted);
    else
        VRLOG("game addresses: %d of %d found -- whatever needs the missing ones stays "
              "OFF. The mod's signatures do not cover this game build (a newer patch, "
              "or another store's build of the game): set DumpGameCode=true in "
              "Snowrunner_VR_config.txt, run once, and update them from the "
              "snowrunner_image_*.bin it writes beside dxgi.dll.", g_found, g_wanted);
}

} // namespace

const GameAddresses& game_addresses()
{
    static std::once_flag once;
    std::call_once(once, resolve);
    return g_addr;
}

} // namespace hooks
