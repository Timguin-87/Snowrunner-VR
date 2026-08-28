#include "hooks/depth_probe.h"
#include "hooks/cbuffer_hook.h"
#include "xr/xr_mirror.h"
#include "common/dibr_policy.h"
#include "common/log.h"

#include <windows.h>
#include <wrl/client.h>
#include <atomic>
#include <vector>
#include <cmath>

#include <d3dcompiler.h>
#include <unordered_map>

#include <MinHook.h>

using Microsoft::WRL::ComPtr;

namespace hooks {
namespace {

constexpr int kIdxOMSetRenderTargets        = 33;  // ID3D11DeviceContext vtable
constexpr int kIdxOMSetRenderTargetsAndUAVs = 34;

std::atomic<bool> g_hooked{false};

// Discovered depth resources, deduped by texture identity. The main scene depth
// is the largest; shadow cascades and small utility targets also show up here
// and are worth logging precisely so we can tell them apart.
struct DepthTex {
    ComPtr<ID3D11Texture2D> tex;
    UINT   width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT   bindFlags = 0, miscFlags = 0, sampleCount = 1;
    bool   srvOk = false;             // could we actually create an SRV on it?
    DXGI_FORMAT srvFormat = DXGI_FORMAT_UNKNOWN;
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<DepthTex> g_depths;
constexpr size_t kMaxDepths = 16;

const char* fmt_name(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R32G8X24_TYPELESS:      return "R32G8X24_TYPELESS";
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:   return "D32_FLOAT_S8X24_UINT";
    case DXGI_FORMAT_R32_TYPELESS:           return "R32_TYPELESS";
    case DXGI_FORMAT_D32_FLOAT:              return "D32_FLOAT";
    case DXGI_FORMAT_R24G8_TYPELESS:         return "R24G8_TYPELESS";
    case DXGI_FORMAT_D24_UNORM_S8_UINT:      return "D24_UNORM_S8_UINT";
    case DXGI_FORMAT_R16_TYPELESS:           return "R16_TYPELESS";
    case DXGI_FORMAT_D16_UNORM:              return "D16_UNORM";
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return "R32_FLOAT_X8X24_TYPELESS";
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:  return "R24_UNORM_X8_TYPELESS";
    case DXGI_FORMAT_R32_FLOAT:              return "R32_FLOAT";
    case DXGI_FORMAT_R16_UNORM:              return "R16_UNORM";
    default:                                 return "(other)";
    }
}

// Depth resources must be TYPELESS to be readable as both DSV and SRV. A plain
// D24_UNORM_S8_UINT / D32_FLOAT resource cannot have an SRV at all -- that is
// the case that would force the CreateTexture2D bind-flag patch.
DXGI_FORMAT srv_format_for(DXGI_FORMAT resFormat)
{
    switch (resFormat) {
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:      return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:    return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:      return DXGI_FORMAT_R16_UNORM;
    default:                            return DXGI_FORMAT_UNKNOWN;   // not SRV-able
    }
}

void consider_depth(ID3D11Texture2D* tex)
{
    if (!tex) return;

    AcquireSRWLockShared(&g_lock);
    bool known = false;
    for (auto& d : g_depths) if (d.tex.Get() == tex) { known = true; break; }
    ReleaseSRWLockShared(&g_lock);
    if (known) return;

    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);

    DepthTex rec;
    rec.tex = tex;
    rec.width = d.Width; rec.height = d.Height; rec.format = d.Format;
    rec.bindFlags = d.BindFlags; rec.miscFlags = d.MiscFlags;
    rec.sampleCount = d.SampleDesc.Count;
    rec.srvFormat = srv_format_for(d.Format);

    // The decisive test: actually try to create the SRV rather than infer it.
    if ((d.BindFlags & D3D11_BIND_SHADER_RESOURCE) && rec.srvFormat != DXGI_FORMAT_UNKNOWN) {
        ComPtr<ID3D11Device> dev;
        tex->GetDevice(&dev);
        if (dev) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = rec.srvFormat;
            sd.ViewDimension = (d.SampleDesc.Count > 1) ? D3D11_SRV_DIMENSION_TEXTURE2DMS
                                                        : D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
            ComPtr<ID3D11ShaderResourceView> srv;
            rec.srvOk = SUCCEEDED(dev->CreateShaderResourceView(tex, &sd, &srv));
        }
    }

    AcquireSRWLockExclusive(&g_lock);
    if (g_depths.size() >= kMaxDepths) { ReleaseSRWLockExclusive(&g_lock); return; }
    g_depths.push_back(rec);
    int idx = (int)g_depths.size() - 1;
    ReleaseSRWLockExclusive(&g_lock);

    VRLOG("DEPTH #%d: %ux%u fmt=%s(%d) samples=%u bind=0x%X%s%s misc=0x%X SRV=%s%s",
          idx, rec.width, rec.height, fmt_name(rec.format), (int)rec.format,
          rec.sampleCount, rec.bindFlags,
          (rec.bindFlags & D3D11_BIND_DEPTH_STENCIL)   ? " DS"  : "",
          (rec.bindFlags & D3D11_BIND_SHADER_RESOURCE) ? " SR"  : "",
          rec.miscFlags,
          rec.srvOk ? "OK as " : "NO",
          rec.srvOk ? fmt_name(rec.srvFormat) : "");
}

void note_dsv(ID3D11DepthStencilView* dsv)
{
    if (!dsv) return;
    ComPtr<ID3D11Resource> res;
    dsv->GetResource(&res);
    ComPtr<ID3D11Texture2D> tex;
    if (res && SUCCEEDED(res.As(&tex)))
        consider_depth(tex.Get());
}

// --- scene-pass-end capture -------------------------------------------------
// Depth reads all-zero at Present: it is cleared or reused by post-processing
// before the frame ends. So instead of sampling at Present we snapshot at the
// moment the main scene's depth target is UNBOUND, which is when it holds the
// finished scene. Capturing from whatever is actually bound (rather than a
// cached pointer) also covers the case where the engine rotates between several
// depth instances across frames.

// Currently bound DSV resource, PER CONTEXT.
//
// THIS IS THE FIX for the occasional DIBR shift frame that ran the warp with every
// guard satisfied and still produced no DIBR shift. It was a single global,
// which looked harmless because only the immediate context is ever captured
// from -- but the OMSetRenderTargets detours run on ALL contexts, and every
// deferred bind overwrote it too. Measured: 146-184 deferred depth binds per
// frame against 53-61 immediate ones, so the "previously bound" texture handed
// to capture_if_pass_ended() was usually one a deferred context had touched
// rather than one the immediate context had just finished with. We sampled
// textures at arbitrary moments and got whatever the engine had left there,
// which under load meant a buffer already cleared and not yet refilled.
//
// That accounts for the whole symptom set: an accumulator with 50 captures in
// it reading cov=0.000; PARTIAL failures where only the view through the
// windscreen stayed flat because just some passes had landed; and the
// correlation with frame rate, since heavier frames mean more deferred traffic
// clobbering the global.
//
// Each context now tracks its own binding, so a transition means what it says.
// Contexts are few (one immediate plus a worker's worth of deferred), so a
// small linear table under a lock costs less than a hash lookup here.
// Set while the mod is binding its own render targets -- see set_self_targets().
// Thread-local: deferred contexts record on worker threads, and one thread's
// capture pass must not blind another's.
thread_local bool t_selfTargets = false;

