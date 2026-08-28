#include "hooks/swapchain_hook.h"
#include "hooks/cbuffer_hook.h"
#include "hooks/viewbuild_hook.h"
#include "hooks/camera_fov.h"
#include "hooks/depth_probe.h"
#include "hooks/ui_hook.h"
#include "hooks/shader_cull.h"
#include "render/mirror_mask.hpp"
#include "render/rigid_mask.hpp"
#include "render/ui_layer.hpp"
#include "render/smudge_layer.hpp"
#include "render/winch_layer.hpp"
#include "hooks/menu_hook.h"
#include "hooks/input_block.h"
#include "hooks/resolution_hook.h"
#include "xr/xr_mirror.h"
#include "common/log.h"

#include <dxgi1_2.h>
#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include <intrin.h>

#include <MinHook.h>

namespace hooks {
namespace {

std::once_flag g_mhInit;
std::atomic<bool> g_mhReady{false};

// One-time guards so we hook each vtable slot only once even though many
// factories / swapchains pass through.
std::atomic<bool> g_factoryHooked{false};
std::atomic<bool> g_presentHooked{false};

// The swapchain most recently seen presenting. Deliberately not AddRef'd: it is
// only ever read from inside Detour_Present, where the game is holding it alive
// by definition.
std::atomic<IDXGISwapChain*> g_lastSwapchain{nullptr};

// --- vtable indices (see docs; IUnknown/IDXGIObject/IDXGIDeviceSubObject) ---
constexpr int kIdxPresent          = 8;   // IDXGISwapChain::Present
constexpr int kIdxResizeBuffers    = 13;  // IDXGISwapChain::ResizeBuffers
constexpr int kIdxResizeTarget     = 14;  // IDXGISwapChain::ResizeTarget
constexpr int kIdxCreateSwapChain  = 10;  // IDXGIFactory::CreateSwapChain
constexpr int kIdxCreateSCForHwnd  = 15;  // IDXGIFactory2::CreateSwapChainForHwnd

void** vtable_of(void* obj) { return *reinterpret_cast<void***>(obj); }

// Formats a raw return address as "exe+0xOFFSET" if it falls inside this
// process's own exe module, else resolves and names whichever module it
// actually belongs to ("<dll>+0xOFFSET"). A hooked vtable slot can just as
// easily be called by another module's own internal code (e.g. DXGI's
// runtime calling ResizeBuffers on itself while servicing ResizeTarget) as
// by the game -- blindly subtracting the exe's base from an address outside
// it silently underflows into a huge, meaningless value instead of erroring,
// which is worse than not logging anything: this makes that case visible and
// named instead. Takes the address as a plain parameter (not captured
// in here via _ReturnAddress()) so it's safe to call from a helper --
// _ReturnAddress() itself must still be read directly at each call site.
void format_caller(void* addr, char* buf, size_t bufSize)
{
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(addr), &owner) || !owner) {
        snprintf(buf, bufSize, "0x%p (unresolved module)", addr);
        return;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(owner);
    const uintptr_t off  = reinterpret_cast<uintptr_t>(addr) - base;
    if (owner == GetModuleHandleW(nullptr)) {
        snprintf(buf, bufSize, "exe+0x%llX", (unsigned long long)off);
        return;
    }
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(owner, path, MAX_PATH);
    wchar_t* name = path;
    for (wchar_t* p = path; *p; ++p) if (*p == L'\\') name = p + 1;
    char name8[MAX_PATH]{};
    WideCharToMultiByte(CP_ACP, 0, name, -1, name8, sizeof(name8), nullptr, nullptr);
    snprintf(buf, bufSize, "%s+0x%llX", name8, (unsigned long long)off);
}

// Reads slot, creates+enables a MinHook trampoline, stores original. No-op if
// the slot was already hooked (tracked by the caller's guard).
template <typename Fn>
bool hook_slot(void* obj, int index, Fn detour, Fn* original)
{
    void* target = vtable_of(obj)[index];
    if (MH_CreateHook(target, reinterpret_cast<void*>(detour),
                      reinterpret_cast<void**>(original)) != MH_OK)
        return false;
    return MH_EnableHook(target) == MH_OK;
}

// --------------------------------------------------------------------------
// Present / ResizeBuffers
// --------------------------------------------------------------------------

using PFN_Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using PFN_ResizeBuffers = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

PFN_Present       real_Present = nullptr;
PFN_ResizeBuffers real_ResizeBuffers = nullptr;

HRESULT STDMETHODCALLTYPE Detour_Present(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    // Reentrancy guard. Anything in the hook chain below -- our own XR submit
    // calling into the runtime's compositor, or another overlay hooking this
    // exact vtable slot on this same swapchain (RTSS/Afterburner, Discord
    // overlay, Steam overlay, GeForce Experience/ShadowPlay are all common)
    // -- could call back into Present before this call returns. Without a
    // guard that's unbounded recursion: a hang or stack overflow. On reentry,
    // skip the whole hook chain and pass straight through to the real
    // Present, rather than running our install/mirror/menu logic again
    // mid-flight.
    thread_local bool s_inPresent = false;
    if (s_inPresent)
        return real_Present(sc, sync, flags);
    s_inPresent = true;

    // Everything below draws through the same hooked context vtables the game
    // does -- the mirror blit, the stale-eye warp, the ImGui menu. Without
    // this bracket their pixel shaders would be registered as cull candidates
    // and could be suppressed by a search step, blacking out the very image
    // the search is being judged from. Cleared again before real_Present.
    hooks::set_self_render(true);

    g_lastSwapchain.store(sc);

    // Keeps looking for the game's runtime-loaded XInput DLL; self-disables
    // once found (see input_block.h).
    hooks::input_block_on_present();

    // Install the CB hook lazily here (device is available, runs without VR).
    hooks::install_cbuffer_hook(sc);

    // Original hud_hook was removed: the HUD's draws AND its constant buffers
    // are baked into replayed command lists (28/frame, zero deferred
    // recording), so no per-Draw/per-CB call interception could reach them --
    // proven by draw census + CB probe. ExecuteCommandList is the one call that
    // DOES fire every frame for replayed work, which is what ui_hook targets.
    hooks::install_ui_hook(sc);
    // After install_ui_hook: this only supplies the decision, ui_hook owns the
    // draw slots that act on it.
    hooks::install_shader_cull(sc);
    hooks::install_depth_probe(sc);       // DIBR shift phase 0 recon (docs/dibr_shift_plan.md)
    hooks::depth_probe_on_present(sc);
    hooks::shader_cull_on_present();
    hooks::viewbuild_on_present();
    hooks::camera_fov_on_present();
    hooks::ui_hook_on_present(sc);
    // Menu update runs BEFORE mirror_on_present() -- it builds this Present's
    // ImGui draw data (NewFrame/Render) without touching any render target
    // yet, so mirror_on_present() can, while it's building the XR frame,
    // submit that same already-built draw data as a headset-visible quad
    // layer alongside the eye projection layer. The actual desktop-backbuffer
    // render (menu_render_to_desktop) still happens AFTER mirror_on_present(),
    // preserving the original ordering guarantee: that function's AER
    // desktop-eye-mirror trick does a raw CopyResource onto the backbuffer on
    // every other Present in every code path, which would stomp (flicker)
    // anything drawn before it, and the VR blit (which reads its source
    // earlier in this same Present) must never see the desktop overlay.
    hooks::install_menu_hook(sc);
    hooks::menu_hook_update();
    xr::mirror_on_present(sc);
    hooks::menu_render_to_desktop(sc);

    // Mirror mask: sized/released here, and cleared AFTER DIBR shift has read it this
    // Present. The game renders the next frame once this returns, so its mirror
    // draws land in a mask that starts empty. See mirror_mask.hpp.
    {
        Microsoft::WRL::ComPtr<ID3D11Device> dev;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> imm;
        uint32_t cw = 0, ch = 0;
        if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) && dev) {
            xr::render_canvas_wh(cw, ch);
            mirrormask::update(dev.Get(), cw, ch, xr::dibr_shift_enabled());
            // Wanted by the 6-DoF stale-eye reprojection WHETHER OR NOT DIBR
            // shift is on: with it, the reprojection fills disocclusion holes;
            // without it, it builds the whole synthesized eye, which is where it
            // matters most. Same condition as depth_capture_wanted()'s second
            // term -- the mask is useless without the depth it is paired with.
            rigidmask::update(dev.Get(), cw, ch,
                              xr::warp_uses_6dof() && xr::warp_enabled());
            // Not gated on DIBR shift: the UI layer feeds a composition layer
            // now, and that applies either way. xr::ui_plane_wanted() is the
            // whole condition -- see ui_layer.hpp.
            uilayer::update(dev.Get(), cw, ch, xr::ui_plane_wanted());
            smudgelayer::update(dev.Get(), cw, ch, xr::dibr_shift_enabled());
            // Same gate, same reason: the winch markers only need taking out
            // of the scene when something is about to depth-reproject it.
            winchlayer::update(dev.Get(), cw, ch, xr::dibr_shift_enabled());
            dev->GetImmediateContext(&imm);
            if (imm) {
                mirrormask::clear(imm.Get());
                // AFTER retain_eye_for_warp() has copied it -- mirror_on_present
                // runs earlier in this same Present.
                rigidmask::clear(imm.Get());
                uilayer::clear(imm.Get());
                smudgelayer::clear(imm.Get());
                // (winchlayer clears itself in winchlayer::latch(), which runs
                // earlier in this same Present -- it has to both promote and
                // clear, and only it knows which buffer is which.)
            }
        }
    }

    hooks::set_self_render(false);
    HRESULT hr = real_Present(sc, sync, flags);

    s_inPresent = false;
    return hr;
}

