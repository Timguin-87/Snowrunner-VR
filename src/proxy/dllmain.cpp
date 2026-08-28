#include "proxy/dxgi_proxy.h"
#include "hooks/swapchain_hook.h"
#include "hooks/resolution_hook.h"
#include "hooks/input_block.h"
#include "common/log.h"
#include "common/config.h"

// Loading the real dxgi.dll inside DllMain runs under loader lock, which is
// normally discouraged — but the forward table must be live before d3d11.dll
// binds to our DXGID3D10* exports, and every shipping dxgi proxy does the
// same. hooks::init()/vrcfg::init() are here for the identical reason: config
// should be loaded and applied as early as possible, before this DLL's own
// normal, lazy per-feature install points (dxgi_proxy.cpp's
// install_hooks_on(), triggered from the first CreateDXGIFactory* call) would
// otherwise fire. Both calls are cheap and idempotent (hooks::init() via
// std::call_once; vrcfg::init() re-validates an already-good file as a
// no-op), and every setter vrcfg::init() applies is a plain atomic store with
// no D3D/window dependency, so running them this early is safe. Nothing else
// (COM, XR, game-side hooks) is initialized here.
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        vrlog::init(instance);
        VRLOG("snowrunner_vr proxy attached");
        if (!dxgi_proxy_init()) {
            // Passthrough is impossible without the real DLL; failing the
            // load is the only honest option and Windows shows a clear error.
            return FALSE;
        }
        hooks::init();
        vrcfg::init();
        // Strictly after vrcfg::init() (which supplies the spoof size) and
        // strictly before the exe's entry point runs, which is where the
        // window -- and the resolution decision this is meant to unblock --
        // gets made. DllMain is the only place that satisfies both.
        hooks::install_resolution_hooks();
        // Same reasoning: the game creates its DirectInput devices during
        // startup, so the DirectInput8Create hook has to be in place before
        // the exe's entry point runs. Only needs hooks::init() (MinHook) --
        // nothing about it depends on the menu existing yet.
        hooks::install_input_block();
    }
    return TRUE;
}
