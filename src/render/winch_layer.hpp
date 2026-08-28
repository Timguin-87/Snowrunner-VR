#pragma once
#include <d3d11.h>
#include <cstdint>

// The winch anchor-point markers, captured out of the scene and composited back
// onto BOTH eyes unshifted.
//
// WHY THEY CANNOT GO THROUGH DIBR. They are UI: screen-space sprites the game
// places from a world position it already projected. The disparity shift has no
// idea of that -- it reads the depth buffer at the marker's pixels, which holds
// whatever geometry is BEHIND the sprite (usually terrain tens of metres away,
// sometimes the truck), and displaces the icon by that. So the marker lands
// somewhere the reprojection chose rather than where it belongs, by an amount
// that jumps whenever what is behind it changes.
//
// WHY NOT THE UI LAYER. The HUD goes to a head-locked composition quad
// (ui_layer.hpp) because it belongs to the display, not the world. These do not:
// they mark a place out in front of the truck, so pinning them to the head would
// slide them off their anchor the moment you look around. They have to stay in
// the eye image, at the pixels the game put them at.
//
// SO: the draws are redirected here instead of the main target (ui_hook.cpp),
// DIBR runs on a scene that no longer contains them, and the final blit paints
// them back at their original pixels on both eyes -- the same screen position in
// each, i.e. zero disparity, which puts them at infinity. That is the same deal
// the HUD gets and the reason the user asked for it: an icon that fuses without
// strain everywhere beats one placed by whatever happened to be behind it.
//
// ONE EYE'S MARKERS, HALF THE RATE. Zero disparity BETWEEN the eyes is only half
// the problem: the capture itself came from whichever eye the game just
// rendered, and the game projects these sprites from the camera it is drawing
// with. So the captured pixel position alternated with the AER parity by exactly
// the marker's own disparity, and painting that onto both eyes moved BOTH of
// them together, every frame. Zero disparity, maximal jitter.
//
// Anchoring the capture to ONE eye removes the alternation, at the cost of the
// layer refreshing every other frame. Same trade and the same reasoning as the
// UI plane's kUiCaptureEye -- see the note there. Two targets ping-pong: the
// draws always land in the capture buffer (so the scene is clean on every
// frame, which is what DIBR needs), and only a capture-eye frame promotes that
// buffer to the one being composited.
//
// It retires by itself. When the winch is put away the draws stop, so the next
// capture-eye frame promotes an EMPTY buffer and the markers go with it -- no
// separate "did anything draw" bookkeeping needed, because compositing a fully
// transparent layer is already a no-op.
//
// ONLY WITH DIBR SHIFT ON. Without it the other eye is a rotation-warped copy
// of a frame that already had the markers in the right pixels, so there is
// nothing to correct and the layer is not built.
//
// Structurally this is smudge_layer.hpp's colour half with the depth machinery
// and the per-pixel shift removed -- one full-canvas premultiplied RGBA target,
// captured with the game's own colour blend and composited flat.
namespace winchlayer {

// True once the target exists and DIBR shift is on. Every capture is inert
// otherwise and the draws fall through to the scene as usual.
bool active();

// State the capture borrows and puts back. Same shape as the smudge layer's,
// minus the depth-stencil entries -- these draws keep whatever depth state the
// game bound, because a marker occluded by the cab should stay occluded.
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

// The markers to composite -- the last capture-eye frame's, not necessarily
// this frame's. Null until the first latch has primed the buffers.
ID3D11ShaderResourceView* srv();

// Promotes this frame's capture if it came from the capture eye, and clears the
// buffer the next frame will draw into. Call from Present BEFORE either eye
// composites, and AFTER the frame's own draws -- which is every Present, since
// the game's drawing for a frame is finished by the time Present is called.
//
// `captureEyeFrame` is "the frame that just rendered was the capture eye". Read
// it from the finished frame rather than from inside the draws: no race with
// the parity flip. If it somehow never comes true the layer would freeze, so a
// few frames of not seeing it forces a promotion anyway -- one wrong-eye frame
// beats markers stuck from a minute ago.
void latch(ID3D11DeviceContext* ctx, bool captureEyeFrame);

void update(ID3D11Device* dev, uint32_t canvasW, uint32_t canvasH, bool dibrActive);
void shutdown();

} // namespace winchlayer
