#include "render/reproject.hpp"
#include "common/log.h"

#include <d3dcompiler.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace reproject {
namespace {

// THREE PASSES, and the reason is an atomics limitation rather than a design
// preference.
//
// A forward scatter has to resolve overlaps by depth: when two source pixels
// land on the same destination, the nearer one wins. dibr.cpp does that in ONE
// pass by packing [disparity | srcX] into a uint and using InterlockedMax, so
// the depth ordering and the payload move together. That works there because
// the payload is a single 16-bit X coordinate -- the warp is horizontal, so Y
// is unchanged.
//
// Here the payload is a full 2D coordinate: 2688x2688 needs 16 bits each, which
// leaves nothing for the depth key. D3D11's cs_5_0 has no 64-bit atomic
// (InterlockedMax on uint64 arrives with SM 6.6), so the two cannot travel in
// one word.
//
// So: scatter the depth key alone, then re-scatter and let each source write
// its coordinate only where its key matches the winner. Ties (two surfaces at
// the same quantised depth) resolve arbitrarily between equals, which is
// exactly what a single-pass InterlockedMax would have done anyway.
const char kHlsl[] = R"(
cbuffer P : register(b0)
{
    row_major float4x4 invOld;      // old clip -> world
    row_major float4x4 newVP;       // world -> new clip
    row_major float4x4 newVPRigid;  // ...with the camera's travel removed
    float2 texSize;
    float  minValidDepth;
    float  hasMask;
    float  rigidSkip;      // 1 = leave rigid pixels to the caller entirely
    float  farDepth;       // stand-in depth for sky; <= 0 restores the old skip
    float2 _pad;
};

Texture2D<float>    SrcDepth : register(t0);
Texture2D<float4>   SrcColor : register(t1);
// Coverage of camera-rigid geometry in the RETAINED frame: 1 = truck.
Texture2D<float>    RigidMask : register(t2);
RWTexture2D<uint>   Key      : register(u0);
RWTexture2D<uint>   Idx      : register(u1);
RWTexture2D<float4> Dst      : register(u2);

// How much of the camera's TRANSLATION this pixel should receive.
// 1 = world geometry, 0 = rigid with the camera.
//
// Sampled by integer coordinate: the mask is the same size as the retained
// frame by construction, so there is nothing to filter and a bilinear tap would
// only smear the silhouette by half a texel. The hard edge is correct -- the
// truck really does end there.
float rigid_weight(uint2 p)
{
    if (!(hasMask > 0.5f)) return 1.0f;    // no mask: everything is world
    return 1.0f - saturate(RigidMask[p]);
}

bool to_ndc(float4x4 m, float4 world, out float3 ndc)
{
    ndc = float3(0.0f, 0.0f, 0.0f);
    float4 c = mul(m, world);
    // Behind the camera, or on its plane: no valid projection.
    if (!(c.w > 1e-6f)) return false;
    ndc = c.xyz / c.w;
    return true;
}

// Returns false when this source pixel has nothing to contribute.
bool project(uint2 p, out int2 dst, out uint key)
{
    dst = int2(0, 0);
    key = 0u;

    float d = SrcDepth[p].r;
    // THE SKY SCATTERS TOO, at a finite stand-in distance.
    //
    // Reverse-Z puts the far plane at 0, which is also what a cleared
    // accumulator holds, so the skybox and any never-written pixel arrive here
    // as d = 0. Rejecting them looks right -- a point at infinity has no
    // parallax, so there is nothing to move -- and it is exactly wrong, for a
    // reason that only shows at silhouettes.
    //
    // CS_Resolve composites rather than replaces: a destination no source
    // reached keeps what the caller pre-filled, which is the whole frame under
    // the rotation-only warp. When the camera translates, a tree scatters to its
    // new position and VACATES the pixels it used to occupy. Those pixels want
    // sky. Sky is the one thing that never scatters, so nothing overwrites them
    // and they keep the pre-fill -- which still holds the tree, at its old
    // place. The tree ends up drawn twice: once translated, once not. Against
    // terrain the neighbouring ground pixels scatter in and cover it, which is
    // why this only ever showed against the skybox.
    //
    // Clamping instead of rejecting fixes it with no special case downstream. A
    // stand-in around 1e-4 is roughly a kilometre for a typical near plane, far
    // enough that the translation term is sub-pixel -- so the sky still lands
    // where rotation alone would put it -- and finite enough to unproject
    // without the precision collapse d = 0 would cause. The depth key it earns
    // is the lowest possible (ndc.z ~ 0 -> key 1), so sky can only ever win a
    // pixel that no real geometry reached: precisely the vacated ones.
    if (!(d > minValidDepth)) {
        if (!(farDepth > 0.0f)) return false;    // opt out: the old behaviour
        d = farDepth;
    }

    float2 uv = (float2(p) + 0.5f) / texSize;
    float4 clip = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, d, 1.0f);

