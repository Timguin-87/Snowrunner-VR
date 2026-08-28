#include "render/frame_dump.hpp"
#include "common/log.h"

#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <atomic>
#include <string>
#include <vector>
#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace framedump {
namespace {

std::atomic<int>  g_remaining{0};
std::atomic<bool> g_fullRes{false};
std::atomic<int>  g_frameIndex{0};
std::string       g_dir;

// One BGR buffer per eye, PERSISTED across frames. Only one eye is
// blitted per frame and the headset keeps showing the other eye's previous
// image, so carrying the buffers over makes each composite match what is
// actually on screen rather than blanking the un-blitted half.
std::vector<uint8_t> g_eyeBuf[2];
int g_eyeW = 0, g_eyeH = 0;

// Each press writes into its own timestamped subfolder, so bursts taken in
// different views never overwrite each other.
std::string g_burstDir;

// Written next to our own DLL, not the process CWD -- the game's working
// directory is not guaranteed to be somewhere writable.
const std::string& dump_dir()
{
    if (!g_dir.empty()) return g_dir;

    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&dump_dir), &self);
    char path[MAX_PATH]{};
    GetModuleFileNameA(self, path, MAX_PATH);
    std::string p(path);
    size_t slash = p.find_last_of("\\/");
    g_dir = (slash == std::string::npos) ? "dibr_dump" : p.substr(0, slash) + "\\dibr_dump";
    CreateDirectoryA(g_dir.c_str(), nullptr);
    return g_dir;
}

// JPEG via WIC -- part of Windows, so no encoder dependency to add. Top-down
// 24bpp BGR in, which is exactly the layout capture() builds.
ComPtr<IWICImagingFactory> g_wic;

bool wic_ready()
{
    if (g_wic) return true;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&g_wic));
    if (hr == CO_E_NOTINITIALIZED) {
        // The Present thread may not have COM up; MTA keeps us off any STA
        // message pump. RPC_E_CHANGED_MODE means it is already initialized
        // differently, which is fine -- retry regardless.
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&g_wic));
    }
    if (FAILED(hr)) { VRLOG("frame dump: WIC unavailable (hr=0x%08X)", (unsigned)hr); return false; }
    return true;
}

bool write_jpg(const char* file, const uint8_t* bgr, int w, int h, float quality = 0.92f)
{
    if (!wic_ready()) return false;

    wchar_t wpath[MAX_PATH]{};
    MultiByteToWideChar(CP_ACP, 0, file, -1, wpath, MAX_PATH);

    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(g_wic->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(wpath, GENERIC_WRITE)) ||
        FAILED(g_wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)) ||
        FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(enc->CreateNewFrame(&frame, &props)))
        return false;

    PROPBAG2 opt{}; opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT v{}; v.vt = VT_R4; v.fltVal = quality;
    props->Write(1, &opt, &v);

    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->Initialize(props.Get())) ||
        FAILED(frame->SetSize((UINT)w, (UINT)h)) ||
        FAILED(frame->SetPixelFormat(&fmt)))
        return false;

    const UINT stride = (UINT)w * 3;
    if (FAILED(frame->WritePixels((UINT)h, stride, stride * (UINT)h,
                                  const_cast<BYTE*>(bgr))) ||
        FAILED(frame->Commit()) || FAILED(enc->Commit()))
        return false;
    return true;
}

ComPtr<ID3D11Texture2D> g_staging;
UINT g_sw = 0, g_sh = 0;
DXGI_FORMAT g_sfmt = DXGI_FORMAT_UNKNOWN;

bool ensure_staging(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& src)
{
    if (g_staging && g_sw == src.Width && g_sh == src.Height && g_sfmt == src.Format)
        return true;
    g_staging.Reset();
    D3D11_TEXTURE2D_DESC d = src;
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &g_staging))) return false;
    g_sw = src.Width; g_sh = src.Height; g_sfmt = src.Format;
    return true;
}

} // namespace

