#pragma once
#include <cmath>
#include <cstdint>

// DIBR shift decision policy -- the parts of the warp that are ARITHMETIC AND RULES
// rather than D3D.
//
// Why this file exists at all: everything here used to live inline in
// xr_mirror.cpp's ~500-line per-Present render path, interleaved with device
// calls, so none of it could be exercised without a headset, a running game
// and a truck pointed at the right piece of scenery. The rules are not
// complicated, but they are exact, and several of them encode a measurement
// that cost a session in-headset to establish. Those are precisely the things
// that should not be re-derived by reading a diff.
//
// The constraint that makes it testable: NOTHING in this header may include
// Windows, D3D or OpenXR, or touch global state. Inputs in, decision out.
// See tests/dibr_policy_tests.cpp, which links this header and nothing else.
//
// The disparity functions here MIRROR THE HLSL in render/dibr.cpp deliberately,
// clamp for clamp. That duplication is the point: a test against these pins
// the contract the shader is supposed to honour, so a change to one that is
// not made to the other shows up as a failing test rather than as an artefact
// somebody has to photograph in a headset.
namespace dibrpolicy {

// --- constants the rules are built from --------------------------------------

// Hard ceiling on a single pixel's disparity. A degenerate projection (mid
// camera/FOV transition, or before the camera CB has been seen) can otherwise
// produce an unbounded scatter span, which hangs the dispatch and takes the
// driver out via TDR.
inline constexpr float kMaxDisparityPx = 512.0f;

// The full rendered eye separation must land in this band. No human IPD is
// outside it, and a value outside it saturates the clamp above -- which does
// not look like a bug, it looks like the whole synthesized eye rigidly
// translated.
inline constexpr float kMinSeparationM = 0.005f;
inline constexpr float kMaxSeparationM = 0.15f;

// How long a last-good projection may be reused against freshly captured
// depth before it is dropped. A transient bad camera-CB read lasts a frame or
// two; beyond that the held projection describes a camera the depth no longer
// came from.
inline constexpr uint32_t kMaxProjectionReusePresents = 8;

// Presents an eye may go without being the REAL rendered one before the
// partner is treated as starving.
inline constexpr int kParityStallFrames = 3;

// --- outcomes ----------------------------------------------------------------

// Every outcome one DIBR shift frame can have. Ordered so that everything at or after
// kFirstDecline is a decline, which is what lets a report separate the two
// groups by index rather than by a second table that could disagree with this
// one.
enum class Decision : uint8_t {
    Warped = 0,
    WarpedPreHud,
    DeclinedCantedEyes,
    DeclinedNotGameplay,
    DeclinedNoAppliedEye,
    DeclinedStaleAppliedEye,
    DeclinedEyeNotCameraMove,
    DeclinedMapEyeSign,
    DeclinedNoDepth,
    DeclinedBadProjection,
    DeclinedEyeMismatch,
    DeclinedWarpFailed,
    Count
};

inline constexpr size_t kFirstDecline = (size_t)Decision::DeclinedCantedEyes;

constexpr bool is_decline(Decision d) noexcept
{
    return (size_t)d >= kFirstDecline;
}

// Long form, for the frame-dump line where there is room and no context.
constexpr const char* decision_name(Decision d) noexcept
{
    switch (d) {
    case Decision::Warped:               return "WARPED (from backbuffer)";
    case Decision::WarpedPreHud:         return "WARPED (from pre-HUD)";
    case Decision::DeclinedCantedEyes:   return "declined: canted eyes";
    case Decision::DeclinedNotGameplay:  return "declined: not gameplay / drive idle";
    case Decision::DeclinedNoAppliedEye: return "declined: no applied-eye record";
    case Decision::DeclinedStaleAppliedEye:
        return "declined: applied-eye record is from an earlier frame";
    case Decision::DeclinedEyeNotCameraMove:
        return "declined: eye offset was a projection shift, not a camera move";
    case Decision::DeclinedMapEyeSign:
        return "declined: render used the map eye sign (opposite to gameplay)";
    case Decision::DeclinedNoDepth:      return "declined: no scene depth";
    case Decision::DeclinedBadProjection:return "declined: bad projection/IPD";
    case Decision::DeclinedEyeMismatch:  return "declined: colour/depth eye mismatch";
    case Decision::DeclinedWarpFailed:   return "declined: warp() returned null";
    default:                             return "(unknown)";
    }
}

// Short form, so a report naming several reasons still fits one line.
constexpr const char* decision_tag(Decision d) noexcept
{
    switch (d) {
    case Decision::Warped:               return "warped";
    case Decision::WarpedPreHud:         return "warped-prehud";
    case Decision::DeclinedCantedEyes:   return "canted-eyes";
    case Decision::DeclinedNotGameplay:  return "not-gameplay";
    case Decision::DeclinedNoAppliedEye: return "no-applied-eye";
    case Decision::DeclinedStaleAppliedEye:   return "stale-applied-eye";
    case Decision::DeclinedEyeNotCameraMove:  return "eye-not-camera-move";
    case Decision::DeclinedMapEyeSign:        return "map-eye-sign";
    case Decision::DeclinedNoDepth:      return "no-depth";
    case Decision::DeclinedBadProjection:return "bad-projection";
    case Decision::DeclinedEyeMismatch:  return "eye-mismatch";
    case Decision::DeclinedWarpFailed:   return "warp-null";
    default:                             return "?";
    }
}

// --- the gate ----------------------------------------------------------------

struct GateInputs {
    // Property of the headset (or the debug cant slider), not of the frame.
    bool  eyesCanted       = false;
    // Pause menu, loading, map, garage -- anything where no scene camera is
    // being driven.
    bool  driveIdle        = false;
    // Did the camera hook publish which eye it baked, and what it baked?
    bool  appliedEyeValid  = false;
    int   appliedEye       = -1;
    // ...and can that record be believed for THIS frame? Both default to false
    // so a caller that forgets to fill them declines rather than shifts by a
    // number it has no right to trust. See sixdof_model_applies() for what each
    // one rules out -- the shift is depth-weighted for exactly the same reason
    // the 6-DoF reprojection is, so it fails in exactly the same way.
    bool  appliedEyeFresh  = false;
    bool  appliedEyeIsCameraMove = false;
    // The render baked the eye on the side OPPOSITE to gameplay (kMapEyeSign).
    // Shifting by a separation derived from that offset puts the synthesized eye
    // on the wrong side -- measured at -1.1x the true disparity during a level
    // fly-in. See kEyeAppliedMapSign in xr_mirror.h.
    bool  appliedEyeMapSign = false;
    // Was scene depth captured for the frame being presented?
    bool  haveDepth        = false;
    int   depthEye         = -1;
    // Has a plausible projection triple been observed (and not expired)?
    bool  projectionValid  = false;
    // 2 * |baked offset|, i.e. the FULL separation between the two eyes.
    float fullSeparationM  = 0.0f;
};

// Decides whether the warp may run, and if not, why.
//
// ORDER IS LOAD-BEARING, and not merely cosmetic:
//
//  - Cant is first because it is not a per-frame condition. When it fires it
//    fires for the whole session, so letting a later guard claim those frames
//    would hide the only reason that actually matters.
//  - The two applied-eye trust guards sit immediately after the validity test,
//    because they are about the SAME record and are meaningless before it
//    exists. They come before depth because a record that cannot be believed
//    makes the depth question moot.
//  - The eye mismatch is LAST of the declines because it is the most specific:
//    it is only meaningful once we know both an applied eye and a depth
//    capture exist. Testing it earlier would report a mismatch against the
//    -1 sentinel of a missing capture, which is a different fault entirely.
//
// Returns Decision::Warped to mean "proceed" -- the caller refines that to
// Warped / WarpedPreHud / DeclinedWarpFailed via warp_outcome() once the warp
// has actually been attempted.
constexpr Decision gate(const GateInputs& in) noexcept
{
    if (in.eyesCanted)        return Decision::DeclinedCantedEyes;
    if (in.driveIdle)         return Decision::DeclinedNotGameplay;
    if (!in.appliedEyeValid)  return Decision::DeclinedNoAppliedEye;
    if (!in.appliedEyeFresh)  return Decision::DeclinedStaleAppliedEye;
    if (!in.appliedEyeIsCameraMove)
        return Decision::DeclinedEyeNotCameraMove;
    if (in.appliedEyeMapSign) return Decision::DeclinedMapEyeSign;
    if (!in.haveDepth)        return Decision::DeclinedNoDepth;
    if (!in.projectionValid ||
        !(in.fullSeparationM > kMinSeparationM) ||
        !(in.fullSeparationM < kMaxSeparationM))
        return Decision::DeclinedBadProjection;
    if (in.depthEye != in.appliedEye)
        return Decision::DeclinedEyeMismatch;
    return Decision::Warped;
}

// The outcome once the warp has been attempted. `fromPreHud` records which
// source it consumed, which is worth keeping distinct: a session that never
// warps from the pre-HUD copy means the UI command list is never being
// identified, and that is invisible if both collapse to "warped".
constexpr Decision warp_outcome(bool warpProduced, bool fromPreHud) noexcept
{
    if (!warpProduced) return Decision::DeclinedWarpFailed;
    return fromPreHud ? Decision::WarpedPreHud : Decision::Warped;
}

// --- geometry ----------------------------------------------------------------

// The FULL separation between the two rendered eyes, from the offset the
// camera hook actually baked. Derived from the baked value rather than from
// the runtime IPD so that any scaling the hook applied is reproduced exactly
// instead of assumed away.
constexpr float full_separation_m(float bakedOffsetM) noexcept
{
    const float a = bakedOffsetM < 0.0f ? -bakedOffsetM : bakedOffsetM;
    return 2.0f * a;
}

// Which way the synthesized eye shifts.
//
// This is the SIGN OF THE BAKED OFFSET and nothing else: the camera sat at
// +offset, the eye being synthesized sits at -offset, so the direction follows
// by definition rather than from a chain of independently-tuned conventions
// (which is what it used to depend on, and any one of them flipping silently
// reversed the stereo). `rightIsScreenRight` is the single empirical constant
// in the DIBR shift path.
constexpr float eye_sign(float bakedOffsetM, float rightIsScreenRight) noexcept
{
    return rightIsScreenRight * (bakedOffsetM >= 0.0f ? +1.0f : -1.0f);
}

// How far the stale eye's camera sits from the one that rendered this frame.
//
// NOT ALWAYS THE FULL SEPARATION, and assuming it was is what produced jitter
// on the main menu. The stale eye is normally the partner of the rendered one,
// but the two identities come from different places: the eye being warped comes
// from the AER parity ring, while the rendered eye comes from the camera hook's
// applied-eye record. On any screen where no camera build runs -- the main menu,
// loading, the map -- the record FREEZES while the Present hook keeps advancing
// the parity on its own 250 ms fallback. The two then disagree every other
// frame.
//
// Displacing by a full separation on those frames moves the whole reprojected
// image by an IPD, and because it alternates with the frames that agree, it
// reads as jitter along silhouettes rather than as a constant offset.
//
// When the retained frame belongs to the SAME eye we are warping, the correct
// lateral displacement is zero: it is that eye's own earlier render, so only
// the camera's motion through the world separates them, which the reprojection
// still corrects.
constexpr float stale_eye_lateral(int staleEye, int renderedEye,
                                  float separationM) noexcept
{
    return staleEye == renderedEye ? 0.0f : separationM;
}

// WHETHER THE 6-DoF REPROJECTION'S MODEL OF THE EYE OFFSET IS TRUE THIS FRAME.
//
// The reprojection displaces the view matrix laterally by the eye separation and
// re-projects through depth, so what it produces is DEPTH-DEPENDENT: near pixels
// move a long way, distant ones barely at all. That is only right if the render
// being reprojected got its eye offset the same way -- by MOVING THE CAMERA.
//
// Two things break it, and both look identical from the outside: an oscillation
// that alternates with the AER parity and scales with local disparity, so near,
// high-frequency edges shimmer while distant ground sits perfectly still.
// Measured from a frame dump 2026-08-26: far content panned smoothly at
// -3..-5 px/frame while near content alternated -8/+9 px every single frame,
// with stereo disparity itself correct and steady throughout.
//
//   eyeIsCameraMove  false when the offset was applied as a PROJECTION SHIFT --
//                    the map/garage projection site, which shears m[12] instead
//                    of moving the camera. A projection shift displaces every
//                    depth by the SAME amount, so reprojecting it through depth
//                    both over- and under-shoots, and its sign is opposite too
//                    (see kMapEyeSign in viewbuild_hook.cpp).
//   eyeFresh         false when no camera build baked an eye into THIS frame.
//                    The applied-eye record is sticky: it keeps answering with
//                    the last frame's eye while the parity advances past it, so
//                    half the answers are for the wrong eye. This is the level
//                    fly-in -- a camera that MOVES, so the idle timer stays
//                    open, on a screen that is not gameplay, so the
//                    static-screen test does not catch it either. It fell
//                    through both existing guards.
//
// Both facts were previously inferred from screen identity. Asking the record
// what it actually holds is exact, and it costs nothing.
//   eyeMapSign       true when the render baked the eye on the side OPPOSITE
//                    to gameplay (kMapEyeSign). The flip corrects for how those
//                    screens render; it does not say where the camera was, so a
//                    model built on camera geometry must not inherit it. This is
//                    the LEVEL FLY-IN as actually measured. The flip there
//                    comes from the menu/cutscene camera CALL SITE (isMenuCam),
//                    which is a property of where the view was built, not a
//                    screen classification -- so it fires whatever the screen
//                    gate is set to, while that gate calls the same frame a
//                    garage and leaves the reprojection switched on.
inline bool sixdof_model_applies(bool uses6dof, bool onStaticScreen,
                                 bool driveIdle, bool eyeValid,
                                 bool eyeFresh, bool eyeIsCameraMove,
                                 bool eyeMapSign) noexcept
{
    return uses6dof && !onStaticScreen && !driveIdle &&
           eyeValid && eyeFresh && eyeIsCameraMove && !eyeMapSign;
}

// --- disparity: mirrors the HLSL in render/dibr.cpp ---------------------------

// Reverse-Z: z_ndc = A + B/z_view  =>  z_view = B / (z_ndc - A).
inline float view_z(float projA, float projB, float depth) noexcept
{
    const float denom = depth - projA;
    return projB / (denom > 1.0e-7f ? denom : 1.0e-7f);
}

// Pixels of horizontal shift for one depth sample. NaN-safe by the same
// construction the shader uses: `!(disp > 0.0f)` is true for NaN, where a
// `disp <= 0.0f` test would be false and let it through.
inline float disparity_px(float separationM, float focalPx,
                          float projA, float projB, float depth) noexcept
{
    const float z = view_z(projA, projB, depth);
    const float disp = separationM * focalPx / (z > 1.0e-4f ? z : 1.0e-4f);
    if (!(disp > 0.0f)) return 0.0f;
    return disp < kMaxDisparityPx ? disp : kMaxDisparityPx;
}

// --- projection derivation ---------------------------------------------------

// Recovers the projection from the pair of matrices the game hands the GPU:
// proj = viewProj * inverse(view). The depth row gives z_ndc = A + B/z_view and
// [0][0] gives the horizontal focal ratio.
//
// `view` is rigid (rotation + translation), so it is inverted analytically --
// transpose the rotation, negate the rotated translation -- rather than by a
// general solve. Both matrices are row-major, m[r*4+c].
//
// It lives here rather than in the renderer because BOTH the live read at
// Present and the frozen snapshot taken beside a depth capture must produce
// bit-identical numbers; two copies of this could drift and the symptom would
// be a disparity that is subtly wrong only during camera transitions.
//
// Returns false on an implausible near plane, which is the one degenerate case
// that silently destroys the disparity rather than merely perturbing it.
inline bool derive_projection(const float* view, const float* viewProj,
                              float& a, float& b, float& p00) noexcept
{
    if (view == nullptr || viewProj == nullptr) return false;

    const auto V = [&](int r, int c) { return view[r * 4 + c]; };
    float inv[16] = {};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) inv[r * 4 + c] = V(c, r);
    for (int r = 0; r < 3; ++r)
        inv[r * 4 + 3] = -(V(0, r) * V(0, 3) + V(1, r) * V(1, 3) + V(2, r) * V(2, 3));
    inv[15] = 1.0f;

