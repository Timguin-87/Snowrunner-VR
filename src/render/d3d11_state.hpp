#pragma once
#include <d3d11_1.h>

// THE WHOLE D3D11 PIPELINE STATE, captured and put back exactly.
//
// For the mod's own work done MID-FRAME on the game's immediate context --
// DLSS in place of the TAA resolve (render/dlss.cpp) -- where the game's next
// draw relies on everything it bound earlier still being bound: shaders,
// constant buffers, textures, samplers, render targets, viewports and the
// fixed-function states. A third-party library (NGX) runs inside the bracket,
// so nothing is assumed about what it touches.
//
// Every Get* AddRefs what it returns; restore() hands each pointer back to the
// matching Set* and releases it. Use one capture per restore. Constant buffers
// go through the D3D11.1 calls when the context has them, so a buffer bound
// with an offset comes back with the same offset.
struct D3D11StateBackup {
    static constexpr UINT kCb  = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;   // 14
    static constexpr UINT kSrv = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;        // 128
    static constexpr UINT kSmp = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;               // 16
    static constexpr UINT kVb  = D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;           // 32
    static constexpr UINT kRtv = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;              // 8
    static constexpr UINT kUav = D3D11_PS_CS_UAV_REGISTER_COUNT;                      // 8
    static constexpr UINT kVp  = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;

    struct Stage {
        IUnknown*                 shader = nullptr;
        ID3D11Buffer*             cb[kCb] = {};
        UINT                      cbFirst[kCb] = {}, cbNum[kCb] = {};
        ID3D11ShaderResourceView* srv[kSrv] = {};
        ID3D11SamplerState*       smp[kSmp] = {};
    };
    enum { VS, HS, DS, GS, PS, CS, kStages };
    Stage st[kStages];

    ID3D11InputLayout*       il = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11Buffer*            vb[kVb] = {};
    UINT                     vbStride[kVb] = {}, vbOffset[kVb] = {};
    ID3D11Buffer*            ib = nullptr;
    DXGI_FORMAT              ibFmt = DXGI_FORMAT_UNKNOWN;
    UINT                     ibOffset = 0;

    ID3D11RasterizerState*   rs = nullptr;
    UINT                     vpCount = kVp, scCount = kVp;
    D3D11_VIEWPORT           vp[kVp] = {};
    D3D11_RECT               sc[kVp] = {};

    ID3D11BlendState*        blend = nullptr;
    FLOAT                    blendFactor[4] = {};
    UINT                     sampleMask = 0;
    ID3D11DepthStencilState* ds = nullptr;
    UINT                     stencilRef = 0;
    ID3D11RenderTargetView*  rtv[kRtv] = {};
    ID3D11DepthStencilView*  dsv = nullptr;
    ID3D11UnorderedAccessView* csUav[kUav] = {};

    ID3D11DeviceContext1* c1 = nullptr;

    void capture(ID3D11DeviceContext* c)
    {
        c->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1));
        c->IAGetInputLayout(&il);
        c->IAGetPrimitiveTopology(&topo);
        c->IAGetVertexBuffers(0, kVb, vb, vbStride, vbOffset);
        c->IAGetIndexBuffer(&ib, &ibFmt, &ibOffset);

        c->VSGetShader(reinterpret_cast<ID3D11VertexShader**>(&st[VS].shader), nullptr, nullptr);
        c->HSGetShader(reinterpret_cast<ID3D11HullShader**>(&st[HS].shader), nullptr, nullptr);
        c->DSGetShader(reinterpret_cast<ID3D11DomainShader**>(&st[DS].shader), nullptr, nullptr);
        c->GSGetShader(reinterpret_cast<ID3D11GeometryShader**>(&st[GS].shader), nullptr, nullptr);
        c->PSGetShader(reinterpret_cast<ID3D11PixelShader**>(&st[PS].shader), nullptr, nullptr);
        c->CSGetShader(reinterpret_cast<ID3D11ComputeShader**>(&st[CS].shader), nullptr, nullptr);
