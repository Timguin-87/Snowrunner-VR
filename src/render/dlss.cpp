#include "render/dlss.hpp"
#include "render/d3d11_state.hpp"
#include "render/mat4d.hpp"
#include "render/sharpen.hpp"
#include "hooks/cbuffer_hook.h"
#include "hooks/resolution_hook.h"
#include "hooks/shader_cull.h"
#include "xr/xr_mirror.h"
#include "common/log.h"

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#if SRVR_HAVE_DLSS
#include <nvsdk_ngx_helpers.h>
#endif

using Microsoft::WRL::ComPtr;

namespace dlss {
namespace {

std::atomic<int>   g_mode{kOff};
std::atomic<bool>  g_flipY{false};
std::atomic<int>   g_preset{kPresetDefault};
std::atomic<float> g_sharpness{0.30f};   // config is authoritative
// Set by the panel, acted on by the render thread: the features are recreated
// (the preset is a creation-time hint) at the next resolve.
std::atomic<bool>  g_recreate{false};
// Set by the panel, acted on by the render thread at the next resolve: the
// histories restart, and an upscaler that failed gets another try.
std::atomic<bool>  g_modeChanged{false};

// The settings panel reads this; the render thread writes it. Copied out under
// the lock so the panel never sees half a line.
SRWLOCK g_statusLock = SRWLOCK_INIT;
char    g_status[192] = "off";

void set_status(const char* fmt, ...)
{
    char buf[sizeof(g_status)];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    AcquireSRWLockExclusive(&g_statusLock);
    std::memcpy(g_status, buf, sizeof(buf));
    ReleaseSRWLockExclusive(&g_statusLock);
}

#if SRVR_HAVE_DLSS

// THE MOTION VECTORS DLSS WANTS: from each pixel back to where it was in THIS
// EYE's last render. On a flat screen that is simply the previous frame; in VR
// the frames alternate eyes, so every "previous" the game knows -- its velocity
// buffer, its previous camera -- is the OTHER eye's render, and the vectors are
// rebuilt here from three sources (docs/dlss_investigation.md section 7):
//
//   W   the pixel's world position now: its depth through this render's own
//       (jittered) camera.
//   Wy  for a MOVING object, where it was at the game's previous frame Y: the
//       velocity buffer's vector and stored previous depth, through the game's
//       previous camera (g_tmViewProjPrev, which carries this frame's jitter).
//       The static world (velocity (1,1)) did not move: Wy = W.
//   Wz  where it was at this eye's last render Z:
//         mode 0  Y is that render (same eye -- Synched Sequential's first
//                 render of a pair), or shows the same moment: Wz = Wy.
//         mode 1  Y is one render later than Z (AER; the second render of a
//                 pair): Y's own motion field says how far the point had moved
//                 since Z -- looked up where Wy falls in Y's image, accepted if
//                 Y saw that same surface there.
//         mode 2  no usable Z: vectors 0, and DLSS resets that eye.
//
// The vector is Wz through Z's camera WITHOUT its jitter, minus where the pixel
// sits without THIS render's jitter (the pixel centre less the jitter) -- so it
// is jitter-free, as DLSS expects by default. Each render also leaves its own
// motion field (W - Wy, moving pixels only) and depth for the next one's mode 1.
//
// Everything is relative to this render's camera position (the CPU moves every
// matrix there in double precision), so world coordinates stay small.
const char kMvHlsl[] = R"(
cbuffer Frame : register(b0) {
    row_major float4x4 InvVPj;          // this render: clip -> world, as rasterised
    row_major float4x4 InvVPprevGame;   // the game's previous camera (+ this jitter)
    row_major float4x4 VPz;             // this eye's last render, without its jitter
    row_major float4x4 VPy;             // the previous render, as rasterised
    row_major float4x4 InvVPy;
    float4 CamY;                        // the previous render's camera position
    float2 Size; float2 InvSize;
    float2 JitterPx; uint Mode; uint HaveY;
};
Texture2D<float4>   Velocity  : register(t0);   // the game's, this render
Texture2D<float>    Depth     : register(t1);   // the game's R16 device depth
Texture2D<float4>   MotionY   : register(t2);   // the previous render's motion field
Texture2D<float>    DepthY    : register(t3);   // the previous render's depth
RWTexture2D<float2> OutMV     : register(u0);
RWTexture2D<float>  OutDepth  : register(u1);   // DLSS's depth; the next render's DepthY
RWTexture2D<float4> OutMotion : register(u2);   // the next render's MotionY

float3 unproject(float4x4 inv, float2 px, float d)
{
    const float2 ndc = float2(px.x * InvSize.x * 2.0 - 1.0, 1.0 - px.y * InvSize.y * 2.0);
    const float4 w = mul(inv, float4(ndc, d, 1.0));
    return w.xyz / w.w;
}

float3 to_px(float4x4 m, float3 p)   // pixel x, y and device depth
{
    const float4 c = mul(m, float4(p, 1.0));
    const float2 ndc = c.xy / c.w;
    return float3((ndc.x * 0.5 + 0.5) * Size.x, (0.5 - ndc.y * 0.5) * Size.y, c.z / c.w);
}

[numthreads(8, 8, 1)]
void CS_MotionVectors(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)Size.x || id.y >= (uint)Size.y) return;
    const float2 p = float2(id.xy) + 0.5;
    const float  d = Depth[id.xy];
    OutDepth[id.xy] = d;
    const float3 W = unproject(InvVPj, p, d);
    const float4 v = Velocity[id.xy];
    float3 Wz = W;
    float4 motion = float4(0.0, 0.0, 0.0, 0.0);
    if (!(v.x == 1.0 && v.y == 1.0)) {
        const float2 s   = v.xy * 2.007874 - 1.0;
        const float2 dpx = sign(s) * s * s * 256.0;          // current - previous, px
        const float3 Wy  = unproject(InvVPprevGame, p - dpx, v.z);
        motion = float4(W - Wy, 1.0);
        Wz = Wy;
        if (Mode == 1 && HaveY != 0) {
            const float3 q = to_px(VPy, Wy);
            if (q.x >= 0.0 && q.y >= 0.0 && q.x < Size.x && q.y < Size.y) {
                const int2   qi  = int2(q.xy);
                const float4 m   = MotionY[qi];
                const float3 Wq  = unproject(InvVPy, float2(qi) + 0.5, DepthY[qi]);
                const float  tol = 0.05 * length(Wy - CamY.xyz) + 0.05;
                if (m.w > 0.5 && distance(Wq, Wy) < tol) Wz = Wy - m.xyz;
            }
        }
    }
    OutMotion[id.xy] = motion;
    if (Mode == 2) { OutMV[id.xy] = float2(0.0, 0.0); return; }
    OutMV[id.xy] = to_px(VPz, Wz).xy - (p - JitterPx);
}
)";

struct CbFrame {
    float    invVPj[16];
    float    invVPprevGame[16];
    float    vpZ[16];
    float    vpY[16];
    float    invVPy[16];
    float    camY[4];
    float    size[2], invSize[2];
    float    jitterPx[2];
    uint32_t mode, haveY;
};
static_assert(sizeof(CbFrame) % 16 == 0, "constant buffer size");

// --- matrices: row-major, clip = M * p, as cbuffer_hook snapshots them ---
using mat4d::M4;
using mat4d::load;
using mat4d::store;
using mat4d::mul;
using mat4d::inverse;
using mat4d::relative_to;

// NGX identifies a non-registered application by a project id plus "custom
// engine". Any stable GUID will do; this one is ours.
constexpr const char* kProjectId     = "3f6c2b1e-8a47-4d5e-9b2c-5e1d7a0c4f93";
constexpr const char* kEngineVersion = "1.0";

