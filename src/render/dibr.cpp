#include "render/dibr.hpp"
#include "render/mirror_mask.hpp"
#include "common/log.h"

#include <d3dcompiler.h>
#include <wrl/client.h>
#include <atomic>

using Microsoft::WRL::ComPtr;

namespace dibr {
namespace {

// 0 magenta, 1 stale eye, 2 stretch, 3 disparity. Mode 1 is the shipped
// default -- see the fillMode comment in dibr.hpp for why, and keep the two in
// step: WarpParams::fillMode carries the same value.
std::atomic<int>  g_fillMode{1};

// Scatter buffer packing: [ disparity : 16 | srcX+1 : 16 ].
// Disparity in the high bits so InterlockedMax resolves overlaps by
// nearest-wins (larger disparity == closer to the camera == occluder).
// srcX is stored +1 so that a value of 0 unambiguously means "never written",
// which is what identifies a disocclusion hole.
// TRIED AND REVERTED 2026-08-24: making the continuity threshold below RELATIVE
// to the local disparity, max(2 px, 0.06 * dmax), instead of a flat 2 px.
//
// THE REASONING WAS SOUND and is worth keeping. With the head ROLLED, the
// dashboard ornament's disocclusion broke into horizontal dashes where upright
// it gave one clean hole, and the depth was fine both times (checked on the
// disparity debug view -- a crisp near-uniform shape either way). What changes
// is the direction the scan cuts the geometry: upright a row crosses the doll's
// limbs across their width, rolled it runs along them, so identical geometry
// presents a far steeper disparity gradient per pixel and crosses a threshold
// that is stated in pixels rather than as a fraction of the disparity.
//
// IT DID NOT PAY. In the headset the rolled case looked unchanged, and the
// widened threshold sends many more near-field pixels down the SPAN path --
// where the span is filled by stretching the source pixel's own 2 px footprint
// (see the clamp in CS_Resolve). At 150 px of disparity it allowed 9 px spans,
// i.e. a 4.5x horizontal stretch of near geometry in the SYNTHESIZED eye only,
// against a rendered eye that is sharp. A width mismatch between the eyes on
// near geometry does not read as blur, it reads as failing to fuse -- and the
// file already records the measured ancestor of that artefact ("two copies of
// the dashboard ornament 10px apart"). No visible gain against a real risk of
// double vision on the cab, so it goes back.
//
// If the rolled dashes are worth another attempt, the lever is NOT this
// threshold. It is `ramp`'s requirement that a step continue the previous one in
// SIGN, which is what fails at the start of every run and at every local
// extremum -- and any replacement has to leave the span length alone.

const char kWarpHlsl[] = R"(
cbuffer Params : register(b0)
{
    float2 texSize;
    float  focalPx;
    float  ipd;
    float  eyeSign;
    float  eyeOffsetPx;
    float  projA;
    float  projB;
    float  maxSplat;
    float  fillMode;
    float  hasFill;      // is FillTex bound and meaningful this frame
    float  hasMirror;    // is MirrorTex bound this frame
};

SamplerState        LinSmp   : register(s0);
Texture2D<float>    DepthTex : register(t0);
Texture2D<float4>   SrcTex   : register(t1);
Texture2D<float4>   FillTex  : register(t2);   // stale eye, rotation-warped to the current pose
// Mirror coverage, 1 where the game drew a mirror. See mirror_mask.hpp: the
// depth under a mirror describes the surface, never the reflection, so
// reprojecting there is guaranteed wrong however good the depth is.
Texture2D<float>    MirrorTex : register(t3);
RWTexture2D<uint>   Scatter  : register(u0);
RWTexture2D<float4> OutTex   : register(u1);
// Mirror coverage in DESTINATION space. MirrorTex marks the mirror where the
// RENDERED eye saw it; the hole it leaves behind lands at that position plus
// its disparity, which for near-field geometry is hundreds of pixels. Testing
// the source mask at the destination coordinate therefore only agrees with
// itself where the two regions happen to overlap -- observed in-headset as
// part of a mirror filled correctly and the rest smeared. The scatter writes
// this as it goes, so the mark ends up exactly where the hole is.
RWTexture2D<uint>   MirrorHole : register(u2);

float view_z(float d)
{
    // Reverse-Z: z_ndc = A + B/z_view  =>  z_view = B / (z_ndc - A)
    return projB / max(d - projA, 1e-7f);
}

float disparity_at(uint2 p)
{
    float d = DepthTex[p].r;
    float disp = ipd * focalPx / max(view_z(d), 1e-4f);
    // Hard clamp. A degenerate projection (during a camera/FOV transition, or
    // before the camera CB has been seen) can yield NaN/Inf here, which would
    // produce an unbounded scatter span, hang the dispatch, and take the driver
    // out via TDR. Anything non-finite is treated as zero disparity.
    if (!(disp > 0.0f)) disp = 0.0f;          // false for NaN as well
    return min(disp, 512.0f);
}

// Forward scatter. Each source pixel covers a span in the destination whose
// extent is set by the disparity difference to its neighbour; splatting across
// that span is what prevents 1-px cracks from being mistaken for real
// disocclusion. The span is clamped so near-field geometry (which can shift
// hundreds of pixels) cannot blow up the cost.
bool is_mirror(uint2 p)
{
    return hasMirror > 0.5f && MirrorTex[p].r > 0.5f;
}

[numthreads(8, 8, 1)]
void CS_Scatter(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)texSize.x || tid.y >= (uint)texSize.y) return;

