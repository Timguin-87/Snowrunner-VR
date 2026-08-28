#include "render/mirror_mask.hpp"
#include "hooks/depth_probe.h"
#include "common/log.h"

#include <d3dcompiler.h>
#include <wrl/client.h>
#include <atomic>

using Microsoft::WRL::ComPtr;

namespace mirrormask {
namespace {

// Only SV_Position is declared, which every vertex shader emits by definition,
// so this is signature-compatible with whatever VS the mirror draw happens to
// be using. A PS that declared any other input would have to match that
// specific VS's output signature and would fail to bind.
const char kMarkPs[] =
    "float4 psmain(float4 p : SV_Position) : SV_Target { return 1.0; }";

struct State {
    ComPtr<ID3D11Texture2D>          tex;
    ComPtr<ID3D11RenderTargetView>   rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11PixelShader>        markPs;
    ComPtr<ID3D11BlendState>         opaqueBlend;
    uint32_t w = 0, h = 0;
} g;

// Read from the recording threads on every mirror draw, written from Present.
// The ComPtrs above are only ever rebuilt while this is false, so a recording
// thread that sees true has a stable set of objects to use.
std::atomic<bool> g_active{false};

void release_all()
{
    g_active.store(false);
    g.srv.Reset(); g.rtv.Reset(); g.tex.Reset();
    g.markPs.Reset(); g.opaqueBlend.Reset();
    g.w = g.h = 0;
}

bool build(ID3D11Device* dev, uint32_t w, uint32_t h)
{
    // R8_UNORM: one byte per pixel of pure coverage. At 2880x2880 that is 8MB,
    // against ~33MB for a colour copy of the same frame.
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &g.tex))) {
        VRLOG("mirror mask: CreateTexture2D %ux%u FAILED", w, h);
        return false;
    }
    if (FAILED(dev->CreateRenderTargetView(g.tex.Get(), nullptr, &g.rtv)) ||
        FAILED(dev->CreateShaderResourceView(g.tex.Get(), nullptr, &g.srv))) {
        VRLOG("mirror mask: view creation FAILED");
        return false;
    }

    ComPtr<ID3DBlob> code, err;
    if (FAILED(D3DCompile(kMarkPs, sizeof(kMarkPs) - 1, nullptr, nullptr, nullptr,
                          "psmain", "ps_5_0", 0, 0, &code, &err))) {
        VRLOG("mirror mask: mark PS compile FAILED: %s",
              err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    if (FAILED(dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(),
                                      nullptr, &g.markPs))) {
        VRLOG("mirror mask: CreatePixelShader FAILED");
        return false;
    }

    // The mirror's own blend state could be anything -- additive, multiplied,
    // alpha-weighted -- and a mark that goes through it is a mark whose value
    // depends on the reflection's colour. Coverage has to be unconditional, so
    // the duplicate draws with blending off.
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bd, &g.opaqueBlend))) {
        VRLOG("mirror mask: CreateBlendState FAILED");
        return false;
    }

    g.w = w; g.h = h;
    VRLOG("mirror mask: %ux%u R8 ready", w, h);
    return true;
}

} // namespace

bool active() { return g_active.load(std::memory_order_acquire); }

ID3D11ShaderResourceView* srv()
{
    return g_active.load(std::memory_order_acquire) ? g.srv.Get() : nullptr;
}

void update(ID3D11Device* dev, uint32_t canvasW, uint32_t canvasH, bool dibrActive)
{
    if (!dibrActive || !dev || canvasW == 0 || canvasH == 0) {
        // Nothing samples the mask outside DIBR shift, and the marking draws are the
        // only cost that scales with scene complexity -- so give the memory
        // back rather than keep an unread 8MB target alive in AER.
        if (g.tex) { release_all(); VRLOG("mirror mask: released (DIBR shift inactive)"); }
        return;
    }
    if (g.tex && g.w == canvasW && g.h == canvasH) return;

    release_all();
    if (build(dev, canvasW, canvasH))
        g_active.store(true, std::memory_order_release);
    else
        release_all();
}

bool begin_mark(ID3D11DeviceContext* ctx, Saved& saved)
{
    if (!ctx || !g_active.load(std::memory_order_acquire) || !g.rtv) return false;

    // OMGetRenderTargets AddRefs everything it hands back; end_mark releases.
    ctx->OMGetRenderTargets(8, saved.rtv, &saved.dsv);
    saved.rtvCount = 0;
    for (UINT i = 0; i < 8; ++i) if (saved.rtv[i]) saved.rtvCount = i + 1;
    ctx->PSGetShader(&saved.ps, nullptr, nullptr);
    ctx->OMGetBlendState(&saved.blend, saved.blendFactor, &saved.sampleMask);

    // The caller's depth view is kept, so the mark is depth-tested exactly as
    // the real draw is: geometry in front of a mirror occludes its mark too,
    // and the mask ends up describing the mirror as it is actually visible
    // rather than as a flat silhouette.
    ID3D11RenderTargetView* mask[1] = { g.rtv.Get() };
    hooks::set_self_targets(true);
    ctx->OMSetRenderTargets(1, mask, saved.dsv);
    hooks::set_self_targets(false);
    ctx->PSSetShader(g.markPs.Get(), nullptr, 0);
    const float bf[4] = {1, 1, 1, 1};
    ctx->OMSetBlendState(g.opaqueBlend.Get(), bf, 0xFFFFFFFFu);
    saved.valid = true;
    return true;
}

void end_mark(ID3D11DeviceContext* ctx, Saved& saved)
{
    if (!ctx || !saved.valid) return;
    hooks::set_self_targets(true);
    ctx->OMSetRenderTargets(saved.rtvCount, saved.rtvCount ? saved.rtv : nullptr, saved.dsv);
    hooks::set_self_targets(false);
    ctx->PSSetShader(saved.ps, nullptr, 0);
    ctx->OMSetBlendState(saved.blend, saved.blendFactor, saved.sampleMask);

    for (UINT i = 0; i < 8; ++i) if (saved.rtv[i]) { saved.rtv[i]->Release(); saved.rtv[i] = nullptr; }
    if (saved.dsv)   { saved.dsv->Release();   saved.dsv = nullptr; }
    if (saved.ps)    { saved.ps->Release();    saved.ps = nullptr; }
    if (saved.blend) { saved.blend->Release(); saved.blend = nullptr; }
    saved.valid = false;
}

void clear(ID3D11DeviceContext* ctx)
{
    if (!ctx || !g_active.load(std::memory_order_acquire) || !g.rtv) return;
    const float zero[4] = {0, 0, 0, 0};
    ctx->ClearRenderTargetView(g.rtv.Get(), zero);
}

void shutdown() { release_all(); }

} // namespace mirrormask
