#pragma once
#include <d3d11.h>
#include <cstdint>

// A screen-space coverage mask of everything RIGID WITH THE CAMERA -- truck
// body, cab interior, trailers and their loads.
//
// WHY. The 6-DoF stale-eye reprojection moves every pixel by the camera's own
// travel between the retained frame and now. That is exactly right for the
// world and exactly wrong for anything bolted to the camera: the cab does not
// move relative to a camera mounted inside it, yet it sits at the near depth
// where that travel displaces most. One present at ~106/s is ~9.4 ms; at
// 30 km/h that is ~0.08 m of forward travel, which expands the image radially
// by dz/z -- 11% at 0.7 m against 0.4% for the world beyond the glass. That is
// what shreds the A-pillar, mirror, dashboard and hands.
//
// A depth band used to answer this, and was removed 2026-08-20: it was a guess
// that only held inside a cab -- and only because nothing in the world is
// within a metre or two there -- so it had to be gated to cockpit view and did
// nothing in the exterior cameras. This is the renderer's own answer instead:
// these draws ARE the truck. It is exact, it does not care how far away the
// geometry is, and it works in exterior views where no depth threshold could
// (the hood at 2 m and the ground at 2 m are indistinguishable by depth).
//
// HOW THE MASK IS BUILT. Identical trick to mirror_mask.hpp, and for the same
// reason: there is no way to ask D3D "which pixels did that draw cover". The
// truck draws are issued TWICE, once into this mask with a trivial pixel shader
// that writes 1, using the same depth-stencil view as the real draw so anything
// occluding the truck occludes its mark too.
//
// COST, and why it is gated. The truck is hundreds of draws per frame where a
// mirror is a handful, so the duplicate is not free -- the pixel shader is
// trivial and the target is R8, but the vertex work is doubled for that
// geometry. The whole path is inert unless the 6-DoF reprojection is actually
// running, which is the only thing that reads it.
namespace rigidmask {

// True once the mask exists and something wants it -- the whole path is inert
// otherwise, including the duplicated draws.
bool active();

// Bound state saved across a marking draw. The duplicate has to leave the
// context exactly as it found it: this is recording INTO the game's own command
// list, and anything left behind applies to every draw after it.
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

// Clears the mask for the next frame. Call from Present AFTER the reprojection
// has retained it.
void clear(ID3D11DeviceContext* ctx);

// The mask, same dimensions as the render canvas so a mask texel is a scene
// pixel. Null when inactive.
ID3D11ShaderResourceView* srv();

// The underlying texture, for CopyResource into a per-eye retained copy.
//
// THE REPROJECTION NEEDS THE MASK THAT BELONGS TO THE RETAINED FRAME, not the
// one being built now: it reprojects the other eye's previous render, and this
// mask is cleared every Present. Sampling the live one would mask the wrong
// eye's truck by one frame -- see retain_eye_for_warp().
ID3D11Texture2D* texture();

// Called from the swapchain Present hook: creates or resizes the mask, and
// releases it when nothing wants it.
void update(ID3D11Device* dev, uint32_t canvasW, uint32_t canvasH, bool wanted);

void shutdown();

} // namespace rigidmask