    float disp  = disparity_at(tid.xy);
    float dispN = (tid.x + 1 < (uint)texSize.x) ? disparity_at(uint2(tid.x + 1, tid.y)) : disp;

    // eyeOffsetPx is the constant part of the mapping (zero unless the two
    // eyes were rendered at different, off-centre frusta); the disparity is
    // the part that depends on depth.
    int x0 = (int)round((float)tid.x + eyeOffsetPx + eyeSign * disp);
    int x1 = (int)round((float)(tid.x + 1) + eyeOffsetPx + eyeSign * dispN);

    // Only splat across the span when this pixel and its neighbour lie on the
    // SAME surface. Across a silhouette the two disparities differ wildly, and
    // filling that span would stretch the foreground over the gap -- the classic
    // DIBR smear. Leaving it empty is correct: it is a genuine disocclusion, to
    // be filled from the background (or the stale eye) later.
    //
    // The test is RELATIVE to the local disparity, not a fixed pixel count. A
    // discontinuity is a large FRACTION of the disparity; a slanted surface is a
    // small one. The old absolute 2px threshold was tuned against half-resolution
    // measurements, where near-field disparity peaked around 65px -- at full
    // resolution it is roughly double that, so continuous but steeply slanted
    // near geometry (the entire dashboard) crossed 2px between ADJACENT pixels
    // and was shattered into one-pixel writes with gaps between them. Those gaps
    // are not disocclusions, and filling them from another source is what reads
    // as heavy ghosting on near objects.
    // Magnitude alone cannot decide this. A dashboard seen edge-on is
    // continuous and moves several px per pixel; an ornament's arm is a
    // silhouette whose step is the same size. They differ in SHAPE: a slanted
    // surface RAMPS (each step continues the last), a silhouette is one
    // isolated spike. One extra depth fetch buys the distinction.
    float dispP = (tid.x > 0) ? disparity_at(uint2(tid.x - 1, tid.y)) : disp;
    float g1 = dispN - disp;      // step to the right neighbour
    float g0 = disp - dispP;      // step the pixel before already made
    float dmax = max(disp, dispN);

    bool flat = abs(g1) <= 2.0f;
    // Slanted but continuous. Still capped by the old relative limit, so this
    // can only ever be more conservative than what shipped before.
    bool ramp = (g1 * g0 > 0.0f) &&
                (abs(g1) <= abs(g0) * 2.0f + 2.0f) &&
                (abs(g1) <= 0.15f * dmax);

    int lo, hi;
    if (flat || ramp) {
        lo = min(x0, x1); hi = max(x0, x1);
    } else {
        lo = x0; hi = x0;                  // silhouette: write one pixel only
    }

    // Clamp the span unconditionally and to the image, so the loop count is
    // bounded no matter what the inputs were.
    lo = clamp(lo, 0, (int)texSize.x - 1);
    hi = clamp(hi, 0, (int)texSize.x - 1);
    // Also scaled by the local disparity, and for the same reason: a legitimate
    // span on a steeply slanted NEAR surface is proportionally longer. Still
    // hard-capped so the loop count stays bounded whatever the depth says.
    int splatCap = (int)clamp(max(maxSplat, 0.35f * dmax), 1.0f, 192.0f);
    hi = min(hi, lo + splatCap);

    // A mirror scatters NOTHING, but it does record where it would have gone.
    // Skipping outright leaves the destination unwritten, which is exactly a
    // disocclusion and flows into the ordinary hole-filling path; marking the
    // same span here is what lets the resolve know that this particular hole
    // was a mirror and must come from the stale eye. The span is computed from
    // the same disparity the pixel would have used, so the mark and the hole
    // coincide by construction however wrong that disparity is.
    if (is_mirror(tid.xy)) {
        for (int mx = lo; mx <= hi; ++mx)
            MirrorHole[uint2((uint)mx, tid.y)] = 1u;
        return;
    }

    // Quantised to 1/16 px, not whole pixels. This value only orders
    // overlapping writes (nearest wins), but at ORBIT distances the whole
    // scene sits within a few pixels of disparity, so integer steps made
    // neighbouring surfaces tie and resolve arbitrarily.
    uint dispQ  = (uint)clamp(disp * 16.0f, 0.0f, 65535.0f);
    uint packed = (dispQ << 16) | ((tid.x + 1) & 0xFFFF);

