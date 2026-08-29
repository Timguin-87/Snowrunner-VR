#include "hooks/ui_hook.h"
#include "hooks/cbuffer_hook.h"
#include "hooks/shader_cull.h"
#include "render/mirror_mask.hpp"
#include "render/rigid_mask.hpp"
#include "render/ui_layer.hpp"
#include "render/smudge_layer.hpp"
#include "render/winch_layer.hpp"
#include "hooks/depth_probe.h"
#include "hooks/camera_hook.h"
#include "xr/xr_mirror.h"
#include "common/log.h"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdint>

#include <MinHook.h>

using Microsoft::WRL::ComPtr;

namespace hooks {
namespace {

bool is_imm(ID3D11DeviceContext* ctx) { return ctx->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE; }

constexpr int kIdxExecuteCommandList  = 58;   // ID3D11DeviceContext vtable
constexpr int kIdxDrawIndexed          = 12;
constexpr int kIdxDraw                 = 13;
constexpr int kIdxDrawIndexedInstanced = 20;
constexpr int kIdxDrawInstanced        = 21;
constexpr int kIdxFinishCommandList    = 114;  // verified against the SDK header this session

// Predicted position of the HUD's ExecuteCommandList call within the frame.
// Confirmed by direct headset testing: reliably the LAST such call, every
// mode. The command list is recorded once (FinishCommandList never fires
// during play) and simply replayed unchanged, so there is nothing to
// intercept at record time; this predicts "last" fresh each frame from the
// previous frame's total call count.
std::atomic<int> g_callIndexThisFrame{0};
std::atomic<int> g_predictedLastPos{-1};

// Latched when the pre-HUD copy has been taken this frame, so only the FIRST
// UI command list triggers it. Reset in ui_hook_on_present alongside the call
// index.
std::atomic<bool> g_capturedThisFrame{false};

std::atomic<bool> g_hideHud{false};

using PFN_ExecuteCL = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
PFN_ExecuteCL real_ExecuteCommandList = nullptr;

using PFN_DrawIndexed = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using PFN_Draw = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using PFN_DrawIndexedInstanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using PFN_DrawInstanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
// Immediate and deferred contexts turned out to use DISTINCT vtables (proven
// via VTABLE PROBE: two different addresses) -- not two instances sharing one
// vtable, as every hook in this codebase had assumed. Each vtable's slot has
// its OWN original function pointer, so hooking both requires two separate
// "real" pointers per method; using one shared pointer for both would have
// the second MH_CreateHook silently clobber the first context type's original,
// crashing (or worse, silently corrupting) the other context type's calls.
PFN_DrawIndexed          real_DrawIndexed_imm = nullptr, real_DrawIndexed_def = nullptr;
PFN_Draw                 real_Draw_imm = nullptr, real_Draw_def = nullptr;
PFN_DrawIndexedInstanced real_DrawIndexedInstanced_imm = nullptr, real_DrawIndexedInstanced_def = nullptr;
PFN_DrawInstanced        real_DrawInstanced_imm = nullptr, real_DrawInstanced_def = nullptr;

// --- "force re-record" recon: correlate a deferred context's RECORDING with
// the specific ExecuteCommandList call it later feeds ---------------------
// A flat capped log (first N deferred draws/binds, ever) drowned in noise
// from whichever subsystem happened to record first each session -- 15000+
// FinishCommandList calls/session means this engine re-records constantly,
// not just for the HUD. So while a recording is in progress, track one fact
// per deferred context: did it draw the UI onto the main canvas? That answer
// is transferred to the resulting ID3D11CommandList* the moment
// FinishCommandList completes, which is what makes "which command list is the
// UI" an OBSERVATION rather than a prediction -- see g_uiCl below.
//
// (This used to carry the whole recording profile -- draw count, last bound
// RTV size/format/texture, source context -- snapshotted into a ring keyed by
// command list and printed as a "HUD CL RECORDING" line when the predicted-last
// list was replayed. The prediction it served is no longer the capture trigger,
// and the line reported all zeros: the ring HAD an entry for that list, so its
// FinishCommandList was observed, but no tracked draw ever landed on the
// context that recorded it. That is consistent with the predicted-last position
// not being the HUD's list at all -- see the note in Detour_ExecuteCommandList
// about it landing on three different things across one session. Ring, lookup
// and log are gone.)
struct RecordSummary { bool sawUi = false; };

// Command lists observed to contain UI draws, by pointer. Deliberately NOT a
// small ring: a list recorded once and replayed for the rest of the session
// would age out of one and silently stop being recognised, which is precisely
// the failure the positional predictor already has. Entries here are never
// evicted, only overwritten if the table fills.
//
// No AddRef -- same best-effort pointer identity as g_knownHudTex/g_boundDepth.
// A freed command list whose address is later reused by a new one would be
// misidentified; in practice this engine holds a small stable pool.
constexpr int kMaxUiCls = 256;
std::atomic<void*> g_uiCl[kMaxUiCls] = {};

void register_ui_cl(void* cl)
{
    if (!cl) return;
    for (int i = 0; i < kMaxUiCls; ++i) if (g_uiCl[i].load() == cl) return;
    for (int i = 0; i < kMaxUiCls; ++i) {
        void* expected = nullptr;
        if (g_uiCl[i].compare_exchange_strong(expected, cl)) return;
    }
}

bool is_ui_cl(void* cl)
{
    if (!cl) return false;
    for (int i = 0; i < kMaxUiCls; ++i) if (g_uiCl[i].load() == cl) return true;
    return false;
}

// PIXEL SHADER identity, learned from ground truth: a RenderDoc Pixel History
// on an actual on-screen HUD icon (EID 70032, DrawIndexed(30)) showed a real
// per-element UI draw -- Pixel Shader with CB_DYNAMIC_UI bound at slot 4
// (both VS and PS), a small icon/text atlas at PS_UI_DIFF_TEX[0], and
// RenderDoc's own shader reflection names for the CB fields:
// UI_COORD_TRANSFORM_X/Y/Z, UI_TEXGEN_U/V, PS_REG_UI_CXFORM_MUL/ADD,
// PS_REG_UI_CONST_COLOR, PS_REG_UI_GAMMA_PARAMS (9 float4s = 144 bytes,
// matches kUiElemStride) -- this is Scaleform/GFx-style UI compositing, and
// the layout is NOT the simple 2D-scale/shear/translate guess used earlier
// (is_confirmed_ui_transform_buffer's value ranges were tuned against that
// wrong guess, which is almost certainly why gameplay HUD content never
// matched: real UI_COORD_TRANSFORM_X/Y/Z values include a 3rd row and
// translate constants in the tens of thousands, far outside the old
// -1000..3688 window).
//
// Rather than re-guess a new content heuristic (burned twice already: too
// narrow missed real elements, too loose caught fog/shadows), gate on the
// PIXEL SHADER'S IDENTITY instead -- learned once per session by watching
// which shader is bound whenever a plausibly-UI-shaped (144-byte-multiple)
// buffer gets bound at slot 4 on a context we've already confirmed records
// the HUD. Distinct compiled shader objects for unrelated systems (fog,
// shadows, post-process) can't collide with this by construction.
//
// A viewport-based HUD SHRINK used to live here, and a counter-shift that
// cancelled the eye cant for screen-space content. Both are gone with the UI
// itself: a quad layer is placed by the compositor, so its angular size is a
// property of the layer (see build_ui_quad_layer) rather than something to
// forge by resizing a viewport, and a canted eye needs no compensation at all
// because the layer is never baked into either eye image in the first place.


constexpr int kMaxTrackedCtx = 16;
struct CtxSlot { std::atomic<void*> ctx{nullptr}; RecordSummary sum; };
CtxSlot g_ctxSummaries[kMaxTrackedCtx];
SRWLOCK g_ctxSumLock = SRWLOCK_INIT;

void note_deferred_draw(ID3D11DeviceContext* ctx)
{
    if (ctx->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE) return;

    // Only the render target's SIZE is needed: the canvas test below is what
    // distinguishes the late composite onto the main canvas from the offscreen
    // atlas passes. Format and texture identity were carried for the removed
    // recording log and are not read by anything.
    ID3D11RenderTargetView* rtv[1] = {nullptr};
    ctx->OMGetRenderTargets(1, rtv, nullptr);
    UINT w = 0, h = 0;
    if (rtv[0]) {
        ID3D11Resource* res = nullptr;
        rtv[0]->GetResource(&res);
        if (res) {
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&tex)))) {
                D3D11_TEXTURE2D_DESC d{};
                tex->GetDesc(&d);
                w = d.Width; h = d.Height;
                tex->Release();
            }
            res->Release();
        }
        rtv[0]->Release();
    }

    void* key = (void*)ctx;
    AcquireSRWLockExclusive(&g_ctxSumLock);
    int idx = -1;
    for (int i = 0; i < kMaxTrackedCtx; ++i) if (g_ctxSummaries[i].ctx.load() == key) { idx = i; break; }
    if (idx < 0) {
        for (int i = 0; i < kMaxTrackedCtx; ++i) {
            void* expected = nullptr;
            if (g_ctxSummaries[i].ctx.compare_exchange_strong(expected, key)) { idx = i; g_ctxSummaries[i].sum = RecordSummary{}; break; }
        }
    }
    if (idx >= 0) {
        RecordSummary& s = g_ctxSummaries[idx].sum;
        // UI drawn to the MAIN CANVAS only. A plain "bound a UI shader" test
        // tags the offscreen atlas passes too (rt=256x256, ~48 draws), and
        // those run early -- before colour grading. Capturing there produced a
        // synthesized eye that was nearly black, which is the same pre-grading
        // darkness this file has hit before. The composite onto the canvas is
        // the late pass, and the one worth knowing about.
        if (w && hooks::current_draw_role() == hooks::kCullUi) {
            uint32_t cw = 0, ch = 0;
            if (xr::render_canvas_wh(cw, ch) && w == cw && h == ch) s.sawUi = true;
        }
    }
    ReleaseSRWLockExclusive(&g_ctxSumLock);
}