using PFN_ResizeTarget = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, const DXGI_MODE_DESC*);
PFN_ResizeTarget real_ResizeTarget = nullptr;

// NOTE, so this is not "fixed" again by someone reading the log the way it was
// first misread: a session showing ResizeTarget go 2880x2880 -> 1920x1080 ->
// 1902x977 a few seconds after startup is NOT the game spontaneously
// re-applying its work-area clamp and undoing ForceGameResolution. That is
// what a manual fullscreen toggle looks like from in here, and the small final
// size is the deliberate result of coming back out of it.
//
// Re-asserting the forced size on the game's own ResizeTarget/ResizeBuffers
// calls was tried and reverted for exactly that reason: it would override
// every deliberate resolution change too -- the in-game video settings and the
// fullscreen toggle both route through here -- for no benefit, since the
// creation-time override already holds on its own.

// This, together with Detour_ResizeTarget below, is the runtime (not startup)
// path a live in-game resolution change actually goes through -- Detour_CreateSwapChain
// only ever fires once, at the swapchain's original creation. _ReturnAddress()
// MUST be read directly here (not via a shared helper function -- a call
// through another function would capture that function's own return address
// instead, one frame too shallow) to get the exe-relative address of the
// GAME code that called this, i.e. the exact call site to open in a
// disassembler/CE and trace backward (what decided this size, did it already
// clamp before calling here) or forward (what happens after, does it revert
// on failure).
HRESULT STDMETHODCALLTYPE Detour_ResizeBuffers(
    IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags)
{
    void* retAddr = _ReturnAddress();
    char callerBuf[MAX_PATH + 32];
    format_caller(retAddr, callerBuf, sizeof(callerBuf));
    VRLOG("ResizeBuffers: game requested %ux%u fmt=%d flags=0x%X, called from %s",
          w, h, (int)fmt, flags, callerBuf);

    uint32_t nw = 0, nh = 0;
    if (w && h && xr::desired_render_size_for(w, h, nw, nh)) { w = nw; h = nh; }
    xr::mirror_on_resize();  // drop cached backbuffer views before the resize
    hooks::menu_hook_on_resize();
    HRESULT hr = real_ResizeBuffers(sc, count, w, h, fmt, flags);

    DXGI_SWAP_CHAIN_DESC scd{};
    if (SUCCEEDED(hr) && SUCCEEDED(sc->GetDesc(&scd))) {
        RECT cr{};
        if (scd.OutputWindow) GetClientRect(scd.OutputWindow, &cr);
        VRLOG("ResizeBuffers: result hr=0x%08X actual swapchain=%ux%u window client=%ldx%ld",
              (unsigned)hr, scd.BufferDesc.Width, scd.BufferDesc.Height,
              cr.right - cr.left, cr.bottom - cr.top);
    } else {
        VRLOG("ResizeBuffers: result hr=0x%08X", (unsigned)hr);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Detour_ResizeTarget(IDXGISwapChain* sc, const DXGI_MODE_DESC* mode)
{
    void* retAddr = _ReturnAddress();
    char callerBuf[MAX_PATH + 32];
    format_caller(retAddr, callerBuf, sizeof(callerBuf));
    if (mode)
        VRLOG("ResizeTarget: game requested %ux%u fmt=%d, called from %s",
              mode->Width, mode->Height, (int)mode->Format, callerBuf);
    else
        VRLOG("ResizeTarget: called with null mode, from %s", callerBuf);

    HRESULT hr = real_ResizeTarget(sc, mode);
    VRLOG("ResizeTarget: result hr=0x%08X", (unsigned)hr);
    return hr;
}

// (apply_pending_resize() lived here, draining a queued live resize at the top
// of Present for the settings UI's "Apply resolution now" button. Removed
// 2026-08-24 with the button -- see the note in resolution_hook.cpp for why
// a live resize cannot work, and why succeeding DXGI calls are not evidence
// that it does.)

void hook_present_once(IDXGISwapChain* sc)
{
    bool expected = false;
    if (!g_presentHooked.compare_exchange_strong(expected, true))
        return;

    bool ok = hook_slot(sc, kIdxPresent, &Detour_Present, &real_Present);
    ok = hook_slot(sc, kIdxResizeBuffers, &Detour_ResizeBuffers, &real_ResizeBuffers) && ok;
    ok = hook_slot(sc, kIdxResizeTarget, &Detour_ResizeTarget, &real_ResizeTarget) && ok;
    VRLOG("Present/ResizeBuffers/ResizeTarget hook %s", ok ? "installed" : "FAILED (passthrough)");
    if (!ok)
        g_presentHooked = false;  // allow a later swapchain to retry
}

// --------------------------------------------------------------------------
// Factory CreateSwapChain*
// --------------------------------------------------------------------------

using PFN_CreateSwapChain = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using PFN_CreateSCForHwnd = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);

PFN_CreateSwapChain real_CreateSwapChain = nullptr;
PFN_CreateSCForHwnd real_CreateSCForHwnd = nullptr;

HRESULT STDMETHODCALLTYPE Detour_CreateSwapChain(
    IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out)
{
    // Ground truth for resolution debugging: what the GAME itself asked
    // DXGI for, before anything of ours (including the currently-inert
    // xr::desired_render_size_for() rewrite below) touches it. If this is
    // already small, whatever's clamping the resolution happens inside the
    // game's own logic before this call, not in DXGI/the OS.
    if (desc)
        VRLOG("CreateSwapChain: game requested %ux%u (windowed=%d)",
              desc->BufferDesc.Width, desc->BufferDesc.Height, desc->Windowed);

    // Force the backbuffer bigger again. This alone previously caused a
    // corner-crop (the engine's 3D viewport width/height, now known to live
    // at exe+2AA1808/+2AA180C via CE, stayed at the small config value while
    // the backbuffer grew) -- viewbuild_hook now continuously re-asserts
    // those two fields to match, every frame, the same way it already does
    // for FOV. With both pinned together the crop should disappear.
    uint32_t nw = 0, nh = 0;
    if (desc && xr::desired_render_size_for(desc->BufferDesc.Width, desc->BufferDesc.Height, nw, nh)) {
        VRLOG("CreateSwapChain: forcing render size %ux%u -> %ux%u",
              desc->BufferDesc.Width, desc->BufferDesc.Height, nw, nh);
        desc->BufferDesc.Width  = nw;
        desc->BufferDesc.Height = nh;
    }
    HRESULT hr = real_CreateSwapChain(f, dev, desc, out);
    if (SUCCEEDED(hr) && out && *out) {
        hook_present_once(*out);
        // Install context-vtable hooks (CB/UI/depth probe) HERE, not lazily at
        // first Present -- the HUD's command list is recorded once (measured:
        // zero FinishCommandList calls ever fire once hooks were only
        // installed at first Present), meaning that recording happens earlier
        // than the first Present call, during boot/load. Hooking as early as
        // the swapchain itself exists is the earliest point we CAN attach
        // (the device doesn't exist before this), and is the only way to have
        // a chance of catching that recording live.
        hooks::install_cbuffer_hook(*out);
        hooks::install_ui_hook(*out);
        // Earliest possible point, and it matters more here than for the
        // others: every pixel shader the game creates BEFORE this attaches has
        // no recorded bytecode hash, so it can be culled for a session but
        // never saved to the config file.
        hooks::install_shader_cull(*out);
        hooks::install_depth_probe(*out);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Detour_CreateSCForHwnd(
    IDXGIFactory2* f, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* out, IDXGISwapChain1** ppsc)
{
    // See the matching comment in Detour_CreateSwapChain.
    if (desc)
        VRLOG("CreateSCForHwnd: game requested %ux%u", desc->Width, desc->Height);

    uint32_t nw = 0, nh = 0;
    DXGI_SWAP_CHAIN_DESC1 local;
    const DXGI_SWAP_CHAIN_DESC1* use = desc;
    if (desc && xr::desired_render_size_for(desc->Width, desc->Height, nw, nh)) {
        local = *desc;
        VRLOG("CreateSCForHwnd: forcing render size %ux%u -> %ux%u",
              desc->Width, desc->Height, nw, nh);
        local.Width  = nw;
        local.Height = nh;
        use = &local;
    }
    HRESULT hr = real_CreateSCForHwnd(f, dev, hwnd, use, fs, out, ppsc);
    if (SUCCEEDED(hr) && ppsc && *ppsc) {
        hook_present_once(*ppsc);
        // See the matching comment in Detour_CreateSwapChain.
        hooks::install_cbuffer_hook(*ppsc);
        hooks::install_ui_hook(*ppsc);
        hooks::install_shader_cull(*ppsc);   // see Detour_CreateSwapChain
        hooks::install_depth_probe(*ppsc);
    }
    return hr;
}

} // namespace

bool init()
{
    std::call_once(g_mhInit, [] {
        g_mhReady = (MH_Initialize() == MH_OK);
        VRLOG("MinHook init %s", g_mhReady.load() ? "ok" : "FAILED");
    });
    return g_mhReady;
}

void hook_factory(IDXGIFactory* factory)
{
    if (!factory || !g_mhReady)
        return;

    bool expected = false;
    if (!g_factoryHooked.compare_exchange_strong(expected, true))
        return;

    bool ok = hook_slot(factory, kIdxCreateSwapChain,
                        &Detour_CreateSwapChain, &real_CreateSwapChain);

    // CreateSwapChainForHwnd only exists on IDXGIFactory2. The game creates a
    // Factory1 (per recon log) but the underlying object is usually a
    // Factory2 as well; hook it when present since modern D3D11 uses it.
    IDXGIFactory2* f2 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&f2))) && f2) {
        ok = hook_slot<PFN_CreateSCForHwnd>(
                 f2, kIdxCreateSCForHwnd, &Detour_CreateSCForHwnd, &real_CreateSCForHwnd) && ok;
        f2->Release();
    }
    VRLOG("factory CreateSwapChain hooks %s", ok ? "installed" : "PARTIAL/FAILED");
    if (!ok)
        g_factoryHooked = false;
}

} // namespace hooks
