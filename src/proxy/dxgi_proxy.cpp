#include "proxy/dxgi_proxy.h"
#include "hooks/swapchain_hook.h"
#include "hooks/camera_hook.h"
#include "hooks/viewbuild_hook.h"
#include "common/log.h"

#include <dxgi1_6.h>

// ---------------------------------------------------------------------------
// Real dxgi.dll resolution
// ---------------------------------------------------------------------------

extern "C" FARPROC g_dxgiForwards[16] = {};

// Order must match dxgi_stubs.asm slot indices.
static const char* const kForwardNames[16] = {
    "ApplyCompatResolutionQuirking",  // 0
    "CompatString",                   // 1
    "CompatValue",                    // 2
    "DXGID3D10CreateDevice",          // 3  (called by d3d11.dll internally)
    "DXGID3D10CreateLayeredDevice",   // 4
    "DXGID3D10ETWRundown",            // 5
    "DXGID3D10GetLayeredDeviceSize",  // 6
    "DXGID3D10RegisterLayers",        // 7  (called by d3d11.dll internally)
    "DXGIDisableVBlankVirtualization",// 8
    "DXGIDumpJournal",                // 9
    "DXGIReportAdapterConfiguration", // 10
    "PIXBeginCapture",                // 11
    "PIXEndCapture",                  // 12
    "PIXGetCaptureState",             // 13
    "SetAppCompatStringPointer",      // 14
    "UpdateHMDEmulationStatus",       // 15
};

static HMODULE g_realDxgi = nullptr;

using PFN_CreateDXGIFactory  = HRESULT(WINAPI*)(REFIID, void**);
using PFN_CreateDXGIFactory1 = HRESULT(WINAPI*)(REFIID, void**);
using PFN_CreateDXGIFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
using PFN_DXGIGetDebugInterface1 = HRESULT(WINAPI*)(UINT, REFIID, void**);
using PFN_DXGIDeclareAdapterRemovalSupport = HRESULT(WINAPI*)();

static PFN_CreateDXGIFactory  real_CreateDXGIFactory  = nullptr;
static PFN_CreateDXGIFactory1 real_CreateDXGIFactory1 = nullptr;
static PFN_CreateDXGIFactory2 real_CreateDXGIFactory2 = nullptr;
static PFN_DXGIGetDebugInterface1 real_DXGIGetDebugInterface1 = nullptr;
static PFN_DXGIDeclareAdapterRemovalSupport real_DXGIDeclareAdapterRemovalSupport = nullptr;

bool dxgi_proxy_init()
{
    wchar_t path[MAX_PATH];
    UINT len = GetSystemDirectoryW(path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH - 10)
        return false;
    lstrcatW(path, L"\\dxgi.dll");

    g_realDxgi = LoadLibraryW(path);
    if (!g_realDxgi) {
        VRLOG("FATAL: could not load %ls (err=%lu)", path, GetLastError());
        return false;
    }

    for (int i = 0; i < 16; ++i) {
        g_dxgiForwards[i] = GetProcAddress(g_realDxgi, kForwardNames[i]);
        if (!g_dxgiForwards[i])
            VRLOG("note: real dxgi has no export '%s' (ok unless the game calls it)", kForwardNames[i]);
    }

    real_CreateDXGIFactory  = reinterpret_cast<PFN_CreateDXGIFactory>(GetProcAddress(g_realDxgi, "CreateDXGIFactory"));
    real_CreateDXGIFactory1 = reinterpret_cast<PFN_CreateDXGIFactory1>(GetProcAddress(g_realDxgi, "CreateDXGIFactory1"));
    real_CreateDXGIFactory2 = reinterpret_cast<PFN_CreateDXGIFactory2>(GetProcAddress(g_realDxgi, "CreateDXGIFactory2"));
    real_DXGIGetDebugInterface1 = reinterpret_cast<PFN_DXGIGetDebugInterface1>(GetProcAddress(g_realDxgi, "DXGIGetDebugInterface1"));
    real_DXGIDeclareAdapterRemovalSupport = reinterpret_cast<PFN_DXGIDeclareAdapterRemovalSupport>(GetProcAddress(g_realDxgi, "DXGIDeclareAdapterRemovalSupport"));

    VRLOG("real dxgi.dll loaded at %p", static_cast<void*>(g_realDxgi));
    return real_CreateDXGIFactory && real_CreateDXGIFactory1 && real_CreateDXGIFactory2;
}

// ---------------------------------------------------------------------------
// Implemented exports.
//
// Each factory the game creates is passed to hooks::hook_factory(), which
// vtable-hooks CreateSwapChain* so the first swapchain's Present drives the
// OpenXR mirror. If hooking fails anywhere, the game still gets its real,
// unmodified factory back — passthrough is always the floor.
// ---------------------------------------------------------------------------

static void install_hooks_on(void** ppFactory)
{
    if (!ppFactory || !*ppFactory)
        return;
    hooks::init();  // idempotent; first call initializes MinHook off the loader lock
    hooks::hook_factory(reinterpret_cast<IDXGIFactory*>(*ppFactory));

    static bool camHooked = false;
    if (!camHooked) {
        camHooked = true;  // attempt once; failure logs and leaves mono head-locked
        hooks::install_camera_hook();
        // CPU-side: rotate the main render camera (call site exe+0xA17C2B) about
        // a tunable camera-relative pivot (the head). Stereo translation to be
        // added here too for AER.
        hooks::install_viewbuild_hook();
    }
}

extern "C" HRESULT WINAPI Proxy_CreateDXGIFactory(REFIID riid, void** ppFactory)
{
    HRESULT hr = real_CreateDXGIFactory(riid, ppFactory);
    if (SUCCEEDED(hr)) install_hooks_on(ppFactory);
    return hr;
}

extern "C" HRESULT WINAPI Proxy_CreateDXGIFactory1(REFIID riid, void** ppFactory)
{
    HRESULT hr = real_CreateDXGIFactory1(riid, ppFactory);
    if (SUCCEEDED(hr)) install_hooks_on(ppFactory);
    return hr;
}

extern "C" HRESULT WINAPI Proxy_CreateDXGIFactory2(UINT flags, REFIID riid, void** ppFactory)
{
    HRESULT hr = real_CreateDXGIFactory2(flags, riid, ppFactory);
    if (SUCCEEDED(hr)) install_hooks_on(ppFactory);
    return hr;
}

extern "C" HRESULT WINAPI Proxy_DXGIGetDebugInterface1(UINT flags, REFIID riid, void** ppDebug)
{
    if (!real_DXGIGetDebugInterface1)
        return E_NOINTERFACE;
    return real_DXGIGetDebugInterface1(flags, riid, ppDebug);
}

extern "C" HRESULT WINAPI Proxy_DXGIDeclareAdapterRemovalSupport()
{
    if (!real_DXGIDeclareAdapterRemovalSupport)
        return E_NOTIMPL;
    return real_DXGIDeclareAdapterRemovalSupport();
}
