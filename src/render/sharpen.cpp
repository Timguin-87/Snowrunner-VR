#include "render/sharpen.hpp"
#include "render/d3d11_state.hpp"
#include "hooks/depth_probe.h"      // set_self_targets()
#include "hooks/shader_cull.h"      // set_self_render()
#include "common/log.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace sharpen {
namespace {

// AMD's FidelityFX CAS, its filter at native size, ported from AMD's ffx_cas.h
// (1.20190610). The exact reciprocal and square root -- AMD's "go slower" path
// -- since one pass a frame has no need of the approximations.
//
// Like AMD's, the strength is worked out from green alone and applied to all
// three channels, so it can never shift a colour.
//
// The filter (kCasHlsl) is AMD's work under AMD's notice, which follows; the
// rest of this file is the mod's own.
//
// Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
const char kCasHlsl[] = R"(
cbuffer Cas : register(b0)
{
    float  Peak;     // the side weight where the filter is strongest, < 0
    float  Amount;   // how much of the filtered image goes over the frame
    float2 Pad;
};
Texture2D<float4> Src : register(t0);

float4 VS(uint id : SV_VertexID) : SV_Position
{
    const float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 tap(int2 p, int2 o, int2 hi)
{
    return Src[uint2(clamp(p + o, int2(0, 0), hi))];
}

float4 PS(float4 pos : SV_Position) : SV_Target
{
    uint sw, sh;
    Src.GetDimensions(sw, sh);
    const int2 p  = int2(pos.xy);
    const int2 hi = int2(sw, sh) - 1;
    // a b c
    // d e f
    // g h i
    const float3 a = tap(p, int2(-1, -1), hi).rgb;
    const float3 b = tap(p, int2( 0, -1), hi).rgb;
    const float3 c = tap(p, int2( 1, -1), hi).rgb;
    const float3 d = tap(p, int2(-1,  0), hi).rgb;
    const float4 e = tap(p, int2( 0,  0), hi);
    const float3 f = tap(p, int2( 1,  0), hi).rgb;
    const float3 g = tap(p, int2(-1,  1), hi).rgb;
    const float3 h = tap(p, int2( 0,  1), hi).rgb;
    const float3 i = tap(p, int2( 1,  1), hi).rgb;

    // Soft minimum and maximum: the cross's plus the whole 3x3's, so twice an
    // average of the two -- which is why the limit below is 2 rather than 1.
    float mn = min(min(min(d.g, e.g), min(f.g, b.g)), h.g);
    mn += min(mn, min(min(a.g, c.g), min(g.g, i.g)));
    float mx = max(max(max(d.g, e.g), max(f.g, b.g)), h.g);
    mx += max(mx, max(max(a.g, c.g), max(g.g, i.g)));

    // The room left before the filter would clip, at either end, over the
    // brightness, shaped by a square root: 1 where the image is soft, falling
    // to 0 at an edge that already reaches black or white.
    const float amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1.0e-5)));

    //  0 w 0
    //  w 1 w    w < 0, normalised so a flat area keeps its value
    //  0 w 0
    const float  w     = amp * Peak;
    const float3 sharp = saturate(((b + d + f + h) * w + e.rgb) / (1.0 + 4.0 * w));
    return float4(lerp(e.rgb, sharp, Amount), e.a);
}
)";

// CAS's own strongest setting: -1 / lerp(8, 5, sharpness) at sharpness 1. The
// amount blends that over the frame instead of moving along CAS's own scale,
// because the bottom of that scale already sharpens -- this way 0 is nothing
// and the slider rises from there.
constexpr float kPeak = -1.0f / 5.0f;

struct Cb {
    float peak, amount;
    float pad[2];
};
static_assert(sizeof(Cb) % 16 == 0, "constant buffer size");

struct State {
    ID3D11Device* dev = nullptr;   // what the objects below belong to, not owned
    ComPtr<ID3D11VertexShader>      vs;
    ComPtr<ID3D11PixelShader>       ps;
    ComPtr<ID3D11Buffer>            cb;
    ComPtr<ID3D11RasterizerState>   rs;
    ComPtr<ID3D11DepthStencilState> ds;
    bool failed = false;

    // The result, and -- for a frame the caller has no view onto -- a copy of
    // the frame to read. Both the frame's own size and format, which is what
    // they were last made for (or tried for: a failure is not retried at the
    // same size).
    ComPtr<ID3D11Texture2D>          out;
    ComPtr<ID3D11RenderTargetView>   outRtv;
    ComPtr<ID3D11Texture2D>          copy;
    ComPtr<ID3D11ShaderResourceView> copySrv;
    bool        copyTried = false;
    UINT        w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
} g;

void release_textures()
{
    g.out.Reset(); g.outRtv.Reset();
    g.copy.Reset(); g.copySrv.Reset();
    g.copyTried = false;
    g.w = g.h = 0;
    g.fmt = DXGI_FORMAT_UNKNOWN;
}

void release_all()
{
    release_textures();
    g.vs.Reset(); g.ps.Reset(); g.cb.Reset(); g.rs.Reset(); g.ds.Reset();
    g.dev = nullptr;
    g.failed = false;
}

