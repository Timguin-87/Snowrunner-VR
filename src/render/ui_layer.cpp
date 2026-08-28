#include "render/ui_layer.hpp"
#include "hooks/depth_probe.h"
#include "common/log.h"

#include <wrl/client.h>
#include <atomic>

using Microsoft::WRL::ComPtr;

namespace uilayer {
namespace {

struct State {
    ComPtr<ID3D11Texture2D>          tex;
    ComPtr<ID3D11RenderTargetView>   rtv;
    ComPtr<ID3D11ShaderResourceView> srv;        // raw  -- values as the game wrote them
    ComPtr<ID3D11ShaderResourceView> srvSrgb;    // decodes those values to linear
    ComPtr<ID3D11BlendState>         blend;
    uint32_t w = 0, h = 0;
} g;

std::atomic<bool> g_active{false};
std::atomic<bool> g_drew{false};

void release_all()
{
    g_active.store(false);
    g_drew.store(false);
    g.srv.Reset(); g.srvSrgb.Reset(); g.rtv.Reset(); g.tex.Reset(); g.blend.Reset();
    g.w = g.h = 0;
}

bool build(ID3D11Device* dev, uint32_t w, uint32_t h)
{
    // TYPELESS, WITH TWO READ VIEWS -- the same pattern (and the same hazard)
    // xr_mirror.cpp's staging texture and prevEyeSrv/prevEyeSrvRaw document.
    //
    // WRITE side is plain UNORM: the game's UI shaders emit the gamma-encoded
    // values they would have written into the backbuffer, and an _SRGB render
    // target would silently encode them a second time.
    //
    // READ side depends on where the pixels are going, which is why there are
    // two. Copying into an _SRGB swapchain image needs the sRGB view, so the
    // decode on read and the encode on write cancel and the stored bits come
    // out identical to what the game drew -- exactly what the eye images do.
    // Copying into a plain UNORM one needs the raw view instead. Getting this
    // backwards is a whole-UI brightness error, not a subtle one.
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &g.tex))) {
        VRLOG("ui layer: CreateTexture2D %ux%u FAILED", w, h);
        return false;
    }
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    D3D11_SHADER_RESOURCE_VIEW_DESC sds = sd;
    sds.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (FAILED(dev->CreateRenderTargetView(g.tex.Get(), &rd, &g.rtv)) ||
        FAILED(dev->CreateShaderResourceView(g.tex.Get(), &sd, &g.srv)) ||
        FAILED(dev->CreateShaderResourceView(g.tex.Get(), &sds, &g.srvSrgb))) {
        VRLOG("ui layer: view creation FAILED");
        return false;
    }

    // Colour blends the ordinary way, alpha accumulates as coverage.
    //
    // The game's own UI draws with SrcAlpha/InvSrcAlpha onto an OPAQUE
    // backbuffer, where the destination alpha is irrelevant. Against a
    // transparent target it is not: reusing that blend for alpha would give
    // a = src.a*src.a, so every semi-transparent element would come out too
    // sparse to composite back. ONE/InvSrcAlpha is the standard fix and makes
    // the result premultiplied, which is what the compositor expects from a
    // quad layer submitted without UNPREMULTIPLIED_ALPHA.
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend       = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend      = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp        = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bd, &g.blend))) {
        VRLOG("ui layer: CreateBlendState FAILED");
        return false;
    }

    g.w = w; g.h = h;
    VRLOG("ui layer: %ux%u RGBA8 typeless ready (UNORM write, raw + sRGB reads)", w, h);
    return true;
}

} // namespace

bool active() { return g_active.load(std::memory_order_acquire); }
bool drew_this_frame() { return g_drew.load(std::memory_order_acquire); }

bool size(uint32_t& w, uint32_t& h)
{
    if (!g_active.load(std::memory_order_acquire)) return false;
    w = g.w; h = g.h;
    return w != 0 && h != 0;
}

ID3D11ShaderResourceView* srv()
{
    return g_active.load(std::memory_order_acquire) ? g.srv.Get() : nullptr;
}

ID3D11ShaderResourceView* srv_srgb()
{
    return g_active.load(std::memory_order_acquire) ? g.srvSrgb.Get() : nullptr;
}

void update(ID3D11Device* dev, uint32_t canvasW, uint32_t canvasH, bool wanted)
{
    if (!wanted || !dev || canvasW == 0 || canvasH == 0) {
        if (g.tex) { release_all(); VRLOG("ui layer: released -- UI renders into the frame again"); }
        return;
    }
    if (g.tex && g.w == canvasW && g.h == canvasH) return;

    release_all();
    if (build(dev, canvasW, canvasH))
        g_active.store(true, std::memory_order_release);
    else
        release_all();
}

bool begin_capture(ID3D11DeviceContext* ctx, Saved& saved)
{
    if (!ctx || !g_active.load(std::memory_order_acquire) || !g.rtv) return false;

    ctx->OMGetRenderTargets(8, saved.rtv, &saved.dsv);
    saved.rtvCount = 0;
    for (UINT i = 0; i < 8; ++i) if (saved.rtv[i]) saved.rtvCount = i + 1;
    ctx->OMGetBlendState(&saved.blend, saved.blendFactor, &saved.sampleMask);

    // NO depth view. The UI is screen-space and its own draws do not depend on
    // scene depth, but the depth buffer bound at that point describes the 3D
    // frame -- leaving it attached would let a stencil or depth test that only
    // makes sense against the real target reject parts of the copy.
    ID3D11RenderTargetView* rt[1] = { g.rtv.Get() };
    hooks::set_self_targets(true);
    ctx->OMSetRenderTargets(1, rt, nullptr);
    hooks::set_self_targets(false);
    const float bf[4] = {1, 1, 1, 1};
    ctx->OMSetBlendState(g.blend.Get(), bf, 0xFFFFFFFFu);
    saved.valid = true;
    g_drew.store(true, std::memory_order_release);
    return true;
}

void end_capture(ID3D11DeviceContext* ctx, Saved& saved)
{
    if (!ctx || !saved.valid) return;
    hooks::set_self_targets(true);
    ctx->OMSetRenderTargets(saved.rtvCount, saved.rtvCount ? saved.rtv : nullptr, saved.dsv);
    hooks::set_self_targets(false);
    ctx->OMSetBlendState(saved.blend, saved.blendFactor, saved.sampleMask);

    for (UINT i = 0; i < 8; ++i) if (saved.rtv[i]) { saved.rtv[i]->Release(); saved.rtv[i] = nullptr; }
    if (saved.dsv)   { saved.dsv->Release();   saved.dsv = nullptr; }
    if (saved.blend) { saved.blend->Release(); saved.blend = nullptr; }
    saved.valid = false;
}

void clear(ID3D11DeviceContext* ctx)
{
    g_drew.store(false, std::memory_order_release);
    if (!ctx || !g_active.load(std::memory_order_acquire) || !g.rtv) return;
    const float zero[4] = {0, 0, 0, 0};
    ctx->ClearRenderTargetView(g.rtv.Get(), zero);
}

void shutdown() { release_all(); }

} // namespace uilayer
