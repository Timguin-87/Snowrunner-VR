#include "render/motion_field.hpp"
#include "render/gpu_timer.hpp"
#include "render/mat4d.hpp"
#include "hooks/cbuffer_hook.h"
#include "hooks/shader_cull.h"
#include "xr/xr_mirror.h"
#include "common/log.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace motionfield {
namespace {

using mat4d::M4;

// The velocity buffer, decoded exactly as the game's TAA decodes it: .xy =
// 0.498 + 0.498 * sign(d) * sqrt(|d| / 256) with d = current - previous in
// pixels, .z = the surface's previous device depth, (1, 1) = static (or behind
// the camera, where the game falls back to reprojecting by depth).
//
// The previous position is the surface in the PREVIOUS render's image, which
// under AER is the other eye's -- the image the stale-eye warp moves. Its world
// position there comes from that pixel and .z through the game's previous
// camera; its world position now from this render's depth through this
// render's camera. Both matrices are moved to this render's camera position
// on the CPU (render/mat4d.hpp), so the difference is taken in small numbers.
const char kHlsl[] = R"(
cbuffer Frame : register(b0) {
    row_major float4x4 InvVP;       // this render: clip -> world, relative to its camera
    row_major float4x4 InvVPprev;   // the game's previous camera (with this jitter)
    float2 Size; float2 InvSize;
};
Texture2D<float4>   Velocity : register(t0);
Texture2D<float>    Depth    : register(t1);   // the game's R16_FLOAT device depth
Texture2D<float4>   OwnIn    : register(t2);   // CS_Smooth: the raw motion; CS_Write: smoothed
RWTexture2D<uint>   Key      : register(u0);
RWTexture2D<float4> OnPrev   : register(u1);
RWTexture2D<float4> OwnOut   : register(u2);   // CS_Key: the raw motion; CS_Smooth: smoothed

float3 unproject(float4x4 inv, float2 px, float d)
{
    const float2 ndc = float2(px.x * InvSize.x * 2.0 - 1.0, 1.0 - px.y * InvSize.y * 2.0);
    const float4 w = mul(inv, float4(ndc, d, 1.0));
    return w.xyz / w.w;
}

// A moving surface: where it was in the previous render's image (q) and its
// device depth there (dPrev).
//
// CLIPPED AT 256 PX. The encoding ends there -- sqrt(|d| / 256) = 1 -- and the
// game saturates anything further. A flat screen never gets near it; under AER
// the previous render is the OTHER EYE, so every near surface carries the whole
// stereo disparity (~120 px at 0.5 m on a 2560 render) on top of its motion,
// and a fast head turn adds as much again toward the edge of the view. Where
// the two point the same way -- one eye order, for a given turn -- a clipped
// vector puts the surface's previous position short of where it was, and the
// stale eye's cab parts at the edge flew apart (Tim's dump 233845 f2,
// 2026-11-02). It says nothing usable, so the pixel is left out: the warp falls
// back to the surface's own motion from the render before, whose eye order
// subtracted the disparity instead. `clipped` marks it for the overlay.
bool moving(uint2 id, out float2 q, out float dPrev, out bool clipped)
{
    q = float2(0.0, 0.0); dPrev = 0.0; clipped = false;
    const float4 v = Velocity[id];
    if (v.x == 1.0 && v.y == 1.0) return false;
    const float2 s   = v.xy * 2.007874 - 1.0;
    if (max(abs(s.x), abs(s.y)) >= 0.999) { clipped = true; return false; }
    const float2 dpx = sign(s) * s * s * 256.0;
    q = float2(id) + 0.5 - dpx;
    dPrev = v.z;
    return dPrev > 0.0;
}

bool on_grid(int2 q) { return q.x >= 0 && q.y >= 0 && q.x < (int)Size.x && q.y < (int)Size.y; }

uint depth_key(float d) { return (uint)(saturate(d) * 16777215.0) + 1u; }

[numthreads(8, 8, 1)]
void CS_Clear(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)Size.x || id.y >= (uint)Size.y) return;
    Key[id.xy] = 0u;
    OnPrev[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
}

