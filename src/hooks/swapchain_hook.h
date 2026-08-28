#pragma once
#include <dxgi.h>
#include <cstdint>

// SWAPCHAIN INTERCEPTION -- where every frame of this mod starts.
//
// dxgi_proxy passes every factory it hands back to hook_factory(), which
// vtable-hooks CreateSwapChain / CreateSwapChainForHwnd. The first swapchain
// the game creates gets its Present (and ResizeBuffers) vtable-hooked exactly
// once; from then on every Present drives the OpenXR mirror before the real
// Present runs.
namespace hooks {

// Initializes MinHook. Safe to call more than once. Returns false if MinHook
// could not initialize (caller then stays in pure passthrough).
bool init();

// Hooks CreateSwapChain* on a factory just returned to the game.
void hook_factory(IDXGIFactory* factory);

} // namespace hooks
