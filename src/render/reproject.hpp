#pragma once
#include <d3d11.h>

// Camera-only 6-DoF reprojection of a retained frame.
//
// WHAT THIS IS FOR. DIBR shift fills its disocclusion holes from the synthesized eye's
// own last real render -- the one source in the system that actually observed
// the surface behind the occluder. That retained frame is brought to the
// current pose by a rotation-only homography (see stale_eye_fill), which is
// exact for a head that only turned and wrong for everything else: the truck
// moving, the head leaning, the camera following. In a driving game the camera
// is translating almost continuously, so the fill sits displaced against the
// DIBR content beside it, which is the ghosting fill mode 1 is known for.
//
// A rotation is a homography and needs no depth. A TRANSLATION does: how far a
// pixel moves depends on how far away it is. So this takes the retained frame's
// own depth, unprojects each pixel to world space with the matrices that frame
// was rendered with, and reprojects it with the matrices the synthesized eye
// would have now. That corrects translation exactly, for the static world.
//
// WHAT IT STILL DOES NOT FIX: genuinely moving objects. Another vehicle, or
// physics debris, moved in the world between the two frames, and no camera
// transform can account for that -- only motion vectors could, and SnowRunner
// has no located velocity buffer (docs/dibr_shift_plan.md section 6). Those are a small
// fraction of the frame; the camera is the dominant term and this removes it.
//
// COMPOSITES OVER, RATHER THAN REPLACING. It writes only destination pixels it
// can actually reach, leaving everything else as the caller left it -- which is
// the rotation-warped image. So the result is strictly better than, or equal
// to, today's fill: where the reprojection has no source it degrades to exactly
// the previous behaviour rather than to a hole.
namespace reproject {

struct Params {
    // Old clip -> world, for the frame being reprojected.
    float invOldViewProj[16] = {};
    // World -> new clip, for the eye and instant being reprojected TO.
    float newViewProj[16] = {};

    // THE SAME TARGET CAMERA WITH ITS TRAVEL TAKEN BACK OUT: new orientation,
    // old position (dibrpolicy::rigid_view_at_old_position). Used for pixels
    // that are rigid with the camera rather than with the world -- the cab,
    // which does not move relative to a camera bolted inside it, but which sits
    // at the near depth where newViewProj displaces things most. Applying the
    // world transform there expands the cab radially by dz/z every frame, ~11%
    // at 0.7 m, which is what shreds the A-pillar and dashboard.
    float newViewProjRigid[16] = {};

    // COVERAGE MASK of geometry rigid with the camera (render/rigid_mask.hpp),
    // for the retained frame. Marked pixels get newViewProjRigid, everything
    // else gets newViewProj.
    //
    // THE SOLE CLASSIFIER. A depth band used to stand in for this -- near
    // pixels treated as cab, far ones as world, smoothstepped between -- which
    // only held inside a cab and only because nothing in the WORLD is within a
    // metre or two there. It could not tell the hood from the ground at the
    // same distance, so it had to be gated to cockpit view and did nothing in
    // the exterior cameras. This is the renderer's own answer instead: these
    // draws ARE the truck, at whatever depth they happen to be.
    //
    // Null means no pixel is rigid, so the reprojection behaves exactly as it
    // did before any of this existed.
    ID3D11ShaderResourceView* rigidMaskSrv = nullptr;

    // WHAT TO DO WITH THE RIGID PIXELS: false reprojects them by
    // newViewProjRigid, true skips them entirely and leaves the destination
    // untouched there.
    //
    // Skipping is for the ORBIT camera. newViewProjRigid keeps the camera's
    // rotation and drops its travel, which is exact when the camera turns about
    // its own optical centre -- a cockpit under stick look. An orbit camera
    // does not: it swings AROUND the truck, so the truck is the thing that
    // stays put while the rotation sweeps past it, and applying that rotation
    // displaces it by the full swing. There is no transform to substitute
    // either, since the rigid geometry is stationary in view by definition, so
    // the honest answer is to contribute nothing and let the caller's own
    // pose-only warp stand -- which carries the head rotation those pixels do
    // need. Same conclusion warp_homography_for() reached for the whole frame.
    bool rigidSkip = false;

    // Reverse-Z depth values at or below this are treated as "nothing was
    // drawn here" -- a cleared accumulator rather than far geometry.
    float minValidDepth = 1.0e-6f;

    // What depth those pixels scatter AT, instead of being dropped.
    //
    // Dropping them was the cause of a doubled tree line against the sky: the
    // resolve composites rather than replaces, so pixels that geometry moved
    // AWAY from keep the caller's rotation-only pre-fill -- still showing the
    // tree at its old position -- unless something scatters over them. Only the
    // sky is behind a tree, and the sky was the one thing excluded. See the long
    // note in project().
    //
    // Reverse-Z, so smaller is farther: 1e-4 is on the order of a kilometre for
    // a typical near plane, which makes the translation term sub-pixel while
    // staying finite enough to unproject. Set to 0 to restore the old drop.
    float farDepth = 1.0e-4f;
};

// Scatters `srcColor` (sampled through `srcColorSrv`) into `dstUav` using
// `srcDepthSrv`. `dstUav` must address a texture of the same dimensions.
//
// Restores the compute stage it used. Returns false if the shaders or scratch
// buffers could not be created, in which case the destination is untouched.
bool composite(ID3D11DeviceContext* ctx,
               ID3D11ShaderResourceView* srcColorSrv,
               ID3D11ShaderResourceView* srcDepthSrv,
               ID3D11UnorderedAccessView* dstUav,
               unsigned width, unsigned height,
               const Params& params);

// Frees the scratch buffers. Called when the feature is turned off so it costs
// no memory while unused.
void release();

// GPU time of the last completed composite, in milliseconds; negative until one
// has been measured. Same non-stalling query ring as dibr::gpu_ms().
float gpu_ms();

} // namespace reproject