constexpr int kMaxCtx = 16;
struct CtxDepth { ID3D11DeviceContext* ctx; ID3D11Texture2D* bound; };
CtxDepth g_ctxDepth[kMaxCtx] = {};
SRWLOCK  g_ctxLock = SRWLOCK_INIT;

// Returns the depth previously bound on `ctx` and records the new one.
ID3D11Texture2D* swap_bound_depth(ID3D11DeviceContext* ctx, ID3D11Texture2D* incoming)
{
    ID3D11Texture2D* prev = nullptr;
    AcquireSRWLockExclusive(&g_ctxLock);
    int free = -1;
    for (int i = 0; i < kMaxCtx; ++i) {
        if (g_ctxDepth[i].ctx == ctx) {
            prev = g_ctxDepth[i].bound;
            g_ctxDepth[i].bound = incoming;
            ReleaseSRWLockExclusive(&g_ctxLock);
            return prev;
        }
        if (!g_ctxDepth[i].ctx && free < 0) free = i;
    }
    if (free >= 0) {
        g_ctxDepth[free] = CtxDepth{ctx, incoming};
    } else {
        // Full: further contexts get no tracking, so they never report a
        // transition and simply never capture. Safe, but worth knowing about --
        // if the immediate context ever lost its slot the warp would starve.
        static bool warned = false;
        if (!warned) { warned = true;
            VRLOG("depth probe: context table full (%d) -- extra contexts will not "
                  "be tracked; raise kMaxCtx", kMaxCtx); }
    }
    ReleaseSRWLockExclusive(&g_ctxLock);
    return nullptr;
}

// CAPTURING FROM DEFERRED CONTEXTS WAS TRIED HERE AND DOES NOT WORK. Most of
// the depth work is deferred (measured: 146-184 deferred depth unbinds a frame
// against 53-61 immediate), so folding it in looks attractive -- but enabling it
// jitters every view. The accumulation window's bookkeeping (firstOfFrame,
// camJumped, slot choice) is evaluated when a command list is RECORDED, which
// can be an arbitrary distance from when it RUNS, so windows get started and
// restarted against a camera unrelated to the depth being folded in. max() is
// order-insensitive so the accumulation itself survives, but the window
// BOUNDARIES do not -- and those are what keep two viewpoints out of one
// buffer. It would need the camera and eye recorded into the list beside the
// dispatch rather than read from globals at record time.
//
// It is also not needed: the blank-accumulator bug was the shared g_boundDepth
// described above, not the missing deferred passes.

// Camera world position the current accumulation window belongs to.
//
// THE WINDOW IS ONE VIEWPOINT, and this is what enforces it. The window folds
// every scene-sized depth pass of a frame together with max(), which is only
// valid while they share a camera. They do not: a frame's trailing depth passes
// unbind AFTER the next frame's camera has been baked, so passes taken an eye
// separation apart landed in one window and max() kept the nearer of the two --
// putting near geometry into the depth TWICE. Seen as doubled handles and a
// pipe measuring 92px wide with one flat plateau, worst looking back along the
// truck, where a frame runs 20-30 depth passes against the 1-2 of an orbit
// frame and the odds of straddling the boundary rise with it.
//
// applied_eye() cannot catch this: it is published at camera BUILD time and has
// already moved on by the time those trailing passes unbind. The camera
// position here comes from the same view matrix the pass rendered with, so it
// cannot be out of step with the depth it describes.
//
// Ruled out on the way, each by measurement: not two depth TARGETS (cockpit
// uses two, and restricting to the main one alone left the doubling intact),
// not the eye label, not the pass count, and not the game's depth clears --
// keying the window on those broke DIBR shift outright, since the buffer is cleared
// several times a frame.
float g_windowCamPos[3] = {};
bool  g_windowCamValid = false;
// An eye separation is ~0.06 m and genuine within-frame camera motion is zero,
// so anything above this is a viewpoint change rather than the same view being
// drawn again. Verified in-headset: fixes the doubling in every view while
// keeping the accumulation that cockpit-forward depends on.
constexpr float kCamSpreadLimit = 0.001f;

std::atomic<UINT> g_renderW{0}, g_renderH{0};   // backbuffer size = main scene size

// Below this the target is a utility/shadow depth, not the main scene's --
// the scene depth is backbuffer-sized, and everything smaller is something we
// must not capture or accumulate.
constexpr UINT64 kMinDepthArea = 1000000ull;

// --- Phase 1: scene depth acquisition (shader-based) -------------------------
// A raw CopyResource of the depth-stencil resource into a DEFAULT texture reads
// back EMPTY, even though the identical copy into a STAGING texture (the probe's
// ring) reads back valid from the very same source in the same call. Rather than
// keep fighting that, sample the depth through an SRV in a compute shader and
// write plain R32_FLOAT. That avoids depth-stencil copy rules entirely, drops
// the unused stencil (half the bandwidth), and hands the warp exactly the format
// it wants.
//
// Compute is used deliberately: it leaves the graphics pipeline (render targets,
// viewport, VS/PS) untouched, so running mid-frame does not disturb the game's
// own rendering. Only CS bindings are clobbered, and they are cleared after.
const char kDepthCopyHlsl[] = R"(
Texture2D<float>    SrcDepth : register(t0);
RWTexture2D<float>  OutDepth : register(u0);
[numthreads(8, 8, 1)]
void CS_CopyDepth(uint3 tid : SV_DispatchThreadID)
{
    uint w, h;
    OutDepth.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) return;
    // MAX-accumulate across every capture in the frame's tail. Some of those
    // transitions hand us a buffer the engine has already cleared (seen in
    // cockpit: the second-to-last source reads all zeros). Overwriting meant the
    // final capture could be one of those, wiping a good one. Under reverse-Z a
    // cleared texel is 0.0 = infinitely far, so it can never beat real depth --
    // max() therefore picks whichever source actually held the scene, without
    // needing to know which transition that was.
    OutDepth[tid.xy] = max(OutDepth[tid.xy], SrcDepth[tid.xy]);
}
)";