    for (int x = lo; x <= hi; ++x)
        InterlockedMax(Scatter[uint2((uint)x, tid.y)], packed);
}

// How far the fill bleeds outward past a hole's edge, in pixels. The scatter is
// least trustworthy exactly at a hole boundary -- that is where a splat span was
// cut short at a silhouette, so the last written pixel is often a stretched or
// half-covered one -- and cutting hard from it to the fill is what leaves a thin
// rim around a filled region.
//
// Feathering both GROWS the hole slightly (the rim gets replaced rather than
// kept) and ramps the transition, so the two sources meet gradually. Small on
// purpose: the fill is a different image, and past a couple of pixels the blend
// starts eating real reprojected detail.
static const int kFeather = 2;

// THE UI IS NOT COMPOSITED HERE, deliberately.
//
// It used to be, and that made the ordering wrong: this is the reprojection,
// and the windscreen smudge is composited later, during the blit into the eye's
// swapchain. So the HUD went on first and the smudge went on top of it -- the
// reverse of how the game draws them, where the splatter is on the glass and
// the HUD is drawn over everything.
//
// The output of this pass is now scene only, and the blit owns both composites
// in the right order: smudge, then UI. See kBlitHlsl in xr_mirror.cpp.
void write_out(uint2 p, float4 c)
{
    OutTex[p] = c;
}

float4 fill_colour(uint2 p)
{
    // A hole left by a mirror takes the stale eye whatever the configured
    // mode is. Stretch smears the surrounding cab across the mirror; the stale
    // eye holds a real mirror the game rendered, one frame old.
    //
    // EXCEPT in the magenta diagnostic, where taking real content would be a
    // lie: mode 0 exists to show exactly which pixels the reprojection did not
    // produce, and quietly substituting the stale eye there makes the mirror
    // look like successfully reprojected image. CYAN instead, so the mask's
    // real extent is visible and separable from ordinary disocclusion.
    if (MirrorHole[p] != 0u) {
        if (fillMode < 0.5f) return float4(0.0f, 1.0f, 1.0f, 1.0f);
        if (hasFill > 0.5f)  return FillTex[p];
    }
    // Disocclusion: the rendered eye never saw this surface.
    //
    // fillMode 0 = magenta (measure hole extent)
    //          1 = the stale eye, rotation-warped to this pose
    //          2 = background stretch from the current frame
    //
    // A disocclusion always reveals what was hidden BEHIND the occluder, so the
    // correct source is the neighbouring surface with the SMALLER disparity
    // (further away). Taking the nearer one would smear the foreground across
    // the gap -- the artefact the splat guard avoids.
    if (fillMode < 0.5f) return float4(1.0f, 0.0f, 1.0f, 1.0f);

    if (fillMode < 1.5f) {
        // The synthesized eye's OWN last real render, brought to the current
        // pose. A disocclusion is a surface only this eye can see, so this is
        // the one source in the system that actually observed it.
        return FillTex[p];
    }

    const int kSearch = 96;
    uint leftPacked = 0, rightPacked = 0;
    for (int k = 1; k <= kSearch; ++k) {
        if (leftPacked == 0 && (int)p.x - k >= 0) {
            uint q = Scatter[uint2((uint)((int)p.x - k), p.y)];
            if (q != 0) leftPacked = q;
        }
        if (rightPacked == 0 && p.x + k < (uint)texSize.x) {
            uint q = Scatter[uint2(p.x + k, p.y)];
            if (q != 0) rightPacked = q;
        }
        if (leftPacked != 0 && rightPacked != 0) break;
    }

    uint chosen = 0;
    if (leftPacked != 0 && rightPacked != 0)
        chosen = ((leftPacked >> 16) <= (rightPacked >> 16)) ? leftPacked : rightPacked;
    else
        chosen = (leftPacked != 0) ? leftPacked : rightPacked;

    if (chosen == 0)                             // nothing within range
        return (hasFill > 0.5f) ? FillTex[p] : float4(0, 0, 0, 1);
    return SrcTex[uint2((chosen & 0xFFFFu) - 1u, p.y)];
}

// How much this NON-hole pixel should be pulled toward the fill: 0 when no hole
// is within kFeather, rising as one gets closer. Horizontal only -- disparity is
// a horizontal shift, so holes are horizontal runs and their edges are vertical.
float feather_weight(uint2 p)
{
    float w = 0.0f;
    for (int dx = -kFeather; dx <= kFeather; ++dx) {
        if (dx == 0) continue;
        int x = (int)p.x + dx;
        if (x < 0 || x >= (int)texSize.x) continue;
        if (Scatter[uint2((uint)x, p.y)] == 0)
            w = max(w, 1.0f - (float)abs(dx) / (float)(kFeather + 1));
    }
    return w;
}

