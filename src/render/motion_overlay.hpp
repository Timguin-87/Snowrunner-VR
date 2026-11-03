#pragma once

struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11ShaderResourceView;

// THE MOTION-VECTOR OVERLAY (Advanced tab, a debug view): the motion the stale-eye
// warp moves each pixel by, painted half transparent over both eyes.
//
// THE COLOUR is the surface's motion RELATIVE TO THE HEAD, as it lands on screen
// in one frame. The hue says which way it went -- red right, yellow-green up, cyan
// left, violet down -- and the strength how far: white for a surface the game
// measured that kept its place relative to the head, as the cab does, full colour
// from about 30 px. No tint: the game reported nothing moving there, so the warp
// moves it as static world, by the camera alone.
//
// TWO PLACES PAINT IT, with one encoding (kColourHlsl):
//   - the WARPED eye, in reproject.cpp's resolve: every pixel by the motion it
//     actually took. Solid: the next render's vectors put it there. Striped one
//     way: a neighbour's were borrowed. Striped the other way: nothing landed on
//     it, and its own motion from the frame before was carried on. Untinted:
//     moved as static world. Magenta on every other pixel: no source pixel
//     landed there at all, so it shows the rotation-only base, a frame old --
//     a disocclusion, the edge of view after a turn, or a mirror.
//   - the RENDERED eye, here: the render's own motion, which is the very motion
//     the other eye's warp takes as "the next render's" this frame. So the
//     overlay sits in both eyes at once rather than flickering between them.
//     Yellow on every other pixel: the game's vector was clipped at its 256 px
//     limit, which AER reaches near the edge of view in a fast head turn, so
//     the warp cannot use it (motion_field.cpp, moving()).
namespace motionoverlay {

// The shared encoding, prepended to both shaders. It defines
//   float4 motion_colour(float2 px, uint how, uint2 at)
// with px the motion on screen in pixels of the image (x right, y up), how 1 the
// next render's own, 2 a neighbour's, 3 its own last one, and at the pixel, for
// the stripes. Alpha included.
extern const char kColourHlsl[];

// Not saved: a debug view, off at every start.
bool enabled();
void set_enabled(bool on);

struct Params {
    ID3D11ShaderResourceView* motionSrv = nullptr;   // the render's own (motion_field.hpp)
    ID3D11ShaderResourceView* depthSrv  = nullptr;   // its device depth
    float invViewProj[16] = {};    // that render's camera: clip -> world
    float viewProj[16]    = {};    // and world -> clip
    float headMove[3]     = {};    // the head's own travel over the same frame, world
    unsigned colourW = 0, colourH = 0;   // the image's size: the pixels px counts in
    bool srgbTarget = false;       // the target encodes on write, so hand it linear
};

// Paints over `rtv` within the viewport (x, y, w, h): the image, letterboxed.
// Puts back everything it touched. False if the shaders could not be built.
bool draw(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv,
          float x, float y, float w, float h, const Params& p);

void release();

} // namespace motionoverlay