ComPtr<ID3D11ComputeShader> g_csCopyDepth;
// TWO accumulators, swapped once per Present.
//
// MEASURED RACE. depth_probe_on_present() re-arms g_clearedThisFrame at
// swapchain_hook.cpp:151, but the warp does not sample the depth until
// xr::mirror_on_present() at :168. A scene-depth unbind arriving on the render
// thread in that gap sees firstOfFrame == true and clears the accumulator --
// the very buffer the warp is about to read. Caught in a frame dump: a frame
// with caps=40/52 (a full, healthy window, same as its neighbours) and
// cov=0.000 max=0.00000, every warp parameter identical to the frames either
// side. Zero depth means z = infinity everywhere, so the disparity rounds to
// nothing and the synthesized eye comes out as an unshifted copy -- one frame
// of plain AER inside DIBR shift, which reads as a jitter in one eye. It tracks with
// frame-rate dips because that is what widens the gap.
//
// Moving the re-arm later is not enough: camJumped clears mid-frame as well.
// The buffer simply has to stop being shared. The render thread now writes and
// clears ONE slot, the warp reads the OTHER, and they trade at Present.
//
// NOT the ping-pong that was reverted earlier. That one swapped on every window
// RESTART -- several times a frame -- and then chose a slot by comparing eye
// labels at Present, so a label that was out of step handed the warp a stale
// window and produced jitter of its own. This swaps exactly ONCE per frame and
// has no selection rule at all: the warp always reads the slot that was being
// filled during the frame being presented.
// A RING, NOT A PING-PONG.
//
// Two slots could only ever offer the warp "the newest capture", and the newest
// capture belongs to whichever eye the game happened to render last. When that
// disagreed with the eye in the backbuffer being presented, the warp had no
// option but to decline -- and those declines DOMINATED the decline path,
// thousands per session, because the disagreement is structural rather than
// exceptional (depth is captured mid-frame; the eye identity is read at
// Present).
//
// With a ring the warp asks for the eye it actually needs and gets it if any
// recent capture matches, which converts those declines directly into warped
// frames. Four is chosen so both eyes can be resident with a spare either side
// for the in-flight capture; a capture older than the ring is worthless anyway.
//
// This is the local equivalent of Witcher 3 VR's producer bundle -- see
// docs/dibr_shift_comparison_witcher3.md section 2.1. Their D3D12 version needs a
// state machine and cross-queue fences; on the D3D11 immediate context at
// Present, selection by identity is the whole of it.
constexpr int kDepthSlots = 4;
ComPtr<ID3D11UnorderedAccessView> g_depthUAV[kDepthSlots];
// The slot the capture path owns. Only ever advanced at Present.
std::atomic<int> g_captureSlot{0};
// The slot the warp reads for the frame being presented.
std::atomic<int> g_warpSlot{0};
// SRVs on the game's depth textures, cached by resource pointer.
std::unordered_map<ID3D11Texture2D*, ComPtr<ID3D11ShaderResourceView>> g_srcDepthSRVs;

ID3D11ShaderResourceView* src_depth_srv(ID3D11Device* dev, ID3D11Texture2D* tex,
                                        const D3D11_TEXTURE2D_DESC& d)
{
    auto it = g_srcDepthSRVs.find(tex);
    if (it != g_srcDepthSRVs.end()) return it->second.Get();

    DXGI_FORMAT f = srv_format_for(d.Format);
    if (f == DXGI_FORMAT_UNKNOWN) return nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = f;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(dev->CreateShaderResourceView(tex, &sd, &srv))) {
        VRLOG("depth acquire: source SRV creation FAILED (%s)", fmt_name(d.Format));
        return nullptr;
    }
    g_srcDepthSRVs.emplace(tex, srv);
    return srv.Get();
}

bool ensure_copy_shader(ID3D11Device* dev)
{
    if (g_csCopyDepth) return true;
    ComPtr<ID3DBlob> code, err;
    if (FAILED(D3DCompile(kDepthCopyHlsl, sizeof(kDepthCopyHlsl) - 1, "depthcopy",
                          nullptr, nullptr, "CS_CopyDepth", "cs_5_0", 0, 0, &code, &err))) {
        VRLOG("depth acquire: shader compile FAILED: %s",
              err ? (const char*)err->GetBufferPointer() : "(no message)");
        return false;
    }
    if (FAILED(dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(),
                                        nullptr, &g_csCopyDepth))) {
        VRLOG("depth acquire: CreateComputeShader FAILED");
        return false;
    }
    VRLOG("depth acquire: R32_FLOAT copy shader ready");
    return true;
}

// Measured frame structure (stable across runs): the scene depth is complete
// only AFTER the shadow cascade atlas passes. Depth written before that is
// still accumulating, and by Present the buffer has been cleared. So we take
// exactly one copy per frame: the first scene-sized depth unbind that happens
// after a shadow-atlas unbind. Verified byte-identical to the last transition
// of the frame, so it is the finished scene depth.
ComPtr<ID3D11Texture2D>          g_depthCopy[kDepthSlots];  // DEFAULT + SHADER_RESOURCE, ours
ComPtr<ID3D11ShaderResourceView> g_depthSRV[kDepthSlots];
UINT g_depthW = 0, g_depthH = 0;
// Scene depth is only complete near the END of the frame, so we must capture
// late -- but "late" cannot be keyed off the shadow atlas: SnowRunner renders
// shadow cascades at a REDUCED RATE, so on most frames that marker never
// appears and nothing was captured at all (measured: ~90% of frames missed,
// leaving the warp on stale wrong-eye depth).
//
// Instead, count scene-sized depth unbinds and copy only for the last few,
// using the previous frame's count as the predictor. Self-calibrating, costs a
// handful of copies per frame, and does not care what else the frame did.
std::atomic<int> g_sceneTransIdx{0};        // scene-sized unbinds so far this frame
std::atomic<int> g_lastSceneTransCount{0};  // how many the previous frame had
// How many trailing scene-sized unbinds to accumulate. Max-accumulation makes
// a WIDER window strictly safer: under reverse-Z, depth only ever increases as
// the frame draws (nearer geometry wins) and cleared sources contribute 0, so
// extra captures can only add information, never destroy it. Too NARROW a window
// is the failure mode -- if it lands entirely on already-cleared sources the
// frame yields nothing, which is the residual cockpit flicker.
constexpr int kCaptureTail = 10;
// Self-correcting capture threshold. Deriving it purely from the previous
// frame's count is unsafe: when the count SHRINKS, the window lands past the
// end of the frame and nothing is captured at all -- the buffer then keeps
// whatever it last held, which may be a cleared one. That is exactly what made
// cockpit fail while orbit (identical transition structure) worked. So: on any
// frame that captured nothing, halve the threshold. It converges back to the
// tail on its own and cannot get stuck beyond the end.
std::atomic<int> g_captureThreshold{0};
std::atomic<bool> g_capturedThisFrame{false};  // did THIS frame capture?
std::atomic<bool> g_clearedThisFrame{false};   // depth copy zeroed for accumulation
// Eye offset that was baked into the frame this depth belongs to, snapshotted at
// capture time (the camera build always precedes the geometry pass that fills
// depth, so this is the right frame's value).
std::atomic<float> g_depthEyeOffset{0.0f};
std::atomic<int>   g_depthEye{-1};

// What each slot holds, so a consumer can ask for an eye rather than be handed
// whatever was newest.
//
// THE CAMERA IS FROZEN WITH THE DEPTH, which closes a real hole: the projection
// used to be read live at Present while the depth had been copied mid-frame, so
// during an FOV animation or a camera transition the two described different
// cameras -- and since the disparity is built from both together, neither input
// could reveal the error on its own.
struct SlotTag {
    std::atomic<bool>  valid{false};      // holds a completed capture
    std::atomic<bool>  mixed{false};      // window spanned two viewpoints
    std::atomic<int>   eye{-1};
    std::atomic<float> offset{0.0f};
    std::atomic<float> projA{0.0f};
    std::atomic<float> projB{0.1f};
    std::atomic<float> projP00{0.73454f};
    std::atomic<bool>  projValid{false};
    std::atomic<uint64_t> serial{0};      // capture order, for "newest match"
};
SlotTag g_slotTag[kDepthSlots];
std::atomic<uint64_t> g_captureSerial{0};
// The slot the last scene_depth_for_eye() handed out, so the frame-dump
// diagnostic reports the buffer the warp actually consumed.
std::atomic<int> g_lastServedSlot{0};

std::atomic<int>  g_missedFrames{0};           // frames that produced no capture

