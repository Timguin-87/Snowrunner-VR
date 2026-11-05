#include "render/ui_layer.hpp"
#include "hooks/depth_probe.h"
#include "common/log.h"

#include <windows.h>
#include <wrl/client.h>
#include <atomic>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace uilayer {
namespace {

struct State {
    ComPtr<ID3D11Texture2D>          tex;
    ComPtr<ID3D11RenderTargetView>   rtv;
    ComPtr<ID3D11ShaderResourceView> srv;        // raw  -- values as the game wrote them
    ComPtr<ID3D11ShaderResourceView> srvSrgb;    // decodes those values to linear
    ComPtr<ID3D11BlendState>         blend;
    ComPtr<ID3D11Texture2D>          stencilTex; // Scaleform's masks -- see the header
    ComPtr<ID3D11DepthStencilView>   dsv;
    uint32_t w = 0, h = 0;
} g;

std::atomic<bool> g_active{false};
std::atomic<bool> g_drew{false};

// Set by the first mask the layer captures, and never cleared. Until then a
// draw that only TESTS the stencil runs without it, unclipped as before: were
// the game's masks drawn by a shader outside the UI role, the layer's stencil
// would stay empty, and every masked element would test against nothing and
// vanish.
std::atomic<bool> g_masksCaptured{false};

// For the periodic "ui layer:" line.
std::atomic<uint32_t> g_nCaptured{0}, g_nMasks{0}, g_nClipped{0}, g_nAdditive{0};
std::atomic<uint32_t> g_nOffscreen{0}, g_offscreenW{0}, g_offscreenH{0};

// THE CAPTURE'S VERSIONS OF THE GAME'S STATES, keyed by the game's object.
// Scaleform makes a fixed set at start-up, so each table settles into a handful
// of entries. The game's description is kept beside the key, so an address the
// game freed and reused for another state cannot hand back the wrong one.
// NEVER EVICTED: a worker thread may be binding an entry while another adds to
// the table. A full table falls back to the forced blend, or to no stencil.
constexpr int kMaxDerived = 64;
enum : uint32_t {
    kNoColour      = 1u,   // the draw writes no colour: a mask, or a stencil clear
    kAdditive      = 2u,
    kWritesStencil = 4u,
};

template <typename Desc, typename St>
struct Derived {
    const void* key = nullptr;
    Desc        desc{};
    ComPtr<St>  mine;
    uint32_t    flags = 0;
};
using BlendEntry   = Derived<D3D11_BLEND_DESC, ID3D11BlendState>;
using StencilEntry = Derived<D3D11_DEPTH_STENCIL_DESC, ID3D11DepthStencilState>;
BlendEntry   g_blends[kMaxDerived];
StencilEntry g_stencils[kMaxDerived];
SRWLOCK      g_derivedLock = SRWLOCK_INIT;

template <typename E, typename Desc>
const E* find_derived(const E (&table)[kMaxDerived], const void* key, const Desc& desc)
{
    for (const E& e : table)
        if (e.mine && e.key == key && std::memcmp(&e.desc, &desc, sizeof(Desc)) == 0)
            return &e;
    return nullptr;
}

template <typename E, typename Desc, typename St>
const E* add_derived(E (&table)[kMaxDerived], const void* key, const Desc& desc,
                     const ComPtr<St>& mine, uint32_t flags)
{
    AcquireSRWLockExclusive(&g_derivedLock);
    const E* out = find_derived(table, key, desc);   // another thread got there first
    if (!out) {
        for (E& e : table) {
            if (e.mine) continue;
            e.key = key; e.desc = desc; e.flags = flags; e.mine = mine;
            out = &e;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_derivedLock);
    return out;
}

// THE GAME'S OWN BLEND, with the alpha made coverage -- the move
// smudge_layer.cpp's alpha_fixed_blend() makes, plus what a UI layer needs on
// top of it.
//
// It used to force one blend on every captured draw: SrcAlpha/InvSrcAlpha for
// colour, One/InvSrcAlpha for alpha, every channel written. That is right for
// ordinary alpha blending and wrong for the rest of what Scaleform draws:
//   - a MASK is drawn with colour off, into the stencil only; forced on, its
//     shape went into the layer as colour;
//   - an ADDITIVE element ("add" in the HUD movies) came out alpha-blended,
//     which over a transparent target darkens where it should only brighten.
// So the write mask is always the game's, and so is the colour blend when it is
// ordinary or additive. An additive draw adds light without covering anything,
// so its coverage is left alone (Zero/One) and the compositor adds it over the
// scene. Anything else -- multiply, screen, blending switched off -- keeps the
// forced blend: those have no premultiplied form over a layer that does not
// hold the scene, and the forced blend is how they have always looked.
//
// Null for no state bound (D3D's default: the forced blend, every channel), or
// when the table is full.
const BlendEntry* capture_blend(ID3D11DeviceContext* ctx, ID3D11BlendState* game)
{
    if (!game) return nullptr;
    D3D11_BLEND_DESC gd;
    std::memset(&gd, 0, sizeof(gd));
    game->GetDesc(&gd);

    AcquireSRWLockShared(&g_derivedLock);
    const BlendEntry* hit = find_derived(g_blends, game, gd);
    ReleaseSRWLockShared(&g_derivedLock);
    if (hit) return hit;

    const D3D11_RENDER_TARGET_BLEND_DESC& r = gd.RenderTarget[0];
    D3D11_BLEND_DESC bd{};
    D3D11_RENDER_TARGET_BLEND_DESC& o = bd.RenderTarget[0];
    o.BlendEnable    = TRUE;
    o.SrcBlend       = D3D11_BLEND_SRC_ALPHA;
    o.DestBlend      = D3D11_BLEND_INV_SRC_ALPHA;
    o.BlendOp        = D3D11_BLEND_OP_ADD;
    o.SrcBlendAlpha  = D3D11_BLEND_ONE;
    o.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    o.BlendOpAlpha   = D3D11_BLEND_OP_ADD;
    o.RenderTargetWriteMask = r.RenderTargetWriteMask;
    uint32_t flags = 0;
    if (r.BlendEnable && r.BlendOp == D3D11_BLEND_OP_ADD) {
        const bool srcOk = r.SrcBlend == D3D11_BLEND_SRC_ALPHA || r.SrcBlend == D3D11_BLEND_ONE;
        if (srcOk && r.DestBlend == D3D11_BLEND_INV_SRC_ALPHA) {
            o.SrcBlend = r.SrcBlend;                 // ordinary, plain or premultiplied
        } else if (srcOk && r.DestBlend == D3D11_BLEND_ONE) {
            o.SrcBlend       = r.SrcBlend;           // additive
            o.DestBlend      = D3D11_BLEND_ONE;
            o.SrcBlendAlpha  = D3D11_BLEND_ZERO;
            o.DestBlendAlpha = D3D11_BLEND_ONE;
            flags |= kAdditive;
        } else if (r.SrcBlend == D3D11_BLEND_ZERO && r.DestBlend == D3D11_BLEND_ONE) {
            o.RenderTargetWriteMask = 0;             // leaves the target as it was
        }
    }
    // THE LAYER'S ALPHA FOLLOWS THE COLOUR, whatever the game's mask says about
    // its own. Alpha is the coverage here: a draw that writes colour has to
    // write it, or the compositor adds that colour over the scene uncovered --
    // and on the game's opaque canvas an alpha write is invisible either way,
    // so a game state that leaves alpha out would have turned the whole UI
    // additive. No colour means nothing for the layer at all: alpha alone would
    // be coverage with nothing in it, a dark patch over the scene.
    constexpr UINT8 kRgb = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN |
                           D3D11_COLOR_WRITE_ENABLE_BLUE;
    const UINT8 rgb = o.RenderTargetWriteMask & kRgb;
    o.RenderTargetWriteMask = rgb ? UINT8(rgb | D3D11_COLOR_WRITE_ENABLE_ALPHA) : UINT8(0);
    if (!rgb) flags |= kNoColour;

    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    ComPtr<ID3D11BlendState> mine;
    if (!dev || FAILED(dev->CreateBlendState(&bd, &mine))) return nullptr;
    return add_derived(g_blends, game, gd, mine, flags);
}

// THE GAME'S STENCIL STATE, with the depth test off -- see the header. Null for
// a state that does not use the stencil, which then runs with no depth view at
// all, as every captured draw used to.
const StencilEntry* capture_stencil(ID3D11DeviceContext* ctx, ID3D11DepthStencilState* game)
{
    if (!game || !g.dsv) return nullptr;
    D3D11_DEPTH_STENCIL_DESC gd;
    std::memset(&gd, 0, sizeof(gd));
    game->GetDesc(&gd);
    if (!gd.StencilEnable) return nullptr;

    AcquireSRWLockShared(&g_derivedLock);
    const StencilEntry* hit = find_derived(g_stencils, game, gd);
    ReleaseSRWLockShared(&g_derivedLock);
    if (hit) return hit;

    // With the depth test off a depth fail cannot happen, so only these two
    // decide whether the draw writes the stencil.
    auto writes = [](const D3D11_DEPTH_STENCILOP_DESC& f) {
        return f.StencilFailOp != D3D11_STENCIL_OP_KEEP || f.StencilPassOp != D3D11_STENCIL_OP_KEEP;
    };
    const uint32_t flags =
        (gd.StencilWriteMask && (writes(gd.FrontFace) || writes(gd.BackFace))) ? kWritesStencil : 0u;
    D3D11_DEPTH_STENCIL_DESC d = gd;
    d.DepthEnable    = FALSE;
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;

    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    ComPtr<ID3D11DepthStencilState> mine;
    if (!dev || FAILED(dev->CreateDepthStencilState(&d, &mine))) return nullptr;
    return add_derived(g_stencils, game, gd, mine, flags);
}

// THE CANVAS, by size: the composite onto it is what the plane shows. Every
// other target a UI shader draws into is a Scaleform offscreen pass -- a filter,
// a cached bitmap -- whose texture the composite reads later. Drawn into the
// layer instead, that texture stayed empty, and the pass landed in the layer at
// its own viewport, in the top-left corner.
bool on_canvas(ID3D11RenderTargetView* rtv)
{
    if (!rtv) return false;
    ComPtr<ID3D11Resource> res;
    rtv->GetResource(&res);
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    if (res) res->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
    D3D11_TEXTURE2D_DESC d{};
    static_cast<ID3D11Texture2D*>(res.Get())->GetDesc(&d);
    if (d.Width == g.w && d.Height == g.h) return true;
    g_offscreenW.store(d.Width, std::memory_order_relaxed);
    g_offscreenH.store(d.Height, std::memory_order_relaxed);
    return false;
}

void release_saved(Saved& s)
{
    for (UINT i = 0; i < 8; ++i) if (s.rtv[i]) { s.rtv[i]->Release(); s.rtv[i] = nullptr; }
    if (s.dsv)   { s.dsv->Release();   s.dsv = nullptr; }
    if (s.blend) { s.blend->Release(); s.blend = nullptr; }
    if (s.dss)   { s.dss->Release();   s.dss = nullptr; }
    s.rtvCount = 0;
    s.stencil = false;
    s.valid = false;
}

void release_all()
{
    g_active.store(false);
    g_drew.store(false);
    g.srv.Reset(); g.srvSrgb.Reset(); g.rtv.Reset(); g.tex.Reset(); g.blend.Reset();
    g.dsv.Reset(); g.stencilTex.Reset();
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

    // The forced blend: colour the ordinary way, alpha accumulating as coverage.
    //
    // The game's own UI draws with SrcAlpha/InvSrcAlpha onto an OPAQUE
    // backbuffer, where the destination alpha is irrelevant. Against a
    // transparent target it is not: reusing that blend for alpha would give
    // a = src.a*src.a, so every semi-transparent element would come out too
    // sparse to composite back. ONE/InvSrcAlpha is the standard fix and makes
    // the result premultiplied, which is what the compositor expects from a
    // quad layer submitted without UNPREMULTIPLIED_ALPHA. What a draw gets
    // instead of this is decided per game state -- see capture_blend().
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

    // The stencil Scaleform's masks run against. D24S8 because D3D11 has no
    // stencil-only format; the depth half is never tested. Without it the layer
    // still works, with every masked element drawn whole as before.
    D3D11_TEXTURE2D_DESC dsd = d;
    dsd.Format    = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(dev->CreateTexture2D(&dsd, nullptr, &g.stencilTex)) ||
        FAILED(dev->CreateDepthStencilView(g.stencilTex.Get(), nullptr, &g.dsv))) {
        g.dsv.Reset(); g.stencilTex.Reset();
        VRLOG("ui layer: stencil %ux%u FAILED -- masked UI is drawn unclipped", w, h);
    }

    g.w = w; g.h = h;
    VRLOG("ui layer: %ux%u RGBA8 typeless ready (UNORM write, raw + sRGB reads)%s", w, h,
          g.dsv ? ", D24S8 stencil for the game's masks" : "");
    return true;
}

// Every 10 s while the layer captures anything: whether the masks reach the
// layer's stencil, and what was left on the game's own targets.
void log_counts()
{
    static ULONGLONG t0 = 0;
    const ULONGLONG now = GetTickCount64();
    if (!t0) t0 = now;
    if (now - t0 < 10000) return;
    t0 = now;
    const uint32_t captured = g_nCaptured.exchange(0, std::memory_order_relaxed);
    const uint32_t masks    = g_nMasks.exchange(0, std::memory_order_relaxed);
    const uint32_t clipped  = g_nClipped.exchange(0, std::memory_order_relaxed);
    const uint32_t additive = g_nAdditive.exchange(0, std::memory_order_relaxed);
    const uint32_t off      = g_nOffscreen.exchange(0, std::memory_order_relaxed);
    if (!captured && !off) return;
    VRLOG("ui layer: %u UI draws captured in 10 s -- %u masks into its stencil, %u clipped by "
          "them, %u additive%s | %u left on the game's offscreen targets (last %ux%u)",
          captured, masks, clipped, additive,
          g_masksCaptured.load(std::memory_order_relaxed)
              ? "" : " (no mask seen yet: masked draws run unclipped)",
          off, g_offscreenW.load(std::memory_order_relaxed),
          g_offscreenH.load(std::memory_order_relaxed));
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

Capture begin_capture(ID3D11DeviceContext* ctx, Saved& saved, bool masksOnly)
{
    if (!ctx || !g_active.load(std::memory_order_acquire) || !g.rtv) return kNotCaptured;

    ctx->OMGetRenderTargets(8, saved.rtv, &saved.dsv);
    saved.rtvCount = 0;
    for (UINT i = 0; i < 8; ++i) if (saved.rtv[i]) saved.rtvCount = i + 1;
    if (!on_canvas(saved.rtv[0])) {
        g_nOffscreen.fetch_add(1, std::memory_order_relaxed);
        release_saved(saved);
        return kNotCaptured;
    }
    ctx->OMGetBlendState(&saved.blend, saved.blendFactor, &saved.sampleMask);
    ctx->OMGetDepthStencilState(&saved.dss, &saved.stencilRef);

    const BlendEntry*   be = capture_blend(ctx, saved.blend);
    const StencilEntry* se = capture_stencil(ctx, saved.dss);
    const uint32_t bflags = be ? be->flags : 0u;
    const bool writesStencil = se && (se->flags & kWritesStencil);
    const bool mask = writesStencil && (bflags & kNoColour);
    if (masksOnly && !mask) {
        release_saved(saved);
        return kNotCaptured;
    }
    if (writesStencil) g_masksCaptured.store(true, std::memory_order_relaxed);
    // The stencil for whatever writes it, and for anything tested against it
    // once masks are known to reach the layer -- see g_masksCaptured.
    const bool useStencil =
        se && (writesStencil || g_masksCaptured.load(std::memory_order_relaxed));

    // NO DEPTH. The UI is screen-space and its own draws do not depend on scene
    // depth, but the depth buffer bound at that point describes the 3D frame --
    // leaving it attached would let a test that only makes sense against the
    // real target reject parts of the copy. The stencil is the layer's own.
    ID3D11RenderTargetView* rt[1] = { g.rtv.Get() };
    hooks::set_self_targets(true);
    ctx->OMSetRenderTargets(1, rt, useStencil ? g.dsv.Get() : nullptr);
    hooks::set_self_targets(false);
    ctx->OMSetBlendState(be ? be->mine.Get() : g.blend.Get(), saved.blendFactor, saved.sampleMask);
    if (useStencil) ctx->OMSetDepthStencilState(se->mine.Get(), saved.stencilRef);
    saved.stencil = useStencil;
    saved.valid = true;

    g_nCaptured.fetch_add(1, std::memory_order_relaxed);
    if (mask)            g_nMasks.fetch_add(1, std::memory_order_relaxed);
    else if (useStencil) g_nClipped.fetch_add(1, std::memory_order_relaxed);
    if (bflags & kAdditive) g_nAdditive.fetch_add(1, std::memory_order_relaxed);
    // A mask alone puts nothing on the plane.
    if (!mask) g_drew.store(true, std::memory_order_release);
    return mask ? kCapturedMask : kCaptured;
}

void end_capture(ID3D11DeviceContext* ctx, Saved& saved)
{
    if (!ctx || !saved.valid) return;
    hooks::set_self_targets(true);
    ctx->OMSetRenderTargets(saved.rtvCount, saved.rtvCount ? saved.rtv : nullptr, saved.dsv);
    hooks::set_self_targets(false);
    ctx->OMSetBlendState(saved.blend, saved.blendFactor, saved.sampleMask);
    if (saved.stencil) ctx->OMSetDepthStencilState(saved.dss, saved.stencilRef);
    release_saved(saved);
}

void clear(ID3D11DeviceContext* ctx)
{
    g_drew.store(false, std::memory_order_release);
    if (!ctx || !g_active.load(std::memory_order_acquire) || !g.rtv) return;
    const float zero[4] = {0, 0, 0, 0};
    ctx->ClearRenderTargetView(g.rtv.Get(), zero);
    // A clean start for each frame's masks, whatever clearing Scaleform does of
    // its own: where that is a ClearDepthStencilView of the game's buffer it
    // never reaches this one.
    if (g.dsv) ctx->ClearDepthStencilView(g.dsv.Get(), D3D11_CLEAR_STENCIL, 1.0f, 0);
    log_counts();
}

void shutdown() { release_all(); }

} // namespace uilayer
