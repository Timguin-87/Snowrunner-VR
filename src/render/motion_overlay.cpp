#include "render/motion_overlay.hpp"
#include "render/d3d11_state.hpp"
#include "hooks/depth_probe.h"      // set_self_targets()
#include "common/log.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <atomic>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;

namespace motionoverlay {

// THE ENCODING. Saturation on a log scale, so a sway of a pixel or two already
// shows as a colour while a 30 px swing does not drown everything else.
const char kColourHlsl[] = R"(
float3 motion_hue(float h)
{
    return saturate(abs(frac(h + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
}

float4 motion_colour(float2 px, uint how, uint2 at)
{
    const float sat = saturate(log2(1.0 + length(px)) / 5.0);
    const float hue = frac(atan2(px.y, px.x) / 6.2831853 + 1.0);
    float a = 0.5;
    if (how == 2u && ((at.x + at.y) / 6u) % 2u == 1u) a = 0.0;
    if (how == 3u && ((at.x + 8192u - at.y) / 6u) % 2u == 1u) a = 0.0;
    if (how == 4u && (at.y / 6u) % 2u == 1u) a = 0.0;
    return float4(lerp(float3(1.0, 1.0, 1.0), motion_hue(hue), sat), a);
}
)";

namespace {

// The rendered eye's pass: a full-screen triangle over the image's viewport, each
// pixel taking the render's own motion at the render pixel beneath it.
const char kDrawHlsl[] = R"(
cbuffer Overlay : register(b0)
{
    row_major float4x4 InvVP;
    row_major float4x4 VP;
    float4 Rect;          // the image's viewport in the target: x, y, w, h
    float2 ColourSize;    // the image's own size
    float  Srgb;          // 1 = the target encodes on write
    float  Pad0;
    float3 HeadMove;
    float  Pad1;
};
Texture2D<float4> Motion : register(t0);   // xyz world motion, w 1 = moving
Texture2D<float>  Depth  : register(t1);   // device depth, reverse-Z

float4 VS(uint id : SV_VertexID) : SV_Position
{
    const float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float3 to_linear(float3 c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float4 PS(float4 pos : SV_Position) : SV_Target
{
    const float2 uv = (pos.xy - Rect.xy) / Rect.zw;
    if (any(uv < 0.0) || any(uv >= 1.0)) discard;
    uint mw, mh, dw, dh;
    Motion.GetDimensions(mw, mh);
    Depth.GetDimensions(dw, dh);
    const float4 m = Motion[min(uint2(uv * float2(mw, mh)), uint2(mw - 1, mh - 1))];
    if (!(m.w > 0.5)) {
        // CLIPPED: the game's vector ran past its 256 px limit, so the warp
        // cannot use it (motion_field.cpp). Yellow on every other pixel.
        if (m.w > 0.2 && ((uint(pos.x) + uint(pos.y)) & 1u) == 0u)
            return float4(1.0, 1.0, 0.0, 1.0);
        discard;
    }
    const float d = Depth[min(uint2(uv * float2(dw, dh)), uint2(dw - 1, dh - 1))];
    if (!(d > 1.0e-6)) discard;

    // Where the surface is, and where the head would see it had it moved only
    // by its motion beyond the head's own.
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 w = mul(InvVP, float4(ndc, d, 1.0));
    w /= w.w;
    const float4 c = mul(VP, float4(w.xyz + m.xyz - HeadMove, 1.0));
    float2 px = float2(1.0e4, 0.0);
    if (c.w > 1.0e-6) px = (c.xy / c.w - ndc) * 0.5 * ColourSize;

    float4 o = motion_colour(px, 1u, uint2(uv * ColourSize));
    if (Srgb > 0.5) o.rgb = to_linear(o.rgb);
    return o;
}
)";

struct Cb {
    float invVP[16];
    float vp[16];
    float rect[4];
    float colourSize[2];
    float srgb, pad0;
    float headMove[3];
    float pad1;
};
static_assert(sizeof(Cb) % 16 == 0, "constant buffer size");

struct State {
    ID3D11Device* dev = nullptr;   // what the objects below belong to, not owned
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader>  ps;
    ComPtr<ID3D11Buffer>       cb;
    ComPtr<ID3D11BlendState>   blend;
    ComPtr<ID3D11RasterizerState>   rs;
    ComPtr<ID3D11DepthStencilState> ds;
    bool failed = false;
} g;

std::atomic<bool> g_enabled{false};

void release_all()
{
    g.vs.Reset(); g.ps.Reset(); g.cb.Reset(); g.blend.Reset(); g.rs.Reset(); g.ds.Reset();
    g.dev = nullptr;
    g.failed = false;
}

bool ensure(ID3D11Device* dev)
{
    if (g.dev && g.dev != dev) release_all();
    if (g.vs && g.ps && g.cb && g.blend && g.rs && g.ds) return true;
    if (g.failed) return false;
    g.dev = dev;

    const std::string src = std::string(kColourHlsl) + kDrawHlsl;
    ComPtr<ID3DBlob> vsCode, psCode, err;
    if (FAILED(D3DCompile(src.data(), src.size(), "motion_overlay", nullptr, nullptr, "VS",
                          "vs_5_0", 0, 0, &vsCode, &err)) ||
        FAILED(D3DCompile(src.data(), src.size(), "motion_overlay", nullptr, nullptr, "PS",
                          "ps_5_0", 0, 0, &psCode, &err))) {
        VRLOG("motion overlay: compile FAILED: %s",
              err ? (const char*)err->GetBufferPointer() : "(no message)");
        g.failed = true;
        return false;
    }

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth      = sizeof(Cb);
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    // Colour over the image by its alpha; the image's own alpha left alone.
    D3D11_BLEND_DESC bl{};
    bl.RenderTarget[0].BlendEnable    = TRUE;
    bl.RenderTarget[0].SrcBlend       = D3D11_BLEND_SRC_ALPHA;
    bl.RenderTarget[0].DestBlend      = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp        = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ZERO;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;

    D3D11_DEPTH_STENCIL_DESC dd{};   // no depth, no stencil

    if (FAILED(dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(),
                                       nullptr, &g.vs)) ||
        FAILED(dev->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(),
                                      nullptr, &g.ps)) ||
        FAILED(dev->CreateBuffer(&bd, nullptr, &g.cb)) ||
        FAILED(dev->CreateBlendState(&bl, &g.blend)) ||
        FAILED(dev->CreateRasterizerState(&rd, &g.rs)) ||
        FAILED(dev->CreateDepthStencilState(&dd, &g.ds))) {
        VRLOG("motion overlay: could not create its pipeline objects");
        release_all();
        g.failed = true;
        return false;
    }
    VRLOG("motion overlay: ready");
    return true;
}

} // namespace

bool enabled() { return g_enabled.load(); }

void set_enabled(bool on)
{
    if (g_enabled.exchange(on) != on)
        VRLOG("motion overlay -> %s", on ? "ON" : "off");
}

bool draw(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv,
          float x, float y, float w, float h, const Params& p)
{
    if (!ctx || !rtv || !p.motionSrv || !p.depthSrv || !(w > 0.0f) || !(h > 0.0f) ||
        !p.colourW || !p.colourH)
        return false;
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!dev || !ensure(dev.Get())) return false;

    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    Cb c{};
    std::memcpy(c.invVP, p.invViewProj, sizeof(c.invVP));
    std::memcpy(c.vp, p.viewProj, sizeof(c.vp));
    c.rect[0] = x; c.rect[1] = y; c.rect[2] = w; c.rect[3] = h;
    c.colourSize[0] = (float)p.colourW;
    c.colourSize[1] = (float)p.colourH;
    c.srgb = p.srgbTarget ? 1.0f : 0.0f;
    std::memcpy(c.headMove, p.headMove, sizeof(c.headMove));
    std::memcpy(m.pData, &c, sizeof(c));
    ctx->Unmap(g.cb.Get(), 0);

    D3D11StateBackup saved;
    saved.capture(ctx);

    hooks::set_self_targets(true);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    hooks::set_self_targets(false);
    const D3D11_VIEWPORT vp{x, y, w, h, 0.0f, 1.0f};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(g.rs.Get());
    const float bf[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    ctx->OMSetBlendState(g.blend.Get(), bf, 0xFFFFFFFFu);
    ctx->OMSetDepthStencilState(g.ds.Get(), 0);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g.vs.Get(), nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(g.ps.Get(), nullptr, 0);
    ID3D11Buffer* cbs[1] = {g.cb.Get()};
    ctx->PSSetConstantBuffers(0, 1, cbs);
    ID3D11ShaderResourceView* srvs[2] = {p.motionSrv, p.depthSrv};
    ctx->PSSetShaderResources(0, 2, srvs);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none[2] = {};
    ctx->PSSetShaderResources(0, 2, none);

    hooks::set_self_targets(true);
    saved.restore(ctx);
    hooks::set_self_targets(false);
    return true;
}

void release() { release_all(); }

} // namespace motionoverlay