// Eye of the first capture in the current accumulation window, and whether a
// later capture in the same window came from the OTHER eye (see the clear site).
// A mixed window is unusable: it merges two viewpoints one IPD apart.
std::atomic<int>   g_windowEye{-1};
std::atomic<bool>  g_depthMixed{false};
std::atomic<int>   g_mixedFrames{0};
std::atomic<int>   g_windowRestarts{0};   // eye changed mid-frame -> window restarted
// How many captures the SURVIVING accumulation window received, PER SLOT --
// so "the data went into the other one" can be told apart from "the data was
// never captured". Reset when that slot is cleared.
//
// Strictly diagnostic; nothing branches on it. (An earlier experiment DID
// branch on a count like this, to reject late window jumps, and broke DIBR
// shift three different ways. This is only ever read into a log line.)
//
// It is the direct measure of the "the window was cut short" theory: a frame
// whose depth passes numbered 20-30 but whose surviving window holds 1 is a
// fragment, whatever the depth itself turns out to contain.
std::atomic<int>   g_slotCaps[2] = {};
// Scene-depth unbinds dropped this frame for being on a DEFERRED context. Those
// passes are never captured at all -- if the engine moves the world pass onto a
// worker context under load while the cockpit pass stays on the immediate one,
// the world's depth is simply absent and everything beyond the windscreen keeps
// a depth of 0, i.e. no disparity and no shift, while the cab shifts correctly.
std::atomic<int>   g_deferredFrame{0};
std::atomic<int>   g_deferredForWarp{0};
// WHY the window restarted, counted per frame. There are three reasons and they
// mean very different things: firstOfFrame is normal and happens once, while a
// storm of eyeChanged or camJumped restarts shreds the window into fragments of
// one or two captures -- which is a completely different failure from the
// window being full but blank. g_lastCamMove carries the distance that tripped
// the camera test, so a threshold that is simply too tight can be told from a
// camera genuinely jumping.
std::atomic<int>   g_rsFirst{0}, g_rsEye{0}, g_rsCam{0};
// Scene-sized unbinds that happened while the immediate context's last camera
// commit was NOT the main camera. EVIDENCE ONLY since 2026-08-25 -- these used
// to be refused, and the refusal is what the note in capture_if_pass_ended()
// describes retiring. Kept counted because it is still the only measurement we
// have of whether a reflection pass can reach the accumulator.
std::atomic<int>   g_otherCamPasses{0};
// Total accumulated, so the count above can be read as a ratio rather than as
// a number whose scale depends on how many passes a frame happens to have.
std::atomic<int>   g_scenePasses{0};
// The pass index within a frame at which the non-main commit first shows up.
// It is the ordering, not the count, that decides whether a per-pass fix is
// reachable at all: a commit that lands before the scene passes cannot be told
// apart from one that owns them by anything a latch can see.
std::atomic<int>   g_otherCamFirstIdx{-1};      // this frame
std::atomic<int>   g_otherCamLastFirstIdx{-1};  // last frame that had one
std::atomic<int>   g_rsFirstW{0}, g_rsEyeW{0}, g_rsCamW{0};
std::atomic<float> g_lastCamMove{0.0f};
std::atomic<int>  g_totalFrames{0};
std::atomic<bool> g_depthValid{false};         // copy holds usable depth (persists)

bool ensure_depth_copy(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& src)
{
    static DXGI_FORMAT s_fmt = DXGI_FORMAT_UNKNOWN;
    static UINT s_samples = 0;
    if (g_depthCopy[0] && g_depthCopy[1] && g_depthW == src.Width && g_depthH == src.Height &&
        s_fmt == src.Format && s_samples == src.SampleDesc.Count)
        return true;
    s_fmt = src.Format; s_samples = src.SampleDesc.Count;

    for (int i = 0; i < kDepthSlots; ++i) { g_depthSRV[i].Reset(); g_depthCopy[i].Reset(); }

    // Written by a compute shader that SAMPLES the source depth, not by
    // CopyResource -- so the format is free to be plain R32_FLOAT (depth only,
    // no stencil, half the bandwidth). Note this only works because the copy is
    // a shader write; a CopyResource here would need to match the source format
    // exactly or it fails silently.
    D3D11_TEXTURE2D_DESC d{};
    d.Width = src.Width; d.Height = src.Height;
    d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R32_FLOAT;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    d.CPUAccessFlags = 0;
    d.MiscFlags = 0;
    for (int i = 0; i < kDepthSlots; ++i) {
        g_depthUAV[i].Reset();
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &g_depthCopy[i]))) {
            VRLOG("depth acquire: texture creation FAILED (%s)", fmt_name(src.Format));
            return false;
        }
        if (FAILED(dev->CreateShaderResourceView(g_depthCopy[i].Get(), nullptr, &g_depthSRV[i])) ||
            FAILED(dev->CreateUnorderedAccessView(g_depthCopy[i].Get(), nullptr, &g_depthUAV[i]))) {
            VRLOG("depth acquire: SRV/UAV creation FAILED");
            for (int j = 0; j < kDepthSlots; ++j) {
                g_depthCopy[j].Reset(); g_depthSRV[j].Reset(); g_depthUAV[j].Reset();
            }
            return false;
        }
    }

    g_depthW = src.Width; g_depthH = src.Height;
    VRLOG("depth acquire: %ux%u %s -> R32_FLOAT via compute shader",
          g_depthW, g_depthH, fmt_name(src.Format));
    return true;
}

// Called AFTER the real OMSetRenderTargets, so the old target is already unbound
// and safe to copy.
// GATED, because the captured depth has exactly two consumers and neither is
// on by default. It used to run unconditionally, so every install paid for a
// full-res R32_FLOAT copy plus a compute dispatch on EVERY scene-sized depth
// unbind, every frame, with nothing reading the result.
//
// DIBR shift consumes it every frame -- xr_mirror.cpp's single
// scene_depth_for_eye() call site sits inside its branch. The 6-DoF stale-eye
// warp consumes it too, and needs it just as much: with the shift off, that
// correction covers the WHOLE synthesized eye rather than a few disocclusion
// holes. Leaving the second term out is what made the log read
// "depth acquire: 600/600 frames MISSED" while the reprojection silently
// declined for want of an input nothing was producing.
bool depth_capture_wanted()
{
    return xr::dibr_shift_enabled() ||
           (xr::warp_uses_6dof() && xr::warp_enabled());
}