[numthreads(8, 8, 1)]
void CS_Resolve(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)texSize.x || tid.y >= (uint)texSize.y) return;

    // Mode 3: show the DISPARITY the warp derives, as greyscale (white = ~200px
    // or more). This distinguishes "the warp is wrong" from "the depth is wrong":
    // in cockpit the dashboard should read bright (large disparity). If it reads
    // black, the near geometry is not in the depth buffer we captured and the
    // warp has nothing to displace -- which would explain holes vanishing.
    if (fillMode > 2.5f) {
        float dv = disparity_at(tid.xy) / 200.0f;
        OutTex[tid.xy] = float4(dv, dv, dv, 1.0f);
        return;
    }

    uint packed = Scatter[tid.xy];
    if (packed == 0) {
        write_out(tid.xy, fill_colour(tid.xy));
        return;
    }

    // SUB-PIXEL RESAMPLE, and this is what keeps distant ground smooth.
    //
    // The scatter had to round: it writes to an integer destination pixel. At
    // cockpit distances disparity is 60-130px and half a pixel of rounding is
    // nothing, but in orbit the whole scene lives within a few pixels of
    // disparity, so a ground plane sliding from 3.4 to 2.6px snapped to
    // 3,3,3,2 -- flat iso-disparity bands, which stereo reads as a staircase
    // instead of a slope.
    //
    // The scatter guarantees tid.x == round(srcX + eyeSign*disp(srcX)), so
    // undoing it with the UNROUNDED disparity recovers srcX plus exactly the
    // residual that was thrown away. Sampling there varies continuously
    // between neighbouring destination pixels, so the reconstructed disparity
    // is continuous too. It also makes a splatted span stretch smoothly rather
    // than repeat one texel.
    // A destination pixel the mirror would have covered is taken from the stale
    // eye even when something else DID scatter into it. Nearby cab geometry at
    // a similar depth splats across part of the mirror, and letting that win
    // leaves a smear of dashboard over one edge of the reflection. The mirror
    // is either wholly the stale eye's or it is visibly patched together.
    if (MirrorHole[tid.xy] != 0u) {
        // Same rule as fill_colour: in the magenta diagnostic this must not
        // paste real image over a pixel the reprojection DID produce. Cyan
        // here means "the mirror override claimed this pixel", which is the
        // one thing worth being able to see when judging the mask.
        if (fillMode < 0.5f) { write_out(tid.xy, float4(0.0f, 1.0f, 1.0f, 1.0f)); return; }
        if (hasFill > 0.5f)  { write_out(tid.xy, FillTex[tid.xy]); return; }
    }

    uint  srcX  = (packed & 0xFFFFu) - 1u;
    float dispS = disparity_at(uint2(srcX, tid.y));
    float srcXf = (float)tid.x - eyeSign * dispS;

    // CLAMPED TO THE SPLATTING PIXEL'S OWN FOOTPRINT -- this is what stops near
    // geometry appearing TWICE. The back-projection is exact only for the
    // destination that actually scattered here; across a splatted span every
    // destination stores the same srcX while tid.x keeps moving, so srcXf
    // sweeps the source across the whole span -- and those swept pixels scatter
    // to their own destinations too, so the content lands twice. MEASURED:
    // two copies of the dashboard ornament 10px apart, against a span limit of
    // 0.15*72 = 10.8px. A span covers the gap between where srcX and srcX+1
    // land, so [srcX, srcX+1] is the interval it represents; spans are ~1px in
    // orbit so this is inert there. The half pixel is the rounding residual.
    srcXf = clamp(srcXf, (float)srcX - 0.5f, (float)srcX + 1.5f);
    float2 uv   = float2((srcXf + 0.5f) / texSize.x, ((float)tid.y + 0.5f) / texSize.y);
    float4 dibr = SrcTex.SampleLevel(LinSmp, uv, 0);

    // Feather toward the fill near a hole edge. The search inside fill_colour()
    // is only paid for by the thin band of pixels that are actually close to
    // one; everywhere else this is a handful of Scatter loads and nothing more.
    float w = feather_weight(tid.xy);
    write_out(tid.xy, (w > 0.0f) ? lerp(dibr, fill_colour(tid.xy), w) : dibr);
}
)";

struct CB {
    float texSize[2];
    float focalPx;
    float ipd;
    float eyeSign;
    float eyeOffsetPx;
    float projA;
    float projB;
    float maxSplat;
    float fillMode;
    float hasFill;
    float hasMirror;
    // 12 live floats = 48 bytes, already a 16-byte multiple. MUST stay in the
    // same order as the HLSL cbuffer above -- a mismatch here does not fail to
    // compile, it silently feeds every field the neighbouring one's value.
};
// D3D11 rejects a constant buffer whose ByteWidth is not a multiple of 16.
// Getting this wrong fails CreateBuffer silently and leaves a null CB behind.
static_assert(sizeof(CB) % 16 == 0, "constant buffer must be 16-byte aligned");

