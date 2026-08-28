#pragma once
#include <d3d11.h>
#include <cstdint>

// The frame's UI, captured on its own so it can be shown as a QUAD LAYER in
// the headset instead of being rendered into the eye images.
//
// WHY IT LEFT THE RENDER. Screen-space UI drawn into a stereo pair is stuck at
// infinity, keystones with any per-eye cant, and -- under DIBR shift -- sits over scene
// depth, so reprojecting the finished backbuffer drags it sideways and leaves a
// hole behind it. All three problems are the same problem: a 2D overlay has no
// place in an image that describes a 3D frustum. A composition layer is where
// the runtime expects flat content to go, and once it lives there the
// compositor owns its placement: correct in both eyes by construction, at a
// distance and size we choose, optionally pinned in the world.
//
// So the seven known UI pixel shaders (shader_cull.h's kCullUi) are REDIRECTED
// here -- drawn into this transparent target INSTEAD of the game's backbuffer,
// not in addition to it -- and xr_mirror.cpp submits the result as its own
// quad. What used to be an extra draw is now the only draw.
//
// Two consequences of that move, both deliberate:
//   * DIBR shift no longer needs a pre-HUD copy of the backbuffer at all: with the UI
//     gone from the render, the finished frame IS the pre-HUD frame.
//   * The viewport shrink that used to resize the HUD in place is gone too --
//     the quad's angular size does that job now, and does it without touching
//     any of the game's render state.
//
// A pixel difference between the finished and pre-HUD frames was tried and
// rejected before this: semi-transparent UI does not survive a subtraction, and
// the result looked wrong wherever the HUD was blended rather than opaque.
//
// PREMULTIPLIED. The draws are re-issued with SrcBlendAlpha=ONE so that alpha
// accumulates correctly against a transparent target rather than being squared
// the way ordinary SrcAlpha blending would. The result is a premultiplied
// layer, which is exactly what an XrCompositionLayerQuad expects when
// XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT is not set.
namespace uilayer {

// True once the target exists and the feature is on. Everything -- the
// redirect included -- is inert otherwise, which is the fallback that matters:
// with no layer to draw into, the UI simply renders into the frame the way it
// always did.
bool active();

// Whether anything was actually captured since the last clear(). Lets the
// submission skip the quad entirely on a frame that drew no UI, rather than
// paying for a blit and a layer to show nothing.
bool drew_this_frame();

// Size of the capture target (the game's canvas). False when inactive.
bool size(uint32_t& w, uint32_t& h);

// Bound state saved across a capture draw. The redirect records into the
// game's own command list, so it has to leave the context exactly as it found
// it -- the same rule mirror_mask.hpp follows, and for the same reason.
struct Saved {
    ID3D11RenderTargetView* rtv[8] = {};
    UINT                    rtvCount = 0;
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11BlendState*       blend = nullptr;
    float                   blendFactor[4] = {};
    UINT                    sampleMask = 0;
    bool                    valid = false;
};

bool begin_capture(ID3D11DeviceContext* ctx, Saved& saved);
void end_capture(ID3D11DeviceContext* ctx, Saved& saved);

// Clears the layer for the next frame. Call from Present AFTER the quad has
// been built from it.
void clear(ID3D11DeviceContext* ctx);

// The captured UI, or null. Two views of one texture: srv() reads the values
// the game's UI shaders wrote (gamma-encoded, exactly as they would have landed
// in the backbuffer), srv_srgb() decodes them to linear on read. Which one a
// copy wants is decided by the format it is copying INTO -- see
// build_ui_quad_layer(), and the note at the texture's creation.
ID3D11ShaderResourceView* srv();
ID3D11ShaderResourceView* srv_srgb();

// `wanted` is the whole gate: xr_mirror.cpp decides whether a quad can be
// shown at all (session running, HUD not hidden, swapchain healthy), and the
// capture follows that answer exactly. Releasing the target when it goes false
// is what puts the UI back into the render.
void update(ID3D11Device* dev, uint32_t canvasW, uint32_t canvasH, bool wanted);
void shutdown();

} // namespace uilayer