#define SR_GET(P, S)                                                               \
        if (c1) c1->P##GetConstantBuffers1(0, kCb, st[S].cb, st[S].cbFirst, st[S].cbNum); \
        else    c->P##GetConstantBuffers(0, kCb, st[S].cb);                         \
        c->P##GetShaderResources(0, kSrv, st[S].srv);                               \
        c->P##GetSamplers(0, kSmp, st[S].smp);
        SR_GET(VS, VS) SR_GET(HS, HS) SR_GET(DS, DS) SR_GET(GS, GS) SR_GET(PS, PS) SR_GET(CS, CS)
#undef SR_GET
        c->CSGetUnorderedAccessViews(0, kUav, csUav);

        c->RSGetState(&rs);
        vpCount = kVp; c->RSGetViewports(&vpCount, vp);
        scCount = kVp; c->RSGetScissorRects(&scCount, sc);

        c->OMGetBlendState(&blend, blendFactor, &sampleMask);
        c->OMGetDepthStencilState(&ds, &stencilRef);
        c->OMGetRenderTargets(kRtv, rtv, &dsv);
    }

    void restore(ID3D11DeviceContext* c)
    {
        c->IASetInputLayout(il);
        c->IASetPrimitiveTopology(topo);
        c->IASetVertexBuffers(0, kVb, vb, vbStride, vbOffset);
        c->IASetIndexBuffer(ib, ibFmt, ibOffset);

        c->VSSetShader(static_cast<ID3D11VertexShader*>(st[VS].shader), nullptr, 0);
        c->HSSetShader(static_cast<ID3D11HullShader*>(st[HS].shader), nullptr, 0);
        c->DSSetShader(static_cast<ID3D11DomainShader*>(st[DS].shader), nullptr, 0);
        c->GSSetShader(static_cast<ID3D11GeometryShader*>(st[GS].shader), nullptr, 0);
        c->PSSetShader(static_cast<ID3D11PixelShader*>(st[PS].shader), nullptr, 0);
        c->CSSetShader(static_cast<ID3D11ComputeShader*>(st[CS].shader), nullptr, 0);
#define SR_SET(P, S)                                                               \
        if (c1) c1->P##SetConstantBuffers1(0, kCb, st[S].cb, st[S].cbFirst, st[S].cbNum); \
        else    c->P##SetConstantBuffers(0, kCb, st[S].cb);                         \
        c->P##SetShaderResources(0, kSrv, st[S].srv);                               \
        c->P##SetSamplers(0, kSmp, st[S].smp);
        SR_SET(VS, VS) SR_SET(HS, HS) SR_SET(DS, DS) SR_SET(GS, GS) SR_SET(PS, PS) SR_SET(CS, CS)
#undef SR_SET
        UINT keep[kUav];
        for (UINT i = 0; i < kUav; ++i) keep[i] = (UINT)-1;
        c->CSSetUnorderedAccessViews(0, kUav, csUav, keep);

        c->RSSetState(rs);
        c->RSSetViewports(vpCount, vp);
        c->RSSetScissorRects(scCount, sc);

        c->OMSetBlendState(blend, blendFactor, sampleMask);
        c->OMSetDepthStencilState(ds, stencilRef);
        c->OMSetRenderTargets(kRtv, rtv, dsv);

        release_all();
    }

private:
    template <typename T> static void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }
    void release_all()
    {
        rel(il); for (auto& b : vb) rel(b); rel(ib);
        for (auto& s : st) {
            rel(s.shader);
            for (auto& b : s.cb) rel(b);
            for (auto& v : s.srv) rel(v);
            for (auto& m : s.smp) rel(m);
        }
        for (auto& u : csUav) rel(u);
        rel(rs); rel(blend); rel(ds);
        for (auto& r : rtv) rel(r);
        rel(dsv);
        rel(c1);
    }
};