RecordSummary take_and_reset_summary(ID3D11DeviceContext* ctx)
{
    void* key = (void*)ctx;
    RecordSummary out{};
    AcquireSRWLockExclusive(&g_ctxSumLock);
    for (int i = 0; i < kMaxTrackedCtx; ++i) {
        if (g_ctxSummaries[i].ctx.load() == key) {
            out = g_ctxSummaries[i].sum;
            g_ctxSummaries[i].sum = RecordSummary{};
            g_ctxSummaries[i].ctx.store(nullptr);
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_ctxSumLock);
    return out;
}

// hooks::cull_current_draw() is the whole of the windscreen-removal feature's
// draw-side cost: one thread-local bool, already decided at PSSetShader time
// (see shader_cull.h). It reads false for every draw whenever the feature is
// off, so these four stay exactly as cheap as they were.
// Issues the mirror-marking duplicate of a draw, if this draw is a mirror and
// DIBR shift is running. The lambda is the real draw call, so each detour keeps its
// own signature. Recording into the game's own command list, so begin/end_mark
// restore every piece of state they touch -- see mirror_mask.hpp.
template <typename DrawFn>
void mark_if_mirror(ID3D11DeviceContext* ctx, DrawFn&& draw)
{
    if (hooks::current_draw_role() != hooks::kCullMirror || !mirrormask::active())
        return;
    mirrormask::Saved saved;
    if (!mirrormask::begin_mark(ctx, saved)) return;
    draw();
    mirrormask::end_mark(ctx, saved);
}

// The same duplicate, for geometry that is RIGID WITH THE CAMERA -- truck,
// cab interior, trailers. Marked so the 6-DoF stale-eye reprojection can leave
// those pixels alone instead of moving them by the camera's own travel; see
// render/rigid_mask.hpp.
//
// Costlier than the mirror case by a lot: the truck is hundreds of draws a
// frame where a mirror is a handful. Both guards below are cheap and both
// matter -- current_draw_role() is a thread-local already decided at
// PSSetShader time, and active() is false unless the reprojection is actually
// running, which is what keeps this off for everyone who is not using it.
template <typename DrawFn>
void mark_if_rigid(ID3D11DeviceContext* ctx, DrawFn&& draw)
{
    if (hooks::current_draw_role() != hooks::kCullRigid || !rigidmask::active())
        return;
    rigidmask::Saved saved;
    if (!rigidmask::begin_mark(ctx, saved)) return;
    draw();
    rigidmask::end_mark(ctx, saved);
}

// UI shaders that are genuinely UI -- they stay in the kCullUi role, so the
// pre-HUD command-list tagging and hide-HUD still treat them as such -- but
// which do NOT survive being redrawn into a transparent target.
//
// The capture forces a premultiplied alpha blend so semi-transparent elements
// composite correctly (see ui_layer.hpp). A shader that instead relies on the
// opaque backbuffer underneath it -- writing an alpha that means something
// else, or covering geometry it expects to be already filled -- comes out of
// that as solid black rather than as nothing. Observed for the entry below as
// black bars in the corner of the frame.
//
// An excluded shader is simply left in the render, drawn into the frame the
// way it always was. That is a mixed placement -- that element sits in the eye
// image while the rest of the UI is on the plane -- and it is still the right
// trade: the alternative is a black bar on the plane.
constexpr uint64_t kUiCopyExclude[] = {
    0x4B3885B96F72F43Full,   // black bars in the corners of the captured layer
};

bool ui_copy_excluded(uint64_t hash)
{
    if (!hash) return false;
    for (uint64_t h : kUiCopyExclude) if (h == hash) return true;
    return false;
}

// THE UI LEAVES THE RENDER HERE.
//
// This REPLACES the draw rather than duplicating it -- returning true tells the
// detour to skip the real call, the same suppression redirect_if_smudge
// performs, with the pixels kept instead of discarded. The result is submitted
// as its own quad layer in the headset (see ui_layer.hpp and
// build_ui_quad_layer in xr_mirror.cpp), where the compositor places it
// correctly in both eyes instead of it being frozen into a stereo pair.
//
// Inert whenever the layer is not up, and that is the fallback that matters:
// no OpenXR session, a swapchain that would not create, HUD hidden -- in every
// one of those uilayer::active() is false and the UI draws into the frame
// exactly as it did before any of this existed.
// ONE EYE'S UI ends up on the plane -- but WHICH FRAME'S is not decided here.
//
// Not every UI element is screen-space. Anything positioned by projecting a
// WORLD point -- the map's mapspace markers, waypoint and objective pips, any
// prompt pinned to something nearby -- has its screen position computed
// through the camera of the frame it is drawn in, and under AER that camera
// alternates eyes every frame. In the eye images that was exactly right: each
// eye received the marker projected for itself, which is real disparity and
// fuses. On ONE shared plane it is not: consecutive frames hand the same
// surface two different positions, and it reads as a doubled or jittering
// marker. Worst on the map, where the eye offset is sign-flipped and the
// camera is close in.
//
// The frame's eye is therefore picked at PRESENT, in build_ui_quad_layer, and
// not from here. Two reasons, both about this being the wrong place to ask:
// the UI is recorded on worker threads, so a parity read here races the flip
// the camera build performs mid-frame; and roughly 1% of Presents carry no
// camera build at all (the log's "AER: n/N Presents had no camera build"),
// which shifts the alternation's phase under any draw-time answer. By Present
// the frame is finished and present_route_eye() is simply what it rendered as.
//
// So this captures unconditionally and Present throws away the frames it does
// not want. Re-issuing the draw is what the old copy_if_ui did every frame
// anyway, so nothing got more expensive.
template <typename DrawFn>
bool redirect_if_ui(ID3D11DeviceContext* ctx, DrawFn&& draw)
{
    if (hooks::current_draw_role() != hooks::kCullUi || !uilayer::active())
        return false;
    if (ui_copy_excluded(hooks::current_draw_hash())) return false;
    uilayer::Saved saved;
    if (!uilayer::begin_capture(ctx, saved)) return false;
    draw();
    uilayer::end_capture(ctx, saved);
    return true;
}

// Windscreen splatter: draw it into its own layer INSTEAD of the main target.
//
// Unlike copy_if_ui, which duplicates a draw that also happens normally, this
// REPLACES it -- returning true tells the detour to skip the real call. That is
// the same suppression the cull performs, with the pixels kept instead of
// discarded so both eyes can have them placed at the glass's depth rather than
// the terrain's. See smudge_layer.hpp.
//
// Runs ahead of cull_current_draw() so it takes precedence over the plain hide
// setting; when the layer is inactive this is inert and the cull decides as
// before.
template <typename DrawFn>
bool redirect_if_smudge(ID3D11DeviceContext* ctx, DrawFn&& draw)
{
    if (hooks::current_draw_role() != hooks::kCullWindowSmudge ||
        !smudgelayer::active())
        return false;
    smudgelayer::Saved saved;
    if (!smudgelayer::begin_capture(ctx, saved)) return false;
    draw();
    smudgelayer::end_capture(ctx, saved);
    return true;
}

// The winch anchor markers, captured out of the scene so DIBR never sees them.
//
// REPLACES the draw -- returning true tells the detour to skip the real call,
// exactly as redirect_if_smudge() does. The pixels are kept, on their own
// transparent layer, and painted back onto BOTH eyes unshifted by the final
// blit. See winch_layer.hpp for why a UI element must not be depth-reprojected
// and why the head-locked UI quad is the wrong home for it.
//
// Inert unless DIBR shift is on: winchlayer::active() is false otherwise, so
// the markers go into the scene as the game intended and the stale-eye warp
// carries them like everything else.
//
// Runs ahead of cull_current_draw() for the same reason the smudge redirect
// does -- taking the pixels somewhere else takes precedence over any decision
// to discard them.
//
// (A WINCH MARKER SIZE slider lived here: a viewport scale around these same
// draws. Removed 2026-08-24 -- a viewport scales the whole NDC-to-screen
// mapping, so it moved each icon away from the screen position it exists to
// mark. A pixel-shader hash says which draw, never where inside it anything
// sits, so nothing in a draw detour can scale the icons about their own
// centres; that needs the vertex-side size constant, which has not been found.)
template <typename DrawFn>
bool redirect_if_winch_marker(ID3D11DeviceContext* ctx, DrawFn&& draw)
{
    if (hooks::current_draw_role() != hooks::kCullWinchMarker ||
        !winchlayer::active())
        return false;
    winchlayer::Saved saved;
    if (!winchlayer::begin_capture(ctx, saved)) return false;
    draw();
    winchlayer::end_capture(ctx, saved);
    return true;
}

// Record where the WINDSCREEN is, from the geometry the game already submits.
//
// The glass renders without depth writes -- that is the root of the whole
// problem, since it leaves the terrain's depth at those pixels and DIBR then
// moves anything on the glass by the terrain's disparity. Drawing the same
// geometry a second time into our own depth target with writes forced on
// recovers exactly what the game declined to record.
//
// This target is READ ONLY BY THE SMUDGE COMPOSITE. It is never folded into the
// scene depth the warp uses, so the world keeps warping by its own real depth
// and only the see-through splatter is displaced by the glass.
//
// Fed by the glass role as well as the splatter role: the glass covers the whole
// windscreen while splatter covers only patches of it, so taking both gives the
// composite depth to read wherever a patch happens to sit. Called from AHEAD of
// the cull, so hiding the glass does not also lose its depth.
template <typename DrawFn>
void capture_glass_depth(ID3D11DeviceContext* ctx, DrawFn&& draw)
{
    if (!smudgelayer::active()) return;
    const int role = hooks::current_draw_role();
    if (role != hooks::kCullWindows && role != hooks::kCullWindowSmudge) return;
    smudgelayer::Saved saved;
    if (!smudgelayer::begin_depth_capture(ctx, saved)) return;
    draw();
    smudgelayer::end_depth_capture(ctx, saved);
}

void STDMETHODCALLTYPE Detour_DrawIndexed(ID3D11DeviceContext* ctx, UINT ic, UINT sil, INT bvl)
{
    note_deferred_draw(ctx);
    PFN_DrawIndexed pfn = (is_imm(ctx) && real_DrawIndexed_imm) ? real_DrawIndexed_imm
                        : (real_DrawIndexed_def ? real_DrawIndexed_def : real_DrawIndexed_imm);
    if (!pfn) return;
    auto call = [&] { pfn(ctx, ic, sil, bvl); };
    capture_glass_depth(ctx, call);
    if (redirect_if_smudge(ctx, call)) return;
    if (redirect_if_winch_marker(ctx, call)) return;
    if (hooks::cull_current_draw(ic)) return;
    if (redirect_if_ui(ctx, call)) return;
    mark_if_mirror(ctx, call);
    mark_if_rigid(ctx, call);
    call();
}
void STDMETHODCALLTYPE Detour_Draw(ID3D11DeviceContext* ctx, UINT vc, UINT sv)
{
    note_deferred_draw(ctx);
    PFN_Draw pfn = (is_imm(ctx) && real_Draw_imm) ? real_Draw_imm
                 : (real_Draw_def ? real_Draw_def : real_Draw_imm);
    if (!pfn) return;
    auto call = [&] { pfn(ctx, vc, sv); };
    capture_glass_depth(ctx, call);
    if (redirect_if_smudge(ctx, call)) return;
    if (redirect_if_winch_marker(ctx, call)) return;
    if (hooks::cull_current_draw(vc)) return;
    if (redirect_if_ui(ctx, call)) return;
    mark_if_mirror(ctx, call);
    mark_if_rigid(ctx, call);
    call();
}
void STDMETHODCALLTYPE Detour_DrawIndexedInstanced(ID3D11DeviceContext* ctx, UINT ipi, UINT ic, UINT sil, INT bvl, UINT sii)
{
    note_deferred_draw(ctx);
    PFN_DrawIndexedInstanced pfn = (is_imm(ctx) && real_DrawIndexedInstanced_imm) ? real_DrawIndexedInstanced_imm
                                 : (real_DrawIndexedInstanced_def ? real_DrawIndexedInstanced_def : real_DrawIndexedInstanced_imm);
    if (!pfn) return;
    auto call = [&] { pfn(ctx, ipi, ic, sil, bvl, sii); };
    capture_glass_depth(ctx, call);
    if (redirect_if_smudge(ctx, call)) return;
    if (redirect_if_winch_marker(ctx, call)) return;
    if (hooks::cull_current_draw(ipi)) return;
    if (redirect_if_ui(ctx, call)) return;
    mark_if_mirror(ctx, call);
    mark_if_rigid(ctx, call);
    call();
}
void STDMETHODCALLTYPE Detour_DrawInstanced(ID3D11DeviceContext* ctx, UINT vpi, UINT ic, UINT sv, UINT siv)
{
    note_deferred_draw(ctx);
    PFN_DrawInstanced pfn = (is_imm(ctx) && real_DrawInstanced_imm) ? real_DrawInstanced_imm
                          : (real_DrawInstanced_def ? real_DrawInstanced_def : real_DrawInstanced_imm);
    if (!pfn) return;
    auto call = [&] { pfn(ctx, vpi, ic, sv, siv); };
    capture_glass_depth(ctx, call);
    if (redirect_if_smudge(ctx, call)) return;
    if (redirect_if_winch_marker(ctx, call)) return;
    if (hooks::cull_current_draw(vpi)) return;
    if (redirect_if_ui(ctx, call)) return;
    mark_if_mirror(ctx, call);
    mark_if_rigid(ctx, call);
    call();
}

using PFN_FinishCommandList = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, BOOL, ID3D11CommandList**);
PFN_FinishCommandList real_FinishCommandList = nullptr;