// One render's record, for the next render's vectors.
struct Rec {
    bool      valid = false;
    int       eye = 0;
    uint32_t  moment = 0;
    bool      synched = false;
    bool      hasPrev = false;      // its own previous render (the game's) was recorded
    uint32_t  prevMoment = 0;       // ... and showed this moment
    M4        vpJ{}, vpU{};         // world -> clip, with and without its jitter
    double    cam[3] = {0, 0, 0};
    UINT      w = 0, h = 0;
    int       slot = 0;             // the depth / motion textures holding its data
    ULONGLONG tick = 0;
};

// UPSCALING: a render waiting for its Present. DLSS runs on the finished frame
// there, with what the resolve recorded here.
struct Pending {
    bool     valid = false;
    uint32_t present = 0;           // the Present it belongs to (State::presentCount)
    int      eye = 0;
    uint32_t vecMode = 0;           // how its vectors were built, 0-3 as at the resolve
    float    jx = 0.0f, jy = 0.0f;  // its jitter, render pixels
    UINT     w = 0, h = 0;          // render size: its vectors, depth and frame
    UINT     outW = 0, outH = 0;    // the headset image
    Rec      x;                     // the render itself, for its eye's history
};

struct State {
    bool tried = false;             // NGX init attempted (once per device)
    bool ngxOk = false;
    ID3D11Device* dev = nullptr;    // the device NGX was initialised on, not owned
    NVSDK_NGX_Parameter* params = nullptr;

    // One DLSS feature -- one history -- per eye. The flat screen uses eye 0.
    // What each was created for; any difference recreates it.
    NVSDK_NGX_Handle* feature[2] = {nullptr, nullptr};
    UINT featW[2] = {0, 0}, featH[2] = {0, 0}, featOutW[2] = {0, 0}, featOutH[2] = {0, 0};
    int  featPreset[2] = {-1, -1};
    int  featPq[2] = {-1, -1};
    bool featHdr[2] = {false, false};

    UINT texW = 0, texH = 0;
    ComPtr<ID3D11Texture2D>           mv;
    ComPtr<ID3D11UnorderedAccessView> mvUav;
    // Two slots, ping-ponged: this render writes one while the previous
    // render's data in the other is read.
    ComPtr<ID3D11Texture2D>           depth[2], motion[2];
    ComPtr<ID3D11UnorderedAccessView> depthUav[2], motionUav[2];
    ComPtr<ID3D11ShaderResourceView>  depthSrv[2], motionSrv[2];
    int nextSlot = 0;

    // DLAA: our UAV-capable copy of the resolve's target, which DLSS writes.
    ComPtr<ID3D11Texture2D> out;
    UINT        outW = 0, outH = 0;
    DXGI_FORMAT outFmt = DXGI_FORMAT_UNKNOWN;

    // UPSCALING, at Present: the finished frame copied out of the backbuffer,
    // and DLSS's image of it at the headset size -- with a view onto it, which
    // the sharpening reads it through.
    ComPtr<ID3D11Texture2D> upIn, upOut;
    ComPtr<ID3D11ShaderResourceView> upOutSrv;
    UINT        upInW = 0, upInH = 0, upOutW = 0, upOutH = 0;
    DXGI_FORMAT upFmt = DXGI_FORMAT_UNKNOWN;

    // DLAA ran for the frame the coming Present shows: its result is in that
    // frame, so that is DLSS's image to sharpen. Cleared by on_present().
    bool dlaaThisFrame = false;

    // The unblended pass-through, for a resolve target the frame cannot simply
    // be copied into.
    ComPtr<ID3D11VertexShader>    ptVs;
    ComPtr<ID3D11PixelShader>     ptPs;
    ComPtr<ID3D11RasterizerState> ptRs;
    bool ptTried = false;

    ComPtr<ID3D11ComputeShader> cs;
    ComPtr<ID3D11Buffer>        cb;

    Rec last;              // the previous render: the game's "previous frame"
    Rec lastOfEye[2];      // each eye's last render DLSS has seen
    uint32_t flatCounter = 0;

    Pending  pending;
    uint32_t presentCount = 0;      // advanced by on_present()
    bool     lastUpscale = false;
    // The upscaler would not run here (its feature or textures failed): DLAA
    // instead, until the mode changes, rather than an unblended frame forever.
    bool     upBroken = false;
    uint32_t upFailRun = 0;         // evaluates failed in a row

    bool      resetAll = true;
    ULONGLONG lastEval = 0;
    uint32_t  frames = 0, framesLogged = 0, failures = 0;
    uint32_t  perEye[2] = {0, 0}, modeCount[4] = {0, 0, 0, 0};
    uint32_t  passes = 0, notShown = 0, eyeDisagree = 0, sharpened = 0;
    float     lastJx = 0.0f, lastJy = 0.0f;
    float     jxMin = 1.0f, jxMax = -1.0f, jyMin = 1.0f, jyMax = -1.0f;
    bool      lastVr = false;
} g;

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

void NVSDK_CONV ngx_log_discard(const char*, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {}

bool ensure_ngx(ID3D11Device* dev)
{
    if (g.tried) return g.ngxOk && g.dev == dev;
    g.tried = true;
    g.dev = dev;

    // nvngx_dlss.dll is searched for beside the game's exe by default; the
    // mod's own folder is listed too, and is where its log lines would go.
    const std::wstring dir = module_dir();
    const wchar_t* paths[1] = { dir.c_str() };
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path   = paths;
    info.PathListInfo.Length = 1;
    // NGX's own logging off, and its log files with it: 190-odd lines per start,
    // normal paths reported as errors included. The "DLSS:" lines say what
    // matters -- every failure NGX returns is logged there with its code.
    //
    // THE CALLBACK IS REQUIRED even though nothing is wanted from it: with
    // DisableOtherLoggingSinks set and no callback, init fails with
    // 0xBAD00005 (InvalidParameter) and there is no DLSS at all. Leaving the
    // flag false instead would let NGX write its own log files.
    info.LoggingInfo.LoggingCallback          = &ngx_log_discard;
    info.LoggingInfo.MinimumLoggingLevel      = NVSDK_NGX_LOGGING_LEVEL_OFF;
    info.LoggingInfo.DisableOtherLoggingSinks = true;

    NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID(
        kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion, dir.c_str(), dev, &info);
    if (NVSDK_NGX_FAILED(r)) {
        VRLOG("DLSS: NGX init failed (0x%08X) -- not an NVIDIA RTX GPU, or no "
              "nvngx_dlss.dll beside the mod", (unsigned)r);
        set_status("not available: NGX init failed (0x%08X)", (unsigned)r);
        return false;
    }
    r = NVSDK_NGX_D3D11_GetCapabilityParameters(&g.params);
    if (NVSDK_NGX_FAILED(r) || !g.params) {
        VRLOG("DLSS: NGX capability query failed (0x%08X)", (unsigned)r);
        set_status("not available: capability query failed (0x%08X)", (unsigned)r);
        NVSDK_NGX_D3D11_Shutdown1(dev);
        return false;
    }
    int available = 0, needsDriver = 0, initResult = 0;
    unsigned minMajor = 0, minMinor = 0;
    NVSDK_NGX_Parameter_GetI(g.params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    NVSDK_NGX_Parameter_GetI(g.params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,
                             &needsDriver);
    NVSDK_NGX_Parameter_GetUI(g.params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor,
                              &minMajor);
    NVSDK_NGX_Parameter_GetUI(g.params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor,
                              &minMinor);
    NVSDK_NGX_Parameter_GetI(g.params, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult,
                             &initResult);
    if (!available) {
        VRLOG("DLSS: not available (needs driver update %d, min driver %u.%u, init result "
              "0x%08X) -- is nvngx_dlss.dll beside the mod?", needsDriver, minMajor, minMinor,
              (unsigned)initResult);
        set_status(needsDriver ? "not available: needs driver %u.%u or newer"
                               : "not available (init result 0x%08X)",
                   needsDriver ? minMajor : (unsigned)initResult, minMinor);
        NVSDK_NGX_D3D11_DestroyParameters(g.params);
        g.params = nullptr;
        NVSDK_NGX_D3D11_Shutdown1(dev);
        return false;
    }
    g.ngxOk = true;
    VRLOG("DLSS: NGX up, DLSS available (runtime searched in %ls)", dir.c_str());
    return true;
}

void release_feature(int eye)
{
    if (g.feature[eye]) {
        NVSDK_NGX_D3D11_ReleaseFeature(g.feature[eye]);
        g.feature[eye] = nullptr;
    }
    g.featW[eye] = g.featH[eye] = g.featOutW[eye] = g.featOutH[eye] = 0;
    g.featPreset[eye] = -1;
    g.featPq[eye] = -1;
    g.featHdr[eye] = false;
}

bool compile_cs(ID3D11Device* dev)
{
    if (g.cs) return true;
    ComPtr<ID3DBlob> code, err;
    HRESULT hr = D3DCompile(kMvHlsl, sizeof(kMvHlsl) - 1, "dlss_mv", nullptr, nullptr,
                            "CS_MotionVectors", "cs_5_0", 0, 0, &code, &err);
    if (FAILED(hr)) {
        VRLOG("DLSS: motion vector shader compile FAILED: %s",
              err ? (const char*)err->GetBufferPointer() : "(no message)");
        return false;
    }
    const bool wasSelf = hooks::set_self_render(true);   // ours: never a cull candidate
    hr = dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                  &g.cs);
    hooks::set_self_render(wasSelf);
    if (FAILED(hr)) return false;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth      = sizeof(CbFrame);
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g.cb));
}