struct State {
    ComPtr<ID3D11Device>  device;
    ComPtr<ID3D11ComputeShader> csScatter, csResolve;
    ComPtr<ID3D11Buffer>  cb;
    ComPtr<ID3D11SamplerState> linSmp;   // sub-pixel source resample

    ComPtr<ID3D11Texture2D>          srcCopy;     // SRV-able copy of the rendered eye
    ComPtr<ID3D11ShaderResourceView> srcSRV;
    ComPtr<ID3D11Texture2D>          scatter;     // R32_UINT
    ComPtr<ID3D11UnorderedAccessView> scatterUAV;
    ComPtr<ID3D11Texture2D>          mirrorHole;  // R8_UINT, destination-space
    ComPtr<ID3D11UnorderedAccessView> mirrorHoleUAV;
    ComPtr<ID3D11Texture2D>          out;         // warped result
    ComPtr<ID3D11UnorderedAccessView> outUAV;

    UINT w = 0, h = 0;
    DXGI_FORMAT colorFormat = DXGI_FORMAT_UNKNOWN;
    bool ready = false;

    // --- GPU cost of the two dispatches ----------------------------------
    // The warp runs inline at Present on the immediate context, so its cost
    // lands directly on the frame the player is waiting for -- unlike a plugin
    // that owns its own command list and can be scheduled around. That makes
    // "how much does it cost" a number worth having rather than assuming, and
    // the scatter's per-pixel loop bounds (splatCap up to 192, plus the
    // resolve's kSearch scanned both ways) are large enough that it can move a
    // lot with content.
    //
    // A RING, read with DONOTFLUSH, because the whole point is to not stall:
    // the result for a frame is only collected once the GPU has got round to
    // it, and a frame whose slot is not ready yet is simply not measured.
    static constexpr int kTimerSlots = 4;
    ComPtr<ID3D11Query> tsDisjoint[kTimerSlots], tsBegin[kTimerSlots], tsEnd[kTimerSlots];
    bool  tsPending[kTimerSlots] = {};
    int   tsCursor = 0;
    bool  tsReady = false;
    // Set once creation has failed, so a device that will not give us
    // timestamp queries is asked once rather than twelve times per frame
    // forever. Timing is diagnostic; retrying it is not worth a cost that
    // would land on exactly the path being measured.
    bool  tsUnavailable = false;
    float gpuMs    = -1.0f;   // last resolved measurement, <0 = none yet
    float gpuAvgMs = -1.0f;   // smoothed, for the threshold warning
} g;

// Idempotent; a failure here disables timing and nothing else.
bool ensure_gpu_timer(ID3D11Device* dev)
{
    if (g.tsReady) return true;
    if (g.tsUnavailable) return false;

    D3D11_QUERY_DESC dj{}; dj.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    D3D11_QUERY_DESC ts{}; ts.Query = D3D11_QUERY_TIMESTAMP;
    for (int i = 0; i < State::kTimerSlots; ++i) {
        if (FAILED(dev->CreateQuery(&dj, &g.tsDisjoint[i])) ||
            FAILED(dev->CreateQuery(&ts, &g.tsBegin[i])) ||
            FAILED(dev->CreateQuery(&ts, &g.tsEnd[i]))) {
            VRLOG("DIBR shift: GPU timer queries unavailable -- warp cost will not be measured");
            for (int k = 0; k < State::kTimerSlots; ++k) {
                g.tsDisjoint[k].Reset(); g.tsBegin[k].Reset(); g.tsEnd[k].Reset();
            }
            g.tsUnavailable = true;
            return false;
        }
        g.tsPending[i] = false;
    }
    g.tsReady = true;
    return true;
}

