#include "render/fsr.hpp"
#include "common/log.h"

#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#if SRVR_HAVE_FSR
#include <d3d12.h>
#include <ffx_api.h>
#include <ffx_upscale.h>
#include <dx12/ffx_api_dx12.h>
#endif

using Microsoft::WRL::ComPtr;

namespace fsr {

#if !SRVR_HAVE_FSR

bool compiled_in() { return false; }
bool ensure(ID3D11Device*) { return false; }
const char* why() { return "this build of the mod has no FSR"; }
void retry() {}
bool ensure_context(int, int, uint32_t, uint32_t, uint32_t, uint32_t, bool, bool* created)
{
    if (created) *created = false;
    return false;
}
const char* version_name(int) { return ""; }
bool evaluate(ID3D11DeviceContext*, const Eval&) { return false; }
void release() {}
void shutdown() {}

#else

namespace {

constexpr wchar_t kLoaderDll[]   = L"amd_fidelityfx_loader_dx12.dll";
constexpr wchar_t kUpscalerDll[] = L"amd_fidelityfx_upscaler_dx12.dll";

// D3D12 work in flight at once. One upscale a Present, so three is slack.
constexpr int kFrames = 3;

// A texture both APIs see: made on the D3D12 side, opened on the D3D11 one.
struct Shared {
    ComPtr<ID3D12Resource>  res;
    ComPtr<ID3D11Texture2D> tex;
    UINT        w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    bool        uav = false;
};

// One eye's FSR context, with the descriptions it was made from: AMD's API
// wants those alive for as long as the context is.
struct Eye {
    ffxContext ctx = nullptr;
    int  version = 0;             // asked for: 3 or 4
    UINT w = 0, h = 0, ow = 0, oh = 0;
    bool hdr = false;
    char running[48] = "";        // what the DLL says it runs
    ffxCreateContextDescUpscale        up{};
    ffxCreateBackendDX12Desc           backend{};
    ffxCreateContextDescUpscaleVersion api{};
    ffxOverrideVersion                 pin{};
};

struct State {
    bool tried = false;
    bool ok = false;
    char why[192] = "not started";
    ID3D11Device* dev11 = nullptr;          // the game's, not owned
    ComPtr<ID3D11Device5>        dev5;
    ComPtr<ID3D11DeviceContext4> ctx4;
    HMODULE d3d12 = nullptr, loader = nullptr;
    PfnFfxCreateContext  create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxQuery          query = nullptr;
    PfnFfxDispatch       dispatch = nullptr;
    ComPtr<ID3D12Device>              dev12;
    ComPtr<ID3D12CommandQueue>        queue;
    ComPtr<ID3D12CommandAllocator>    alloc[kFrames];
    uint64_t                          allocDone[kFrames] = {};
    int                               allocNext = 0;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence12;
    ComPtr<ID3D11Fence> fence11;
    HANDLE   fenceEvent = nullptr;
    uint64_t fenceValue = 0;
    // The newest 3.x and 4.x the DLL offers on this GPU, 0 = none.
    uint64_t id3 = 0, id4 = 0;
    char     name3[48] = "", name4[48] = "";
    Shared colour, depth, motion, output;
    Eye eye[2];
    uint32_t failures = 0;
} g;

void set_why(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(g.why, sizeof(g.why), _TRUNCATE, fmt, ap);
    va_end(ap);
}

std::wstring module_dir()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&module_dir), &self);
    wchar_t path[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(self, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return L".";
    for (wchar_t* p = path + len; p > path; --p)
        if (p[-1] == L'\\') { p[-1] = L'\0'; break; }
    return path;
}

// FSR's own warnings and errors, into the mod's log -- capped, as a context
// that dislikes something says so every frame.
std::atomic<int> g_messages{0};
void ffx_message(uint32_t type, const wchar_t* message)
{
    const int n = g_messages.fetch_add(1);
    if (n < 20)
        VRLOG("FSR %s: %ls", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning",
              message ? message : L"");
    else if (n == 20)
        VRLOG("FSR: (further messages not logged)");
}

// "4.1.2" -> 4001002, from wherever the first digit is in AMD's name for it.
uint32_t version_number(const char* name, uint32_t& major)
{
    major = 0;
    if (!name) return 0;
    while (*name && (*name < '0' || *name > '9')) ++name;
    unsigned a = 0, b = 0, c = 0;
    if (sscanf_s(name, "%u.%u.%u", &a, &b, &c) < 1) return 0;
    major = a;
    return a * 1000000u + b * 1000u + c;
}

// THE VERSIONS THE DLL OFFERS for this device: the newest 3.x and 4.x are what
// FSR 3 and FSR 4 pin. A GPU FSR 4 does not run on is offered none.
void find_versions()
{
    ffxQueryDescGetVersions q{};
    q.header.type    = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device         = g.dev12.Get();
    uint64_t count = 0;
    q.outputCount = &count;
    if (g.query(nullptr, &q.header) != FFX_API_RETURN_OK || count == 0) {
        VRLOG("FSR: the upscaler offers no versions on this GPU");
        return;
    }
    constexpr uint64_t kMax = 16;
    if (count > kMax) count = kMax;
    uint64_t ids[kMax] = {};
    const char* names[kMax] = {};
    q.versionIds   = ids;
    q.versionNames = names;
    if (g.query(nullptr, &q.header) != FFX_API_RETURN_OK) return;

    char list[160] = "";
    uint32_t best3 = 0, best4 = 0;
    for (uint64_t i = 0; i < count; ++i) {
        uint32_t major = 0;
        const uint32_t v = version_number(names[i], major);
        if (major == 3 && v > best3) {
            best3 = v; g.id3 = ids[i];
            strncpy_s(g.name3, names[i] ? names[i] : "3", _TRUNCATE);
        } else if (major == 4 && v > best4) {
            best4 = v; g.id4 = ids[i];
            strncpy_s(g.name4, names[i] ? names[i] : "4", _TRUNCATE);
        }
        const size_t len = std::strlen(list);
        _snprintf_s(list + len, sizeof(list) - len, _TRUNCATE, "%s%s", len ? ", " : "",
                    names[i] ? names[i] : "?");
    }
    VRLOG("FSR: versions offered on this GPU: %s", list);
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from,
                                  D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter  = to;
    return b;
}

// Waits on the CPU for everything the D3D12 queue has been given. Only before
// anything it may still be using goes away -- never per frame.
void idle()
{
    if (!g.queue || !g.fence12 || !g.fenceEvent) return;
    const uint64_t v = ++g.fenceValue;
    g.queue->Signal(g.fence12.Get(), v);
    if (g.fence12->GetCompletedValue() < v &&
        SUCCEEDED(g.fence12->SetEventOnCompletion(v, g.fenceEvent)))
        WaitForSingleObject(g.fenceEvent, 2000);
}

void destroy_eye(Eye& e)
{
    if (e.ctx && g.destroy) g.destroy(&e.ctx, nullptr);
    e.ctx = nullptr;
    e.version = 0;
    e.w = e.h = e.ow = e.oh = 0;
    e.running[0] = '\0';
}

// The typed member of a typeless format's group, for a texture FSR reads or
// writes; CopyResource stays legal across one group. Anything already typed
// stays as it is.
DXGI_FORMAT typed_of(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS:       return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS:          return DXGI_FORMAT_R32_FLOAT;
    default:                                return f;
    }
}