    float4 world = mul(invOld, clip);
    if (!(abs(world.w) > 1e-12f)) return false;
    world /= world.w;

    // TWO TRANSFORMS, PICKED BY DEPTH. Both are real projections of the same
    // world point, so interpolating between them traces a path between two
    // valid answers rather than blending into a meaningless one -- which is why
    // the rigid branch removes the translation from the VIEW instead of just
    // leaving the pixel where it was. Leaving it would also undo the rotation,
    // and the cab genuinely does sweep across the screen when you look around.
    float w = rigid_weight(p);

    // ORBIT: contribute nothing at all for rigid pixels, rather than
    // contributing newVPRigid. That transform keeps the ROTATION and drops only
    // the translation, which is right for a camera that turns about its own
    // centre and wrong for one that swings around the truck -- there the truck
    // is what stays still while the camera's rotation sweeps past it, so
    // applying that rotation to it displaces it by the whole swing.
    //
    // Returning false leaves the destination as the caller left it, which is
    // the pose-only homography: the head correction those pixels do need, and
    // nothing else. See warp_homography_for(), which declines the game-rotation
    // warp in orbit for exactly the same reason.
    if (rigidSkip > 0.5f && w <= 0.0f) return false;

    float3 ndc = float3(0.0f, 0.0f, 0.0f);
    if (w >= 1.0f) {
        if (!to_ndc(newVP, world, ndc)) return false;
    } else if (w <= 0.0f) {
        if (!to_ndc(newVPRigid, world, ndc)) return false;
    } else {
        float3 a, b;
        if (!to_ndc(newVPRigid, world, a)) return false;
        if (!to_ndc(newVP, world, b)) return false;
        ndc = lerp(a, b, w);
    }

    // Reverse-Z again on the way out: outside [0,1] is beyond the frustum.
    if (!(ndc.z > 0.0f) || ndc.z > 1.0f) return false;
    if (abs(ndc.x) > 1.0f || abs(ndc.y) > 1.0f) return false;

    float2 dp = float2((ndc.x * 0.5f + 0.5f) * texSize.x - 0.5f,
                       (0.5f - ndc.y * 0.5f) * texSize.y - 0.5f);
    if (!(dp.x > -1.0f) || !(dp.y > -1.0f)) return false;   // false for NaN too

    dst = int2(round(dp.x), round(dp.y));
    if (dst.x < 0 || dst.y < 0 ||
        dst.x >= (int)texSize.x || dst.y >= (int)texSize.y) return false;

    // Nearer must win, and in reverse-Z nearer is LARGER, so the depth maps
    // straight onto InterlockedMax with no inversion. +1 so a real hit is never
    // the 0 that means "nothing written".
    key = (uint)(saturate(ndc.z) * 16777215.0f) + 1u;
    return true;
}

[numthreads(8, 8, 1)]
void CS_Clear(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)texSize.x || tid.y >= (uint)texSize.y) return;
    Key[tid.xy] = 0u;
    Idx[tid.xy] = 0u;
}

[numthreads(8, 8, 1)]
void CS_ScatterDepth(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)texSize.x || tid.y >= (uint)texSize.y) return;
    int2 dst; uint key;
    if (!project(tid.xy, dst, key)) return;
    InterlockedMax(Key[uint2(dst)], key);
}

[numthreads(8, 8, 1)]
void CS_ScatterIndex(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)texSize.x || tid.y >= (uint)texSize.y) return;
    int2 dst; uint key;
    if (!project(tid.xy, dst, key)) return;
    // Only the winner of pass 1 gets to publish its coordinate. Storing +1 on
    // both axes keeps 0 meaning "no source reached this pixel".
    if (Key[uint2(dst)] != key) return;
    Idx[uint2(dst)] = ((tid.y + 1u) << 16) | (tid.x + 1u);
}

[numthreads(8, 8, 1)]
void CS_Resolve(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)texSize.x || tid.y >= (uint)texSize.y) return;
    uint packed = Idx[tid.xy];
    // COMPOSITE, NOT REPLACE: a destination no source reached keeps whatever
    // the caller already put there -- the rotation-warped image. That is what
    // makes this strictly an improvement rather than a different set of holes.
    if (packed == 0u) return;
    uint2 src = uint2((packed & 0xFFFFu) - 1u, (packed >> 16) - 1u);
    Dst[tid.xy] = SrcColor[src];
}
)";

struct State {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11ComputeShader> csClear, csDepth, csIndex, csResolve;
    ComPtr<ID3D11Buffer> cb;

    ComPtr<ID3D11Texture2D>           key, idx;
    ComPtr<ID3D11UnorderedAccessView> keyUav, idxUav;
    UINT w = 0, h = 0;