bool uav_store_ok(ID3D11Device* dev, DXGI_FORMAT f)
{
    UINT s = 0;
    if (FAILED(dev->CheckFormatSupport(f, &s)) ||
        !(s & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW))
        return false;
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 s2{};
    s2.InFormat = f;
    return SUCCEEDED(dev->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &s2, sizeof(s2))) &&
           (s2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE);
}

constexpr UINT kSrvUav = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

bool make_tex(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT f, UINT bind,
              ComPtr<ID3D11Texture2D>& t, ComPtr<ID3D11UnorderedAccessView>* uav,
              ComPtr<ID3D11ShaderResourceView>* srv)
{
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = f;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &t))) return false;
    if (uav && FAILED(dev->CreateUnorderedAccessView(t.Get(), nullptr, uav->GetAddressOf())))
        return false;
    return !srv || SUCCEEDED(dev->CreateShaderResourceView(t.Get(), nullptr, srv->GetAddressOf()));
}

void release_upscale_textures()
{
    g.upIn.Reset(); g.upOut.Reset(); g.upOutSrv.Reset();
    g.upInW = g.upInH = g.upOutW = g.upOutH = 0;
    g.upFmt = DXGI_FORMAT_UNKNOWN;
}

void release_textures()
{
    g.mv.Reset(); g.mvUav.Reset();
    for (int s = 0; s < 2; ++s) {
        g.depth[s].Reset(); g.motion[s].Reset();
        g.depthUav[s].Reset(); g.motionUav[s].Reset();
        g.depthSrv[s].Reset(); g.motionSrv[s].Reset();
    }
    g.texW = g.texH = 0;
    g.out.Reset();
    g.outW = g.outH = 0;
    g.outFmt = DXGI_FORMAT_UNKNOWN;
    release_upscale_textures();
    g.last = Rec{};
    g.lastOfEye[0] = g.lastOfEye[1] = Rec{};
    g.pending.valid = false;
}

// The vectors, DLSS's depth and each render's motion field, at render size.
bool ensure_textures(ID3D11Device* dev, UINT w, UINT h)
{
    if (g.mv && g.texW == w && g.texH == h) return true;
    release_textures();
    bool ok = make_tex(dev, w, h, DXGI_FORMAT_R16G16_FLOAT, kSrvUav, g.mv, &g.mvUav, nullptr);
    for (int s = 0; ok && s < 2; ++s)
        ok = make_tex(dev, w, h, DXGI_FORMAT_R32_FLOAT, kSrvUav, g.depth[s], &g.depthUav[s],
                      &g.depthSrv[s]) &&
             make_tex(dev, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, kSrvUav, g.motion[s],
                      &g.motionUav[s], &g.motionSrv[s]);
    if (!ok) {
        VRLOG("DLSS: could not create its %ux%u textures", w, h);
        release_textures();
        return false;
    }
    g.texW = w; g.texH = h;
    return true;
}

// DLAA's output: the resolve's own format, which DLSS has to be able to write.
bool ensure_dlaa_target(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT f)
{
    if (g.out && g.outW == w && g.outH == h && g.outFmt == f) return true;
    g.out.Reset();
    g.outW = g.outH = 0;
    g.outFmt = DXGI_FORMAT_UNKNOWN;
    if (!uav_store_ok(dev, f)) {
        static DXGI_FORMAT s_logged = DXGI_FORMAT_UNKNOWN;
        if (s_logged != f) {
            s_logged = f;
            VRLOG("DLSS: the TAA target's format %d cannot be written by DLSS here -- not "
                  "replacing the TAA", (int)f);
        }
        set_status("not available: TAA target format %d not UAV-writable", (int)f);
        return false;
    }
    if (!make_tex(dev, w, h, f, kSrvUav, g.out, nullptr, nullptr)) {
        VRLOG("DLSS: could not create its %ux%u output", w, h);
        g.out.Reset();
        return false;
    }
    g.outW = w; g.outH = h; g.outFmt = f;
    return true;
}

// The finished frame's format without any _SRGB. DLSS reads and writes the
// gamma-encoded values themselves -- what it expects of an LDR image -- and
// the eye blit then decodes its output exactly as it decodes the backbuffer.
DXGI_FORMAT unorm_of(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
    default:                              return f;
    }
}

// UPSCALING's two images: the frame (render size, read by DLSS) and DLSS's
// output (headset size, a UAV). The frame copy has the backbuffer's own format
// so it can be copied straight out of it; the output too when that format can
// be a UAV, and 8-bit RGBA otherwise -- the eye blit converts either way.
bool ensure_upscale_textures(ID3D11Device* dev, DXGI_FORMAT bbFmt, UINT w, UINT h,
                             UINT ow, UINT oh)
{
    const DXGI_FORMAT f = unorm_of(bbFmt);
    if (g.upIn && g.upOut && g.upFmt == f && g.upInW == w && g.upInH == h &&
        g.upOutW == ow && g.upOutH == oh)
        return true;
    release_upscale_textures();
    const DXGI_FORMAT of = uav_store_ok(dev, f) ? f : DXGI_FORMAT_R8G8B8A8_UNORM;
    if (!make_tex(dev, w, h, f, D3D11_BIND_SHADER_RESOURCE, g.upIn, nullptr, nullptr) ||
        !make_tex(dev, ow, oh, of, kSrvUav, g.upOut, nullptr, &g.upOutSrv)) {
        VRLOG("DLSS: could not create the %ux%u -> %ux%u upscaling textures (format %d)",
              w, h, ow, oh, (int)f);
        set_status("could not create the upscaling textures");
        release_upscale_textures();
        return false;
    }
    g.upFmt = f;
    g.upInW = w; g.upInH = h; g.upOutW = ow; g.upOutH = oh;
    VRLOG("DLSS: upscaling textures %ux%u -> %ux%u (frame format %d, output %d)",
          w, h, ow, oh, (int)f, (int)of);
    return true;
}

