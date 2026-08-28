#include "render/smudge_layer.hpp"
#include "hooks/depth_probe.h"
#include "hooks/cbuffer_hook.h"
#include "common/log.h"

#include <wrl/client.h>
#include <atomic>

using Microsoft::WRL::ComPtr;

namespace smudgelayer {
namespace {

struct State {
    ComPtr<ID3D11Texture2D>          tex;
    ComPtr<ID3D11RenderTargetView>   rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    // Windscreen depth, written by a second pass over the same geometry -- see
    // begin_depth_capture(). R32_TYPELESS so it can be both a DSV (D32_FLOAT)
    // and an SRV (R32_FLOAT); a plain D32_FLOAT resource cannot be read back.
    ComPtr<ID3D11Texture2D>          depthTex;
    ComPtr<ID3D11DepthStencilView>   depthDsv;
    ComPtr<ID3D11ShaderResourceView> depthSrv;
    ComPtr<ID3D11DepthStencilState>  depthWrite;
    uint32_t w = 0, h = 0;
} g;

std::atomic<bool>  g_active{false};

void release_all()
{
    g_active.store(false);
    g.srv.Reset(); g.rtv.Reset(); g.tex.Reset();
    g.depthSrv.Reset(); g.depthDsv.Reset(); g.depthTex.Reset(); g.depthWrite.Reset();
    g.w = g.h = 0;
}

bool build(ID3D11Device* dev, uint32_t w, uint32_t h)
{
    // Plain UNORM, not _SRGB -- see the note in ui_layer.cpp: a format that
    // decodes on read and re-encodes on write round-trips through gamma twice.
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &g.tex))) {
        VRLOG("smudge layer: CreateTexture2D %ux%u FAILED", w, h);
        return false;
    }
    if (FAILED(dev->CreateRenderTargetView(g.tex.Get(), nullptr, &g.rtv)) ||
        FAILED(dev->CreateShaderResourceView(g.tex.Get(), nullptr, &g.srv))) {
        VRLOG("smudge layer: view creation FAILED");
        return false;
    }

    // --- windscreen depth ------------------------------------------------
    D3D11_TEXTURE2D_DESC dd{};
    dd.Width = w; dd.Height = h; dd.MipLevels = 1; dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_R32_TYPELESS;      // DSV as D32_FLOAT, SRV as R32_FLOAT
    dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&dd, nullptr, &g.depthTex))) {
        VRLOG("smudge layer: depth CreateTexture2D FAILED");
        return false;
    }
    D3D11_DEPTH_STENCIL_VIEW_DESC dvd{};
    dvd.Format = DXGI_FORMAT_D32_FLOAT;
    dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC dsd{};
    dsd.Format = DXGI_FORMAT_R32_FLOAT;
    dsd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    dsd.Texture2D.MipLevels = 1;
    if (FAILED(dev->CreateDepthStencilView(g.depthTex.Get(), &dvd, &g.depthDsv)) ||
        FAILED(dev->CreateShaderResourceView(g.depthTex.Get(), &dsd, &g.depthSrv))) {
        VRLOG("smudge layer: depth view creation FAILED");
        return false;
    }

    // Writes forced ON -- the glass draws with them off, which is the whole
    // reason its depth is missing. GREATER because this is reverse-Z: a larger
    // value is nearer, so where panes or splatter overlap the nearest wins.
    // Stencil off entirely; the game's stencil state is irrelevant to a target
    // that has no stencil plane.
    D3D11_DEPTH_STENCIL_DESC dsdesc{};
    dsdesc.DepthEnable = TRUE;
    dsdesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsdesc.DepthFunc = D3D11_COMPARISON_GREATER;
    dsdesc.StencilEnable = FALSE;
    if (FAILED(dev->CreateDepthStencilState(&dsdesc, &g.depthWrite))) {
        VRLOG("smudge layer: depth-stencil state FAILED");
        return false;
    }

    g.w = w; g.h = h;
    VRLOG("smudge layer: %ux%u RGBA8 + R32 glass depth ready", w, h);
    return true;
}

} // namespace