HRESULT STDMETHODCALLTYPE Detour_FinishCommandList(
    ID3D11DeviceContext* ctx, BOOL restoreState, ID3D11CommandList** out)
{
    // Load-bearing, not diagnostic: this is where a recording's observed
    // "drew UI onto the main canvas" fact becomes a property of the resulting
    // command list, which is what is_ui_cl() then tests to decide when to take
    // the pre-HUD copy.
    //
    // Snapshot the recording summary BEFORE calling through: when
    // restoreState is FALSE (what every call we've seen uses), the deferred
    // context's state resets to defaults as part of this call, so anything
    // read from ctx afterward -- exactly the post-replay Get() dead end hit
    // earlier this session on the immediate-context side -- would be empty.
    RecordSummary sum = take_and_reset_summary(ctx);
    HRESULT hr = real_FinishCommandList(ctx, restoreState, out);
    if (SUCCEEDED(hr) && out && *out) {
        if (sum.sawUi) register_ui_cl((void*)*out);
    }
    return hr;
}

// TWO DEAD ENDS, kept as notes rather than repeated:
//
// (1) A post-replay diagnostic (right after real_ExecuteCommandList for the
// HUD's call, with restoreState=0 so state should persist) queried
// VSGetConstantBuffers / PSGetConstantBuffers / OMGetRenderTargets /
// VSGetShader / PSGetShader. EVERY query came back null/none, every time (5
// captures). The HUD's command list clears its own bindings as its LAST
// recorded action -- a defensive teardown, not something observable around.
// That also explained the "artifacts when toggling hide/show" report:
// skipping the whole list skipped that teardown too, leaving an EARLIER
// pass's bindings dangling into the next frame -- fixed below by replicating
// the teardown ourselves whenever we skip.
//
// (2) Hooked OMSetRenderTargets globally to check whether the HUD renders to
// its OWN offscreen texture (Scaleform's stage was 1920x1080 historically,
// distinctly non-square unlike our 2688x2688 stereo render -- would have been
// a clean, trivial extraction if found). Result: no such texture exists --
// every render target seen was either the square 2688x2688 main target (or
// its mip chain: 1344/672/336/168/84/42) or the already-known false lead
// 2688x2880 fmt=91 (the motion-vector buffer, ruled out earlier this
// session). Also caused a real regression and was REMOVED: depth_probe.cpp
// already hooks the SAME vtable slot (33) for its own capture logic, and
// MinHook does not support two independent hooks on one address -- this one
// installed first in the Present chain and silently stole the slot, so
// depth_probe's hook installation lost the race and its capture stopped
// firing entirely (confirmed in the log: "depth acquire: 600/600 frames
// MISSED", every frame, from the moment this hook went in).