void capture_if_pass_ended(ID3D11DeviceContext* ctx, ID3D11Texture2D* previouslyBound)
{
    if (!previouslyBound || !ctx) return;
    if (!depth_capture_wanted()) return;

    // CopyResource on a DEFERRED context is recorded into a command list, not
    // executed: it runs later (and possibly repeatedly) at ExecuteCommandList
    // time, by which point the engine may have cleared the buffer. That yields a
    // copy that appears to succeed but contains cleared depth -- which is
    // exactly the observed failure. Only the immediate context is safe here.
    if (ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        g_deferredFrame.fetch_add(1);
        return;
    }

    D3D11_TEXTURE2D_DESC d{};
    previouslyBound->GetDesc(&d);
    if ((UINT64)d.Width * d.Height < kMinDepthArea || d.SampleDesc.Count > 1) return;

    // THE SCENE DEPTH IS THE RENDER SIZE ROUNDED UP, not exactly equal to it.
    //
    // The engine aligns its internal targets, so a 1902x977 swapchain renders
    // the world into 1904x980 -- and an exact-equality test rejects that
    // forever. In a headset the forced resolution is already aligned
    // (2880x2880 matches itself), which is why this only ever showed up in a
    // window. The slack is tight and one-directional: LARGER than the render
    // size, by less than one alignment step, in BOTH dimensions.
    constexpr UINT kSizeSlack = 16;
    const UINT rw = g_renderW.load(), rh = g_renderH.load();
    const bool withinSlack = rw && rh &&
                             d.Width  >= rw && d.Width  - rw < kSizeSlack &&
                             d.Height >= rh && d.Height - rh < kSizeSlack;

    // THEN LOCK ONTO EXACTLY ONE OF THEM, because the slack admits more than
    // one buffer and accepting both is worse than accepting neither.
    //
    // At a 1902x977 swapchain both a 1904x980 and a 1902x977 depth buffer
    // qualify. They then alternate every frame, and ensure_depth_copy()
    // reallocates all four ring slots whenever the dimensions change -- four
    // full-res R32_FLOAT textures with SRV and UAV destroyed and recreated
    // twice a frame, with the ring wiped before anything can read it.
    // (Measured 2026-08-20: 14,304 allocations in one session.)
    //
    // LARGEST WINS. The true scene target is the one aligned UP from the
    // window; a buffer at exactly the swapchain size is more likely a post or
    // UI pass. Re-locking only ever moves upward, so it converges once and then
    // holds, and the lock is dropped when the render size itself changes.
    static UINT s_lockW = 0, s_lockH = 0;
    static UINT s_lockForW = 0, s_lockForH = 0;
    if (s_lockForW != rw || s_lockForH != rh) {      // render size changed
        s_lockW = s_lockH = 0;
        s_lockForW = rw; s_lockForH = rh;
    }
    if (withinSlack && (UINT64)d.Width * d.Height > (UINT64)s_lockW * s_lockH) {
        if (s_lockW)
            VRLOG("depth probe: scene depth %ux%u -> %ux%u (larger candidate "
                  "within the alignment window)", s_lockW, s_lockH, d.Width, d.Height);
        else
            VRLOG("depth probe: scene depth locked to %ux%u (render %ux%u)",
                  d.Width, d.Height, rw, rh);
        s_lockW = d.Width; s_lockH = d.Height;
    }
    const bool sceneSized = withinSlack &&
                            d.Width == s_lockW && d.Height == s_lockH;

    // --- Phase 1 acquisition: copy for the last few scene-sized unbinds ---
    if (sceneSized) {
        const int idx = g_sceneTransIdx.fetch_add(1);
        g_scenePasses.fetch_add(1, std::memory_order_relaxed);

        // WHY THIS NO LONGER REFUSES ANYTHING, 2026-08-25.
        //
        // It used to: a scene-sized pass whose most recent camera commit was
        // not the main camera was dropped, on the theory that a reflection pass
        // is another viewpoint with its own depth, and max() would then keep
        // whichever surface is nearer -- putting reflected geometry onto scene
        // pixels. That theory is still plausible and still unproven.
        //
        // What IS proven is that the test cannot express it. The measurement:
        //
        //   depth acquire: 3855/600 scene-sized passes refused as another
        //                  camera's (600 non-main camera commits seen)
        //
        // One non-main commit per frame, exactly, and 6.4 refusals per frame --
        // the whole frame's accumulation, main view included. Because
        // imm_camera_is_main() is a LATCH on the last commit, not a fact about
        // this pass: once a non-main camera commits, every pass after it is
        // refused until a main camera CB commits again. Nothing here can tell
        // "this pass belongs to that camera" from "this pass merely happened
        // after it".
        //
        // It also explains why the damage was zoom-dependent. The commit count
        // does not change with zoom; the ORDERING does. Zoomed out, something
        // that commits its own camera mid-frame (a water plane, a distant
        // probe) comes into view and that commit moves from after the scene
        // passes to before them, which is when everything downstream dies. The
        // symptom was the 6-DoF warp and DIBR shift switching themselves off
        // with the orbit camera pulled back.
        //
        // So the guard is gone and the counters stay. A per-pass version needs
        // the pass attributed to a camera, which is exactly what we do not have
        // -- and g_otherCamFirstIdx below is the measurement that says whether
        // it is worth trying.
        if (!hooks::imm_camera_is_main()) {
            g_otherCamPasses.fetch_add(1, std::memory_order_relaxed);
            int none = -1;
            g_otherCamFirstIdx.compare_exchange_strong(none, idx,
                                                       std::memory_order_relaxed);
        }
        // Accumulate over EVERY scene-sized unbind, not a trailing window.
        // With max() this is correct by construction: reverse-Z depth only rises
        // as a frame draws (nearer geometry wins), so folding in the early
        // partially-drawn passes cannot change the final result -- but it does
        // guarantee we can never miss the frame's real depth, which is what a
        // trailing window kept doing when it landed on cleared sources.
        // Cost is the trade here (a dispatch per unbind); half-res accumulation
        // is the obvious optimisation once this is proven correct.
        if (idx >= 0) {
        ComPtr<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        ID3D11ShaderResourceView* srcSRV = nullptr;
        if (dev && ensure_copy_shader(dev.Get()) && ensure_depth_copy(dev.Get(), d))
            srcSRV = src_depth_srv(dev.Get(), previouslyBound, d);
        if (srcSRV) {
            // Sample the depth through an SRV and write R32_FLOAT. Compute is
            // used so the graphics pipeline (render targets, viewport, VS/PS) is
            // left untouched -- safe to run mid-frame inside the game's own
            // rendering. Only CS bindings are disturbed, and they are cleared
            // immediately afterwards.
            // Zero the accumulator on the frame's first capture so max() starts
            // from "infinitely far" rather than last frame's content.
            // Eye the camera baked into the content being captured. Under
            // deferred contexts and worker threads a frame's depth captures can
            // straddle our Present boundary, so one accumulation window can hold
            // captures from BOTH eyes. max() then merges two viewpoints that are
            // one eye separation apart, and the nearer surface wins -- putting
            // e.g. the A-pillar's depth onto pixels where this eye sees the
            // background. The warp reads too-near depth there and overshoots,
            // which is the residual per-frame hiccup on window posts and door
            // edges (measured: +56 px excess disparity on affected tiles, with
            // distant content untouched).
            //
            // RESTART rather than discard. Marking the window mixed threw away the
            // WHOLE frame's depth, so the warp was skipped outright -- visible as
            // the disparity view dropping out entirely on ~7% of frames. Since the
            // eye can only change once per frame, clearing and restarting the
            // window at the change keeps every capture that belongs to the CURRENT
            // eye and drops only the stale ones. A window then never spans two
            // viewpoints by construction, so the mixed flag should stay near zero;
            // if it does not, an eye is changing more than once per frame and that
            // is a different bug worth knowing about.
            // Compared by EYE INDEX now, not by the sign of the offset. The
            // index is published in the same atomic as the offset, so this is an
            // exact identity test instead of a proxy that a zero or a
            // sign-convention change could confuse.
            //
            // The window restarts on a VIEWPOINT change, measured from the
            // camera the pass actually rendered with -- see the note on
            // g_windowCamPos. Two earlier boundaries were tried and reverted:
            // keying it on Present alone let a frame's trailing passes land in
            // the next eye's window, and keying it on the game's depth clears
            // broke DIBR shift outright, because the engine clears that buffer several
            // times a frame and the window kept only the last partial pass.
            // Camera world position for THIS capture, from the game's own view
            // matrix: rotation is the upper-left 3x3, so the world position is
            // -R^T * t. Derived from the same snapshot dibr_projection uses.
            float camPos[3] = {0, 0, 0};
            bool  camOk = false;
            {
                float view[16], vp[16];
                if (hooks::main_camera_matrices(view, vp)) {
                    auto V = [&](int r, int c) { return view[r*4+c]; };
                    for (int r = 0; r < 3; ++r)
                        camPos[r] = -(V(0,r)*V(0,3) + V(1,r)*V(1,3) + V(2,r)*V(2,3));
                    camOk = true;
                }
            }
            float camMove = 0.0f;
            if (camOk && g_windowCamValid) {
                const float dx = camPos[0] - g_windowCamPos[0];
                const float dy = camPos[1] - g_windowCamPos[1];
                const float dz = camPos[2] - g_windowCamPos[2];
                camMove = std::sqrt(dx*dx + dy*dy + dz*dz);
            }
            const bool  camJumped = camOk && g_windowCamValid &&
                                    (camMove > kCamSpreadLimit);

            const xr::AppliedEye ae = xr::applied_eye();
            const bool  firstOfFrame  = !g_clearedThisFrame.exchange(true);
            const bool  eyeChanged    = !firstOfFrame && ae.valid &&
                                        (ae.eye != g_windowEye.load());
            // Read ONCE and used for both the clear and the dispatch, so a swap
            // landing between them cannot split this capture across two slots.
            const int slot = g_captureSlot.load();

            if (firstOfFrame || eyeChanged || camJumped) {
                const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                ctx->ClearUnorderedAccessViewFloat(g_depthUAV[slot].Get(), zero);
                g_windowEye.store(ae.valid ? ae.eye : -1);
                g_windowCamValid = false;
                g_slotCaps[slot].store(0);
                if (firstOfFrame) g_rsFirst.fetch_add(1);
                if (eyeChanged)   g_rsEye.fetch_add(1);
                if (firstOfFrame) g_depthMixed.store(false);
                if (camJumped)  { g_rsCam.fetch_add(1); g_lastCamMove.store(camMove); }
                if (eyeChanged)   g_windowRestarts.fetch_add(1);
            }

            if (camOk) {
                g_windowCamPos[0] = camPos[0]; g_windowCamPos[1] = camPos[1];
                g_windowCamPos[2] = camPos[2]; g_windowCamValid = true;
            }

            ID3D11ShaderResourceView* srvs[1] = {srcSRV};
            ID3D11UnorderedAccessView* uavs[1] = {g_depthUAV[slot].Get()};
            ctx->CSSetShader(g_csCopyDepth.Get(), nullptr, 0);
            ctx->CSSetShaderResources(0, 1, srvs);
            ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
            ctx->Dispatch((d.Width + 7) / 8, (d.Height + 7) / 8, 1);
            ID3D11ShaderResourceView*  nsrv[1] = {nullptr};
            ID3D11UnorderedAccessView* nuav[1] = {nullptr};
            ctx->CSSetShaderResources(0, 1, nsrv);
            ctx->CSSetUnorderedAccessViews(0, 1, nuav, nullptr);
            ctx->CSSetShader(nullptr, nullptr, 0);

            g_slotCaps[slot].fetch_add(1);
            g_capturedThisFrame.store(true);
            g_depthValid.store(true);
            // The eye this depth belongs to, carried alongside it so the warp
            // can refuse a colour/depth pair that came from different eyes.
            g_depthEye.store(ae.valid ? ae.eye : -1);
            g_depthEyeOffset.store(ae.offset);
        }
        }
    }
}