// Its own motion, raw, and a claim on where it was: in reverse-Z the nearer
// surface has the larger depth, so it wins the max.
[numthreads(8, 8, 1)]
void CS_Key(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)Size.x || id.y >= (uint)Size.y) return;
    float2 q; float dPrev; bool clipped;
    if (!moving(id.xy, q, dPrev, clipped)) {
        // w 0.25 for a clipped vector: not moving to anything that reads this
        // (they all ask w > 0.5), but the overlay shows where it happened.
        OwnOut[id.xy] = float4(0.0, 0.0, 0.0, clipped ? 0.25 : 0.0);
        return;
    }
    const float2 p = float2(id.xy) + 0.5;
    OwnOut[id.xy] = float4(unproject(InvVP, p, Depth[id.xy]) - unproject(InvVPprev, q, dPrev), 1.0);
    const int2 qi = int2(floor(q));
    if (on_grid(qi)) InterlockedMax(Key[qi], depth_key(dPrev));
}

// THE RAW MOTION IS NOISY ALONG THE VIEW RAY, by the half-float precision of
// the depth the game hands its TAA: ~0.05% of the distance, 0.3-0.4 mm on the
// cab and mirrors (measured from the cockpit capture against the D32 depth).
// The error flips sign every pixel or two across a slanted surface, so seen
// from the other eye the motion jitters by ~0.1 px from pixel to pixel, and
// the warp's nearest-pixel scatter turns that into pixels flipping back and
// forth -- a shimmer on the mirrors' fine detail that the rigid path, which
// moves a whole region by one transform, never had.
//
// The motion of one rigid part is smooth (linear across it, for a rotation),
// so averaging it over 5x5 neighbours on the same surface (depth within 1%)
// keeps the motion and cancels the noise.
[numthreads(8, 8, 1)]
void CS_Smooth(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)Size.x || id.y >= (uint)Size.y) return;
    const float4 c = OwnIn[id.xy];
    if (!(c.w > 0.5)) { OwnOut[id.xy] = float4(0.0, 0.0, 0.0, c.w); return; }
    const float dc  = Depth[id.xy];
    const float tol = 0.01 * dc + 1.0e-7;
    float3 sum = float3(0.0, 0.0, 0.0);
    float  n   = 0.0;
    [unroll] for (int y = -2; y <= 2; ++y) {
        [unroll] for (int x = -2; x <= 2; ++x) {
            const int2 t = int2(id.xy) + int2(x, y);
            if (!on_grid(t)) continue;
            const float4 o = OwnIn[t];
            if (o.w > 0.5 && abs(Depth[t] - dc) <= tol) { sum += o.xyz; n += 1.0; }
        }
    }
    OwnOut[id.xy] = float4(sum / max(n, 1.0), 1.0);
}

// Only the winner writes its (smoothed) motion there.
[numthreads(8, 8, 1)]
void CS_Write(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)Size.x || id.y >= (uint)Size.y) return;
    float2 q; float dPrev; bool clipped;
    if (!moving(id.xy, q, dPrev, clipped)) return;
    const int2 qi = int2(floor(q));
    if (on_grid(qi) && Key[qi] == depth_key(dPrev)) OnPrev[qi] = float4(OwnIn[id.xy].xyz, dPrev);
}
)";

struct Cb {
    float invVP[16];
    float invVPprev[16];
    float size[2], invSize[2];
};
static_assert(sizeof(Cb) % 16 == 0, "constant buffer size");

struct State {
    ID3D11Device* dev = nullptr;   // what the resources below belong to, not owned
    ComPtr<ID3D11ComputeShader> csClear, csKey, csSmooth, csWrite;
    ComPtr<ID3D11Buffer> cb;
    bool shadersFailed = false;

    UINT w = 0, h = 0;
    ComPtr<ID3D11Texture2D> key, onPrev, raw, own[2];
    ComPtr<ID3D11UnorderedAccessView> keyUav, onPrevUav, rawUav, ownUav[2];
    ComPtr<ID3D11ShaderResourceView> onPrevSrv, rawSrv, ownSrv[2];
    int slot = 0;                  // own[slot]: the latest render's own motion, smoothed

    // The latest render measured, and whether it followed the one before.
    bool      haveCur = false;
    uint32_t  curBuild = 0, prevBuild = 0;
    ULONGLONG curTick = 0;
    bool      fieldValid = false;  // onPrev holds prevBuild -> curBuild
    uint32_t  madeAt = 0;          // presentCount when it was made
    uint32_t  presentCount = 0;
    ULONGLONG lastChained = 0;     // when the latest such field was made

    uint32_t  made = 0, unchained = 0, noRender = 0, noCamera = 0;
    ULONGLONG lastWanted = 0;
    GpuTimer  timer;
} g;