// --- pre-HUD backbuffer copy (DIBR shift) ------------------------------------------
// The HUD is screen-space -- zero disparity, same pixel position in both eyes --
// but it is drawn after the depth pass with depth writes off, so the depth under
// it holds the SCENE's depth. DIBR shift's reprojection therefore shifts HUD pixels by
// the scene's disparity: the HUD lands displaced in the synthesized eye AND
// leaves a hole where it came from. Reprojecting the frame as it stood before
// the HUD cannot do that.
//
// WHERE the copy is taken is the whole difficulty, and two failures map it out:
//
//   * POSITIONAL (before the predicted-last command list) is correctly lit --
//     that point is after the game's colour grading. Its one flaw is that the
//     HUD spans SEVERAL command lists at some view angles, so the copy there
//     already contains part of the UI and the synthesized eye shows a doubled
//     HUD on those angles only.
//   * CONTENT-BASED (before the first list off a known-HUD context) catches all
//     the UI, but fires early in the frame, BEFORE grading -- the synthesized
//     eye came out geometrically perfect at ~20% brightness because the copy
//     held linear-space values. Restricting it to a window at the END of the
//     frame was tried too and was WORSE, not better: black most of the time.
//
// MEASURED CONCLUSION, and the reason this is not adjustable: capturing at the
// predicted-last list is correctly lit, and capturing ONE list earlier comes out
// dark. The colour-grading pass therefore sits between those two, so there is no
// position that is both after grading and before all of the UI -- the angles
// where the HUD spills into an extra command list cannot be fixed by moving the
// capture at all.
//
// Three things were tried and all are worse, in increasing order of damage: a
// self-correcting one-list backoff (dark most of the time, and it did not even
// fire on the angles that needed it, because is_known_hud_ctx is populated from
// this same unreliable positional guess and mis-identifies); a content trigger
// restricted to the last six lists (black most of the time); a plain content
// trigger (correct geometry at ~20% brightness).
//
// So this stays fixed at the predicted-last position. It is right for the great
// majority of frames and degrades to a doubled HUD -- never a wrong-image eye --
// on those angles. Fixing them needs a different mechanism, not a different
// position: either the difference-overlay composite, or a RenderDoc pass to find
// what actually marks the end of colour grading.