using PFN_OMSetRenderTargets = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
// Immediate and deferred contexts use DISTINCT vtables (see ui_hook.cpp's
// VTABLE PROBE finding) -- separate original-function pointers per context
// type, or the second MH_CreateHook would clobber the first's.
PFN_OMSetRenderTargets real_OMSetRenderTargets_imm = nullptr, real_OMSetRenderTargets_def = nullptr;

// Resolves the DSV's underlying texture without holding a reference (the raw
// pointer is only compared, and used for a copy immediately afterwards).
ID3D11Texture2D* dsv_texture(ID3D11DepthStencilView* dsv)
{
    if (!dsv) return nullptr;
    ComPtr<ID3D11Resource> res;
    dsv->GetResource(&res);
    ComPtr<ID3D11Texture2D> tex;
    if (res && SUCCEEDED(res.As(&tex))) return tex.Get();
    return nullptr;
}

void STDMETHODCALLTYPE Detour_OMSetRenderTargets(
    ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs,
    ID3D11DepthStencilView* dsv)
{
    const bool imm = ctx->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
    if (t_selfTargets) {
        // Our own bind. Pass it through untouched and record nothing, so the
        // tracked binding still describes the GAME's state when we restore it.
        (imm ? real_OMSetRenderTargets_imm : real_OMSetRenderTargets_def)(ctx, n, rtvs, dsv);
        return;
    }

    note_dsv(dsv);
    ID3D11Texture2D* incoming = dsv_texture(dsv);
    ID3D11Texture2D* previous = swap_bound_depth(ctx, incoming);

    (imm ? real_OMSetRenderTargets_imm : real_OMSetRenderTargets_def)(ctx, n, rtvs, dsv);

    // Transition away from the scene depth == the scene pass just finished.
    if (previous && previous != incoming)
        capture_if_pass_ended(ctx, previous);
}

using PFN_OMSetRTAndUAV = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*,
    UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
PFN_OMSetRTAndUAV real_OMSetRTAndUAV_imm = nullptr, real_OMSetRTAndUAV_def = nullptr;

void STDMETHODCALLTYPE Detour_OMSetRTAndUAV(
    ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs,
    ID3D11DepthStencilView* dsv, UINT uavStart, UINT numUAVs,
    ID3D11UnorderedAccessView* const* uavs, const UINT* counts)
{
    const bool imm = ctx->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
    if (t_selfTargets) {
        (imm ? real_OMSetRTAndUAV_imm : real_OMSetRTAndUAV_def)(ctx, n, rtvs, dsv, uavStart, numUAVs, uavs, counts);
        return;
    }

    note_dsv(dsv);
    ID3D11Texture2D* incoming = dsv_texture(dsv);
    ID3D11Texture2D* previous = swap_bound_depth(ctx, incoming);

    (imm ? real_OMSetRTAndUAV_imm : real_OMSetRTAndUAV_def)(ctx, n, rtvs, dsv, uavStart, numUAVs, uavs, counts);

    if (previous && previous != incoming)
        capture_if_pass_ended(ctx, previous);
}

} // namespace

