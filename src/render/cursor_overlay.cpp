#include "render/cursor_overlay.hpp"

#include "common/cursor_policy.h"
#include "common/log.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace cursoroverlay {
namespace {

struct CursorImage {
    HCURSOR handle = nullptr;
    UINT width = 0, height = 0;
    UINT hotspotX = 0, hotspotY = 0;
    ComPtr<ID3D11ShaderResourceView> srvRaw;
    ComPtr<ID3D11ShaderResourceView> srvSrgb;
};

struct State {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11BlendState> blend;
    CursorImage cursor;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    DWORD lastAdjustLog = 0;
    bool buildFailed = false;
} g;

struct Constants {
    float left, top, right, bottom;
    float texelX, texelY, pad0, pad1;
};

bool bitmap_pixels(HDC dc, HBITMAP bitmap, UINT width, UINT height,
                   std::vector<uint32_t>& pixels)
{
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = static_cast<LONG>(width);
    bi.bmiHeader.biHeight = -static_cast<LONG>(height);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    pixels.resize(static_cast<size_t>(width) * height);
    return GetDIBits(dc, bitmap, 0, height, pixels.data(), &bi,
                     DIB_RGB_COLORS) == static_cast<int>(height);
}

bool load_cursor(ID3D11Device* device, HCURSOR handle)
{
    ICONINFO icon{};
    if (!handle || !GetIconInfo(handle, &icon)) return false;

    BITMAP colorDesc{}, maskDesc{};
    if (icon.hbmColor) GetObject(icon.hbmColor, sizeof(colorDesc), &colorDesc);
    if (icon.hbmMask)  GetObject(icon.hbmMask, sizeof(maskDesc), &maskDesc);

    const UINT width = icon.hbmColor ? static_cast<UINT>(colorDesc.bmWidth)
                                     : static_cast<UINT>(maskDesc.bmWidth);
    const UINT height = icon.hbmColor ? static_cast<UINT>(colorDesc.bmHeight)
                                      : static_cast<UINT>(maskDesc.bmHeight / 2);
    std::vector<uint32_t> pixels, mask;
    HDC dc = GetDC(nullptr);
    bool ok = dc && width && height;
    if (ok && icon.hbmColor)
        ok = bitmap_pixels(dc, icon.hbmColor, width, height, pixels);
    if (ok && icon.hbmMask)
        ok = bitmap_pixels(dc, icon.hbmMask, width,
                           icon.hbmColor ? height : height * 2, mask);

    if (ok && icon.hbmColor) {
        const bool hasAlpha = std::any_of(pixels.begin(), pixels.end(),
            [](uint32_t p) { return (p >> 24) != 0; });
        if (!hasAlpha) {
            for (size_t i = 0; i < pixels.size(); ++i) {
                const bool transparent = !mask.empty() && (mask[i] & 0x00FFFFFFu);
                pixels[i] = transparent ? 0u : (pixels[i] | 0xFF000000u);
            }
        }
    } else if (ok) {
        pixels.resize(static_cast<size_t>(width) * height);
        const size_t plane = pixels.size();
        for (size_t i = 0; i < plane; ++i) {
            const bool andBit = (mask[i] & 0x00FFFFFFu) != 0;
            const bool xorBit = (mask[i + plane] & 0x00FFFFFFu) != 0;
            if (andBit && !xorBit) pixels[i] = 0u;
            else pixels[i] = xorBit ? 0xFFFFFFFFu : 0xFF000000u;
        }
    }

    if (dc) ReleaseDC(nullptr, dc);
    if (icon.hbmColor) DeleteObject(icon.hbmColor);
    if (icon.hbmMask) DeleteObject(icon.hbmMask);
    if (!ok) return false;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{pixels.data(), width * 4, 0};
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srvRaw, srvSrgb;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateTexture2D(&td, &init, &texture)) ||
        (sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM,
         FAILED(device->CreateShaderResourceView(texture.Get(), &sv, &srvRaw))) ||
        (sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
         FAILED(device->CreateShaderResourceView(texture.Get(), &sv, &srvSrgb))))
        return false;

    g.cursor.handle = handle;
    g.cursor.width = width;
    g.cursor.height = height;
    g.cursor.hotspotX = icon.xHotspot;
    g.cursor.hotspotY = icon.yHotspot;
    g.cursor.srvRaw = std::move(srvRaw);
    g.cursor.srvSrgb = std::move(srvSrgb);
    return true;
}

bool build(ID3D11Device* device)
{
    static const char hlsl[] =
        "cbuffer C : register(b0) { float4 rect; float4 texel; };"
        "struct V { float4 p:SV_Position; float2 uv:TEXCOORD0; };"
        "V vs(uint id:SV_VertexID) {"
        " float2 uv=float2((id==1||id==2||id==4)?1:0,(id>=2&&id<=4)?1:0);"
        " V o; o.p=float4(lerp(rect.xy,rect.zw,uv),0,1); o.uv=uv; return o; }"
        "Texture2D tex:register(t0); SamplerState smp:register(s0);"
        "float4 ps(V i):SV_Target{"
        " float2 dx=float2(texel.x,0),dy=float2(0,texel.y);"
        " float4 c=tex.Sample(smp,i.uv);"
        " c=max(c,tex.Sample(smp,i.uv+dx)); c=max(c,tex.Sample(smp,i.uv-dx));"
        " c=max(c,tex.Sample(smp,i.uv+dy)); c=max(c,tex.Sample(smp,i.uv-dy));"
        " return c;}";
    ComPtr<ID3DBlob> vsb, psb, err;
    if (FAILED(D3DCompile(hlsl, sizeof(hlsl)-1, "cursor", nullptr, nullptr,
                          "vs", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(hlsl, sizeof(hlsl)-1, "cursor", nullptr, nullptr,
                          "ps", "ps_5_0", 0, 0, &psb, &err)) ||
        FAILED(device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g.vs)) ||
        FAILED(device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g.ps))) {
        VRLOG("cursor overlay: shader build failed");
        return false;
    }
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(Constants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&cb, nullptr, &g.constants))) return false;

    D3D11_SAMPLER_DESC sd{};
    // POINT keeps a thin one-pixel Windows arrow crisp in the headset. Linear
    // filtering made the earlier 1.35x enlargement look almost unchanged.
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sd, &g.sampler))) return false;

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    return SUCCEEDED(device->CreateBlendState(&bd, &g.blend));
}

