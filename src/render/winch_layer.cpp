#include "render/winch_layer.hpp"
#include "hooks/depth_probe.h"
#include "common/log.h"

#include <wrl/client.h>
#include <atomic>

using Microsoft::WRL::ComPtr;

namespace winchlayer {
namespace {

struct Buf {
    ComPtr<ID3D11Texture2D>          tex;
    ComPtr<ID3D11RenderTargetView>   rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
};

// See the header: `cap` is what the draws land in, `held` is what gets
// composited, and only a capture-eye frame swaps them.
struct State {
    Buf      buf[2];
    int      cap = 0;
    int      held = 1;
    bool     primed = false;   // both buffers cleared at least once
    int      idle = 0;         // frames since the last promotion
    uint32_t w = 0, h = 0;
} g;

// If the capture eye never comes round, promote anyway rather than leave the
// markers frozen. Same insurance, same size, as the UI plane's kMaxUiQuadIdle.
constexpr int kMaxWinchIdle = 4;

std::atomic<bool> g_active{false};

void release_all()
{
    g_active.store(false);
    for (Buf& b : g.buf) { b.srv.Reset(); b.rtv.Reset(); b.tex.Reset(); }
    g.cap = 0; g.held = 1; g.primed = false; g.idle = 0;
    g.w = g.h = 0;
}

bool build(ID3D11Device* dev, uint32_t w, uint32_t h)
{
    // Plain UNORM, not _SRGB -- a format that decodes on read and re-encodes on
    // write round-trips through gamma twice. Same reasoning as ui_layer.cpp.
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (Buf& b : g.buf) {
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &b.tex))) {
            VRLOG("winch layer: CreateTexture2D %ux%u FAILED", w, h);
            return false;
        }
        if (FAILED(dev->CreateRenderTargetView(b.tex.Get(), nullptr, &b.rtv)) ||
            FAILED(dev->CreateShaderResourceView(b.tex.Get(), nullptr, &b.srv))) {
            VRLOG("winch layer: view creation FAILED");
            return false;
        }
    }
    g.cap = 0; g.held = 1; g.primed = false; g.idle = 0;
    g.w = w; g.h = h;
    VRLOG("winch layer: 2x %ux%u RGBA8 ready (one-eye capture)", w, h);
    return true;
}

// The game's OWN colour blend with only the ALPHA terms replaced, and the same
// cache the smudge layer uses -- see the long note there for why reusing the
// game's colour terms matters and why alpha alone has to be overridden (the
// target is transparent, so the game's destination-alpha terms would leave the
// layer too sparse to composite back).
ID3D11BlendState* alpha_fixed_blend(ID3D11DeviceContext* ctx, ID3D11BlendState* src)
{
    constexpr int kMaxBlends = 8;
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

} // namespace

// NOT gated on gameplay, unlike the smudge layer. That gate exists because the
// splatter composites over the pause menu, which is drawn after it; these are
// drawn only while the winch is actually in use, so there is no screen where a
// captured marker could outlive what it belongs to.
bool active() { return g_active.load(std::memory_order_acquire); }

bool begin_capture(ID3D11DeviceContext* ctx, Saved& saved)
{
    if (!ctx || !active() || !g.buf[g.cap].rtv) return false;

    ctx->OMGetRenderTargets(8, saved.rtv, &saved.dsv);
    saved.rtvCount = 0;
    for (UINT i = 0; i < 8; ++i) if (saved.rtv[i]) saved.rtvCount = i + 1;
    ctx->OMGetBlendState(&saved.blend, saved.blendFactor, &saved.sampleMask);

    // THE CALLER'S DEPTH VIEW IS KEPT, and its depth state with it. A winch
    // marker behind the A-pillar is occluded by it in the game's own render, and
    // capturing it to a target with no depth test would paint it through the
    // cab. The layer is composited flat afterwards, but WHICH pixels reach it is
    // still the game's decision.
    ID3D11RenderTargetView* rt[1] = { g.buf[g.cap].rtv.Get() };
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

// Nothing to composite until the first latch has cleared both buffers -- a
// freshly created texture holds whatever was in that memory.
ID3D11ShaderResourceView* srv()
{
    return (active() && g.primed) ? g.buf[g.held].srv.Get() : nullptr;
}

void latch(ID3D11DeviceContext* ctx, bool captureEyeFrame)
{
    if (!ctx || !g_active.load(std::memory_order_acquire)) return;
    const float zero[4] = {0, 0, 0, 0};

    if (!g.primed) {
        for (Buf& b : g.buf)
            if (b.rtv) ctx->ClearRenderTargetView(b.rtv.Get(), zero);
        g.primed = true;
    } else if (captureEyeFrame || g.idle >= kMaxWinchIdle) {
        // This frame's capture becomes the one both eyes composite. The buffer
        // it displaces is stale by definition and is cleared below, ready for
        // the next frame's draws.
        const int t = g.cap; g.cap = g.held; g.held = t;
        g.idle = 0;
    } else {
        // A non-capture-eye frame. Its markers sit at the other eye's projected
        // position, which is exactly what we are refusing to show, so they are
        // dropped by the clear below and the previous promotion stands.
        ++g.idle;
    }

    if (g.buf[g.cap].rtv) ctx->ClearRenderTargetView(g.buf[g.cap].rtv.Get(), zero);
}

void update(ID3D11Device* dev, uint32_t canvasW, uint32_t canvasH, bool dibrActive)
{
    // dibrActive is the whole gate: without the shift the markers are already in
    // the right pixels in both eyes and there is nothing to take them out of.
    if (!dibrActive || !dev || canvasW == 0 || canvasH == 0) {
        if (g.buf[0].tex) { release_all(); VRLOG("winch layer: released"); }
        return;
    }
    if (g.buf[0].tex && g.w == canvasW && g.h == canvasH) return;

    release_all();
    if (build(dev, canvasW, canvasH))
        g_active.store(true, std::memory_order_release);
    else
        release_all();
}

void shutdown() { release_all(); }

} // namespace winchlayer