bool install_depth_probe(IDXGISwapChain* swapchain)
{
    bool expected = false;
    if (!g_hooked.compare_exchange_strong(expected, true))
        return true;

    ComPtr<ID3D11Device> dev;
    if (!swapchain || FAILED(swapchain->GetDevice(__uuidof(ID3D11Device), (void**)&dev))) {
        g_hooked = false; return false;
    }
    ComPtr<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(&ctx);
    if (!ctx) { g_hooked = false; return false; }

    // Immediate and deferred contexts use DISTINCT vtables (see ui_hook.cpp's
    // VTABLE PROBE finding) -- hook both, with separate original-pointer
    // storage per context type.
    ComPtr<ID3D11DeviceContext> defCtx;
    dev->CreateDeferredContext(0, &defCtx);
    void** vtDef = defCtx ? *reinterpret_cast<void***>(defCtx.Get()) : nullptr;
    if (!vtDef) VRLOG("depth probe: CreateDeferredContext FAILED -- deferred-side hooks skipped");

    void** vt = *reinterpret_cast<void***>(ctx.Get());
    // Idempotent: MH_ERROR_ALREADY_CREATED/MH_ERROR_ENABLED mean a PRIOR call
    // already hooked this address successfully -- still live and working, not
    // a failure. Without this, a retry (install_depth_probe is effectively
    // called every Present) would treat its own earlier success as fresh
    // failure and spam-retry forever.
    auto hook = [&](void** vtbl, int idx, void* detour, void** orig, const char* name) -> bool {
        MH_STATUS c = MH_CreateHook(vtbl[idx], detour, orig);
        if (c != MH_OK && c != MH_ERROR_ALREADY_CREATED) {
            VRLOG("depth probe: create %s FAILED", name); return false;
        }
        MH_STATUS e = MH_EnableHook(vtbl[idx]);
        if (e != MH_OK && e != MH_ERROR_ENABLED) {
            VRLOG("depth probe: enable %s FAILED", name); return false;
        }
        return true;
    };

    bool ok = hook(vt, kIdxOMSetRenderTargets, reinterpret_cast<void*>(&Detour_OMSetRenderTargets),
                   reinterpret_cast<void**>(&real_OMSetRenderTargets_imm), "OMSetRenderTargets(imm)");
    ok = hook(vt, kIdxOMSetRenderTargetsAndUAVs, reinterpret_cast<void*>(&Detour_OMSetRTAndUAV),
              reinterpret_cast<void**>(&real_OMSetRTAndUAV_imm), "OMSetRTAndUAV(imm)") && ok;
    if (vtDef) {
        ok = hook(vtDef, kIdxOMSetRenderTargets, reinterpret_cast<void*>(&Detour_OMSetRenderTargets),
                  reinterpret_cast<void**>(&real_OMSetRenderTargets_def), "OMSetRenderTargets(def)") && ok;
        ok = hook(vtDef, kIdxOMSetRenderTargetsAndUAVs, reinterpret_cast<void*>(&Detour_OMSetRTAndUAV),
                  reinterpret_cast<void**>(&real_OMSetRTAndUAV_def), "OMSetRTAndUAV(def)") && ok;
    }

    if (!ok || !real_OMSetRenderTargets_imm || !real_OMSetRTAndUAV_imm) {
        VRLOG("depth probe hooks PARTIAL/FAILED");
        g_hooked = false;
        return false;
    }
    VRLOG("scene depth acquisition installed -- hooked on both immediate and "
          "deferred vtables.");
    return true;
}

bool scene_depth_for_eye(int eye, SceneDepth& out)
{
    out = SceneDepth{};
    if (!g_depthValid.load() || eye < 0) return false;

    // Newest matching capture wins. Scanning for the EYE rather than taking the
    // most recent slot outright is the entire point of the ring: the most
    // recent capture belongs to whichever eye the game rendered last, which is
    // not reliably the eye now sitting in the backbuffer.
    //
    // A stale buffer for the RIGHT eye is still a far better input than a fresh
    // one for the wrong eye. The wrong eye mislocates every silhouette by a
    // full eye separation -- an error nothing downstream can detect -- whereas
    // a capture one frame old is displaced only by however far the camera moved
    // in that frame, which is exactly the error the warp already tolerates
    // everywhere else.
    //
    // A mixed-eye window is never offered: it spans two viewpoints, so it is
    // wrong in the first way rather than the second.
    int best = -1;
    uint64_t bestSerial = 0;
    for (int i = 0; i < kDepthSlots; ++i) {
        const SlotTag& t = g_slotTag[i];
        if (!t.valid.load() || t.mixed.load()) continue;
        if (t.eye.load() != eye) continue;
        const uint64_t serial = t.serial.load();
        if (serial > bestSerial) { bestSerial = serial; best = i; }
    }
    if (best < 0 || !g_depthSRV[best]) return false;

    const SlotTag& t = g_slotTag[best];
    out.srv       = g_depthSRV[best].Get();
    out.eye       = t.eye.load();
    out.offset    = t.offset.load();
    out.projA     = t.projA.load();
    out.projB     = t.projB.load();
    out.p00       = t.projP00.load();
    out.projValid = t.projValid.load();
    g_lastServedSlot.store(best);
    return true;
}

void set_self_targets(bool on) { t_selfTargets = on; }

namespace {
// Coarse coverage of one slot. A grid is plenty: the distinction being drawn is
// "blank" versus "has a scene in it", not a precise figure.
bool slot_coverage(ID3D11Device* dev, ID3D11DeviceContext* ctx, int slot,
                   float& covered, float& maxDepth)
{
    covered = -1.0f; maxDepth = -1.0f;
    ID3D11Texture2D* const acc = g_depthCopy[slot].Get();
    if (!acc) return false;

    D3D11_TEXTURE2D_DESC d{};
    acc->GetDesc(&d);

    // Cached per slot so a 20-frame burst does not allocate 40 times.
    static ComPtr<ID3D11Texture2D> s_stage[kDepthSlots];
    static UINT s_w[kDepthSlots] = {}, s_h[kDepthSlots] = {};
    if (!s_stage[slot] || s_w[slot] != d.Width || s_h[slot] != d.Height) {
        s_stage[slot].Reset();
        D3D11_TEXTURE2D_DESC sd = d;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        if (FAILED(dev->CreateTexture2D(&sd, nullptr, &s_stage[slot]))) return false;
        s_w[slot] = d.Width; s_h[slot] = d.Height;
    }

    ctx->CopyResource(s_stage[slot].Get(), acc);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(s_stage[slot].Get(), 0, D3D11_MAP_READ, 0, &m))) return false;

    const UINT step = 16;
    uint64_t hit = 0, total = 0;
    float mx = 0.0f;
    for (UINT y = 0; y < d.Height; y += step) {
        const float* row = reinterpret_cast<const float*>(
            static_cast<const uint8_t*>(m.pData) + (size_t)y * m.RowPitch);
        for (UINT x = 0; x < d.Width; x += step) {
            const float v = row[x];
            ++total;
            if (v > 0.0f) { ++hit; if (v > mx) mx = v; }
        }
    }
    ctx->Unmap(s_stage[slot].Get(), 0);

    covered  = total ? (float)((double)hit / (double)total) : 0.0f;
    maxDepth = mx;
    return true;
}
} // namespace

bool scene_depth_diag(ID3D11DeviceContext* ctx, DepthDiag& out)
{
    out = DepthDiag{};
    if (!ctx) return false;
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!dev) return false;

    // Report the slot the warp was actually SERVED, not the newest one -- with
    // a ring those differ whenever selection had to reach back for the right
    // eye, and the newest slot is then not the buffer being warped.
    const int warp = g_lastServedSlot.load();
    const int other = (warp + 1) % kDepthSlots;

    out.warpSlot    = warp;
    out.width       = g_depthW;
    out.height      = g_depthH;
    out.capsWarp    = g_slotCaps[warp].load();
    out.capsOther   = g_slotCaps[other].load();
    out.framePasses = g_lastSceneTransCount.load();
    out.deferred    = g_deferredForWarp.load();
    out.rsFirst     = g_rsFirstW.load();
    out.rsEye       = g_rsEyeW.load();
    out.rsCam       = g_rsCamW.load();
    out.lastCamMove = g_lastCamMove.load();

    const bool a = slot_coverage(dev.Get(), ctx, warp,  out.covWarp,  out.maxWarp);
    slot_coverage(dev.Get(), ctx, other, out.covOther, out.maxOther);
    return a;
}