void adjust_offset()
{
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) return;
    const bool up = (GetAsyncKeyState(VK_PRIOR) & 0x8000) != 0;
    const bool down = (GetAsyncKeyState(VK_NEXT) & 0x8000) != 0;
    if (up == down) return;
    const float delta = up ? 2.0f : -2.0f;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
        g.offsetY -= delta; // Shift+PgUp moves up; Shift+PgDn moves down.
    else
        g.offsetX += delta; // PgUp moves right; PgDn moves left.

    const DWORD now = GetTickCount();
    if (now - g.lastAdjustLog >= 250) {
        VRLOG("cursor offset: X=%.0f Y=%.0f px (PgUp/PgDn X, Shift+PgUp/PgDn Y)",
              g.offsetX, g.offsetY);
        g.lastAdjustLog = now;
    }
}

} // namespace

void draw(ID3D11Device* device, ID3D11DeviceContext* context,
          ID3D11RenderTargetView* target, HWND window,
          uint32_t targetWidth, uint32_t targetHeight,
          bool targetIsSrgb)
{
    draw_in_rect(device, context, target, window, targetWidth, targetHeight,
                 0.0f, 0.0f, static_cast<float>(targetWidth),
                 static_cast<float>(targetHeight), targetIsSrgb, 1.60f);
}

void draw_in_rect(ID3D11Device* device, ID3D11DeviceContext* context,
                  ID3D11RenderTargetView* target, HWND window,
                  uint32_t targetWidth, uint32_t targetHeight,
                  float destX, float destY, float destWidth, float destHeight,
                  bool targetIsSrgb, float cursorScale)
{
    if (!device || !context || !target || !window || !targetWidth || !targetHeight ||
        destWidth <= 0.0f || destHeight <= 0.0f)
        return;
    adjust_offset();
    CURSORINFO ci{sizeof(ci)};
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor)
        return;

    if (g.device.Get() != device) reset();
    if (!g.device) {
        g.device = device;
        if (!build(device)) { g.buildFailed = true; return; }
    }
    if (g.buildFailed) return;
    if (g.cursor.handle != ci.hCursor && !load_cursor(device, ci.hCursor)) return;

    POINT p = ci.ptScreenPos;
    RECT client{};
    if (!ScreenToClient(window, &p) || !GetClientRect(window, &client)) return;
    const float cw = static_cast<float>(client.right - client.left);
    const float ch = static_cast<float>(client.bottom - client.top);
    // Make the thin native cursor easier to resolve in a headset while keeping
    // its hotspot (the click point) fixed in place.
    const float kCursorScale = cursorScale;
    auto r = cursorpolicy::texture_rect(
        static_cast<float>(p.x), static_cast<float>(p.y),
        static_cast<float>(g.cursor.hotspotX) * kCursorScale,
        static_cast<float>(g.cursor.hotspotY) * kCursorScale,
        static_cast<float>(g.cursor.width) * kCursorScale,
        static_cast<float>(g.cursor.height) * kCursorScale,
        cw, ch, destWidth, destHeight);
    r.left += destX + g.offsetX; r.right += destX + g.offsetX;
    r.top += destY + g.offsetY; r.bottom += destY + g.offsetY;
    if (!cursorpolicy::intersects(r, static_cast<float>(targetWidth),
                                  static_cast<float>(targetHeight))) return;

    // Pixel-space top-left coordinates to clip space. Viewport clipping keeps
    // the original UV interpolation correct when a cursor crosses an edge.
    const Constants values{
        2.0f * r.left / targetWidth - 1.0f,
        1.0f - 2.0f * r.top / targetHeight,
        2.0f * r.right / targetWidth - 1.0f,
        1.0f - 2.0f * r.bottom / targetHeight,
        1.0f / g.cursor.width, 1.0f / g.cursor.height, 0.0f, 0.0f};
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(g.constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    *static_cast<Constants*>(mapped.pData) = values;
    context->Unmap(g.constants.Get(), 0);

    ID3D11Buffer* cb = g.constants.Get();
    ID3D11ShaderResourceView* srv = targetIsSrgb ? g.cursor.srvSrgb.Get()
                                                 : g.cursor.srvRaw.Get();
    ID3D11SamplerState* sampler = g.sampler.Get();
    context->OMSetRenderTargets(1, &target, nullptr);
    context->OMSetBlendState(g.blend.Get(), nullptr, 0xFFFFFFFFu);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(g.vs.Get(), nullptr, 0);
    context->VSSetConstantBuffers(0, 1, &cb);
    context->PSSetShader(g.ps.Get(), nullptr, 0);
    context->PSSetShaderResources(0, 1, &srv);
    context->PSSetSamplers(0, 1, &sampler);
    context->Draw(6, 0);
    srv = nullptr;
    context->PSSetShaderResources(0, 1, &srv);
}

void reset()
{
    g = {};
}

} // namespace cursoroverlay