bool ensure_pipeline(ID3D11Device* dev)
{
    if (g.dev && g.dev != dev) release_all();
    if (g.vs && g.ps && g.cb && g.rs && g.ds) return true;
    if (g.failed) return false;
    g.dev = dev;

    ComPtr<ID3DBlob> vsCode, psCode, err;
    if (FAILED(D3DCompile(kCasHlsl, sizeof(kCasHlsl) - 1, "cas", nullptr, nullptr, "VS",
                          "vs_5_0", 0, 0, &vsCode, &err)) ||
        FAILED(D3DCompile(kCasHlsl, sizeof(kCasHlsl) - 1, "cas", nullptr, nullptr, "PS",
                          "ps_5_0", 0, 0, &psCode, &err))) {
        VRLOG("sharpen: CAS shader compile FAILED: %s",
              err ? (const char*)err->GetBufferPointer() : "(no message)");
        g.failed = true;
        return false;
    }

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth      = sizeof(Cb);
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;

    D3D11_DEPTH_STENCIL_DESC dd{};   // no depth, no stencil

    const bool wasSelf = hooks::set_self_render(true);   // ours: never a cull candidate
    const bool ok =
        SUCCEEDED(dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(),
                                          nullptr, &g.vs)) &&
        SUCCEEDED(dev->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(),
                                         nullptr, &g.ps)) &&
        SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g.cb)) &&
        SUCCEEDED(dev->CreateRasterizerState(&rd, &g.rs)) &&
        SUCCEEDED(dev->CreateDepthStencilState(&dd, &g.ds));
    hooks::set_self_render(wasSelf);
    if (!ok) {
        VRLOG("sharpen: could not create its pipeline objects");
        release_all();
        g.failed = true;
        return false;
    }
    VRLOG("sharpen: CAS ready");
    return true;
}

// The output at the frame's size and format, and the copy of the frame when
// `needCopy`.
bool ensure_textures(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& sd, bool needCopy)
{
    if (g.w != sd.Width || g.h != sd.Height || g.fmt != sd.Format) {
        release_textures();
        g.w = sd.Width; g.h = sd.Height; g.fmt = sd.Format;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = sd.Width; d.Height = sd.Height; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = sd.Format;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &g.out)) ||
            FAILED(dev->CreateRenderTargetView(g.out.Get(), nullptr, &g.outRtv))) {
            VRLOG("sharpen: could not create its %ux%u output (format %d) -- not sharpening",
                  sd.Width, sd.Height, (int)sd.Format);
            g.out.Reset(); g.outRtv.Reset();
            return false;
        }
        VRLOG("sharpen: %ux%u output (format %d)", sd.Width, sd.Height, (int)sd.Format);
    }
    if (!g.out) return false;
    if (needCopy && !g.copy) {
        if (g.copyTried) return false;
        g.copyTried = true;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = sd.Width; d.Height = sd.Height; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = sd.Format;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &g.copy)) ||
            FAILED(dev->CreateShaderResourceView(g.copy.Get(), nullptr, &g.copySrv))) {
            VRLOG("sharpen: could not create its %ux%u copy of the frame (format %d)",
                  sd.Width, sd.Height, (int)sd.Format);
            g.copy.Reset(); g.copySrv.Reset();
            return false;
        }
    }
    return true;
}

} // namespace

ID3D11Texture2D* apply(ID3D11DeviceContext* ctx, ID3D11Texture2D* src,
                       ID3D11ShaderResourceView* srcSrv, float amount)
{
    if (!ctx || !src || !(amount > 0.0f)) return nullptr;
    D3D11_TEXTURE2D_DESC sd{};
    src->GetDesc(&sd);
    if (sd.SampleDesc.Count != 1 || sd.ArraySize != 1) return nullptr;
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!dev || !ensure_pipeline(dev.Get()) || !ensure_textures(dev.Get(), sd, !srcSrv))
        return nullptr;

    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return nullptr;
    const Cb c{kPeak, (std::min)(amount, 1.0f), {0.0f, 0.0f}};
    std::memcpy(m.pData, &c, sizeof(c));
    ctx->Unmap(g.cb.Get(), 0);

    if (!srcSrv) {
        ctx->CopyResource(g.copy.Get(), src);
        srcSrv = g.copySrv.Get();
    }

    D3D11StateBackup saved;
    saved.capture(ctx);
    const bool wasSelf = hooks::set_self_render(true);

    ID3D11RenderTargetView* rtv = g.outRtv.Get();
    hooks::set_self_targets(true);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    hooks::set_self_targets(false);
    const D3D11_VIEWPORT vp{0.0f, 0.0f, (float)sd.Width, (float)sd.Height, 0.0f, 1.0f};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(g.rs.Get());
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    ctx->OMSetDepthStencilState(g.ds.Get(), 0);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g.vs.Get(), nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(g.ps.Get(), nullptr, 0);
    ID3D11Buffer* cbs[1] = {g.cb.Get()};
    ctx->PSSetConstantBuffers(0, 1, cbs);
    ctx->PSSetShaderResources(0, 1, &srcSrv);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);

    hooks::set_self_targets(true);
    saved.restore(ctx);
    hooks::set_self_targets(false);
    hooks::set_self_render(wasSelf);
    return g.out.Get();
}

void release() { release_textures(); }

} // namespace sharpen