NVSDK_NGX_PerfQuality_Value perf_quality(int m)
{
    switch (m) {
    case kQuality:          return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    case kBalanced:         return NVSDK_NGX_PerfQuality_Value_Balanced;
    case kPerformance:      return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    case kUltraPerformance: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    default:                return NVSDK_NGX_PerfQuality_Value_DLAA;
    }
}

const char* perf_quality_name(NVSDK_NGX_PerfQuality_Value pq)
{
    switch (pq) {
    case NVSDK_NGX_PerfQuality_Value_MaxQuality:       return "Quality";
    case NVSDK_NGX_PerfQuality_Value_Balanced:         return "Balanced";
    case NVSDK_NGX_PerfQuality_Value_MaxPerf:          return "Performance";
    case NVSDK_NGX_PerfQuality_Value_UltraPerformance: return "Ultra Performance";
    case NVSDK_NGX_PerfQuality_Value_DLAA:             return "DLAA";
    default:                                           return "?";
    }
}

// One eye's DLSS feature, as asked for: render size in, output size out, the
// quality mode, and whether the colour is HDR. Anything different -- or a new
// preset -- recreates it, which empties its history.
bool ensure_feature(ID3D11DeviceContext* ctx, int eye, UINT w, UINT h, UINT ow, UINT oh,
                    NVSDK_NGX_PerfQuality_Value pq, bool hdr, bool* created = nullptr)
{
    if (created) *created = false;
    if (g_recreate.exchange(false)) { release_feature(0); release_feature(1); }
    const int preset = g_preset.load();
    if (g.feature[eye] && g.featW[eye] == w && g.featH[eye] == h && g.featOutW[eye] == ow &&
        g.featOutH[eye] == oh && g.featPq[eye] == (int)pq && g.featHdr[eye] == hdr &&
        g.featPreset[eye] == preset)
        return true;
    release_feature(eye);
    // The model, as a creation-time hint per quality mode -- set for every mode,
    // so whichever one this feature is created for picks it up. 0 = DLSS decides.
    for (const char* hint : { NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                              NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                              NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                              NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                              NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                              NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality })
        NVSDK_NGX_Parameter_SetUI(g.params, hint, (unsigned)preset);

    // What DLSS itself would render this output at, logged once per output and
    // mode: the game's size is set by then (render_size_for() chose it), so this
    // is for reading against it rather than for choosing.
    if (pq != NVSDK_NGX_PerfQuality_Value_DLAA) {
        static UINT s_ow = 0, s_oh = 0;
        static int  s_pq = -1;
        if (s_ow != ow || s_oh != oh || s_pq != (int)pq) {
            s_ow = ow; s_oh = oh; s_pq = (int)pq;
            unsigned optW = 0, optH = 0, maxW = 0, maxH = 0, minW = 0, minH = 0;
            float sharp = 0.0f;
            if (NVSDK_NGX_SUCCEED(NGX_DLSS_GET_OPTIMAL_SETTINGS(g.params, ow, oh, pq, &optW,
                                                               &optH, &maxW, &maxH, &minW,
                                                               &minH, &sharp)))
                VRLOG("DLSS: for %ux%u %s it would render %ux%u (accepts %ux%u to %ux%u); "
                      "the game renders %ux%u", ow, oh, perf_quality_name(pq), optW, optH,
                      minW, minH, maxW, maxH, w, h);
        }
    }

    NVSDK_NGX_DLSS_Create_Params cp{};
    cp.Feature.InWidth            = w;
    cp.Feature.InHeight           = h;
    cp.Feature.InTargetWidth      = ow;
    cp.Feature.InTargetHeight     = oh;
    cp.Feature.InPerfQualityValue = pq;
    // HDR (DLAA, at the resolve): the colour is linear R11G11B10 before
    // tonemapping, and the game's own exposure is applied later, in its
    // tonemapper, so DLSS works it out itself. LDR (upscaling, at Present): the
    // finished, tonemapped frame. Either way the vectors are at render size
    // (MVLowRes) and the depth is reversed-Z (cleared to 0).
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                              NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    if (hdr)
        cp.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
                                   NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    const NVSDK_NGX_Result r = NGX_D3D11_CREATE_DLSS_EXT(ctx, &g.feature[eye], g.params, &cp);
    if (NVSDK_NGX_FAILED(r) || !g.feature[eye]) {
        g.feature[eye] = nullptr;
        VRLOG("DLSS: creating %s for eye %d, %ux%u -> %ux%u, FAILED (0x%08X)",
              perf_quality_name(pq), eye, w, h, ow, oh, (unsigned)r);
        set_status("failed to create DLSS %s at %ux%u -> %ux%u (0x%08X)", perf_quality_name(pq),
                   w, h, ow, oh, (unsigned)r);
        return false;
    }
    g.featW[eye] = w; g.featH[eye] = h; g.featOutW[eye] = ow; g.featOutH[eye] = oh;
    g.featPreset[eye] = preset;
    g.featPq[eye] = (int)pq;
    g.featHdr[eye] = hdr;
    g.lastOfEye[eye] = Rec{};   // its history starts here
    if (created) *created = true;
    VRLOG("DLSS: %s created for eye %d, %ux%u -> %ux%u, preset %s (%s, reversed depth)",
          perf_quality_name(pq), eye, w, h, ow, oh, preset_key(preset),
          hdr ? "HDR, auto exposure" : "LDR");
    return true;
}

// THE FRAME WITHOUT ITS TAA BLEND: the resolve's input straight into its
// output. DLSS wants the raw jittered image, and when it upscales it gets it at
// Present, after the post chain -- which reads the resolve's output, so the
// frame has to arrive there unblended. A plain copy when the two textures are
// alike, one full-screen draw otherwise.
const char kPassHlsl[] = R"(
Texture2D<float4> Src : register(t0);
float4 VS_Full(uint id : SV_VertexID) : SV_Position
{
    const float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float4 PS_Copy(float4 pos : SV_Position) : SV_Target
{
    return Src.Load(int3(pos.xy, 0));
}
)";

bool ensure_pass_through(ID3D11Device* dev)
{
    if (g.ptVs && g.ptPs && g.ptRs) return true;
    if (g.ptTried) return false;
    g.ptTried = true;
    ComPtr<ID3DBlob> vsCode, psCode, err;
    if (FAILED(D3DCompile(kPassHlsl, sizeof(kPassHlsl) - 1, "dlss_pass", nullptr, nullptr,
                          "VS_Full", "vs_5_0", 0, 0, &vsCode, &err)) ||
        FAILED(D3DCompile(kPassHlsl, sizeof(kPassHlsl) - 1, "dlss_pass", nullptr, nullptr,
                          "PS_Copy", "ps_5_0", 0, 0, &psCode, &err))) {
        VRLOG("DLSS: pass-through shader compile FAILED: %s",
              err ? (const char*)err->GetBufferPointer() : "(no message)");
        return false;
    }
    const bool wasSelf = hooks::set_self_render(true);   // ours: never a cull candidate
    const bool ok =
        SUCCEEDED(dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(),
                                          nullptr, &g.ptVs)) &&
        SUCCEEDED(dev->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(),
                                         nullptr, &g.ptPs));
    hooks::set_self_render(wasSelf);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (!ok || FAILED(dev->CreateRasterizerState(&rd, &g.ptRs))) {
        g.ptVs.Reset(); g.ptPs.Reset(); g.ptRs.Reset();
        VRLOG("DLSS: pass-through shaders could not be created");
        return false;
    }
    return true;
}