// A shared texture shaped like `like`: same size, its format's typed member.
//
// MADE ON THE D3D11 SIDE AND OPENED ON THE D3D12 ONE, not the other way round.
// Measured 2026-11-05 on an NVIDIA driver: D3D11 opened a D3D12-made texture
// only as an RGBA8 render target, and refused R32/RG16F/R11G11B10 with any
// flags (E_INVALIDARG), while every one of them made by D3D11 -- shader
// resource and render target, a UAV where wanted -- opened in D3D12 at once.
bool ensure_shared(Shared& s, ID3D11Texture2D* like, bool uav, const char* what)
{
    if (!like) return false;
    D3D11_TEXTURE2D_DESC d{};
    like->GetDesc(&d);
    const DXGI_FORMAT f = typed_of(d.Format);
    if (s.tex && s.w == d.Width && s.h == d.Height && s.fmt == f && s.uav == uav) return true;
    if (s.res) idle();   // the D3D12 queue may still be using the old one
    s = Shared{};

    D3D11_TEXTURE2D_DESC td{};
    td.Width            = d.Width;
    td.Height           = d.Height;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = f;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET |
                          (uav ? D3D11_BIND_UNORDERED_ACCESS : 0u);
    td.MiscFlags        = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    ComPtr<IDXGIResource1> dr;
    HANDLE h = nullptr;
    if (FAILED(g.dev11->CreateTexture2D(&td, nullptr, &s.tex)) || FAILED(s.tex.As(&dr)) ||
        FAILED(dr->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                      nullptr, &h))) {
        VRLOG("FSR: could not make the shared %s texture (%ux%u, format %d)", what, d.Width,
              d.Height, (int)f);
        s = Shared{};
        return false;
    }
    const HRESULT hr = g.dev12->OpenSharedHandle(h, IID_PPV_ARGS(&s.res));
    CloseHandle(h);
    if (FAILED(hr)) {
        VRLOG("FSR: D3D12 could not open the shared %s texture (0x%08X)", what, (unsigned)hr);
        s = Shared{};
        return false;
    }
    s.w = d.Width; s.h = d.Height; s.fmt = f; s.uav = uav;
    return true;
}

