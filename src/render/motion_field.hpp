#pragma once
#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

// THE GAME'S OWN MOTION, for the AER stale-eye warp (WarpType=MotionVectors).
//
// Under AER the game's "previous frame" IS the other eye's last render -- the
// very image the stale-eye warp brings forward. So the velocity buffer the game
// renders for its TAA (docs/dlss_investigation.md, section 3) says, for every
// moving surface in this render, where that surface was in THAT image and,
// with its depth, how far it has moved in the world since. Scattered onto that
// image's own pixel grid, it gives the warp a measured world motion per pixel
// to add before reprojecting: the cab, its sway against the camera, the wheel
// and hands, the truck in orbit, other vehicles -- everything the rigid-truck
// mask could only approximate as "moves exactly with the camera".
//
// Two fields per render, both RGBA16F on the render's grid:
//   on the PREVIOUS render's grid: this render's moving surfaces, at the pixel
//     each one occupied there -- xyz their world motion since, w that pixel's
//     device depth (0 = nothing landed). Nearest surface wins.
//   on its OWN grid: its moving surfaces' world motion since the previous
//     render (w = 1). The warp's fallback for a surface the next render did not
//     see: its last motion, carried one frame on. w = 0.25 where the game's
//     vector was clipped at its 256 px limit: not moving, as far as the warp is
//     concerned, but the overlay shows it (motion_field.cpp, moving()).
//
// Needs the game's TAA switched ON: the velocity pass is drawn for it.
namespace motionfield {

// From ui_hook.cpp's Draw detour at the TAA resolve, before DLSS replaces it:
// the game's depth (t2) and velocity (t3) are bound there. Does nothing unless
// the motion-vector warp is selected and running (wanted()).
void on_taa_resolve(ID3D11DeviceContext* ctx);

bool wanted();

// A field was measured in the last two seconds -- false while wanted() with the
// game's TAA off, which the settings panel says.
bool measuring();

// Fields are arriving: one chained to the render before it in the last half
// second. What the truck mask stands down on (dibrpolicy::rigid_mask_marks()),
// so it is back within half a second of them stopping, and a lone hitch that
// breaks the chain for one render does not restart it.
bool covering();

// The mean GPU time of a field since the last call, for the periodic log line;
// negative if none was measured.
float take_gpu_ms();

// The field the latest render measured, for the warp at its Present.
struct Field {
    ID3D11ShaderResourceView* onPrev  = nullptr;  // this render's motion, on the previous render's grid
    ID3D11ShaderResourceView* prevOwn = nullptr;  // the previous render's own motion, on its grid
    unsigned w = 0, h = 0;                        // that grid
    uint32_t prevBuild = 0;                       // the previous render's camera build
    uint32_t curBuild = 0;                        // the render that measured it
};
// False unless a render measured one since the last Present, chained to the
// render before it (consecutive builds, same size, within 100 ms).
bool latest(Field& out);

// THE LATEST RENDER'S OWN MOTION, on its own grid, for the debug overlay
// (render/motion_overlay.hpp). False unless a render measured it since the last
// Present. `chained` as for latest(): the render before it was the build just
// before, so prevBuild -> curBuild is the frame the motion covers.
struct Own {
    ID3D11ShaderResourceView* srv = nullptr;
    unsigned w = 0, h = 0;
    uint32_t prevBuild = 0, curBuild = 0;
    bool     chained = false;
};
bool latest_own(Own& out);

// Once per Present, after the mirror has used the field: the periodic log line,
// and releasing everything once the warp no longer wants it.
void on_present();

} // namespace motionfield
