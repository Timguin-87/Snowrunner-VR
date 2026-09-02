#pragma once
#include <d3d11.h>

// DIBR SHIFT -- depth-image-based stereo reprojection (see docs/dibr_shift_plan.md).
//
// Synthesizes the un-rendered eye from the rendered eye's colour + scene depth.
// Because our two eyes differ by a pure lateral offset with identical symmetric
// frusta, this reduces to a horizontal disparity shift:
//
//     z_view       = projB / (depth - projA)     (reverse-Z)
//     disparity_px = ipd * focalPx / z_view
//     dest_x       = src_x +/- disparity_px      dest_y = src_y  (exact)
//
// Implemented as a forward scatter: each source pixel is splatted across the
// destination span it covers, resolving overlaps by nearest-wins. Pixels no
// source pixel reaches are genuine disocclusions -- surfaces the rendered eye
// could not see. How those get filled is a setting (see WarpParams::fillMode
// below); leaving them MAGENTA is one of its values, kept because seeing where
// the holes ARE is a different question from what to put in them.
namespace dibr {

struct WarpParams {
    // FULL separation between the two rendered eyes, in metres. Derive it from
    // the offset the camera hook actually baked (2*|applied_eye().offset|), not
    // from the runtime's IPD -- if the hook scaled the offset, the warp has to
    // reproduce the same scaling or every disparity is wrong by that factor.
    float ipd      = 0.062f;
    float focalPx  = 987.2f;   // (width/2) * proj00
    // Which way to shift. This is the SIGN OF THE BAKED OFFSET, not a property
    // of the swapchain index: the camera sat at +offset, the eye being
    // synthesized sits at -offset, so the direction is settled by definition
    // rather than by a chain of tuned conventions.
    float eyeSign  = 1.0f;
    // CONSTANT pixel offset between the two eyes' frusta, added to the shift.
    //
    // Zero for the whole life of this file, and it had to be: the header above
    // says the two eyes differ by a pure lateral offset with IDENTICAL frusta,
    // which makes the mapping a pure depth-dependent disparity. Off-centre
    // projection breaks that half of the assumption -- each eye is then
    // rendered at its own sheared frustum -- but not the useful half: the two
    // frusta are mirror images with the same tangent span, so they still differ
    // by a pure horizontal translation, just no longer a zero one.
    //
    // So the mapping stays dest_x = src_x + const +/- disparity, and this is
    // the const. Set from the difference between the eyes' frustum centres; see
    // dibr_eye_pixel_offset() in xr_mirror.cpp.
    float eyeOffsetPx = 0.0f;
    float projA    = 0.0f;     // depth row A  (z_ndc = A + B/z_view)
    float projB    = 0.1f;     // depth row B  == near plane
    // How disocclusions are filled:
    //   0 = magenta -- leave holes visible, for measuring their extent
    //   1 = the stale eye's last real render, rotation-warped to the current
    //       pose (see stale_eye_fill()). THE DEFAULT, confirmed in-headset.
    //       It is the one source in the system that actually OBSERVED the
    //       disoccluded surface: a hole is by definition geometry only the
    //       synthesized eye can see, so this fills it with the real thing
    //       rather than an approximation of it.
    //       Known limitation, unchanged: the image is one frame old and the
    //       warp corrects ROTATION only, so translation -- the truck moving,
    //       the head leaning, objects moving in the scene -- leaves it
    //       displaced against the DIBR content beside it. That residual is
    //       what a camera-only 6-DoF reprojection of the stale eye would
    //       remove; see docs/dibr_shift_comparison_witcher3.md §2.4.
    //   2 = background stretch from the current frame. Temporally consistent by
    //       construction, since it never leaves this frame, and near-exact on
    //       the narrow holes the cockpit mostly produces -- but it is an
    //       invented surface, so where a hole IS wide it stretches the
    //       background across geometry that was never seen. The better
    //       fallback of the two when the stale eye is unavailable, which is
    //       why mode 1 degrades to it rather than to anything else.
    //   3 = disparity debug view
    // Kept in sync with g_fillMode's initialiser in dibr.cpp -- this default
    // only applies to a WarpParams the caller left untouched, but the two
    // disagreeing is exactly the kind of drift that makes a mode change look
    // like it did nothing.
    int   fillMode = 1;
};

// Warps srcColor into the other eye. Returns a texture owned by this module,
// valid until the next call, or nullptr on failure. Restores no pipeline state
// -- call it at a point where clobbering compute/SRV bindings is safe (Present).
//
// fillSrc is the rotation-warped stale eye used by fill mode 1 only. Passing
// null makes mode 1 degrade to the stretch fill rather than sampling a stale,
// UNWARPED image, which is what the original implementation did.
ID3D11Texture2D* warp(ID3D11DeviceContext* ctx,
                      ID3D11Texture2D* srcColor,
                      ID3D11ShaderResourceView* depthSRV,
                      const WarpParams& params,
                      ID3D11ShaderResourceView* fillSrc = nullptr);

// Magenta is the diagnostic one: it shows exactly which pixels the reprojection
// could not reach, which separates a geometry error (holes in the wrong places)
// from a fill error (holes in the right places, wrong content).
// Selected from the settings UI's combo box (menu_hook.cpp). There is
// deliberately no cycle-through entry point: two of the four modes are
// diagnostics, so blind cycling means passing through an unplayable image to
// reach a playable one.
void set_fill_mode(int mode);
int  fill_mode();

// GPU time of the two warp dispatches, in milliseconds. Negative until the
// first measurement has been collected -- and a frame goes unmeasured whenever
// the query ring is still busy, so these lag the current frame slightly and
// are diagnostic rather than a per-frame signal to steer on.
//
// Worth having because the warp runs inline on the immediate context at
// Present: its cost is on the frame the headset is waiting for, not on a
// command list that could be scheduled elsewhere.
float gpu_ms();       // most recent measurement
float gpu_avg_ms();   // smoothed

// Logs once, rate-limited, if the smoothed cost has become a significant
// fraction of the frame budget. Silent otherwise -- call it every frame.
void report_gpu_cost_if_high();

} // namespace dibr