    float proj[16] = {};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            float t = 0.0f;
            for (int k = 0; k < 4; ++k) t += viewProj[r * 4 + k] * inv[k * 4 + c];
            proj[r * 4 + c] = t;
        }

    a = proj[2 * 4 + 2];
    b = proj[2 * 4 + 3];
    p00 = proj[0];
    return b > 1.0e-6f;   // sane near plane
}

// --- matrices for the 6-DoF stale-eye reprojection ---------------------------
//
// All row-major, m[r*4+c], matching what the game hands the GPU.

// out = a * b.
inline void multiply_4x4(const float* a, const float* b, float* out) noexcept
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            float t = 0.0f;
            for (int k = 0; k < 4; ++k) t += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = t;
        }
}

// Inverse of a RIGID transform (rotation + translation, no scale) -- which a
// view matrix always is. Transpose the rotation, negate the rotated
// translation. Exact and branch-free, unlike a general solve.
inline void rigid_inverse(const float* view, float* out) noexcept
{
    const auto V = [&](int r, int c) { return view[r * 4 + c]; };
    for (int i = 0; i < 16; ++i) out[i] = 0.0f;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out[r * 4 + c] = V(c, r);
    for (int r = 0; r < 3; ++r)
        out[r * 4 + 3] = -(V(0, r) * V(0, 3) + V(1, r) * V(1, 3) + V(2, r) * V(2, 3));
    out[15] = 1.0f;
}