// Non-blocking. True means the slot is free to reuse -- either it held nothing
// or its result has now been taken. False means the GPU has not finished with
// it, and this frame simply goes unmeasured.
bool collect_gpu_timer(ID3D11DeviceContext* ctx, int slot)
{
    if (!g.tsPending[slot]) return true;

    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    UINT64 t0 = 0, t1 = 0;
    if (ctx->GetData(g.tsDisjoint[slot].Get(), &dj, sizeof(dj),
                     D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
        ctx->GetData(g.tsBegin[slot].Get(), &t0, sizeof(t0),
                     D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
        ctx->GetData(g.tsEnd[slot].Get(), &t1, sizeof(t1),
                     D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
        return false;

    g.tsPending[slot] = false;
    // Disjoint means the clock moved under the measurement (power state
    // change); the interval is meaningless rather than merely imprecise, so
    // drop it instead of feeding it into the average.
    if (dj.Disjoint || dj.Frequency == 0 || t1 <= t0) return true;

    const float ms = 1000.0f * (float)(t1 - t0) / (float)dj.Frequency;
    g.gpuMs = ms;
    g.gpuAvgMs = (g.gpuAvgMs < 0.0f) ? ms : (0.9f * g.gpuAvgMs + 0.1f * ms);
    return true;
}

bool compile_shaders(ID3D11Device* dev)
{
    auto build = [&](const char* entry, ComPtr<ID3D11ComputeShader>& out) -> bool {
        ComPtr<ID3DBlob> code, err;
        HRESULT hr = D3DCompile(kWarpHlsl, sizeof(kWarpHlsl) - 1, "dibr", nullptr, nullptr,
                                entry, "cs_5_0", 0, 0, &code, &err);
        if (FAILED(hr)) {
            VRLOG("DIBR shift: %s compile FAILED: %s", entry,
                  err ? (const char*)err->GetBufferPointer() : "(no message)");
            return false;
        }
        return SUCCEEDED(dev->CreateComputeShader(code->GetBufferPointer(),
                                                  code->GetBufferSize(), nullptr, &out));
    };
    if (!build("CS_Scatter", g.csScatter)) return false;
    if (!build("CS_Resolve", g.csResolve)) return false;

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(CB);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    HRESULT hr = dev->CreateBuffer(&bd, nullptr, &g.cb);
    if (FAILED(hr)) {
        VRLOG("DIBR shift: constant buffer creation FAILED hr=0x%08X (size=%zu)",
              (unsigned)hr, sizeof(CB));
        g.csScatter.Reset();   // do not leave a half-initialised state that the
        g.csResolve.Reset();   // "already compiled" check would then skip over
        return false;
    }

    // CLAMP, not border: srcXf lands a fraction of a pixel outside the image
    // only at the very edge columns, and clamping there is right -- a black
    // border would draw a one-pixel dark line down the frame edge.
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sd, &g.linSmp))) {
        VRLOG("DIBR shift: sampler creation FAILED");
        g.csScatter.Reset();
        g.csResolve.Reset();
        return false;
    }

    VRLOG("DIBR shift: compute shaders compiled");
    return true;
}

bool ensure_resources(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& srcDesc)
{
    if (g.ready && g.w == srcDesc.Width && g.h == srcDesc.Height &&
        g.colorFormat == srcDesc.Format)
        return true;

    g.srcCopy.Reset(); g.srcSRV.Reset();
    g.scatter.Reset(); g.scatterUAV.Reset();
    g.mirrorHole.Reset(); g.mirrorHoleUAV.Reset();
    g.out.Reset();     g.outUAV.Reset();
    g.ready = false;

    // Colour copy: the backbuffer itself is not shader-readable, so mirror it
    // into a texture that is. Raw UNORM in and out -- we only move pixels, so
    // no sRGB conversion should happen anywhere in this path.
    D3D11_TEXTURE2D_DESC cd = srcDesc;
    cd.Usage = D3D11_USAGE_DEFAULT;
    cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    cd.CPUAccessFlags = 0;
    cd.MiscFlags = 0;
    if (FAILED(dev->CreateTexture2D(&cd, nullptr, &g.srcCopy))) {
        VRLOG("DIBR shift: src copy creation FAILED (fmt=%d)", (int)srcDesc.Format);
        return false;
    }
    if (FAILED(dev->CreateShaderResourceView(g.srcCopy.Get(), nullptr, &g.srcSRV))) {
        VRLOG("DIBR shift: src SRV creation FAILED");
        return false;
    }

    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = srcDesc.Width; sd.Height = srcDesc.Height;
    sd.MipLevels = 1; sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_R32_UINT;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_DEFAULT;
    sd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(dev->CreateTexture2D(&sd, nullptr, &g.scatter)) ||
        FAILED(dev->CreateUnorderedAccessView(g.scatter.Get(), nullptr, &g.scatterUAV))) {
        VRLOG("DIBR shift: scatter buffer creation FAILED");
        return false;
    }

    // Where a mirror's pixels WOULD have landed. One byte per pixel; written
    // by the scatter, read by the resolve, and cleared alongside the scatter
    // buffer every dispatch.
    D3D11_TEXTURE2D_DESC md = sd;
    md.Format = DXGI_FORMAT_R8_UINT;
    if (FAILED(dev->CreateTexture2D(&md, nullptr, &g.mirrorHole)) ||
        FAILED(dev->CreateUnorderedAccessView(g.mirrorHole.Get(), nullptr, &g.mirrorHoleUAV))) {
        VRLOG("DIBR shift: mirror-hole buffer creation FAILED");
        return false;
    }

    D3D11_TEXTURE2D_DESC od = cd;
    od.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(dev->CreateTexture2D(&od, nullptr, &g.out)) ||
        FAILED(dev->CreateUnorderedAccessView(g.out.Get(), nullptr, &g.outUAV))) {
        VRLOG("DIBR shift: output texture creation FAILED (fmt=%d -- needs UAV support)",
              (int)od.Format);
        return false;
    }

    g.w = srcDesc.Width; g.h = srcDesc.Height; g.colorFormat = srcDesc.Format;
    g.ready = true;
    VRLOG("DIBR shift: resources ready %ux%u fmt=%d", g.w, g.h, (int)g.colorFormat);
    return true;
}

} // namespace