void release_all()
{
    g.csClear.Reset(); g.csKey.Reset(); g.csSmooth.Reset(); g.csWrite.Reset(); g.cb.Reset();
    g.key.Reset(); g.onPrev.Reset(); g.keyUav.Reset(); g.onPrevUav.Reset(); g.onPrevSrv.Reset();
    g.raw.Reset(); g.rawUav.Reset(); g.rawSrv.Reset();
    for (int s = 0; s < 2; ++s) { g.own[s].Reset(); g.ownUav[s].Reset(); g.ownSrv[s].Reset(); }
    g.w = g.h = 0;
    g.dev = nullptr;
    g.shadersFailed = false;
    g.haveCur = false;
    g.fieldValid = false;
    g.lastChained = 0;
    g.timer = GpuTimer();
}

bool ensure_shaders(ID3D11Device* dev)
{
    if (g.csClear && g.csKey && g.csSmooth && g.csWrite && g.cb) return true;
    if (g.shadersFailed) return false;
    auto build = [&](const char* entry, ComPtr<ID3D11ComputeShader>& out) -> bool {
        ComPtr<ID3DBlob> code, err;
        if (FAILED(D3DCompile(kHlsl, sizeof(kHlsl) - 1, "motion_field", nullptr, nullptr, entry,
                              "cs_5_0", 0, 0, &code, &err))) {
            VRLOG("motion field: %s compile FAILED: %s", entry,
                  err ? (const char*)err->GetBufferPointer() : "(no message)");
            return false;
        }
        const bool wasSelf = hooks::set_self_render(true);   // ours: never a cull candidate
        const bool ok = SUCCEEDED(dev->CreateComputeShader(code->GetBufferPointer(),
                                                           code->GetBufferSize(), nullptr, &out));
        hooks::set_self_render(wasSelf);
        return ok;
    };
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth      = sizeof(Cb);
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (!build("CS_Clear", g.csClear) || !build("CS_Key", g.csKey) ||
        !build("CS_Smooth", g.csSmooth) || !build("CS_Write", g.csWrite) ||
        FAILED(dev->CreateBuffer(&bd, nullptr, &g.cb))) {
        g.csClear.Reset(); g.csKey.Reset(); g.csSmooth.Reset(); g.csWrite.Reset(); g.cb.Reset();
        g.shadersFailed = true;
        return false;
    }
    VRLOG("motion field: shaders compiled -- the stale-eye warp can follow the game's "
          "motion vectors");
    return true;
}

bool ensure_textures(ID3D11Device* dev, UINT w, UINT h)
{
    if (g.key && g.w == w && g.h == h) return true;
    g.key.Reset(); g.onPrev.Reset(); g.keyUav.Reset(); g.onPrevUav.Reset(); g.onPrevSrv.Reset();
    g.raw.Reset(); g.rawUav.Reset(); g.rawSrv.Reset();
    for (int s = 0; s < 2; ++s) { g.own[s].Reset(); g.ownUav[s].Reset(); g.ownSrv[s].Reset(); }
    g.w = g.h = 0;
    g.fieldValid = false;
    g.haveCur = false;
    auto make = [&](DXGI_FORMAT f, ComPtr<ID3D11Texture2D>& t,
                    ComPtr<ID3D11UnorderedAccessView>& uav,
                    ComPtr<ID3D11ShaderResourceView>* srv) -> bool {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = f;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_UNORDERED_ACCESS | (srv ? D3D11_BIND_SHADER_RESOURCE : 0);
        return SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &t)) &&
               SUCCEEDED(dev->CreateUnorderedAccessView(t.Get(), nullptr, &uav)) &&
               (!srv || SUCCEEDED(dev->CreateShaderResourceView(t.Get(), nullptr,
                                                               srv->GetAddressOf())));
    };
    const bool ok = make(DXGI_FORMAT_R32_UINT, g.key, g.keyUav, nullptr) &&
                    make(DXGI_FORMAT_R16G16B16A16_FLOAT, g.onPrev, g.onPrevUav, &g.onPrevSrv) &&
                    make(DXGI_FORMAT_R16G16B16A16_FLOAT, g.raw, g.rawUav, &g.rawSrv) &&
                    make(DXGI_FORMAT_R16G16B16A16_FLOAT, g.own[0], g.ownUav[0], &g.ownSrv[0]) &&
                    make(DXGI_FORMAT_R16G16B16A16_FLOAT, g.own[1], g.ownUav[1], &g.ownSrv[1]);
    if (!ok) {
        VRLOG("motion field: could not create its %ux%u textures", w, h);
        g.key.Reset(); g.onPrev.Reset(); g.keyUav.Reset(); g.onPrevUav.Reset();
        g.onPrevSrv.Reset(); g.raw.Reset(); g.rawUav.Reset(); g.rawSrv.Reset();
        for (int s = 0; s < 2; ++s) { g.own[s].Reset(); g.ownUav[s].Reset(); g.ownSrv[s].Reset(); }
        return false;
    }
    g.w = w; g.h = h;
    return true;
}

} // namespace