// Inside the caller's state backup: everything set here is put back after.
bool pass_through(ID3D11DeviceContext* ctx, ID3D11Resource* src, ID3D11ShaderResourceView* srcSrv,
                  ID3D11Resource* dst, ID3D11RenderTargetView* dstRtv,
                  const D3D11_TEXTURE2D_DESC& sd, const D3D11_TEXTURE2D_DESC& dd)
{
    // Unbound first: the target must not be bound as an output while copied
    // into.
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    if (sd.Format == dd.Format && sd.Width == dd.Width && sd.Height == dd.Height &&
        sd.MipLevels == dd.MipLevels && sd.ArraySize == dd.ArraySize &&
        sd.SampleDesc.Count == dd.SampleDesc.Count) {
        ctx->CopyResource(dst, src);
        return true;
    }
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!ensure_pass_through(dev.Get())) return false;
    const D3D11_VIEWPORT vp{0.0f, 0.0f, (float)dd.Width, (float)dd.Height, 0.0f, 1.0f};
    ctx->OMSetRenderTargets(1, &dstRtv, nullptr);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->RSSetState(g.ptRs.Get());
    ctx->RSSetViewports(1, &vp);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g.ptVs.Get(), nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(g.ptPs.Get(), nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &srcSrv);
    ctx->Draw(3, 0);
    return true;
}

// THIS RENDER'S CAMERA, from cbuffer_hook's snapshot (row-major, clip = VP * p):
// the jittered view-projection it was rasterised with, the same without its
// jitter, the game's previous camera, and the camera position.
//
// The jitter is the projection's z column over its w row -- clip.x += jx *
// clip.w -- with the projection = VP * inverse(V). Checked against a capture:
// -0.1250 / +0.0833 px, exactly the game's own PS_REG_REFLECTIONS_JITTER_PARAMS.
struct Camera {
    M4     vpJ, vpU, vpPrevGame;
    double cam[3];
    double jx, jy;   // NDC
};

bool read_camera(Camera& c)
{
    float view[16], vp[16], vpPrev[16];
    if (!hooks::main_camera_matrices(view, vp, vpPrev)) return false;
    const M4 V = load(view);
    c.vpJ = load(vp);
    c.vpPrevGame = load(vpPrev);
    M4 invV;
    if (!inverse(V, invV)) return false;
    const M4 P = mul(c.vpJ, invV);
    const double w32 = P.m[14];
    if (std::fabs(w32) < 1e-9) return false;
    c.jx = P.m[2] / w32;
    c.jy = P.m[6] / w32;
    c.vpU = c.vpJ;
    for (int k = 0; k < 4; ++k) {
        c.vpU.m[k]     -= c.jx * c.vpJ.m[12 + k];
        c.vpU.m[4 + k] -= c.jy * c.vpJ.m[12 + k];
    }
    // camera position: the inverse view's translation
    c.cam[0] = invV.m[3]; c.cam[1] = invV.m[7]; c.cam[2] = invV.m[11];
    return true;
}

void note_jitter(float jx, float jy)
{
    g.lastJx = jx; g.lastJy = jy;
    if (jx < g.jxMin) g.jxMin = jx;
    if (jx > g.jxMax) g.jxMax = jx;
    if (jy < g.jyMin) g.jyMin = jy;
    if (jy > g.jyMax) g.jyMax = jy;
}

#endif // SRVR_HAVE_DLSS

} // namespace

namespace {
struct ModeInfo { const char* key; const char* name; double scale; };
// The render scale per side: NVIDIA's own ratios for each mode.
constexpr ModeInfo kModes[kModeCount] = {
    {"off",              "Off",               1.0},
    {"dlaa",             "DLAA",              1.0},
    {"quality",          "Quality",           2.0 / 3.0},
    {"balanced",         "Balanced",          0.58},
    {"performance",      "Performance",       0.5},
    {"ultraperformance", "Ultra Performance", 1.0 / 3.0},
};
struct PresetInfo { int value; const char* key; };
constexpr PresetInfo kPresets[] = {
    {kPresetDefault, "default"}, {kPresetJ, "J"}, {kPresetK, "K"}, {kPresetL, "L"},
    {kPresetM, "M"},
};
} // namespace

int  mode() { return g_mode.load(); }
bool mode_available(int m) { return m >= 0 && m < kModeCount; }
bool is_upscaling(int m) { return m >= kQuality && m < kModeCount; }
void set_mode(int m)
{
    if (m < 0 || m >= kModeCount) m = kOff;
    if (g_mode.exchange(m) == m) return;
    if (m == kOff)       VRLOG("DLSS -> off");
    else if (m == kDlaa) VRLOG("DLSS -> DLAA (in place of the game's TAA resolve)");
    else                 VRLOG("DLSS -> %s (upscaling: sets the game's render size)",
                               kModes[m].name);
    g_modeChanged.store(true);
    if (m == kOff) set_status("off");
}
void on_resized() { g_modeChanged.store(true); }
const char* mode_name(int m) { return (m >= 0 && m < kModeCount) ? kModes[m].name : "?"; }
const char* mode_key(int m)  { return (m >= 0 && m < kModeCount) ? kModes[m].key : "off"; }
bool mode_from_key(const char* key, int& out)
{
    if (!key) return false;
    for (int m = 0; m < kModeCount; ++m)
        if (_stricmp(key, kModes[m].key) == 0) { out = m; return true; }
    return false;
}

void render_size_for(int m, int outW, int outH, int& rw, int& rh)
{
    rw = outW;
    rh = outH;
    if (!is_upscaling(m) || outW <= 0 || outH <= 0) return;
    const double s = kModes[m].scale;
    auto side = [s](int n) {
        const int v = (int)std::lround((double)n * s / 4.0) * 4;
        return v < 64 ? 64 : v;
    };
    rw = side(outW);
    rh = side(outH);
}

int  preset() { return g_preset.load(); }
void set_preset(int p)
{
    bool known = false;
    for (const auto& i : kPresets) known |= (i.value == p);
    if (!known) p = kPresetDefault;
    if (g_preset.exchange(p) == p) return;
    g_recreate.store(true);
    VRLOG("DLSS: preset -> %s (recreating DLSS)", preset_key(p));
}
const char* preset_key(int p)
{
    for (const auto& i : kPresets) if (i.value == p) return i.key;
    return "default";
}
bool preset_from_key(const char* key, int& out)
{
    if (!key) return false;
    for (const auto& i : kPresets)
        if (_stricmp(key, i.key) == 0) { out = i.value; return true; }
    return false;
}

bool compiled_in()
{
#if SRVR_HAVE_DLSS
    return true;
#else
    return false;
#endif
}

const char* status()
{
    static thread_local char copy[sizeof(g_status)];
    AcquireSRWLockShared(&g_statusLock);
    std::memcpy(copy, g_status, sizeof(copy));
    ReleaseSRWLockShared(&g_statusLock);
    return copy;
}

float sharpness() { return g_sharpness.load(); }
void set_sharpness(float s) { g_sharpness.store((std::max)(0.0f, (std::min)(1.0f, s))); }