std::atomic<bool> g_retry{false};

// Everything ensure() makes, gone -- before a retry, and at exit.
void drop_device()
{
    g.list.Reset();
    for (auto& al : g.alloc) al.Reset();
    for (auto& d : g.allocDone) d = 0;
    g.allocNext = 0;
    g.queue.Reset();
    g.fence11.Reset();
    g.fence12.Reset();
    g.dev12.Reset();
    g.ctx4.Reset();
    g.dev5.Reset();
    if (g.fenceEvent) { CloseHandle(g.fenceEvent); g.fenceEvent = nullptr; }
    g.id3 = g.id4 = 0;
    g.name3[0] = g.name4[0] = '\0';
    g.ok = false;
}

} // namespace

bool compiled_in() { return true; }
const char* why() { return g.why; }
void retry() { g_retry.store(true); }

bool ensure(ID3D11Device* dev)
{
    if (g.tried && !g.ok && g_retry.exchange(false)) {
        drop_device();
        g.tried = false;
    }
    if (g.tried) return g.ok && g.dev11 == dev;
    g.tried = true;
    g.dev11 = dev;
    if (!dev) { set_why("no device"); return false; }

    // AMD's DLLs first: the likeliest reason there is no FSR, and the cheapest
    // to find out. The loader alone would load and then fail at the first
    // context, so the upscaler is looked for by name too.
    const std::wstring dir = module_dir();
    if (GetFileAttributesW((dir + L"\\" + kUpscalerDll).c_str()) == INVALID_FILE_ATTRIBUTES) {
        set_why("no %ls next to SnowRunner.exe", kUpscalerDll);
        VRLOG("FSR: not available -- %s", g.why);
        return false;
    }
    g.loader = LoadLibraryW((dir + L"\\" + kLoaderDll).c_str());
    if (!g.loader) {
        set_why("no %ls next to SnowRunner.exe (error %lu)", kLoaderDll, GetLastError());
        VRLOG("FSR: not available -- %s", g.why);
        return false;
    }
    g.create   = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(g.loader, "ffxCreateContext"));
    g.destroy  = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(g.loader, "ffxDestroyContext"));
    g.query    = reinterpret_cast<PfnFfxQuery>(GetProcAddress(g.loader, "ffxQuery"));
    g.dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(g.loader, "ffxDispatch"));
    if (!g.create || !g.destroy || !g.query || !g.dispatch) {
        set_why("%ls is not AMD's FSR loader", kLoaderDll);
        VRLOG("FSR: not available -- %s", g.why);
        return false;
    }

    // The game's side of the meeting: a D3D11.4 device and context, for the
    // shared fence.
    ComPtr<ID3D11DeviceContext> imm;
    dev->GetImmediateContext(&imm);
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&g.dev5))) || !imm ||
        FAILED(imm.As(&g.ctx4))) {
        set_why("needs Direct3D 11.4");
        VRLOG("FSR: not available -- %s", g.why);
        return false;
    }

    // D3D12 on the game's own adapter. Loaded by hand, so a machine without it
    // costs the mod nothing but this option.
    g.d3d12 = LoadLibraryW(L"d3d12.dll");
    const auto createDevice = g.d3d12
        ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(g.d3d12, "D3D12CreateDevice"))
        : nullptr;
    ComPtr<IDXGIDevice>  dxgiDev;
    ComPtr<IDXGIAdapter> adapter;
    if (!createDevice || FAILED(dev->QueryInterface(IID_PPV_ARGS(&dxgiDev))) ||
        FAILED(dxgiDev->GetAdapter(&adapter)) ||
        FAILED(createDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g.dev12)))) {
        set_why("could not make a Direct3D 12 device on the game's GPU");
        VRLOG("FSR: not available -- %s", g.why);
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    bool made = SUCCEEDED(g.dev12->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue)));
    for (int i = 0; made && i < kFrames; ++i)
        made = SUCCEEDED(g.dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                         IID_PPV_ARGS(&g.alloc[i])));
    made = made && SUCCEEDED(g.dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                        g.alloc[0].Get(), nullptr,
                                                        IID_PPV_ARGS(&g.list))) &&
           SUCCEEDED(g.list->Close());
    // The fence both APIs signal and wait on.
    HANDLE fh = nullptr;
    made = made && SUCCEEDED(g.dev12->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
                                                  IID_PPV_ARGS(&g.fence12))) &&
           SUCCEEDED(g.dev12->CreateSharedHandle(g.fence12.Get(), nullptr, GENERIC_ALL,
                                                 nullptr, &fh));
    if (made) {
        made = SUCCEEDED(g.dev5->OpenSharedFence(fh, IID_PPV_ARGS(&g.fence11)));
        CloseHandle(fh);
    }
    g.fenceEvent = made ? CreateEventW(nullptr, FALSE, FALSE, nullptr) : nullptr;
    if (!made || !g.fenceEvent) {
        set_why("could not set up the Direct3D 12 queue and fence");
        VRLOG("FSR: not available -- %s", g.why);
        return false;
    }

    find_versions();
    if (!g.id3 && !g.id4) {
        set_why("the upscaler DLL offers no FSR 3 or 4 on this GPU");
        VRLOG("FSR: not available -- %s", g.why);
        return false;
    }
    g.ok = true;
    set_why("");
    VRLOG("FSR: up -- Direct3D 12 beside the game's device on its GPU, AMD's loader from %ls; "
          "FSR 3 = %s, FSR 4 = %s", dir.c_str(), g.id3 ? g.name3 : "none",
          g.id4 ? g.name4 : "none (not offered on this GPU)");
    return true;
}