    static constexpr int kTimerSlots = 4;
    ComPtr<ID3D11Query> tsDisjoint[kTimerSlots], tsBegin[kTimerSlots], tsEnd[kTimerSlots];
    bool  tsPending[kTimerSlots] = {};
    int   tsCursor = 0;
    bool  tsReady = false;
    bool  tsUnavailable = false;
    float gpuMs = -1.0f;
} g;

struct CB {
    float invOld[16];
    float newVP[16];
    float newVPRigid[16];
    // Field order MUST match the HLSL cbuffer above, register for register.
    float texSize[2];
    float minValidDepth;
    float hasMask;
    float rigidSkip;
    float farDepth;
    float pad[2];
};
static_assert(sizeof(CB) % 16 == 0, "constant buffer must be 16-byte aligned");

bool ensure_shaders(ID3D11Device* dev)
{
    if (g.csClear && g.csDepth && g.csIndex && g.csResolve && g.cb) return true;

    auto build = [&](const char* entry, ComPtr<ID3D11ComputeShader>& out) -> bool {
        ComPtr<ID3DBlob> code, err;
        const HRESULT hr = D3DCompile(kHlsl, sizeof(kHlsl) - 1, "reproject",
                                      nullptr, nullptr, entry, "cs_5_0", 0, 0,
                                      &code, &err);
        if (FAILED(hr)) {
            VRLOG("reproject: %s compile FAILED: %s", entry,
                  err ? (const char*)err->GetBufferPointer() : "(no message)");
            return false;
        }
        return SUCCEEDED(dev->CreateComputeShader(code->GetBufferPointer(),
                                                  code->GetBufferSize(), nullptr, &out));
    };
    if (!build("CS_Clear", g.csClear) || !build("CS_ScatterDepth", g.csDepth) ||
        !build("CS_ScatterIndex", g.csIndex) || !build("CS_Resolve", g.csResolve)) {
        g.csClear.Reset(); g.csDepth.Reset();
        g.csIndex.Reset(); g.csResolve.Reset();
        return false;
    }

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(CB);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &g.cb))) {
        VRLOG("reproject: constant buffer creation FAILED");
        g.csClear.Reset(); g.csDepth.Reset();
        g.csIndex.Reset(); g.csResolve.Reset();
        return false;
    }
    VRLOG("reproject: 6-DoF stale-eye shaders compiled");
    return true;
}

bool ensure_scratch(ID3D11Device* dev, UINT w, UINT h)
{
    if (g.key && g.idx && g.w == w && g.h == h) return true;
    g.keyUav.Reset(); g.idxUav.Reset(); g.key.Reset(); g.idx.Reset();
    g.w = g.h = 0;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &g.key)) ||
        FAILED(dev->CreateTexture2D(&td, nullptr, &g.idx)) ||
        FAILED(dev->CreateUnorderedAccessView(g.key.Get(), nullptr, &g.keyUav)) ||
        FAILED(dev->CreateUnorderedAccessView(g.idx.Get(), nullptr, &g.idxUav))) {
        VRLOG("reproject: scatter buffer creation FAILED (%ux%u)", w, h);
        g.keyUav.Reset(); g.idxUav.Reset(); g.key.Reset(); g.idx.Reset();
        return false;
    }
    g.w = w; g.h = h;
    return true;
}

bool ensure_timer(ID3D11Device* dev)
{
    if (g.tsReady) return true;
    if (g.tsUnavailable) return false;
    D3D11_QUERY_DESC dj{}; dj.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    D3D11_QUERY_DESC ts{}; ts.Query = D3D11_QUERY_TIMESTAMP;
    for (int i = 0; i < State::kTimerSlots; ++i) {
        if (FAILED(dev->CreateQuery(&dj, &g.tsDisjoint[i])) ||
            FAILED(dev->CreateQuery(&ts, &g.tsBegin[i])) ||
            FAILED(dev->CreateQuery(&ts, &g.tsEnd[i]))) {
            g.tsUnavailable = true;
            return false;
        }
        g.tsPending[i] = false;
    }
    g.tsReady = true;
    return true;
}

bool collect_timer(ID3D11DeviceContext* ctx, int slot)
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
    if (dj.Disjoint || dj.Frequency == 0 || t1 <= t0) return true;
    g.gpuMs = 1000.0f * (float)(t1 - t0) / (float)dj.Frequency;
    return true;
}

} // namespace