ComPtr<IDXGISwapChain>           g_swapchain;
ComPtr<ID3D11Texture2D>          g_preHud;
UINT        g_preHudW = 0, g_preHudH = 0;
DXGI_FORMAT g_preHudFmt = DXGI_FORMAT_UNKNOWN;
std::atomic<bool> g_preHudValid{false};   // usable for the frame being presented


void capture_pre_hud(ID3D11DeviceContext* ctx)
{
    if (!g_swapchain || !ctx) return;

    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(g_swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb)
        return;

    D3D11_TEXTURE2D_DESC bd{};
    bb->GetDesc(&bd);
    if (!g_preHud || g_preHudW != bd.Width || g_preHudH != bd.Height ||
        g_preHudFmt != bd.Format) {
        g_preHud.Reset();
        g_preHudValid.store(false);
        ComPtr<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        if (!dev) return;
        // Same format as the backbuffer, so dibr::warp() builds its source view
        // exactly as it would for the backbuffer itself and the whole path stays
        // in one colour encoding.
        D3D11_TEXTURE2D_DESC cd = bd;
        cd.Usage = D3D11_USAGE_DEFAULT;
        cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        cd.CPUAccessFlags = 0;
        cd.MiscFlags = 0;
        cd.SampleDesc.Count = 1;
        if (FAILED(dev->CreateTexture2D(&cd, nullptr, &g_preHud))) {
            VRLOG("pre-HUD copy: create FAILED (%ux%u fmt=%d)", bd.Width, bd.Height, (int)bd.Format);
            return;
        }
        g_preHudW = bd.Width; g_preHudH = bd.Height; g_preHudFmt = bd.Format;
        VRLOG("pre-HUD copy: armed %ux%u fmt=%d", bd.Width, bd.Height, (int)bd.Format);
    }

    ctx->CopyResource(g_preHud.Get(), bb.Get());
    g_preHudValid.store(true);
}

