#pragma once
#include <d3d11.h>
#include <cstdint>

// A screen-space coverage mask of the truck's mirrors, for DIBR shift.
//
// WHY. A mirror shows a reflection, but the depth buffer at those pixels
// describes the mirror SURFACE (or whatever is behind it) -- never the
// reflected scene. DIBR shift's DIBR reprojects colour by depth, so it drags the
// reflection sideways by a disparity that has nothing to do with what the
// reflection actually shows. Same class of problem as the windscreen, but
// deleting the mirrors is not an option: unlike glass refraction, the mirrors
// are load-bearing for driving.
//
// The fix is to reproject nothing there. Marked pixels are skipped by the
// scatter, which leaves them as disocclusions, and the resolve fills a marked
// hole from the stale eye -- the previous real render of the eye being
// synthesized, rotation-warped to the current pose. That image contains a real
// mirror rendered by the game, one frame old. One frame of staleness on a
// mirror is a far better error than a reflection sliding across its own frame.
//
// HOW THE MASK IS BUILT. There is no way to ask D3D "which pixels did that
// draw cover". So the mirror draws are issued TWICE: once into this mask with
// a trivial pixel shader that writes 1, and once normally. The duplicate is
// baked at record time by the draw detours in ui_hook.cpp, using the same
// depth-stencil view as the real draw so anything occluding a mirror also
// occludes its mark.
namespace mirrormask {

// True once the mask exists and DIBR shift is on -- the whole
// path is inert otherwise, including the duplicated draws.
bool active();

// Bound state saved across a marking draw. The duplicate has to leave the
// context exactly as it found it: this is recording INTO the game's own
// command list, and anything left behind applies to every draw after it.
struct Saved {
    ID3D11RenderTargetView* rtv[8] = {};
    UINT                    rtvCount = 0;
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11PixelShader*      ps = nullptr;
    ID3D11BlendState*       blend = nullptr;
    float                   blendFactor[4] = {};
    UINT                    sampleMask = 0;
    bool                    valid = false;
};

// Binds the mask as the sole render target, keeping the caller's depth view,
// and swaps in the write-1 pixel shader plus an opaque blend state. False if
// the mask is unavailable, in which case nothing was changed and end_mark()
// must not be called.
bool begin_mark(ID3D11DeviceContext* ctx, Saved& saved);
void end_mark(ID3D11DeviceContext* ctx, Saved& saved);

// Clears the mask for the next frame. Call from Present AFTER DIBR shift has read it.
void clear(ID3D11DeviceContext* ctx);

// The mask for DIBR shift to sample, or null. Same dimensions as the render canvas,
// so a mask texel is a scene pixel.
ID3D11ShaderResourceView* srv();

// Called from the swapchain Present hook: creates or resizes the mask, and
// releases it when DIBR shift is not the active mode.
void update(ID3D11Device* dev, uint32_t canvasW, uint32_t canvasH, bool dibrActive);

void shutdown();

} // namespace mirrormask