// GATED ON GAMEPLAY, not just on the layer being built.
//
// The composite happens in the final blit, which is after everything the game
// drew -- so on the pause menu the splatter landed ON TOP of the menu instead
// of behind it. in_gameplay() is false there, and it gates the CAPTURE as well,
// so while paused the draws go through normally into the scene and nothing is
// composited: the frozen cockpit keeps its mud and the menu is clean. One test
// for both halves, so they can never disagree about a frame.
//
// Deliberately not done by releasing the textures in update() -- that would
// rebuild them on every pause, and the layer is full-canvas.
bool  active()
{
    return g_active.load(std::memory_order_acquire) && hooks::in_gameplay();
}
// Fixed: tuned in the headset, see the header for why it is empirical.
float depth_offset() { return -0.015f; }

ID3D11ShaderResourceView* srv()      { return active() ? g.srv.Get() : nullptr; }
ID3D11ShaderResourceView* depth_srv() { return active() ? g.depthSrv.Get() : nullptr; }

bool begin_depth_capture(ID3D11DeviceContext* ctx, Saved& saved)
{
    if (!ctx || !active() || !g.depthDsv) return false;

    ctx->OMGetRenderTargets(8, saved.rtv, &saved.dsv);
    saved.rtvCount = 0;
    for (UINT i = 0; i < 8; ++i) if (saved.rtv[i]) saved.rtvCount = i + 1;
    ctx->OMGetDepthStencilState(&saved.dss, &saved.stencilRef);

    // NO colour target: this pass exists only to record where the glass is.
    // Whatever the pixel shader returns is discarded, so its sampling of scene
    // textures cannot matter and its cost is a depth-only pass.
    hooks::set_self_targets(true);
    ctx->OMSetRenderTargets(0, nullptr, g.depthDsv.Get());
    hooks::set_self_targets(false);
    ctx->OMSetDepthStencilState(g.depthWrite.Get(), 0);
    saved.valid = true;
    return true;
}

void end_depth_capture(ID3D11DeviceContext* ctx, Saved& saved)
{
    if (!ctx || !saved.valid) return;
    hooks::set_self_targets(true);
    ctx->OMSetRenderTargets(saved.rtvCount, saved.rtvCount ? saved.rtv : nullptr, saved.dsv);
    hooks::set_self_targets(false);
    ctx->OMSetDepthStencilState(saved.dss, saved.stencilRef);

    for (UINT i = 0; i < 8; ++i) if (saved.rtv[i]) { saved.rtv[i]->Release(); saved.rtv[i] = nullptr; }
    if (saved.dsv) { saved.dsv->Release(); saved.dsv = nullptr; }
    if (saved.dss) { saved.dss->Release(); saved.dss = nullptr; }
    saved.valid = false;
}

void update(ID3D11Device* dev, uint32_t canvasW, uint32_t canvasH, bool dibrActive)
{
    // No g_enabled: reprojecting the splatter is not an option any more (see
    // the header). dibrActive is the whole gate -- without the shift there is
    // nothing displacing the mud for this to correct, so the layer would be a
    // full-canvas target built for nobody.
    if (!dibrActive || !dev || canvasW == 0 || canvasH == 0) {
        if (g.tex) { release_all(); VRLOG("smudge layer: released"); }
        return;
    }
    if (g.tex && g.w == canvasW && g.h == canvasH) return;

    release_all();
    if (build(dev, canvasW, canvasH))
        g_active.store(true, std::memory_order_release);
    else
        release_all();
}