bool wanted()
{
    return xr::warp_uses_motion() && xr::warp_active() && xr::xr_session_running();
}

void on_taa_resolve(ID3D11DeviceContext* ctx)
{
    if (!ctx || !wanted()) return;
    g.lastWanted = GetTickCount64();

    // The game's resolve inputs: t2 its R16 depth, t3 its velocity buffer.
    ID3D11ShaderResourceView* srvRaw[4] = {};
    ctx->PSGetShaderResources(0, 4, srvRaw);
    ComPtr<ID3D11ShaderResourceView> srv[4];
    for (int i = 0; i < 4; ++i) srv[i].Attach(srvRaw[i]);
    if (!srv[2] || !srv[3]) return;
    ComPtr<ID3D11Resource> depthRes;
    srv[2]->GetResource(&depthRes);
    ComPtr<ID3D11Texture2D> depthTex;
    if (!depthRes || FAILED(depthRes.As(&depthTex))) return;
    D3D11_TEXTURE2D_DESC dd{};
    depthTex->GetDesc(&dd);
    const UINT w = dd.Width, h = dd.Height;

    xr::RenderInfo ri;
    if (!xr::render_in_progress(ri)) { ++g.noRender; g.haveCur = false; g.fieldValid = false; return; }

    float view[16], vp[16], vpPrev[16];
    if (!hooks::main_camera_matrices(view, vp, vpPrev)) {
        ++g.noCamera; g.haveCur = false; g.fieldValid = false; return;
    }
    const M4 V = mat4d::load(view);
    M4 invV;
    if (!mat4d::inverse(V, invV)) return;
    const double O[3] = {invV.m[3], invV.m[7], invV.m[11]};
    M4 invVP, invVPprev;
    if (!mat4d::inverse(mat4d::relative_to(mat4d::load(vp), O), invVP) ||
        !mat4d::inverse(mat4d::relative_to(mat4d::load(vpPrev), O), invVPprev))
        return;

    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (g.dev && g.dev != dev.Get()) release_all();
    g.dev = dev.Get();
    if (!ensure_shaders(dev.Get()) || !ensure_textures(dev.Get(), w, h)) return;

    // The previous render is the one this measures motion against. Only good
    // when it was the render just before -- the next build, same grid, recent.
    const ULONGLONG now = GetTickCount64();
    const bool chained = g.haveCur && ri.build == g.curBuild + 1 && now - g.curTick <= 100;

    Cb c{};
    mat4d::store(c.invVP, invVP);
    mat4d::store(c.invVPprev, invVPprev);
    c.size[0] = (float)w; c.size[1] = (float)h;
    c.invSize[0] = 1.0f / (float)w; c.invSize[1] = 1.0f / (float)h;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    std::memcpy(m.pData, &c, sizeof(c));
    ctx->Unmap(g.cb.Get(), 0);

    // The compute stage is ours for four dispatches; the game's resolve draw
    // comes straight after and uses none of it, but it goes back as it was.
    ComPtr<ID3D11ComputeShader>       oldCs;
    ComPtr<ID3D11Buffer>              oldCb;
    ComPtr<ID3D11ShaderResourceView>  oldSrv[3];
    ComPtr<ID3D11UnorderedAccessView> oldUav[3];
    ctx->CSGetShader(&oldCs, nullptr, nullptr);
    ctx->CSGetConstantBuffers(0, 1, oldCb.GetAddressOf());
    ctx->CSGetShaderResources(0, 3, oldSrv[0].GetAddressOf());
    ctx->CSGetUnorderedAccessViews(0, 3, oldUav[0].GetAddressOf());

    const bool wasSelf = hooks::set_self_render(true);
    const int cur = g.slot ^ 1;
    ID3D11Buffer* cbs = g.cb.Get();
    ctx->CSSetConstantBuffers(0, 1, &cbs);
    const UINT gx = (w + 7) / 8, gy = (h + 7) / 8;
    g.timer.begin(ctx);
    // Clear and claim: the raw motion into `raw`.
    ID3D11ShaderResourceView* in[3] = { srv[3].Get(), srv[2].Get(), nullptr };
    ctx->CSSetShaderResources(0, 3, in);
    ID3D11UnorderedAccessView* outs[3] = { g.keyUav.Get(), g.onPrevUav.Get(), g.rawUav.Get() };
    ctx->CSSetUnorderedAccessViews(0, 3, outs, nullptr);
    ctx->CSSetShader(g.csClear.Get(), nullptr, 0);
    ctx->Dispatch(gx, gy, 1);
    ctx->CSSetShader(g.csKey.Get(), nullptr, 0);
    ctx->Dispatch(gx, gy, 1);
    // Smooth: `raw` is read now, so its UAV goes first.
    ID3D11UnorderedAccessView* smoothOut[1] = { g.ownUav[cur].Get() };
    ctx->CSSetUnorderedAccessViews(2, 1, smoothOut, nullptr);
    ID3D11ShaderResourceView* rawIn[1] = { g.rawSrv.Get() };
    ctx->CSSetShaderResources(2, 1, rawIn);
    ctx->CSSetShader(g.csSmooth.Get(), nullptr, 0);
    ctx->Dispatch(gx, gy, 1);
    // Scatter the smoothed motion: own[cur] is read now.
    ID3D11UnorderedAccessView* noOut[1] = { nullptr };
    ctx->CSSetUnorderedAccessViews(2, 1, noOut, nullptr);
    ID3D11ShaderResourceView* ownIn[1] = { g.ownSrv[cur].Get() };
    ctx->CSSetShaderResources(2, 1, ownIn);
    ctx->CSSetShader(g.csWrite.Get(), nullptr, 0);
    ctx->Dispatch(gx, gy, 1);
    g.timer.end(ctx);
    hooks::set_self_render(wasSelf);

    ID3D11ShaderResourceView* oldS[3] = { oldSrv[0].Get(), oldSrv[1].Get(), oldSrv[2].Get() };
    ID3D11UnorderedAccessView* oldU[3] = { oldUav[0].Get(), oldUav[1].Get(), oldUav[2].Get() };
    ID3D11ShaderResourceView* noIn[3] = {};
    ctx->CSSetShaderResources(0, 3, noIn);
    ctx->CSSetUnorderedAccessViews(0, 3, oldU, nullptr);
    ctx->CSSetShaderResources(0, 3, oldS);
    ID3D11Buffer* oldC = oldCb.Get();
    ctx->CSSetConstantBuffers(0, 1, &oldC);
    ctx->CSSetShader(oldCs.Get(), nullptr, 0);

    g.slot = cur;
    g.prevBuild = g.curBuild;
    g.curBuild = ri.build;
    g.curTick = now;
    g.haveCur = true;
    g.fieldValid = chained;
    g.madeAt = g.presentCount;
    if (chained) g.lastChained = now;
    ++(chained ? g.made : g.unchained);
}