bool jitter_flip_y() { return g_flipY.load(); }
void set_jitter_flip_y(bool on)
{
    if (g_flipY.exchange(on) != on) {
        VRLOG("DLSS: vertical jitter sign %s", on ? "FLIPPED (debug)" : "as derived");
#if SRVR_HAVE_DLSS
        g.resetAll = true;
#endif
    }
}

bool replace_taa_resolve(ID3D11DeviceContext* ctx, unsigned vertexCount)
{
#if !SRVR_HAVE_DLSS
    (void)ctx; (void)vertexCount;
    return false;
#else
    const int selected = g_mode.load();
    if (selected == kOff || !ctx || vertexCount != 3) return false;

    // What the game bound for its resolve: t0 colour, t2 depth, t3 velocity.
    ID3D11ShaderResourceView* srvRaw[4] = {};
    ctx->PSGetShaderResources(0, 4, srvRaw);
    ComPtr<ID3D11ShaderResourceView> srv[4];
    for (int i = 0; i < 4; ++i) srv[i].Attach(srvRaw[i]);
    ID3D11RenderTargetView* rtvRaw = nullptr;
    ctx->OMGetRenderTargets(1, &rtvRaw, nullptr);
    ComPtr<ID3D11RenderTargetView> rtv;
    rtv.Attach(rtvRaw);
    if (!srv[0] || !srv[2] || !srv[3] || !rtv) {
        set_status("TAA inputs not as expected -- game TAA left running");
        return false;
    }
    ComPtr<ID3D11Resource> colourRes, outRes;
    srv[0]->GetResource(&colourRes);
    rtv->GetResource(&outRes);
    ComPtr<ID3D11Texture2D> colourTex, outTex;
    if (FAILED(colourRes.As(&colourTex)) || FAILED(outRes.As(&outTex))) return false;
    D3D11_TEXTURE2D_DESC cd{}, od{};
    colourTex->GetDesc(&cd);
    outTex->GetDesc(&od);
    if (cd.Width != od.Width || cd.Height != od.Height) {
        set_status("TAA output %ux%u differs from its input %ux%u -- left alone",
                   od.Width, od.Height, cd.Width, cd.Height);
        return false;
    }
    const UINT w = cd.Width, h = cd.Height;

    // Which eye, and which moment. The flat screen is one eye, every frame a
    // moment of its own.
    const bool vr = xr::xr_session_running();
    int      eye = 0;
    uint32_t moment = 0;
    bool     synched = false;
    if (vr) {
        xr::RenderInfo ri;
        if (!xr::render_in_progress(ri)) {
            set_status("VR: no record of the build being rendered -- game TAA this frame");
            return false;
        }
        eye = ri.eye & 1;
        moment = ri.moment;
        synched = ri.synched;
    } else {
        moment = ++g.flatCounter;
    }

    if (g_modeChanged.exchange(false)) {
        g.resetAll = true;
        g.upBroken = false;
        g.upFailRun = 0;
    }

    // DLAA OR UPSCALING. Upscaling needs the game SIZED for it: the window at the
    // render size, the headset images at the full size (xr::upscale_canvas).
    // Without that, every mode is DLAA at the size the game renders -- for the
    // frame a mode is waiting to be applied, or until a restart if this session
    // cannot apply it (hooks::live_resize_available()).
    // And the frame DLSS is handed at Present has to be on the same grid as the
    // vectors built here, which holds while the engine's targets are the size
    // of its backbuffer (render_size_for() keeps them so).
    uint32_t outW = 0, outH = 0, frameW = 0, frameH = 0;
    const bool upCanvas = vr && xr::upscale_canvas(outW, outH);
    bool upscale = upCanvas && is_upscaling(selected) && !g.upBroken;
    bool gridOff = false;
    if (upscale && (!xr::render_canvas_wh(frameW, frameH) || frameW != w || frameH != h)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            VRLOG("DLSS: the game renders %ux%u for a %ux%u frame -- its vectors and the "
                  "frame are on different grids, so no upscaling (DLAA instead)",
                  w, h, frameW, frameH);
        }
        upscale = false;
        gridOff = true;
    }
    if (upscale != g.lastUpscale) { g.lastUpscale = upscale; g.resetAll = true; }
    if (vr != g.lastVr) { g.lastVr = vr; g.resetAll = true; }

    Camera cam;
    if (!read_camera(cam)) {
        set_status("no camera matrices readable -- game TAA left running");
        return false;
    }
    const double jxPx = cam.jx * 0.5 * (double)w;
    const double jyPx = -cam.jy * 0.5 * (double)h;   // NDC y up, pixels down
    if (std::fabs(jxPx) > 1.0 || std::fabs(jyPx) > 1.0) {
        set_status("camera jitter out of range -- game TAA left running");
        return false;
    }

    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!ensure_ngx(dev.Get())) return false;
    if (!compile_cs(dev.Get())) { set_status("motion vector shader failed"); return false; }
    if (!ensure_textures(dev.Get(), w, h)) return false;
    if (!upscale && !ensure_dlaa_target(dev.Get(), w, h, od.Format)) return false;

    const ULONGLONG now = GetTickCount64();
    if (g.resetAll) {
        g.last = Rec{};
        g.lastOfEye[0] = g.lastOfEye[1] = Rec{};
        g.pending.valid = false;
        g.resetAll = false;
    }
    D3D11StateBackup saved;
    saved.capture(ctx);
    const bool wasSelf = hooks::set_self_render(true);

    // DLAA: first, so that a feature created just now -- its history empty --
    // is seen below as an eye with no last render. Upscaling creates its
    // feature at Present, and resets it there when it is new.
    bool ok = upscale ||
              ensure_feature(ctx, eye, w, h, w, h, NVSDK_NGX_PerfQuality_Value_DLAA, true);

    // Y: the game's previous frame. Z: this eye's last render DLSS has seen --
    // when upscaling, the last one evaluated at a Present. A record is only
    // good for the frame right after it -- a gap means some render was not seen.
    const Rec& Y = g.last;
    const Rec& Z = g.lastOfEye[eye];
    auto fresh = [&](const Rec& r, ULONGLONG maxAge) {
        return r.valid && r.w == w && r.h == h && r.synched == synched && now - r.tick <= maxAge;
    };
    const bool yOk = fresh(Y, 100);
    const bool zOk = fresh(Z, 200);
    uint32_t mode;
    if (!zOk)                                              mode = 2;   // no history: reset
    else if (yOk && Y.eye == eye)                          mode = 0;   // same eye
    else if (yOk && Y.moment == Z.moment)                  mode = 0;   // same moment
    else if (yOk && Y.hasPrev && Y.prevMoment == Z.moment) mode = 1;   // one render apart
    else                                                   mode = 3;   // approximate
    ++g.modeCount[mode];

    const int slot = g.nextSlot;
    if (ok) {
        // 1. Motion vectors, depth, and this render's own motion field.
        CbFrame c{};
        const double* O = cam.cam;
        M4 inv;
        inverse(relative_to(cam.vpJ, O), inv);            store(c.invVPj, inv);
        inverse(relative_to(cam.vpPrevGame, O), inv);     store(c.invVPprevGame, inv);
        store(c.vpZ, relative_to(zOk ? Z.vpU : cam.vpU, O));
        const M4 vpY = relative_to(yOk ? Y.vpJ : cam.vpJ, O);
        store(c.vpY, vpY);
        inverse(vpY, inv);                                store(c.invVPy, inv);
        if (yOk) for (int i = 0; i < 3; ++i) c.camY[i] = (float)(Y.cam[i] - O[i]);
        c.size[0] = (float)w; c.size[1] = (float)h;
        c.invSize[0] = 1.0f / (float)w; c.invSize[1] = 1.0f / (float)h;
        c.jitterPx[0] = (float)jxPx; c.jitterPx[1] = (float)jyPx;
        c.mode = (mode == 3) ? 0u : mode;
        c.haveY = (yOk && mode == 1) ? 1u : 0u;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(g.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            std::memcpy(m.pData, &c, sizeof(c));
            ctx->Unmap(g.cb.Get(), 0);
        }
        ctx->CSSetShader(g.cs.Get(), nullptr, 0);
        ID3D11Buffer* cbs = g.cb.Get();
        ctx->CSSetConstantBuffers(0, 1, &cbs);
        const int ys = slot ^ 1;
        ID3D11ShaderResourceView* in[4] = { srv[3].Get(), srv[2].Get(),
                                            g.motionSrv[ys].Get(), g.depthSrv[ys].Get() };
        ctx->CSSetShaderResources(0, 4, in);
        ID3D11UnorderedAccessView* outs[3] = { g.mvUav.Get(), g.depthUav[slot].Get(),
                                               g.motionUav[slot].Get() };
        ctx->CSSetUnorderedAccessViews(0, 3, outs, nullptr);
        ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        ID3D11UnorderedAccessView* noUav[3] = {};
        ctx->CSSetUnorderedAccessViews(0, 3, noUav, nullptr);
        ID3D11ShaderResourceView* noIn[4] = {};
        ctx->CSSetShaderResources(0, 4, noIn);

        if (upscale) {
            // 2. The frame goes on UNBLENDED; DLSS takes it at Present.
            ok = pass_through(ctx, colourRes.Get(), srv[0].Get(), outRes.Get(), rtv.Get(),
                              cd, od);
            if (!ok) set_status("could not pass the frame through the TAA -- game TAA "
                                "left running");
        } else {
            // 2. DLSS for this eye, into our own UAV-capable copy of the TAA target.
            NVSDK_NGX_D3D11_DLSS_Eval_Params ep{};
            ep.Feature.pInColor  = colourRes.Get();
            ep.Feature.pInOutput = g.out.Get();
            ep.pInDepth          = g.depth[slot].Get();
            ep.pInMotionVectors  = g.mv.Get();
            ep.InJitterOffsetX   = (float)jxPx;
            ep.InJitterOffsetY   = (float)(g_flipY.load() ? -jyPx : jyPx);
            ep.InRenderSubrectDimensions.Width  = w;
            ep.InRenderSubrectDimensions.Height = h;
            ep.InReset           = (mode == 2) ? 1 : 0;
            ep.InMVScaleX        = 1.0f;
            ep.InMVScaleY        = 1.0f;
            const NVSDK_NGX_Result r =
                NGX_D3D11_EVALUATE_DLSS_EXT(ctx, g.feature[eye], g.params, &ep);
            ok = NVSDK_NGX_SUCCEED(r);
            if (ok) {
                g.lastEval = now;
                ++g.frames;
                ++g.perEye[eye];
                g.dlaaThisFrame = true;
                // 3. Into the target the resolve would have written -- unbound
                // first, restore() binds it back; the game's copy of it into the
                // post-processing buffer comes next.
                ctx->OMSetRenderTargets(0, nullptr, nullptr);
                ctx->CopyResource(outRes.Get(), g.out.Get());
            } else if (g.failures++ < 5) {
                VRLOG("DLSS: evaluate FAILED (0x%08X) -- the game's TAA draws instead",
                      (unsigned)r);
                set_status("evaluate failed (0x%08X)", (unsigned)r);
            }
        }
    }

    hooks::set_self_render(wasSelf);
    saved.restore(ctx);

    // This render, for the next one -- recorded even if DLSS failed, so the
    // chain of "previous frames" stays true to what the game rendered.
    Rec x;
    x.valid = true;
    x.eye = eye;
    x.moment = moment;
    x.synched = synched;
    x.hasPrev = yOk;
    x.prevMoment = Y.moment;
    x.vpJ = cam.vpJ;
    x.vpU = cam.vpU;
    for (int i = 0; i < 3; ++i) x.cam[i] = cam.cam[i];
    x.w = w; x.h = h;
    x.slot = slot;
    x.tick = now;
    g.last = x;
    // DLAA: DLSS has seen this render now. Upscaling: once it is evaluated at
    // its Present -- a frame that is never shown never enters an eye's history.
    if (!upscale) g.lastOfEye[eye] = x;
    g.nextSlot = slot ^ 1;
    note_jitter((float)jxPx, (float)jyPx);

    if (upscale && ok) {
        Pending& p = g.pending;
        p.valid   = true;
        p.present = g.presentCount;
        p.eye     = eye;
        p.vecMode = mode;
        p.jx = (float)jxPx;
        p.jy = (float)jyPx;
        p.w = w; p.h = h;
        p.outW = outW; p.outH = outH;
        p.x = x;
        ++g.passes;
    }

    if (!ok) return false;
    if (!upscale) {
        if (is_upscaling(selected) && g.upBroken)
            set_status("upscaling failed (see the log) -- DLAA at %ux%u instead", w, h);
        else if (is_upscaling(selected) && !vr)
            set_status("DLAA at %ux%u -- upscaling is for VR only", w, h);
        else if (is_upscaling(selected) && gridOff)
            set_status("DLAA at %ux%u per eye -- the frame is not the render size, see "
                       "the log", w, h);
        else if (is_upscaling(selected))
            set_status("DLAA at %ux%u per eye -- %s %s", w, h, kModes[selected].name,
                       hooks::live_resize_available() ? "is being applied" : "takes a restart");
        else if (upCanvas)
            set_status("DLAA at %ux%u per eye, stretched to %ux%u -- full size %s", w, h, outW,
                       outH, hooks::live_resize_available() ? "in a moment" : "after a restart");
        else
            set_status(vr ? "DLAA running at %ux%u per eye" : "DLAA running at %ux%u", w, h);
    }
    return true;
