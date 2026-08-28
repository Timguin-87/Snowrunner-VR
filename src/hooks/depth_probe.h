#pragma once
#include <dxgi.h>
#include <d3d11.h>

// Scene depth acquisition for DIBR shift (see docs/dibr_shift_plan.md).
//
// The Phase 0 recon this file began as -- an End-key-armed snapshot of every
// depth transition in a frame, read back and reported -- has been removed now
// that its three questions are answered and recorded in the plan: the resource
// is R32G8X24_TYPELESS at render size, it is SRV-able as-is with no bind-flag
// patch, and it is REVERSE-Z. What remains is the acquisition path those
// answers made possible.
namespace hooks {

// Hooks OMSetRenderTargets on the immediate context (shared vtable also covers
// deferred contexts) to discover depth-stencil resources. Idempotent.
bool install_depth_probe(IDXGISwapChain* swapchain);

// Per-frame: finalises acquisition bookkeeping and reports miss/mixed-eye rates.
void depth_probe_on_present(IDXGISwapChain* swapchain);

// DIBR shift Phase 1 -- scene depth acquisition.
//
// The scene depth does NOT survive to Present (measured: cleared by then), so
// it is copied once per frame, mid-frame, into a texture we own
// (R32_FLOAT_X8X24_TYPELESS, REVERSE-Z: near=1, far=0).
//
// One captured frame's depth, with the identity and the camera it was captured
// under. The camera travels WITH the depth deliberately: reading the projection
// live at consume time while the depth was copied mid-frame let the two
// describe different cameras during an FOV animation or a camera transition,
// and since the disparity is built from both, neither could reveal that alone.
struct SceneDepth {
    ID3D11ShaderResourceView* srv = nullptr;
    int   eye = -1;          // which eye this depth was rendered for
    float offset = 0.0f;     // the eye offset baked at capture time, metres
    float projA = 0.0f;      // depth row A  (z_ndc = A + B/z_view)
    float projB = 0.1f;      // depth row B == near plane
    float p00 = 0.73454f;    // horizontal focal ratio
    bool  projValid = false; // was the frozen projection plausible?
};

// Finds the newest usable capture for `eye`, or returns false.
//
// ASK FOR THE EYE YOU NEED. Handing over "the newest capture" instead was the
// single largest source of declined DIBR shift frames: the newest capture belongs to
// whichever eye the game rendered last, which is not reliably the eye in the
// backbuffer now being presented. Warping colour with the other eye's depth
// displaces every silhouette by a full eye separation and nothing downstream
// can detect it, so the only safe response was to decline -- thousands of times
// per session. Selecting by identity converts those into warped frames.
bool scene_depth_for_eye(int eye, SceneDepth& out);

// Make the probe blind to render-target changes the MOD itself makes, for the
// duration of one of its own capture passes. Thread-local, so a deferred
// context recording on a worker thread is unaffected by another thread's pass.
//
// The mod's own OMSetRenderTargets calls go through this file's detour like
// anyone else's, and the probe reads them as scene depth transitions. That is
// harmless for a target it ignores, but smudge_layer's glass depth is
// CANVAS-SIZED, so it passes the sceneSized test and gets accumulated into the
// scene depth -- putting the windscreen's depth into the buffer the world is
// warped by, which is exactly what that feature must not do. Each bind also
// manufactures a transition that restarts or mis-pairs the accumulation window.
//
// Bracket any pass that rebinds targets with this and none of that happens: the
// real call still goes through, only the bookkeeping is skipped, so the tracked
// binding still matches reality once the pass restores it.
void set_self_targets(bool on);

// Coarse content check on the accumulator the warp is being handed THIS frame.
// `covered` is the fraction of sampled texels holding real depth (> 0 under
// reverse-Z, where 0.0 means infinitely far / never written) and `maxDepth` the
// nearest value present.
//
// The question it exists to answer: an DIBR shift frame that ran the warp with every
// guard satisfied and still produced no shift. Zero disparity is what an EMPTY
// accumulator produces by construction -- z = projB/(0 - projA) puts everything
// at infinity, and ipd*focalPx/z rounds to nothing -- so distinguishing "the
// depth was blank" from "the maths went wrong" needs the depth itself, not
// more inference from the output image.
//
// Everything needed to tell the remaining failure modes apart, for a frame
// whose warp ran with every guard satisfied and still did not shift.
//
// The regions that fail to shift are exactly the regions where the accumulator
// holds ZERO -- reverse-Z 0.0 is infinitely far, so the disparity rounds to
// nothing there. Observed both totally (whole frame flat) and PARTIALLY (the
// scene beyond the windscreen flat while the cab shifts correctly), and a
// partial one cannot come from a clear, which is all-or-nothing. So the
// question is why that geometry's depth never arrived, and these separate the
// three candidates:
//
//   covWarp low, covOther high   -> the data landed in the other slot; the
//                                   Present-time hand-over is misaligned with
//                                   the capture stream.
//   both low, deferred > 0       -> those passes were recorded on a DEFERRED
//                                   context and dropped uncaptured. The world
//                                   pass moving to a worker context under load
//                                   while the cab stays on the immediate one is
//                                   exactly a partial failure.
//   both low, deferred == 0      -> captured but the sources themselves read
//                                   zero, i.e. sampled after the engine reused
//                                   or cleared them.
//
// capsWarp/capsOther are captures into each slot since that slot was last
// cleared; framePasses is every scene-sized unbind the frame ran.
//
// `width`/`height` are the accumulator's own dimensions, worth comparing
// against the colour size: it is built from the first scene-sized depth seen
// and only rebuilt when that changes, so a render-size change would leave the
// warp sampling a buffer that does not correspond to the image it is shifting.
struct DepthDiag {
    unsigned width = 0, height = 0;
    float covWarp = -1.0f,  maxWarp = -1.0f;
    float covOther = -1.0f, maxOther = -1.0f;
    int   capsWarp = -1, capsOther = -1;
    int   framePasses = -1;
    int   deferred = -1;        // scene-depth unbinds dropped as deferred
    int   warpSlot = -1;
    // Window restarts this frame, by reason. One firstOfFrame is normal; a
    // storm of the other two shreds the window into fragments.
    int   rsFirst = -1, rsEye = -1, rsCam = -1;
    float lastCamMove = -1.0f;   // distance that tripped the camera test, m
};

// STALLS the pipeline (two CopyResource + Map pairs). Diagnostic only: call it
// while a frame dump is running and never on a normal frame. False if there is
// nothing to read.
bool scene_depth_diag(ID3D11DeviceContext* ctx, DepthDiag& out);

// Whether the captured depth is max()-accumulated over every scene-sized
// pass of a frame (default) or replaced by the last pass only. Diagnostic
// for the doubled-near-geometry artefact: if doubling survives with this
// off, the merge is not what creates it.
// Accumulate only into the frame's MAIN scene depth target, ignoring other
// scene-sized ones. Cockpit renders two; folding both together is what put
// a second copy of near geometry into the depth map.
// Scene-sized depth targets seen this session, in first-sighting order.
// Selection 0 folds in all of them (original behaviour); N restricts the
// capture to target N-1. Orbit uses one target, cockpit two, and the two
// are indistinguishable by size, format or capture count -- so which is the
// main one is settled by trying them.

} // namespace hooks