// The game's OWN colour blend, with only the ALPHA terms replaced.
//
// This used to force SrcAlpha/InvSrcAlpha on every captured draw. That is right
// for ordinary alpha blending and wrong for anything else -- an additive or
// modulated pass composited as if it were alpha-blended comes out at the wrong
// strength, which is the window tint reading brighter and more present than the
// same tint does in AER.
//
// Only alpha needs overriding, and only because the target is TRANSPARENT: the
// game blends against an opaque backbuffer where destination alpha is
// meaningless, so reusing its alpha terms here would give a = src.a*src.a and
// leave the layer too sparse to composite back. SrcBlendAlpha=ONE makes alpha
// accumulate as coverage and the result premultiplied, without touching how the
// colour itself combines.
//
// Cached by source state pointer -- the game reuses a handful of blend states,
// so this settles into a couple of entries rather than creating one per draw.
ID3D11BlendState* alpha_fixed_blend(ID3D11DeviceContext* ctx, ID3D11BlendState* src)
{
    constexpr int kMaxBlends = 16;
    static ID3D11BlendState* s_key[kMaxBlends] = {};
    static ComPtr<ID3D11BlendState> s_val[kMaxBlends];
    static SRWLOCK s_lock = SRWLOCK_INIT;

    AcquireSRWLockExclusive(&s_lock);
    int free = -1;
    for (int i = 0; i < kMaxBlends; ++i) {
        if (s_key[i] == src) { ID3D11BlendState* v = s_val[i].Get();
                               ReleaseSRWLockExclusive(&s_lock); return v; }
        if (!s_val[i] && free < 0) free = i;
    }

    D3D11_BLEND_DESC bd{};
    if (src) {
        src->GetDesc(&bd);
    } else {
        // No state bound means D3D's default: blending off, writes on. Capturing
        // that as-is would overwrite the layer opaquely, which is correct.
        bd.RenderTarget[0].BlendEnable = FALSE;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    }
    const int n = bd.IndependentBlendEnable ? 8 : 1;
    for (int i = 0; i < n; ++i) {
        if (!bd.RenderTarget[i].BlendEnable) continue;
        bd.RenderTarget[i].SrcBlendAlpha  = D3D11_BLEND_ONE;
        bd.RenderTarget[i].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[i].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
    }

    ID3D11BlendState* out = nullptr;
    if (free >= 0) {
        ComPtr<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        if (dev && SUCCEEDED(dev->CreateBlendState(&bd, &s_val[free]))) {
            s_key[free] = src;
            out = s_val[free].Get();
        }
    }
    ReleaseSRWLockExclusive(&s_lock);
    return out;
}

bool begin_capture(ID3D11DeviceContext* ctx, Saved& saved)
{
    if (!ctx || !active() || !g.rtv) return false;

    ctx->OMGetRenderTargets(8, saved.rtv, &saved.dsv);
    saved.rtvCount = 0;
    for (UINT i = 0; i < 8; ++i) if (saved.rtv[i]) saved.rtvCount = i + 1;
    ctx->OMGetBlendState(&saved.blend, saved.blendFactor, &saved.sampleMask);

    // The caller's DEPTH VIEW IS KEPT (uilayer drops its equivalent). Splatter
    // is geometry on a surface that the dashboard, wheel and A-pillars occlude;
    // without the depth test it would be painted over the cab interior.
    ID3D11RenderTargetView* rt[1] = { g.rtv.Get() };
    hooks::set_self_targets(true);
    ctx->OMSetRenderTargets(1, rt, saved.dsv);
    hooks::set_self_targets(false);
    if (ID3D11BlendState* bs = alpha_fixed_blend(ctx, saved.blend))
        ctx->OMSetBlendState(bs, saved.blendFactor, saved.sampleMask);
    saved.valid = true;
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
    if (!ctx || !g_active.load(std::memory_order_acquire) || !g.rtv) return;
    const float zero[4] = {0, 0, 0, 0};
    ctx->ClearRenderTargetView(g.rtv.Get(), zero);
    // 0.0 = infinitely far under reverse-Z, so an untouched texel reads as
    // "no glass here" and the composite falls back to the manual distance.
    if (g.depthDsv) ctx->ClearDepthStencilView(g.depthDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
}

void shutdown() { release_all(); }

} // namespace smudgelayer