bool ensure_context(int eye, int version, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh,
                    bool hdr, bool* created)
{
    if (created) *created = false;
    if (!g.ok) return false;
    Eye& e = g.eye[eye & 1];
    if (e.ctx && e.version == version && e.w == w && e.h == h && e.ow == ow && e.oh == oh &&
        e.hdr == hdr)
        return true;
    idle();   // the old context may still be in use
    destroy_eye(e);

    // FSR 4 where this GPU is offered it, FSR 3 otherwise.
    const uint64_t pin = (version >= 4 && g.id4) ? g.id4 : g.id3;
    e.up = {};
    e.up.header.type   = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    e.up.flags         = FFX_UPSCALE_ENABLE_DEPTH_INVERTED |
                         (hdr ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE
                              : FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE);
    e.up.maxRenderSize  = {w, h};
    e.up.maxUpscaleSize = {ow, oh};
    e.up.fpMessage      = &ffx_message;
    e.backend = {};
    e.backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    e.backend.device      = g.dev12.Get();
    e.api = {};
    e.api.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    e.api.version     = FFX_UPSCALER_VERSION;
    e.pin = {};
    e.pin.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    e.pin.versionId   = pin;
    e.up.header.pNext      = &e.backend.header;
    e.backend.header.pNext = &e.api.header;
    e.api.header.pNext     = pin ? &e.pin.header : nullptr;

    const ffxReturnCode_t r = g.create(&e.ctx, &e.up.header, nullptr);
    if (r != FFX_API_RETURN_OK || !e.ctx) {
        e.ctx = nullptr;
        set_why("creating FSR for eye %d at %ux%u -> %ux%u failed (%u)", eye & 1, w, h, ow, oh,
                (unsigned)r);
        VRLOG("FSR: %s", g.why);
        return false;
    }
    e.version = version;
    e.w = w; e.h = h; e.ow = ow; e.oh = oh;
    e.hdr = hdr;
    ffxQueryGetProviderVersion pv{};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (g.query(&e.ctx, &pv.header) == FFX_API_RETURN_OK && pv.versionName)
        strncpy_s(e.running, pv.versionName, _TRUNCATE);
    else
        strncpy_s(e.running, pin == g.id4 ? g.name4 : g.name3, _TRUNCATE);
    if (created) *created = true;
    VRLOG("FSR: %s created for eye %d, %ux%u -> %ux%u (%s, reversed depth)%s", e.running,
          eye & 1, w, h, ow, oh, hdr ? "HDR, auto exposure" : "the finished frame, sRGB",
          (version >= 4 && !g.id4) ? " -- FSR 4 is not offered on this GPU, FSR 3 instead" : "");
    return true;
}