void STDMETHODCALLTYPE Detour_ExecuteCommandList(
    ID3D11DeviceContext* ctx, ID3D11CommandList* cl, BOOL restoreState)
{
    const int pos = g_callIndexThisFrame.fetch_add(1);
    const bool isPredictedHud = (pos == g_predictedLastPos.load());
    // Before the replay, so the copy holds the frame WITHOUT the HUD.
    //
    // The trigger is now the FIRST command list this frame that was observed
    // recording a known UI shader -- not the predicted-last ExecuteCommandList.
    // That prediction is what produced the doubled UI at particular view
    // angles: the log shows "last" landing on three different things across a
    // session (a 1-draw 2880x2880 pass, a 48-draw 256x256 atlas, and a list
    // with no render target at all), so on the angles where the call sequence
    // changed the copy was taken a step too late and caught half the UI.
    //
    // LAST-WINS, not first. Capturing before the FIRST UI list was tried and
    // reverted: colour grading sits partway through the UI, so the earliest
    // point that precedes all UI also precedes the grade, and the copy came
    // out nearly black. Re-copying before each canvas-UI list means the
    // surviving copy is the one taken before the LAST of them -- correctly
    // graded, which is the property the old predicted-last trigger had and the
    // one worth keeping. Any UI drawn by an earlier canvas list is in the copy;
    // that is the same trade the predictor made, and the lesser artefact.
    //
    // Falls back to the old prediction only while no UI list has been
    // identified yet -- the first frames of a session.
    //
    // SKIPPED ENTIRELY once the UI has its own quad layer. Everything below
    // exists to find a moment when the backbuffer holds the frame but not the
    // HUD; with the UI redirected out of the render (redirect_if_ui) every
    // moment is that moment, so the copy would be a full-canvas CopyResource
    // per frame to produce a duplicate of the finished frame. Returning no
    // pre-HUD texture makes DIBR shift reproject the backbuffer itself, which is now
    // the correct source rather than the fallback.
    const bool uiCl = is_ui_cl((void*)cl);
    const bool anyUiKnown = g_uiCl[0].load() != nullptr;
    if (xr::dibr_shift_enabled() && !uilayer::active() &&
        (uiCl || (!anyUiKnown && isPredictedHud))) {
        capture_pre_hud(ctx);
        g_capturedThisFrame.store(true);
    }

    // Hide-HUD used to skip this whole command list, which also skipped
    // whatever non-UI work shared it -- the reason it was only ever billed as a
    // screenshot fallback. It is now done by shader identity in shader_cull
    // (set_hide_hud_enabled toggles the UI role), so exactly the UI draws are
    // dropped and everything else in the list still runs. Nothing to do here.

    real_ExecuteCommandList(ctx, cl, restoreState);

    // The replay rebound its own pixel shader without passing through our
    // PSSetShader detour, so the cached cull answer for this thread now
    // describes a shader that is no longer bound.
    hooks::note_ps_state_lost();
}

} // namespace