#endif
}

#if SRVR_HAVE_DLSS
// UPSCALING, at Present: DLSS's image of the frame in `backbuffer` at the
// headset size, or null -- not upscaling, or nothing to upscale this frame.
static ID3D11Texture2D* upscale(ID3D11DeviceContext* ctx, ID3D11Texture2D* backbuffer, int eye)
{
    Pending& p = g.pending;
    if (!p.valid) return nullptr;
    p.valid = false;
    // Made for an earlier Present: that frame was never routed to an eye.
    if (p.present != g.presentCount) { ++g.notShown; return nullptr; }
    if (!ctx || !backbuffer || g_mode.load() == kOff) return nullptr;
    D3D11_TEXTURE2D_DESC bd{};
    backbuffer->GetDesc(&bd);
    if (bd.Width != p.w || bd.Height != p.h || bd.SampleDesc.Count != 1) {
        set_status("the frame is %ux%u, its vectors %ux%u -- not upscaled", bd.Width,
                   bd.Height, p.w, p.h);
        return nullptr;
    }
    if ((eye & 1) != p.eye) ++g.eyeDisagree;

    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!ensure_upscale_textures(dev.Get(), bd.Format, p.w, p.h, p.outW, p.outH)) {
        g.upBroken = true;
        return nullptr;
    }

    // The quality mode the game is sized for: what matches the render size, until
    // a mode picked since has been applied.
    const int sizedMode = hooks::upscale_mode_applied();
    const int pqMode = is_upscaling(sizedMode) ? sizedMode : g_mode.load();

    D3D11StateBackup saved;
    saved.capture(ctx);
    // Inside Present's own bracket, so it goes back to that rather than off.
    const bool wasSelf = hooks::set_self_render(true);
    bool created = false;
    bool ok = ensure_feature(ctx, p.eye, p.w, p.h, p.outW, p.outH, perf_quality(pqMode), false,
                             &created);
    if (ok) {
        ctx->CopyResource(g.upIn.Get(), backbuffer);
        NVSDK_NGX_D3D11_DLSS_Eval_Params ep{};
        ep.Feature.pInColor  = g.upIn.Get();
        ep.Feature.pInOutput = g.upOut.Get();
        ep.pInDepth          = g.depth[p.x.slot].Get();
        ep.pInMotionVectors  = g.mv.Get();
        ep.InJitterOffsetX   = p.jx;
        ep.InJitterOffsetY   = g_flipY.load() ? -p.jy : p.jy;
        ep.InRenderSubrectDimensions.Width  = p.w;
        ep.InRenderSubrectDimensions.Height = p.h;
        // A feature created just now has no history to keep.
        ep.InReset           = (p.vecMode == 2 || created) ? 1 : 0;
        ep.InMVScaleX        = 1.0f;
        ep.InMVScaleY        = 1.0f;
        const NVSDK_NGX_Result r =
            NGX_D3D11_EVALUATE_DLSS_EXT(ctx, g.feature[p.eye], g.params, &ep);
        ok = NVSDK_NGX_SUCCEED(r);
        if (!ok) {
            if (g.failures++ < 5)
                VRLOG("DLSS: upscaling evaluate FAILED (0x%08X)", (unsigned)r);
            set_status("upscaling evaluate failed (0x%08X)", (unsigned)r);
            // Not for ever: the frames it should have upscaled are unblended.
            if (++g.upFailRun >= 10) g.upBroken = true;
        }
    } else {
        g.upBroken = true;   // no feature: the game's frames stop being passed through
    }
    hooks::set_self_render(wasSelf);
    saved.restore(ctx);
    if (!ok) return nullptr;

    g.upFailRun = 0;
    g.lastOfEye[p.eye] = p.x;   // its history holds this render now
    g.lastEval = GetTickCount64();
    ++g.frames;
    ++g.perEye[p.eye];
    const int selected = g_mode.load();
    if (is_upscaling(selected) && selected != pqMode)
        set_status("%s: %ux%u -> %ux%u per eye -- %s %s", kModes[pqMode].name, p.w, p.h, p.outW,
                   p.outH, kModes[selected].name,
                   hooks::live_resize_available() ? "is being applied" : "takes a restart");
    else
        set_status("%s: %ux%u -> %ux%u per eye", kModes[pqMode].name, p.w, p.h, p.outW,
                   p.outH);
    return g.upOut.Get();
}
#endif // SRVR_HAVE_DLSS