const char* version_name(int eye) { return g.eye[eye & 1].running; }

bool evaluate(ID3D11DeviceContext* ctx, const Eval& ev)
{
    Eye& e = g.eye[ev.eye & 1];
    if (!g.ok || !e.ctx || !ctx) return false;
    if (!ensure_shared(g.colour, ev.colour, false, "colour") ||
        !ensure_shared(g.depth, ev.depth, false, "depth") ||
        !ensure_shared(g.motion, ev.motion, false, "motion vector") ||
        !ensure_shared(g.output, ev.output, true, "output"))
        return false;

    // In: the inputs into the shared textures, then the fence, so the D3D12
    // queue starts on them only once they are there. Flushed, or the signal
    // would sit in D3D11's buffer while the D3D12 queue waits for it.
    ctx->CopyResource(g.colour.tex.Get(), ev.colour);
    ctx->CopyResource(g.depth.tex.Get(), ev.depth);
    ctx->CopyResource(g.motion.tex.Get(), ev.motion);
    const uint64_t ready = ++g.fenceValue;
    g.ctx4->Signal(g.fence11.Get(), ready);
    ctx->Flush();
    g.queue->Wait(g.fence12.Get(), ready);

    // A free allocator: three in turn, each free once the work it recorded has
    // finished -- long since, at one upscale a Present. One still busy after a
    // second is a GPU in trouble; this frame is not upscaled rather than its
    // allocator reset under the GPU's feet. (The queue's wait above is harmless
    // left alone: the D3D11 signal it waits for is already on its way.)
    const int a = g.allocNext;
    if (g.fence12->GetCompletedValue() < g.allocDone[a] &&
        SUCCEEDED(g.fence12->SetEventOnCompletion(g.allocDone[a], g.fenceEvent)))
        WaitForSingleObject(g.fenceEvent, 1000);
    if (g.fence12->GetCompletedValue() < g.allocDone[a]) {
        if (g.failures++ < 5) VRLOG("FSR: the D3D12 queue is a second behind -- frame skipped");
        set_why("the D3D12 queue fell behind");
        return false;
    }
    g.alloc[a]->Reset();
    g.list->Reset(g.alloc[a].Get(), nullptr);

    // Shared textures hand over in COMMON both ways.
    D3D12_RESOURCE_BARRIER in[4] = {
        transition(g.colour.res.Get(), D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(g.depth.res.Get(), D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(g.motion.res.Get(), D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(g.output.res.Get(), D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    g.list->ResourceBarrier(4, in);

    ffxDispatchDescUpscale d{};
    d.header.type   = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList   = g.list.Get();
    d.color         = ffxApiGetResourceDX12(g.colour.res.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.depth         = ffxApiGetResourceDX12(g.depth.res.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.motionVectors = ffxApiGetResourceDX12(g.motion.res.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.output        = ffxApiGetResourceDX12(g.output.res.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset      = {ev.jitterX, ev.jitterY};
    d.motionVectorScale = {1.0f, 1.0f};       // render pixels already
    d.renderSize        = {ev.w, ev.h};
    d.upscaleSize       = {ev.ow, ev.oh};
    d.enableSharpening  = false;              // the mod's own CAS comes after
    d.sharpness         = 0.0f;
    d.frameTimeDelta    = ev.frameMs > 0.0f ? ev.frameMs : 11.1f;
    d.preExposure       = 1.0f;
    d.reset             = ev.reset;
    d.cameraNear        = ev.nearZ;
    d.cameraFar         = ev.farZ;
    d.cameraFovAngleVertical  = ev.vfovRad;
    d.viewSpaceToMetersFactor = 1.0f;         // the game's units are metres
    d.flags = ev.hdr ? 0u : (uint32_t)FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
    const ffxReturnCode_t r = g.dispatch(&e.ctx, &d.header);

    D3D12_RESOURCE_BARRIER out[4];
    for (int i = 0; i < 4; ++i) {
        out[i] = in[i];
        out[i].Transition.StateBefore = in[i].Transition.StateAfter;
        out[i].Transition.StateAfter  = in[i].Transition.StateBefore;
    }
    g.list->ResourceBarrier(4, out);
    const bool closed = SUCCEEDED(g.list->Close());
    if (r == FFX_API_RETURN_OK && closed) {
        ID3D12CommandList* lists[1] = { g.list.Get() };
        g.queue->ExecuteCommandLists(1, lists);
    }
    // Out: signalled whatever happened, so the wait below always clears.
    const uint64_t done = ++g.fenceValue;
    g.queue->Signal(g.fence12.Get(), done);
    g.allocDone[a] = done;
    g.allocNext = (a + 1) % kFrames;
    g.ctx4->Wait(g.fence11.Get(), done);
    if (r != FFX_API_RETURN_OK || !closed) {
        if (g.failures++ < 5)
            VRLOG("FSR: dispatch for eye %d FAILED (%u)", ev.eye & 1, (unsigned)r);
        set_why("upscaling failed (%u)", (unsigned)r);
        return false;
    }
    ctx->CopyResource(ev.output, g.output.tex.Get());
    return true;
}

void release()
{
    if (!g.eye[0].ctx && !g.eye[1].ctx && !g.colour.tex && !g.output.tex) return;
    idle();
    destroy_eye(g.eye[0]);
    destroy_eye(g.eye[1]);
    g.colour = Shared{}; g.depth = Shared{}; g.motion = Shared{}; g.output = Shared{};
    VRLOG("FSR: released (not in use)");
}

void shutdown()
{
    if (!g.dev12) return;
    release();
    idle();
    drop_device();
    VRLOG("FSR: shut down before exit");
}

#endif // SRVR_HAVE_FSR

} // namespace fsr