ID3D11Texture2D* warp(ID3D11DeviceContext* ctx,
                      ID3D11Texture2D* srcColor,
                      ID3D11ShaderResourceView* depthSRV,
                      const WarpParams& p,
                      ID3D11ShaderResourceView* fillSrc)
{
    if (!ctx || !srcColor || !depthSRV) return nullptr;

    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;

    // Reject degenerate parameters before they reach the GPU. dibr_proj_*() can
    // hand back garbage if the camera CB has not been seen yet or is mid-
    // transition, and an unbounded disparity hangs the dispatch (TDR).
    if (!(p.projB > 1e-6f) || !(p.focalPx > 1.0f) || !(p.ipd > 0.0f) ||
        !(p.focalPx < 1e6f) || !(p.ipd < 1.0f)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            VRLOG("DIBR shift: rejecting bad params (projA=%.6f projB=%.6f focalPx=%.2f ipd=%.4f)",
                  p.projA, p.projB, p.focalPx, p.ipd);
        }
        return nullptr;
    }

    if ((!g.csScatter || !g.csResolve || !g.cb) && !compile_shaders(dev.Get()))
        return nullptr;
    if (!g.cb) return nullptr;

    D3D11_TEXTURE2D_DESC sd{};
    srcColor->GetDesc(&sd);
    if (!ensure_resources(dev.Get(), sd)) return nullptr;

    ctx->CopyResource(g.srcCopy.Get(), srcColor);

    // Resolved before the constant buffer is filled, since the shader gates on
    // whether it exists.
    ID3D11ShaderResourceView* mirror = mirrormask::srv();

    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return nullptr;
    CB* cb = static_cast<CB*>(m.pData);
    cb->texSize[0] = (float)g.w; cb->texSize[1] = (float)g.h;
    cb->focalPx = p.focalPx;
    cb->ipd     = p.ipd;
    cb->eyeSign = p.eyeSign;
    cb->eyeOffsetPx = p.eyeOffsetPx;
    cb->projA   = p.projA;
    cb->projB   = p.projB;
    cb->maxSplat = 48.0f;
    // Mode 1 needs the caller's rotation-warped stale eye. Without it, fall
    // back to the STRETCH fill rather than to the old behaviour of sampling an
    // unwarped previous frame -- that is what made the fill lag the head.
    int mode = p.fillMode;
    if (mode == 1 && !fillSrc) mode = 2;
    cb->fillMode = (float)mode;
    cb->hasFill  = fillSrc ? 1.0f : 0.0f;
    // The mirror path costs nothing when the mask is absent: the shader
    // short-circuits on this before ever sampling t3.
    cb->hasMirror = mirror ? 1.0f : 0.0f;
    ctx->Unmap(g.cb.Get(), 0);

    // SAVE the compute stage, rather than nulling it afterwards.
    //
    // Nulling was safe for the one call site this has always had (Present,
    // after the game has finished with the context), but it made that call
    // site part of the contract: anywhere else, the game would silently lose
    // whatever it had bound. Saving costs a handful of Get calls on a path
    // that already does a full-resolution CopyResource and two 2688^2
    // dispatches, and it makes the module movable.
    //
    // ComPtr throughout because every CSGet* returns an AddRef'd pointer --
    // the array forms return one per slot, including the null ones.
    ComPtr<ID3D11ComputeShader>       oldCs;
    ComPtr<ID3D11Buffer>              oldCb;
    ComPtr<ID3D11SamplerState>        oldSmp;
    ComPtr<ID3D11ShaderResourceView>  oldSrv[4];
    ComPtr<ID3D11UnorderedAccessView> oldUav[3];
    // Class instances are deliberately not preserved: retrieving them needs a
    // correctly-sized array up front, and nothing in this game's compute paths
    // uses shader linkage. Restoring the shader with none is what CSSetShader
    // does for every non-linked shader anyway.
    ctx->CSGetShader(&oldCs, nullptr, nullptr);
    ctx->CSGetConstantBuffers(0, 1, oldCb.GetAddressOf());
    ctx->CSGetSamplers(0, 1, oldSmp.GetAddressOf());
    ctx->CSGetShaderResources(0, 4, oldSrv[0].GetAddressOf());
    ctx->CSGetUnorderedAccessViews(0, 3, oldUav[0].GetAddressOf());

    const UINT clearVal[4] = {0, 0, 0, 0};
    ctx->ClearUnorderedAccessViewUint(g.scatterUAV.Get(), clearVal);
    ctx->ClearUnorderedAccessViewUint(g.mirrorHoleUAV.Get(), clearVal);
    ID3D11ShaderResourceView* srvs[4] = {depthSRV, g.srcSRV.Get(), fillSrc, mirror};
    ID3D11UnorderedAccessView* uavs[3] = {g.scatterUAV.Get(), g.outUAV.Get(), g.mirrorHoleUAV.Get()};
    ID3D11Buffer* cbs[1] = {g.cb.Get()};

    ID3D11SamplerState* smps[1] = {g.linSmp.Get()};
    ctx->CSSetConstantBuffers(0, 1, cbs);
    ctx->CSSetSamplers(0, 1, smps);
    ctx->CSSetShaderResources(0, 4, srvs);
    ctx->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);

    const UINT gx = (g.w + 7) / 8, gy = (g.h + 7) / 8;

    // Measure only when the slot we are about to write has already given up
    // its result. Otherwise this frame goes unmeasured -- which is the correct
    // trade: the number is diagnostic, the stall it would take to force it is
    // not.
    const int tslot = g.tsCursor;
    const bool measure = ensure_gpu_timer(dev.Get()) && collect_gpu_timer(ctx, tslot);
    if (measure) {
        ctx->Begin(g.tsDisjoint[tslot].Get());
        ctx->End(g.tsBegin[tslot].Get());
    }

    ctx->CSSetShader(g.csScatter.Get(), nullptr, 0);
    ctx->Dispatch(gx, gy, 1);

    ctx->CSSetShader(g.csResolve.Get(), nullptr, 0);
    ctx->Dispatch(gx, gy, 1);

    if (measure) {
        ctx->End(g.tsEnd[tslot].Get());
        ctx->End(g.tsDisjoint[tslot].Get());
        g.tsPending[tslot] = true;
        g.tsCursor = (tslot + 1) % State::kTimerSlots;
    }

    // RESTORE. The UAV set has to go back before the SRVs: our output is bound
    // as a UAV here and the caller may well have it (or the depth) bound as an
    // SRV, and D3D11 resolves that conflict by silently unbinding whichever
    // comes second.
    ID3D11UnorderedAccessView* restoreUav[3] = {
        oldUav[0].Get(), oldUav[1].Get(), oldUav[2].Get()};
    ID3D11ShaderResourceView* restoreSrv[4] = {
        oldSrv[0].Get(), oldSrv[1].Get(), oldSrv[2].Get(), oldSrv[3].Get()};
    ID3D11Buffer*       restoreCb[1]  = {oldCb.Get()};
    ID3D11SamplerState* restoreSmp[1] = {oldSmp.Get()};
    ctx->CSSetUnorderedAccessViews(0, 3, restoreUav, nullptr);
    ctx->CSSetShaderResources(0, 4, restoreSrv);
    ctx->CSSetConstantBuffers(0, 1, restoreCb);
    ctx->CSSetSamplers(0, 1, restoreSmp);
    ctx->CSSetShader(oldCs.Get(), nullptr, 0);

    // No internal previous-frame copy any more: the fill comes from the
    // caller, which owns the retained eye and the pose to warp it by. That also
    // drops a full-resolution CopyResource per frame.
    return g.out.Get();
}