bool composite(ID3D11DeviceContext* ctx,
               ID3D11ShaderResourceView* srcColorSrv,
               ID3D11ShaderResourceView* srcDepthSrv,
               ID3D11UnorderedAccessView* dstUav,
               unsigned width, unsigned height,
               const Params& params)
{
    if (!ctx || !srcColorSrv || !srcDepthSrv || !dstUav || !width || !height)
        return false;

    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    if (!ensure_shaders(dev.Get())) return false;
    if (!ensure_scratch(dev.Get(), width, height)) return false;

    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    CB* cb = static_cast<CB*>(m.pData);
    for (int i = 0; i < 16; ++i) cb->invOld[i] = params.invOldViewProj[i];
    for (int i = 0; i < 16; ++i) cb->newVP[i] = params.newViewProj[i];
    for (int i = 0; i < 16; ++i) cb->newVPRigid[i] = params.newViewProjRigid[i];
    cb->texSize[0] = (float)width;
    cb->texSize[1] = (float)height;
    cb->minValidDepth = params.minValidDepth;
    cb->farDepth      = params.farDepth;
    cb->hasMask = params.rigidMaskSrv ? 1.0f : 0.0f;
    cb->rigidSkip = params.rigidSkip ? 1.0f : 0.0f;
    ctx->Unmap(g.cb.Get(), 0);

    // Save the compute stage, same contract as dibr::warp().
    ComPtr<ID3D11ComputeShader>       oldCs;
    ComPtr<ID3D11Buffer>              oldCb;
    ComPtr<ID3D11ShaderResourceView>  oldSrv[3];
    ComPtr<ID3D11UnorderedAccessView> oldUav[3];
    ctx->CSGetShader(&oldCs, nullptr, nullptr);
    ctx->CSGetConstantBuffers(0, 1, oldCb.GetAddressOf());
    ctx->CSGetShaderResources(0, 3, oldSrv[0].GetAddressOf());
    ctx->CSGetUnorderedAccessViews(0, 3, oldUav[0].GetAddressOf());

    // t2 may be null: the shader gates on hasMask rather than on the bind, so
    // a missing mask reads as "no truck anywhere" and the depth band alone
    // decides -- which is exactly the pre-mask behaviour.
    ID3D11ShaderResourceView*  srvs[3] = {srcDepthSrv, srcColorSrv,
                                          params.rigidMaskSrv};
    ID3D11UnorderedAccessView* uavs[3] = {g.keyUav.Get(), g.idxUav.Get(), dstUav};
    ID3D11Buffer* cbs[1] = {g.cb.Get()};
    ctx->CSSetConstantBuffers(0, 1, cbs);
    ctx->CSSetShaderResources(0, 3, srvs);
    ctx->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);

    const UINT gx = (width + 7) / 8, gy = (height + 7) / 8;

    const int tslot = g.tsCursor;
    const bool measure = ensure_timer(dev.Get()) && collect_timer(ctx, tslot);
    if (measure) {
        ctx->Begin(g.tsDisjoint[tslot].Get());
        ctx->End(g.tsBegin[tslot].Get());
    }

    // Cleared by a dispatch rather than ClearUnorderedAccessViewUint so both
    // scratch buffers are zeroed inside the same barrier chain as the scatters
    // that follow.
    ctx->CSSetShader(g.csClear.Get(), nullptr, 0);
    ctx->Dispatch(gx, gy, 1);
    ctx->CSSetShader(g.csDepth.Get(), nullptr, 0);
    ctx->Dispatch(gx, gy, 1);
    ctx->CSSetShader(g.csIndex.Get(), nullptr, 0);
    ctx->Dispatch(gx, gy, 1);
    ctx->CSSetShader(g.csResolve.Get(), nullptr, 0);
    ctx->Dispatch(gx, gy, 1);

    if (measure) {
        ctx->End(g.tsEnd[tslot].Get());
        ctx->End(g.tsDisjoint[tslot].Get());
        g.tsPending[tslot] = true;
        g.tsCursor = (tslot + 1) % State::kTimerSlots;
    }

    ID3D11UnorderedAccessView* restoreUav[3] = {
        oldUav[0].Get(), oldUav[1].Get(), oldUav[2].Get()};
    ID3D11ShaderResourceView* restoreSrv[3] = {oldSrv[0].Get(), oldSrv[1].Get(),
                                               oldSrv[2].Get()};
    ID3D11Buffer* restoreCb[1] = {oldCb.Get()};
    ctx->CSSetUnorderedAccessViews(0, 3, restoreUav, nullptr);
    ctx->CSSetShaderResources(0, 3, restoreSrv);
    ctx->CSSetConstantBuffers(0, 1, restoreCb);
    ctx->CSSetShader(oldCs.Get(), nullptr, 0);
    return true;
}

void release()
{
    g.keyUav.Reset(); g.idxUav.Reset(); g.key.Reset(); g.idx.Reset();
    g.w = g.h = 0;
}

float gpu_ms() { return g.gpuMs; }

} // namespace reproject