// General 4x4 inverse by Gauss-Jordan with partial pivoting. Needed for
// viewProj, which is NOT rigid. Returns false if singular, which is the case
// the caller must decline on rather than warp by garbage.
inline bool invert_4x4(const float* m, float* out) noexcept
{
    float aug[4][8]{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) aug[r][c] = m[r * 4 + c];
        aug[r][4 + r] = 1.0f;
    }
    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        for (int r = col + 1; r < 4; ++r)
            if (std::fabs(aug[r][col]) > std::fabs(aug[pivot][col])) pivot = r;
        if (!(std::fabs(aug[pivot][col]) > 1.0e-12f)) return false;
        if (pivot != col)
            for (int c = 0; c < 8; ++c) {
                const float t = aug[pivot][c]; aug[pivot][c] = aug[col][c]; aug[col][c] = t;
            }
        const float inv = 1.0f / aug[col][col];
        for (int c = 0; c < 8; ++c) aug[col][c] *= inv;
        for (int r = 0; r < 4; ++r) {
            if (r == col) continue;
            const float f = aug[r][col];
            if (f == 0.0f) continue;
            for (int c = 0; c < 8; ++c) aug[r][c] -= f * aug[col][c];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r * 4 + c] = aug[r][4 + c];
    for (int i = 0; i < 16; ++i) if (!std::isfinite(out[i])) return false;
    return true;
}