bool install_ui_hook(IDXGISwapChain* swapchain)
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
    g_swapchain = swapchain;   // for the pre-HUD copy

    // Immediate and deferred contexts use DISTINCT vtables (proven via a
    // VTABLE PROBE comparison: two different addresses), not one vtable
    // shared across instances as every hook in this codebase had assumed.
    // That's why FinishCommandList -- which RenderDoc proved fires live, on
    // worker threads, for the HUD specifically -- was never once observed:
    // we were only ever patching the immediate context's copy of the
    // function. Get a deferred context's vtable too and hook both.
    ComPtr<ID3D11DeviceContext> defProbe;
    dev->CreateDeferredContext(0, &defProbe);
    void** vt = *reinterpret_cast<void***>(ctx.Get());
    void** vtDef = defProbe ? *reinterpret_cast<void***>(defProbe.Get()) : nullptr;
    if (!vtDef) VRLOG("UI hook: CreateDeferredContext FAILED -- deferred-side hooks skipped");

    // Idempotent: MH_ERROR_ALREADY_CREATED/MH_ERROR_ENABLED mean a PRIOR call
    // already hooked this address successfully -- still a live, working hook,
    // not a failure. Without this, install_ui_hook (called every Present)
    // would treat its own earlier success as a fresh failure on every retry,
    // permanently reset the "installed" guard, and spam-retry forever.
    auto hookOn = [](void** vtbl, int idx, void* detour, void** orig) -> bool {
        MH_STATUS c = MH_CreateHook(vtbl[idx], detour, orig);
        if (c != MH_OK && c != MH_ERROR_ALREADY_CREATED) return false;
        MH_STATUS e = MH_EnableHook(vtbl[idx]);
        return e == MH_OK || e == MH_ERROR_ENABLED;
    };
    bool ok = hookOn(vt, kIdxExecuteCommandList, reinterpret_cast<void*>(&Detour_ExecuteCommandList),
                     reinterpret_cast<void**>(&real_ExecuteCommandList));
    ok = hookOn(vt, kIdxDrawIndexed, reinterpret_cast<void*>(&Detour_DrawIndexed),
               reinterpret_cast<void**>(&real_DrawIndexed_imm)) && ok;
    ok = hookOn(vt, kIdxDraw, reinterpret_cast<void*>(&Detour_Draw),
               reinterpret_cast<void**>(&real_Draw_imm)) && ok;
    ok = hookOn(vt, kIdxDrawIndexedInstanced, reinterpret_cast<void*>(&Detour_DrawIndexedInstanced),
               reinterpret_cast<void**>(&real_DrawIndexedInstanced_imm)) && ok;
    ok = hookOn(vt, kIdxDrawInstanced, reinterpret_cast<void*>(&Detour_DrawInstanced),
               reinterpret_cast<void**>(&real_DrawInstanced_imm)) && ok;

    if (vtDef) {
        if (vtDef[kIdxDrawIndexed] == vt[kIdxDrawIndexed]) real_DrawIndexed_def = real_DrawIndexed_imm;
        else ok = hookOn(vtDef, kIdxDrawIndexed, reinterpret_cast<void*>(&Detour_DrawIndexed),
                         reinterpret_cast<void**>(&real_DrawIndexed_def)) && ok;

        if (vtDef[kIdxDraw] == vt[kIdxDraw]) real_Draw_def = real_Draw_imm;
        else ok = hookOn(vtDef, kIdxDraw, reinterpret_cast<void*>(&Detour_Draw),
                         reinterpret_cast<void**>(&real_Draw_def)) && ok;

        if (vtDef[kIdxDrawIndexedInstanced] == vt[kIdxDrawIndexedInstanced]) real_DrawIndexedInstanced_def = real_DrawIndexedInstanced_imm;
        else ok = hookOn(vtDef, kIdxDrawIndexedInstanced, reinterpret_cast<void*>(&Detour_DrawIndexedInstanced),
                         reinterpret_cast<void**>(&real_DrawIndexedInstanced_def)) && ok;

        if (vtDef[kIdxDrawInstanced] == vt[kIdxDrawInstanced]) real_DrawInstanced_def = real_DrawInstanced_imm;
        else ok = hookOn(vtDef, kIdxDrawInstanced, reinterpret_cast<void*>(&Detour_DrawInstanced),
                         reinterpret_cast<void**>(&real_DrawInstanced_def)) && ok;

        // FinishCommandList is invalid on the immediate context (D3D11 spec),
        // so it only ever needs hooking on the deferred vtable -- one real
        // pointer, no immediate-side counterpart to conflict with.
        ok = hookOn(vtDef, kIdxFinishCommandList, reinterpret_cast<void*>(&Detour_FinishCommandList),
                   reinterpret_cast<void**>(&real_FinishCommandList)) && ok;
    }

    VRLOG("UI hook %s -- HUD auto-detected as the last ExecuteCommandList/frame. "
          "L = hide HUD (clears state properly on skip). "
          "Draw/FinishCommandList now hooked on BOTH immediate and deferred vtables.",
          ok ? "installed" : "FAILED");
    if (!ok) installed = false;
    return ok;
}