ID3D11Texture2D* image_for_present(ID3D11DeviceContext* ctx, ID3D11Texture2D* backbuffer,
                                   int eye)
{
#if !SRVR_HAVE_DLSS
    (void)ctx; (void)backbuffer; (void)eye;
    return nullptr;
#else
    ID3D11Texture2D* img = upscale(ctx, backbuffer, eye);
    // Ours, so the sharpening can read it through a view of its own; the
    // backbuffer it has to copy.
    ID3D11ShaderResourceView* view = img ? g.upOutSrv.Get() : nullptr;
    // DLAA: DLSS's image IS the frame by now, through the game's own
    // tonemapping -- the space the sharpening is meant for.
    if (!img && g.dlaaThisFrame && backbuffer && g_mode.load() != kOff) img = backbuffer;
    if (!img) return nullptr;
    const float amount = g_sharpness.load();
    if (!(amount > 0.0f)) {
        sharpen::release();
        return img == backbuffer ? nullptr : img;
    }
    if (ID3D11Texture2D* sharp = sharpen::apply(ctx, img, view, amount)) {
        ++g.sharpened;
        return sharp;
    }
    return img == backbuffer ? nullptr : img;
#endif
}

void on_present()
{
#if SRVR_HAVE_DLSS
    // A render's upscale belongs to the Present it was made before; from here
    // on it is a frame that was not shown (see upscale()). Its DLAA too.
    ++g.presentCount;
    g.dlaaThisFrame = false;
    if (g_mode.load() == kOff && (g.feature[0] || g.feature[1] || g.upOut)) {
        release_feature(0);
        release_feature(1);
        release_textures();
        sharpen::release();
        VRLOG("DLSS: released (off)");
    }
    static ULONGLONG t0 = 0;
    const ULONGLONG now = GetTickCount64();
    if (!t0) t0 = now;
    // Nothing to take over: the resolve has not drawn for a while. Either the
    // game's TAA is off -- the usual reason -- or no 3D scene is up (menus,
    // loading). Said in the panel rather than leaving it looking idle.
    static ULONGLONG onSince = 0;
    if (g_mode.load() == kOff) onSince = 0;
    else if (!onSince) onSince = now;
    if (onSince && now - onSince > 2000 && (!g.lastEval || now - g.lastEval > 2000) &&
        (!g.tried || g.ngxOk))
        set_status("waiting for the game's TAA -- switch it on in the game's graphics "
                   "settings");
    if (now - t0 >= 10000) {
        const bool any = g.frames != g.framesLogged || g.passes;
        const int sharpPct = (int)std::lround(g_sharpness.load() * 100.0f);
        if (any && g.passes)
            VRLOG("DLSS upscaling: %u renders in 10 s, %u upscaled (eye 0 %u, eye 1 %u), %u "
                  "never shown, %u routed to the other eye | vectors: same eye or moment %u, "
                  "looked up one render back %u, approximated %u, reset %u | jitter "
                  "%+.3f..%+.3f / %+.3f..%+.3f px | sharpened %u at %d%%",
                  g.passes, g.frames - g.framesLogged, g.perEye[0], g.perEye[1], g.notShown,
                  g.eyeDisagree, g.modeCount[0], g.modeCount[1], g.modeCount[3],
                  g.modeCount[2], g.jxMin, g.jxMax, g.jyMin, g.jyMax, g.sharpened, sharpPct);
        else if (any)
            VRLOG("DLSS: %u resolves in 10 s (eye 0 %u, eye 1 %u) | vectors: same eye or "
                  "moment %u, looked up one render back %u, approximated %u, reset %u | "
                  "jitter %+.3f..%+.3f / %+.3f..%+.3f px | sharpened %u at %d%%",
                  g.frames - g.framesLogged, g.perEye[0], g.perEye[1], g.modeCount[0],
                  g.modeCount[1], g.modeCount[3], g.modeCount[2], g.jxMin, g.jxMax,
                  g.jyMin, g.jyMax, g.sharpened, sharpPct);
        g.framesLogged = g.frames;
        g.perEye[0] = g.perEye[1] = 0;
        g.modeCount[0] = g.modeCount[1] = g.modeCount[2] = g.modeCount[3] = 0;
        g.passes = g.notShown = g.eyeDisagree = g.sharpened = 0;
        g.jxMin = g.jyMin = 1.0f;
        g.jxMax = g.jyMax = -1.0f;
        t0 = now;
    }
#endif
}

void shutdown()
{
#if SRVR_HAVE_DLSS
    if (!g.ngxOk || !g.dev) return;
    release_feature(0);
    release_feature(1);
    if (g.params) { NVSDK_NGX_D3D11_DestroyParameters(g.params); g.params = nullptr; }
    NVSDK_NGX_D3D11_Shutdown1(g.dev);
    g.ngxOk = false;
    VRLOG("DLSS: NGX shut down before exit");
#endif
}

} // namespace dlss
