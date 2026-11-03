#pragma once

struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;

// SHARPENING AFTER DLSS: AMD's contrast adaptive sharpening (FidelityFX CAS) on
// the finished frame, which DLSS leaves soft (render/dlss.hpp).
//
// A plus-shaped sharpening filter whose strength is set per pixel by how much
// room the neighbourhood leaves before it would clip: full where the image is
// soft, little at an edge that already spans most of the range. That is what
// keeps it from the halos and the shimmer a fixed unsharp mask adds -- which a
// headset magnifies.
//
// It works on the values as stored: gamma encoded, as the frame is shown. Both
// eyes get it through the frame itself, so the stale-eye warp carries a
// sharpened image too.
namespace sharpen {

// `src` sharpened by `amount` (0 - 1) into a texture of the mod's own, of the
// same size and format, or null -- `amount` is 0, or the pass could not run --
// and the caller shows `src` as it is. Valid until the next call. Puts back the
// pipeline state it touches.
//
// `srcSrv` is a view onto `src` when the caller has one. Without it `src` is
// copied first, which is the backbuffer's case: a view held onto a swapchain
// buffer would make the game's own ResizeBuffers fail.
ID3D11Texture2D* apply(ID3D11DeviceContext* ctx, ID3D11Texture2D* src,
                       ID3D11ShaderResourceView* srcSrv, float amount);

// The textures go: sharpening is off, or DLSS is. The shaders stay.
void release();

} // namespace sharpen