ID3D11Texture2D* pre_hud_texture()
{
    return g_preHudValid.load() ? g_preHud.Get() : nullptr;
}


bool hide_hud_enabled() { return g_hideHud.load(); }

void set_hide_hud_enabled(bool on)
{
    g_hideHud.store(on);
    // The UI role IS the hide. Suppressing the seven known UI shaders drops
    // exactly the UI draws, wherever they were recorded, instead of skipping a
    // whole command list and taking any non-UI work in it along with them.
    hooks::set_cull_role_enabled(hooks::kCullUi, on);
}

void ui_hook_on_present(IDXGISwapChain* /*swapchain*/)
{
    const int callsThisFrame = g_callIndexThisFrame.exchange(0);
    const bool captured = g_capturedThisFrame.exchange(false);

    // The retrospective validation below only applies to the PREDICTED capture
    // path, which is now the fallback used before any UI command list has been
    // identified. Once one has, the trigger is an observation -- this list was
    // seen recording UI draws -- and there is nothing to validate: it cannot
    // have landed in the wrong place, so invalidating it here would throw away
    // a good copy every time the call count moved.
    const bool anyUiKnown = g_uiCl[0].load() != nullptr;
    if (!anyUiKnown) {
        // The capture position is predicted from the PREVIOUS frame's call
        // count, and that count moves with what is on screen. A prediction that
        // turned out wrong means the copy was taken somewhere else in the frame
        // -- discard it and let DIBR shift reproject the finished backbuffer for this
        // one frame. A dragged HUD for a frame is a far smaller artefact than an
        // eye built from the wrong image.
        const bool predictionHeld = (g_predictedLastPos.load() == callsThisFrame - 1);
        if (!predictionHeld) g_preHudValid.store(false);
    } else if (!captured) {
        // A frame with UI command lists known but none executed drew no UI at
        // all, so the previous copy is stale -- do not hand DIBR shift a frame from
        // before whatever just changed.
        g_preHudValid.store(false);
    }

    g_predictedLastPos.store(callsThisFrame - 1);


    // DIAGNOSTIC: edge-triggered, so this only prints on the transition, not
    // every frame. Threshold (400ms) is generous relative to any normal
    // frame-time hitch -- meant to catch "DRIVE_CAMERA hasn't run in a
    // while", not a single dropped frame. Purely observational for now: log
    // transitions and correlate them by hand against what's on screen
    // in-headset (map, pause, garage, ...) before wiring this into any
    // actual behavior change.
    {
        static bool s_camIdle = false;
        const bool camIdle = hooks::logic_camera_idle_ms() > 400;
        if (camIdle != s_camIdle) {
            VRLOG("CAMERA: DRIVE_CAMERA %s (idle %llums) -- possibly map/menu/garage",
                  camIdle ? "went IDLE" : "resumed", (unsigned long long)hooks::logic_camera_idle_ms());
            s_camIdle = camIdle;
        }
    }
}

} // namespace hooks