// The view matrix of the eye DIBR shift is synthesizing, from the one the game
// rendered.
//
// THE SIGN IS TAKEN FROM THE WARP, NOT RE-DERIVED. dibr.cpp displaces a pixel at
// view distance z by `eyeSign * separation * focal / z` pixels of screen X. A
// camera that moves right by d makes world points move LEFT on screen by
// d*focal/z. So the synthesized eye's camera has moved by -eyeSign*separation
// along view +x, and a camera displacement of d subtracts d from the view
// matrix's x translation -- leaving `+= eyeSign * separation`.
//
// Deriving it independently from the offset's sign would create a second
// convention that could disagree with the first, and a stale fill built with
// the wrong one would be displaced by a full separation exactly where it is
// supposed to be repairing a hole.
inline void synth_eye_view(const float* renderedView, float eyeSign,
                           float separationM, float* out) noexcept
{
    for (int i = 0; i < 16; ++i) out[i] = renderedView[i];
    out[0 * 4 + 3] += eyeSign * separationM;
}

// --- camera-rigid geometry ---------------------------------------------------

// THE PROBLEM THESE SOLVE. The 6-DoF reprojection moves every pixel by the
// camera's motion between the retained frame and now, which is right for the
// world and catastrophically wrong for the cab. The cab is bolted to the truck,
// the camera is bolted to the cab, so the cab does not move relative to the
// camera at all -- yet it sits at ~0.7 m, where the camera's own motion produces
// the largest displacement in the frame. One present at ~106/s is ~9.4 ms; at
// 30 km/h that is ~0.08 m of forward travel, which for a pure forward
// translation expands the image radially by dz/z -- 11% at 0.7 m, versus 0.4%
// for the world beyond the glass. That is the shredded A-pillar and dashboard.
//
// NOT A MOTION-VECTOR PROBLEM. Motion vectors are for geometry moving
// INDEPENDENTLY of the camera, whose displacement no transform we hold can
// predict. The cab's motion is the camera's own motion, which we have exactly.
// What is missing is one bit per pixel -- world-rigid or camera-rigid -- and a
// second transform to apply to the second kind.