void depth_probe_on_present(IDXGISwapChain* swapchain)
{
    if (!swapchain) return;

    // (A release-on-leaving-DIBR shift block lived here and CRASHED THE GAME on a mode
    // switch, 2026-08-16. It freed g_depthCopy/g_depthSRV and cleared the
    // g_srcDepthSRVs map from the PRESENT thread, while capture_if_pass_ended()
    // may be using all three from the render thread the OMSetRenderTargets
    // detour runs on -- a use-after-free plus an unordered_map cleared under a
    // concurrent reader. The dispatches are already gated by
    // depth_capture_wanted(), which is where the real cost was; holding the
    // texture while DIBR shift is inactive is a few tens of MB and is not worth a race.
    // Do not reinstate without moving the capture path behind the same lock.)

    // The main scene depth is identified by matching the render size, so the
    // probe needs the CURRENT backbuffer dimensions. RE-READ EVERY PRESENT.
    //
    // This used to latch on the first non-zero read, and that silently killed
    // depth capture for the rest of the session the moment the swapchain
    // resized: every sceneSized test then compared against a resolution nothing
    // was being rendered at any more, so no pass was ever captured and the only
    // symptom was "depth acquire: N/N frames MISSED" -- a clean 100%, which is
    // the signature of a comparison that cannot succeed rather than one that is
    // flaky.
    //
    // MEASURED 2026-08-20: resolution_hook forces 2880x2880 at startup, the
    // game then resizes down through 1920x1080 to 1902x977, and the probe went
    // on looking for 2880x2880 for the rest of the session. Latching was safe
    // only while the render size could never change after the first Present,
    // which was never actually true.
    {
        DXGI_SWAP_CHAIN_DESC scd{};
        if (SUCCEEDED(swapchain->GetDesc(&scd))) {
            const UINT w = scd.BufferDesc.Width, h = scd.BufferDesc.Height;
            if (w && h && (w != g_renderW.load() || h != g_renderH.load())) {
                VRLOG("depth probe: render size %ux%u -> %ux%u (scene depth is "
                      "matched by size, so a stale value captures nothing)",
                      g_renderW.load(), g_renderH.load(), w, h);
                g_renderW.store(w);
                g_renderH.store(h);
            }
        }
    }

    // Per-frame acquisition state resets AFTER the report, so the report sees
    // what this frame captured.
    // A frame with no capture means the warp would reuse the previous frame's
    // depth -- the WRONG EYE. Surface that rather than letting it degrade
    // silently into flickering disocclusions.
    const int total = g_totalFrames.fetch_add(1) + 1;
    if (!g_capturedThisFrame.load()) g_missedFrames.fetch_add(1);
    if (g_depthMixed.load()) g_mixedFrames.fetch_add(1);
    if ((total % 600) == 0) {
        const int missed = g_missedFrames.exchange(0);
        const int mixed  = g_mixedFrames.exchange(0);
        g_totalFrames.store(0);
        if (missed)
            VRLOG("depth acquire: %d/%d frames MISSED -- warp skipped (no depth)",
                  missed, total);
        if (mixed)
            VRLOG("depth acquire: %d/%d frames MIXED-EYE -- warp skipped (two viewpoints "
                  "in one accumulation window)", mixed, total);
        // THE ANSWER TO "does a reflection pass poison the scene depth", in
        // one line. Both figures matter: commits is how many non-main camera
        // buffers were committed where the capture can see them, refused is
        // how many depth passes that actually kept out. A zero on the first
        // means the guard cannot fire and the doubling is something else.
        const int otherPasses = g_otherCamPasses.exchange(0);
        const int scenePasses  = g_scenePasses.exchange(0);
        const uint32_t otherCams = hooks::non_main_camera_commits();
        if (otherPasses || otherCams)
            VRLOG("depth acquire: %d of %d scene-sized passes accumulated with "
                  "another camera as the last commit (%u non-main commits, first "
                  "at pass %d) -- observed, NOT refused",
                  otherPasses, scenePasses, otherCams,
                  g_otherCamLastFirstIdx.load(std::memory_order_relaxed));
        const int restarts = g_windowRestarts.exchange(0);
        if (restarts)
            VRLOG("depth acquire: %d/%d windows RESTARTED at an eye change "
                  "(kept current-eye captures instead of dropping the frame)",
                  restarts, total);
    }

    // Carried to the report window: which pass index the non-main commit first
    // reached, from the most recent frame that had one at all.
    const int firstIdx = g_otherCamFirstIdx.exchange(-1, std::memory_order_relaxed);
    if (firstIdx >= 0)
        g_otherCamLastFirstIdx.store(firstIdx, std::memory_order_relaxed);

    const int seen = g_sceneTransIdx.exchange(0);
    g_lastSceneTransCount.store(seen);
    if (g_capturedThisFrame.load()) {
        // Healthy: aim at the tail of what this frame actually did.
        g_captureThreshold.store(seen > kCaptureTail ? seen - kCaptureTail : 0);
    } else {
        // Missed entirely -- the window was past the end. Widen and retry.
        g_captureThreshold.store(g_captureThreshold.load() / 2);
    }
    // PUBLISH THE SLOT JUST FILLED, then move capture on to the next one -- so
    // the clears the render thread is about to issue for the next frame land
    // somewhere no consumer is reading. (That much was the original fix for the
    // blank-accumulator frame, and still is.)
    //
    // What is new is the TAG: the eye the capture belongs to and the camera it
    // was taken under travel with it, so a consumer can select by identity
    // instead of being handed whichever slot happened to be newest.
    {
        const int justFilled = g_captureSlot.load();

        // Only a frame that actually captured, and whose accumulation window
        // did not span two viewpoints, is offered to the warp. A mixed window
        // merges two positions one IPD apart, which is wrong in a way nothing
        // downstream can detect -- unlike a slightly stale capture, which is
        // merely displaced by however far the camera moved.
        SlotTag& tag = g_slotTag[justFilled];
        if (g_capturedThisFrame.load() && !g_depthMixed.load()) {
            tag.eye.store(g_depthEye.load());
            tag.offset.store(g_depthEyeOffset.load());
            tag.mixed.store(false);

            // FREEZE THE CAMERA BESIDE THE DEPTH rather than letting the
            // consumer read it live. Both then come from the same frame by
            // construction instead of by hoping the camera has not moved since.
            float view[16], viewProj[16];
            float a = 0.0f, b = 0.1f, p00 = 0.73454f;
            const bool projOk =
                main_camera_matrices(view, viewProj) &&
                dibrpolicy::derive_projection(view, viewProj, a, b, p00) &&
                dibrpolicy::projection_plausible(a, b, p00);
            tag.projA.store(a);
            tag.projB.store(b);
            tag.projP00.store(p00);
            tag.projValid.store(projOk);

            // Written LAST: `valid` is what a reader gates on, so everything it
            // will then read must already be in place.
            tag.serial.store(g_captureSerial.fetch_add(1) + 1);
            tag.valid.store(true);
        } else {
            tag.valid.store(false);
        }

        g_warpSlot.store(justFilled);
        g_captureSlot.store((justFilled + 1) % kDepthSlots);
    }

    g_deferredForWarp.store(g_deferredFrame.exchange(0));
    g_rsFirstW.store(g_rsFirst.exchange(0));
    g_rsEyeW.store(g_rsEye.exchange(0));
    g_rsCamW.store(g_rsCam.exchange(0));
    g_capturedThisFrame.store(false);
    g_clearedThisFrame.store(false);
}

} // namespace hooks