void request(int frames, bool fullRes)
{
    g_fullRes.store(fullRes);
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char sub[MAX_PATH];
    sprintf_s(sub, "%s\\%02u%02u%02u", dump_dir().c_str(), st.wHour, st.wMinute, st.wSecond);
    g_burstDir = sub;
    CreateDirectoryA(g_burstDir.c_str(), nullptr);

    g_frameIndex.store(0);
    g_remaining.store(frames);
    VRLOG("frame dump -> %d stereo frames to %s", frames, g_burstDir.c_str());
}

bool active() { return g_remaining.load() > 0; }

int frame_index() { return g_frameIndex.load(); }

void capture(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, uint32_t eye)
{
    if (g_remaining.load() <= 0 || !ctx || !src) return;

    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!dev) return;

    D3D11_TEXTURE2D_DESC sd{};
    src->GetDesc(&sd);
    if (!ensure_staging(dev.Get(), sd)) return;

    ctx->CopyResource(g_staging.Get(), src);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g_staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return;

    // Half resolution keeps a burst manageable while staying more than sharp
    // enough to compare the two eyes against each other -- but it is a POINT
    // decimation, so it both destroys and aliases fine detail. Anything about
    // sharpness (edge softness, UI legibility, upscale artefacts) has to be
    // judged at step 1; there is nothing to salvage from the halved image.
    const int step = g_fullRes.load() ? 1 : 2;
    const int ow = (int)(sd.Width / step), oh = (int)(sd.Height / step);
    if (ow != g_eyeW || oh != g_eyeH) {
        g_eyeW = ow; g_eyeH = oh;
        g_eyeBuf[0].assign((size_t)ow * oh * 3, 0);
        g_eyeBuf[1].assign((size_t)ow * oh * 3, 0);
    }
    std::vector<uint8_t>& bgr = g_eyeBuf[eye & 1u];
    const uint8_t* base = static_cast<const uint8_t*>(m.pData);
    const bool bgraOrder = (sd.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                            sd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                            sd.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS);
    for (int y = 0; y < oh; ++y) {
        const uint8_t* row = base + (size_t)(y * step) * m.RowPitch;
        uint8_t* out = bgr.data() + (size_t)y * ow * 3;
        for (int x = 0; x < ow; ++x) {
            const uint8_t* px = row + (size_t)(x * step) * 4;
            if (bgraOrder) { out[x*3+0] = px[0]; out[x*3+1] = px[1]; out[x*3+2] = px[2]; }
            else           { out[x*3+0] = px[2]; out[x*3+1] = px[1]; out[x*3+2] = px[0]; }
        }
    }
    ctx->Unmap(g_staging.Get(), 0);
}

void end_frame()
{
    if (g_remaining.load() <= 0) return;
    const int idx = g_frameIndex.fetch_add(1);

    // One stereo pair per file: left eye on the left half, right on the right,
    // so a pair can be judged at a glance instead of flipping between files.
    if (g_eyeW > 0) {
        const int cw = g_eyeW * 2, ch = g_eyeH;
        std::vector<uint8_t> side((size_t)cw * ch * 3, 0);
        for (int y = 0; y < ch; ++y) {
            uint8_t* out = side.data() + (size_t)y * cw * 3;
            const size_t half = (size_t)g_eyeW * 3;
            memcpy(out,        g_eyeBuf[0].data() + (size_t)y * half, half);
            memcpy(out + half, g_eyeBuf[1].data() + (size_t)y * half, half);
        }
        char file[MAX_PATH];
        sprintf_s(file, "%s\\f%d_LR.jpg", g_burstDir.c_str(), idx);
        if (!write_jpg(file, side.data(), cw, ch))
            VRLOG("frame dump: write failed (%s)", file);
    }

    if (g_remaining.fetch_sub(1) <= 1)
        VRLOG("frame dump complete: %d frames in %s", idx + 1, g_burstDir.c_str());
}

} // namespace framedump