int fill_mode() { return g_fillMode.load(); }

void set_fill_mode(int mode)
{
    if (mode < 0) mode = 0;
    if (mode > 3) mode = 3;
    g_fillMode.store(mode);
}

float gpu_ms()     { return g.gpuMs; }
float gpu_avg_ms() { return g.gpuAvgMs; }

void report_gpu_cost_if_high()
{
    // A threshold report, not a heartbeat. A steady "the warp costs 1.2 ms"
    // line every few seconds is exactly the kind of noise that buries the
    // lines worth reading; "it is now costing a sixth of the frame" is not.
    //
    // 4 ms against an 11.1 ms budget at 90 Hz, measured on the SMOOTHED value
    // so one heavy frame (a near-field view where splatCap is doing real work)
    // cannot trip it.
    constexpr float kHighMs = 4.0f;
    constexpr DWORD kQuietMs = 30000;

    const float avg = g.gpuAvgMs;
    if (avg < kHighMs) return;

    static DWORD lastLog = 0;
    const DWORD now = GetTickCount();
    if (lastLog != 0 && now - lastLog < kQuietMs) return;
    lastLog = now;
    VRLOG("DIBR shift: warp GPU cost is high -- %.2f ms avg (last %.2f ms) at %ux%u. "
          "It runs inline at Present, so this is on the frame the headset is "
          "waiting for.", avg, g.gpuMs, g.w, g.h);
}

} // namespace dibr