// The world position a view matrix places the eye at.
//
// A view matrix is [R | -R*P] for a camera at P with orientation R, so
// P = -R^T * t. R is a rotation, so the transpose IS the inverse -- no general
// inversion needed, and none of invert_4x4()'s failure modes apply.
inline void camera_position(const float* view, float* outP) noexcept
{
    const float t[3] = {view[0 * 4 + 3], view[1 * 4 + 3], view[2 * 4 + 3]};
    for (int r = 0; r < 3; ++r)
        outP[r] = -(view[0 * 4 + r] * t[0] +
                    view[1 * 4 + r] * t[1] +
                    view[2 * 4 + r] * t[2]);
}

// How far the camera travelled between two views. Diagnostic only: it is the
// term the rigid branch exists to remove, so seeing it at zero would mean the
// engine's view matrices are camera-relative and this whole correction is
// addressing the wrong thing.
inline float camera_translation_delta(const float* newView,
                                      const float* oldView) noexcept
{
    float a[3], b[3];
    camera_position(newView, a);
    camera_position(oldView, b);
    const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

// The target view with the camera's TRANSLATION taken back out: current
// orientation, retained frame's position.
//
// WHY THIS AND NOT THE IDENTITY. Leaving camera-rigid pixels exactly where they
// were would be right only if the camera had not turned either. It does turn --
// look around inside a moving cab and the dashboard sweeps across the screen,
// which is real parallax the reprojection should keep. Rebuilding the view at
// the OLD position with the NEW orientation keeps the rotation and drops only
// the translation.
//
// Exact for geometry rigid with the camera: such a point X sits at X + D at the
// new instant, seen from a camera at P + D, so its camera-relative position is
// (X + D) - (P + D) = X - P -- which is what a camera left at P sees of the
// point left at X. The translation cancels identically, whatever D was.
//
// The residual is the truck's own rotation: the cab is rigid with the truck,
// while this applies the full camera rotation (truck turning plus head look).
// Separating those needs the truck's transform, which we do not have. Over one
// present the truck's share is a fraction of a degree, and it vanishes entirely
// on a straight road -- against an 11% radial expansion every frame.
inline void rigid_view_at_old_position(const float* newView, const float* oldView,
                                       float* out) noexcept
{
    float pOld[3];
    camera_position(oldView, pOld);
    for (int i = 0; i < 16; ++i) out[i] = newView[i];
    for (int r = 0; r < 3; ++r)
        out[r * 4 + 3] = -(newView[r * 4 + 0] * pOld[0] +
                           newView[r * 4 + 1] * pOld[1] +
                           newView[r * 4 + 2] * pOld[2]);
}


// --- input validation --------------------------------------------------------

// The projection triple, as read live from the game's camera CB. Deliberately
// loose: this is a nonsense filter, not a calibration. Measured on this build:
// a = -2.9e-5, b = 0.1, p00 = 0.73454.
inline bool projection_plausible(float a, float b, float p00) noexcept
{
    return std::isfinite(a) && std::isfinite(b) && std::isfinite(p00) &&
           b > 1.0e-3f && b < 100.0f &&      // near plane, metres
           p00 > 0.05f && p00 < 20.0f &&     // horizontal focal ratio
           std::fabs(a) < 10.0f;
}

// A held projection may only cover a transient failure. Past that it is being
// paired with depth from a camera it no longer describes.
constexpr bool projection_reuse_expired(
    uint32_t consecutiveReuses,
    uint32_t maxReuses = kMaxProjectionReusePresents) noexcept
{
    return consecutiveReuses > maxReuses;
}

// Final guard immediately before the dispatch. Separate from gate() because it
// is the shader's own precondition rather than a frame-scheduling rule: an
// unbounded disparity here is a TDR, not an artefact.
inline bool warp_params_usable(float separationM, float focalPx,
                               float projA, float projB) noexcept
{
    (void)projA;   // any finite A is fine; it is B that can collapse z_view
    return std::isfinite(projA) &&
           projB > 1.0e-6f && focalPx > 1.0f && focalPx < 1.0e6f &&
           separationM > 0.0f && separationM < 1.0f;
}

// --- submitted eye poses -----------------------------------------------------

// A pose in OpenXR terms, kept as plain floats so this header stays free of
// openxr.h. Quaternion is (x, y, z, w).
struct EyePose {
    float orientation[4]{0.0f, 0.0f, 0.0f, 1.0f};
    float position[3]{};
};

// Places the SYNTHESIZED eye for the projection layer.
//
// The problem this solves: the layer must describe the geometry the pixels were
// actually produced with. The rendered eye's pixels come from the camera the
// hook built mid-frame, and the synthesized eye's come from displacing that
// camera by the separation the warp used -- which is the BAKED offset, not
// necessarily the runtime's own inter-eye distance. Submitting the runtime's
// freshly located eye poses instead describes a stereo pair that was never
// rendered, and the compositor's reprojection then corrects toward the wrong
// reference.
//
// So: take the DIRECTION from the captured runtime pair (it is the true
// inter-pupillary axis in tracking space, whatever the game did), and the
// MAGNITUDE from what was actually baked. Both eyes inherit the rendered eye's
// orientation, which is exact here because a canted headset has already been
// refused by gate() -- the two rendered eyes are parallel by construction.
//
// Returns false if the captured pair is degenerate, in which case the caller
// should submit the runtime poses unchanged rather than invent geometry.
inline bool rebase_synth_eye(const EyePose& renderedAtBuild,
                             const EyePose& otherAtBuild,
                             float bakedSeparationM,
                             EyePose& out) noexcept
{
    if (!(bakedSeparationM > kMinSeparationM) ||
        !(bakedSeparationM < kMaxSeparationM))
        return false;

    float axis[3];
    for (int i = 0; i < 3; ++i)
        axis[i] = otherAtBuild.position[i] - renderedAtBuild.position[i];
    const float len = std::sqrt(axis[0]*axis[0] + axis[1]*axis[1] + axis[2]*axis[2]);
    // The runtime's own separation, used only as a direction and a sanity
    // check. A pair this far outside the plausible band is not a headset
    // geometry we can reason about.
    if (!std::isfinite(len) || len < kMinSeparationM || len > kMaxSeparationM)
        return false;

    for (int i = 0; i < 4; ++i) out.orientation[i] = renderedAtBuild.orientation[i];
    for (int i = 0; i < 3; ++i)
        out.position[i] = renderedAtBuild.position[i] +
                          (axis[i] / len) * bakedSeparationM;
    return true;
}

// --- parity ------------------------------------------------------------------

// Both eyes get the same frame when the AER parity has stopped alternating.
//
// This measures the failure DIRECTLY -- "the partner eye is starving" -- rather
// than inferring it from a camera-idle timer, which answers a different
// question that merely correlated on the pause menu. The garage is where the
// two came apart: its parity alternates perfectly well, so a timer-based test
// forced mono there and turned a stable stereo pair into a shared oscillation.
constexpr bool mono_screen(int presentsSinceRealEye0,
                           int presentsSinceRealEye1,
                           int stallFrames = kParityStallFrames) noexcept
{
    return presentsSinceRealEye0 > stallFrames ||
           presentsSinceRealEye1 > stallFrames;
}

}  // namespace dibrpolicy