bool measuring()
{
    return g.haveCur && GetTickCount64() - g.curTick <= 2000;
}

bool covering()
{
    return g.lastChained && GetTickCount64() - g.lastChained <= 500;
}

float take_gpu_ms() { return g.timer.take_mean_ms(); }

bool latest(Field& out)
{
    if (!g.fieldValid || g.madeAt != g.presentCount || !g.onPrevSrv) return false;
    out.onPrev    = g.onPrevSrv.Get();
    out.prevOwn   = g.ownSrv[g.slot ^ 1].Get();
    out.w         = g.w;
    out.h         = g.h;
    out.prevBuild = g.prevBuild;
    out.curBuild  = g.curBuild;
    return true;
}

bool latest_own(Own& out)
{
    if (!g.haveCur || g.madeAt != g.presentCount || !g.ownSrv[g.slot]) return false;
    out.srv       = g.ownSrv[g.slot].Get();
    out.w         = g.w;
    out.h         = g.h;
    out.prevBuild = g.prevBuild;
    out.curBuild  = g.curBuild;
    out.chained   = g.fieldValid;
    return true;
}

void on_present()
{
    ++g.presentCount;
    const ULONGLONG now = GetTickCount64();
    // Not wanted for a while -- another warp type, Synched Sequential, no
    // session: nothing will read the fields, so they go.
    if (g.key && now - g.lastWanted > 2000) {
        release_all();
        VRLOG("motion field: released (the motion-vector warp is not running)");
    }
    static ULONGLONG t0 = 0;
    if (!t0) t0 = now;
    if (now - t0 >= 10000) {
        t0 = now;
        if (g.made || g.unchained || g.noRender || g.noCamera)
            VRLOG("motion field: %u measured in 10 s against the render before, %u with no "
                  "render just before it, %u with no build record, %u with no camera",
                  g.made, g.unchained, g.noRender, g.noCamera);
        g.made = g.unchained = g.noRender = g.noCamera = 0;
    }
}

} // namespace motionfield
