#include "render/reproject.hpp"
#include "render/gpu_timer.hpp"
#include "render/motion_overlay.hpp"   // kColourHlsl
#include "common/log.h"

#include <d3dcompiler.h>
#include <wrl/client.h>
#include <string>

using Microsoft::WRL::ComPtr;

namespace reproject {
namespace {

// THREE PASSES, and the reason is an atomics limitation rather than a design
// preference.
//
// A forward scatter has to resolve overlaps by depth: when two source pixels
// land on the same destination, the nearer one wins. dibr.cpp does that in ONE
// pass by packing [disparity | srcX] into a uint and using InterlockedMax, so
// the depth ordering and the payload move together. That works there because
// the payload is a single 16-bit X coordinate -- the warp is horizontal, so Y
// is unchanged.
//
// Here the payload is a full 2D coordinate: 2688x2688 needs 16 bits each, which
// leaves nothing for the depth key. D3D11's cs_5_0 has no 64-bit atomic
// (InterlockedMax on uint64 arrives with SM 6.6), so the two cannot travel in
// one word.
//
// So: scatter the depth key alone, then re-scatter and let each source write
// its coordinate only where its key matches the winner. Ties (two surfaces at
// the same quantised depth) resolve arbitrarily between equals, which is
// exactly what a single-pass InterlockedMax would have done anyway.
const char kHlsl[] = R"(
cbuffer P : register(b0)
{
    row_major float4x4 invOld;      // old clip -> world
    row_major float4x4 newVP;       // world -> new clip
    row_major float4x4 newVPRigid;  // ...with the camera's travel removed
    float2 texSize;
    float  minValidDepth;
    float  hasMask;
    float  rigidSkip;      // 1 = leave rigid pixels to the caller entirely
    float  farDepth;       // stand-in depth for sky; <= 0 restores the old skip
    float2 depthScale;     // depth grid / colour grid: 1 unless DLSS upscaled the colour
    float2 depthSize;      // the depth grid, for the motion lookup's neighbours
    float  hasMotion;      // 1 = move pixels by the game's motion, not the rigid mask
    float  hasMirror;      // 1 = MirrorMask is bound: mirrors keep the rotation-only base
    float3 headMove;       // the head's own travel over the frame, for the overlay
    float  debugMotion;    // 1 = paint the motion-vector overlay (motion_overlay.hpp)
    // The rotation-only base's homography, destination uv -> retained uv, one
    // row each; rotH0.w = 1 when the caller drew the base with it (housing_motion).
    float4 rotH0;
    float4 rotH1;
    float4 rotH2;
};

Texture2D<float>    SrcDepth : register(t0);
Texture2D<float4>   SrcColor : register(t1);
// Coverage of camera-rigid geometry in the RETAINED frame: 1 = truck.
Texture2D<float>    RigidMask : register(t2);
// The game's motion, on the depth grid (render/motion_field.hpp).
Texture2D<float4>   MotionNext : register(t3);   // xyz world motion since, w the surface's depth here
Texture2D<float4>   MotionOwn  : register(t4);   // xyz its own last motion, w 1 = moving
// The retained frame's mirrors (render/mirror_mask.hpp), on the depth grid.
Texture2D<float>    MirrorMask : register(t5);
RWTexture2D<uint>   Key      : register(u0);
RWTexture2D<uint>   Idx      : register(u1);
RWTexture2D<float4> Dst      : register(u2);

// How much of the camera's TRANSLATION this pixel should receive.
// 1 = world geometry, 0 = rigid with the camera.
//
// The depth (and mask) texel under colour pixel p: p itself on one grid, else
// the render pixel the upscaled colour pixel came from.
uint2 depth_px(uint2 p)
{
    return uint2((float2(p) + 0.5f) * depthScale);
}

// HOW FAR THE SURFACE AT dp HAS MOVED since the retained frame, in the world.
//
// 1. The next render saw it: its velocity buffer says where the surface had
//    been, and the nearest one landed here -- if that is the surface this
//    pixel shows (same depth, 3%), not one hidden behind it.
// 2. This frame's own velocity buffer says something moving is here, but
//    nothing landed on it -- a scatter gap: a neighbour on the same surface.
// 3. Wider than that -- a strip one eye sees and the other does not: the
//    surface's own last motion, corrected by how much the motion has changed
//    since, measured where the next render did see that surface
//    (motion_change()).
// 4. Nothing measured on that surface within reach: its own last motion,
//    carried one frame on.
// Otherwise static.
static const int2 kNeighbour[8] = { int2(-1, -1), int2(0, -1), int2(1, -1), int2(-1, 0),
                                    int2(1, 0), int2(-1, 1), int2(0, 1), int2(1, 1) };

bool is_glass(uint2 mp)
{
    return hasMirror > 0.5f && MirrorMask[mp] > 0.5f;
}

// CARRYING THE LAST MOTION ON ASSUMED THE TRUCK COVERS THE SAME DISTANCE EVERY
// RENDER, and it does not. The game moves it by each frame's own time, and the
// renders are not evenly spaced (about 80 a second in Tim's log), so the travel
// varies from one render to the next: millimetres in a steady drive,
// centimetres at a hitch or over a bump. In the cab that travel is nearly all
// of the motion; the cab only keeps its place because the camera moved as far.
// Last render's travel against this render's camera left a one-eye strip short
// or long by the difference, which it showed one for one while the measured cab
// beside it held still: the strip beside the dashboard flickering in a steady
// drive (Tim, 2026-11-04). In dump 225855 f2 the dump's own first stall made it
// extreme: the render before had moved the truck by 12.5 ms, this one by about
// 40, so the strip lagged the cab by some 19 cm, came that much nearer and
// landed across the door.
//
// So the change is measured: walk out from the pixel along its own surface
// (depth continuous, 3% a step) to the first pixel the next render did see,
// each of eight ways, and take that pixel's motion now less its own last
// motion. On one rigid body that is the change in travel and turn, which holds
// for the whole body, while the pixel's own last motion keeps whatever was its
// own, an arm moving against the cab. Averaged over the ways that found one.
static const int kWalkStep  = 3;    // depth texels
static const int kWalkSteps = 32;   // 96 texels: a hand at 35 cm against the dashboard

bool motion_change(uint2 dp, float dHere, out float3 change)
{
    change = float3(0.0f, 0.0f, 0.0f);
    float n = 0.0f;
    [loop] for (int k = 0; k < 8; ++k) {
        float dLast = dHere;
        [loop] for (int i = 1; i <= kWalkSteps; ++i) {
            const int2 t = int2(dp) + kNeighbour[k] * (i * kWalkStep);
            if (t.x < 0 || t.y < 0 || t.x >= (int)depthSize.x || t.y >= (int)depthSize.y) break;
            if (is_glass(uint2(t))) break;
            const float d = SrcDepth[uint2(t)].r;
            if (abs(d - dLast) > 0.03f * dLast + 1.0e-7f) break;   // another surface
            dLast = d;
            const float4 o = MotionNext[uint2(t)];
            if (o.w > 0.0f && abs(o.w - d) <= 0.03f * d + 1.0e-7f) {
                const float4 own = MotionOwn[uint2(t)];
                if (own.w > 0.5f) { change += o.xyz - own.xyz; n += 1.0f; }
                break;
            }
        }
    }
    if (!(n > 0.5f)) return false;
    change /= n;
    return true;
}

// `how` says which case it was, for the overlay (motion_overlay.hpp): the
// case's number, 0 for static. `walk` false leaves case 3 out.
float3 motion_how(uint2 dp, float dHere, bool walk, out uint how)
{
    how = 1u;
    const float tol = 0.03f * dHere + 1.0e-7f;
    const float4 o = MotionNext[dp];
    if (o.w > 0.0f && abs(o.w - dHere) <= tol) return o.xyz;
    how = 0u;
    const float4 own = MotionOwn[dp];
    if (!(own.w > 0.5f)) return float3(0.0f, 0.0f, 0.0f);
    how = 2u;
    [unroll] for (int k = 0; k < 8; ++k) {
        const int2 n = int2(dp) + kNeighbour[k];
        if (n.x < 0 || n.y < 0 || n.x >= (int)depthSize.x || n.y >= (int)depthSize.y) continue;
        const float4 on = MotionNext[n];
        if (on.w > 0.0f && abs(on.w - dHere) <= tol) return on.xyz;
    }
    how = 3u;
    float3 change;
    if (walk && motion_change(dp, dHere, change)) return own.xyz + change;
    how = 4u;
    return own.xyz;
}

// THE MIRROR GLASS on the motion path. Its own velocity describes the
// reflection, not the glass (2026-10-29), and what the reflection shows is the
// far scene, so it cannot be moved by its own vectors nor by its glass's depth.
// It used to be left out entirely, keeping the rotation-only base: right for
// the reflection, but the glass then stayed where rotation alone put it while
// its housing moved with the cab, and the two parted whenever the cab moved
// against the view -- the truck turning or bouncing, the head smoothing lagging
// (Tim's dump 233845 f16, 2026-11-02).
//
// So the glass is MOVED by its housing's motion -- the first surface past the
// glass's edge on a straight walk out each way, at the glass's own depth -- and
// FILLED in the resolve from what the rotation-only base has at that pixel, when
// that is glass too (source_colour()): the reflection warped as the far scene it
// is, inside the outline its housing carries. Without the base's homography
// the glass is left out as before.
static const int2 kOut[4] = { int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1) };

bool housing_motion(uint2 mp, float dGlass, out float3 m)
{
    m = float3(0.0f, 0.0f, 0.0f);
    float n = 0.0f;
    [unroll] for (int k = 0; k < 4; ++k) {
        [loop] for (int i = 1; i <= 32; ++i) {
            const int2 t = int2(mp) + kOut[k] * (i * 6);
            if (t.x < 0 || t.y < 0 || t.x >= (int)depthSize.x || t.y >= (int)depthSize.y) break;
            if (is_glass(uint2(t))) continue;
            const float d = SrcDepth[uint2(t)].r;
            if (abs(d - dGlass) <= 0.15f * dGlass) {
                // No walk here: one for every glass pixel's housing is too many.
                uint how;
                const float3 hm = motion_how(uint2(t), d, false, how);
                if (how != 0u) { m += hm; n += 1.0f; }
            }
            break;
        }
    }
    if (!(n > 0.5f)) return false;
    m /= n;
    return true;
}

// How far the surface at depth texel mp has moved, and which of motion_how()'s
// cases said so (the glass counts as the first). False: leave it to the base.
bool surface_motion(uint2 mp, float d, out float3 m, out uint how)
{
    m = float3(0.0f, 0.0f, 0.0f);
    how = 1u;
    bool ok = true;
    if (is_glass(mp)) {
        ok = rotH0.w > 0.5f;
        if (ok) ok = housing_motion(mp, d, m);
    } else {
        m = motion_how(mp, d, true, how);
    }
    return ok;
}

// Sampled by integer coordinate: the mask is the same size as the retained
// depth by construction, so there is nothing to filter and a bilinear tap would
// only smear the silhouette by half a texel. The hard edge is correct -- the
// truck really does end there.
float rigid_weight(uint2 p)
{
    if (!(hasMask > 0.5f)) return 1.0f;    // no mask: everything is world
    return 1.0f - saturate(RigidMask[depth_px(p)]);
}

bool to_ndc(float4x4 m, float4 world, out float3 ndc)
{
    ndc = float3(0.0f, 0.0f, 0.0f);
    float4 c = mul(m, world);
    // Behind the camera, or on its plane: no valid projection.
    if (!(c.w > 1e-6f)) return false;
    ndc = c.xyz / c.w;
    return true;
}

// Returns false when this source pixel has nothing to contribute.
bool project(uint2 p, out int2 dst, out uint key)
{
    dst = int2(0, 0);
    key = 0u;

    float d = SrcDepth[depth_px(p)].r;
    // THE SKY SCATTERS TOO, at a finite stand-in distance.
    //
    // Reverse-Z puts the far plane at 0, which is also what a cleared
    // accumulator holds, so the skybox and any never-written pixel arrive here
    // as d = 0. Rejecting them looks right -- a point at infinity has no
    // parallax, so there is nothing to move -- and it is exactly wrong, for a
    // reason that only shows at silhouettes.
    //
    // CS_Resolve composites rather than replaces: a destination no source
    // reached keeps what the caller pre-filled, which is the whole frame under
    // the rotation-only warp. When the camera translates, a tree scatters to its
    // new position and VACATES the pixels it used to occupy. Those pixels want
    // sky. Sky is the one thing that never scatters, so nothing overwrites them
    // and they keep the pre-fill -- which still holds the tree, at its old
    // place. The tree ends up drawn twice: once translated, once not. Against
    // terrain the neighbouring ground pixels scatter in and cover it, which is
    // why this only ever showed against the skybox.
    //
    // Clamping instead of rejecting fixes it with no special case downstream. A
    // stand-in around 1e-4 is roughly a kilometre for a typical near plane, far
    // enough that the translation term is sub-pixel -- so the sky still lands
    // where rotation alone would put it -- and finite enough to unproject
    // without the precision collapse d = 0 would cause. The depth key it earns
    // is the lowest possible (ndc.z ~ 0 -> key 1), so sky can only ever win a
    // pixel that no real geometry reached: precisely the vacated ones.
    if (!(d > minValidDepth)) {
        if (!(farDepth > 0.0f)) return false;    // opt out: the old behaviour
        d = farDepth;
    }

    float2 uv = (float2(p) + 0.5f) / texSize;
    float4 clip = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, d, 1.0f);

    float4 world = mul(invOld, clip);
    if (!(abs(world.w) > 1e-12f)) return false;
    world /= world.w;

    float3 ndc = float3(0.0f, 0.0f, 0.0f);
    if (hasMotion > 0.5f) {
        // THE GAME'S OWN MOTION, in place of the rigid classification below:
        // the surface is where it was plus how far it has moved since -- the
        // truck and cab included, each by its own measured motion -- seen by
        // the new camera.
        // A mirror's glass moves with its housing and is filled from the
        // rotation-only base (housing_motion()), or left to that base entirely.
        const uint2 mp = depth_px(p);
        float3 m;
        uint how;
        if (!surface_motion(mp, SrcDepth[mp].r, m, how)) return false;
        world.xyz += m;
        if (!to_ndc(newVP, world, ndc)) return false;
    } else {
        // TWO TRANSFORMS, PICKED BY DEPTH. Both are real projections of the same
        // world point, so interpolating between them traces a path between two
        // valid answers rather than blending into a meaningless one -- which is why
        // the rigid branch removes the translation from the VIEW instead of just
        // leaving the pixel where it was. Leaving it would also undo the rotation,
        // and the cab genuinely does sweep across the screen when you look around.
        float w = rigid_weight(p);

        // ORBIT: contribute nothing at all for rigid pixels, rather than
        // contributing newVPRigid. That transform keeps the ROTATION and drops only
        // the translation, which is right for a camera that turns about its own
        // centre and wrong for one that swings around the truck -- there the truck
        // is what stays still while the camera's rotation sweeps past it, so
        // applying that rotation to it displaces it by the whole swing.
        //
        // Returning false leaves the destination as the caller left it, which is
        // the pose-only homography: the head correction those pixels do need, and
        // nothing else. See warp_homography_for(), which declines the game-rotation
        // warp in orbit for exactly the same reason.
        if (rigidSkip > 0.5f && w <= 0.0f) return false;

        if (w >= 1.0f) {
            if (!to_ndc(newVP, world, ndc)) return false;
        } else if (w <= 0.0f) {
            if (!to_ndc(newVPRigid, world, ndc)) return false;
        } else {
            float3 a, b;
            if (!to_ndc(newVPRigid, world, a)) return false;
            if (!to_ndc(newVP, world, b)) return false;
            ndc = lerp(a, b, w);
        }
    }

    // Reverse-Z again on the way out: outside [0,1] is beyond the frustum.
    if (!(ndc.z > 0.0f) || ndc.z > 1.0f) return false;
    if (abs(ndc.x) > 1.0f || abs(ndc.y) > 1.0f) return false;

    float2 dp = float2((ndc.x * 0.5f + 0.5f) * texSize.x - 0.5f,
                       (0.5f - ndc.y * 0.5f) * texSize.y - 0.5f);
    if (!(dp.x > -1.0f) || !(dp.y > -1.0f)) return false;   // false for NaN too

    dst = int2(round(dp.x), round(dp.y));
    if (dst.x < 0 || dst.y < 0 ||
        dst.x >= (int)texSize.x || dst.y >= (int)texSize.y) return false;

    // Nearer must win, and in reverse-Z nearer is LARGER, so the depth maps
    // straight onto InterlockedMax with no inversion. +1 so a real hit is never
    // the 0 that means "nothing written".
    key = (uint)(saturate(ndc.z) * 16777215.0f) + 1u;
    return true;
}

// CRACKS. A forward scatter leaves a destination unreached wherever the surface
// it belongs to has spread apart since the retained frame: a surface coming
// closer, a wheel turning, foliage in the wind, or a sub-pixel shift rounding two
// sources onto one pixel and none onto the next. Such a pixel kept the caller's
// rotation-only pre-fill, which shows that surface's OLD place -- or what was
// behind it -- so it read as a missing pixel inside an otherwise right surface
// (Tim, in the motion warp, 2026-11-02).
//
// So a pixel no source reached, with reached pixels on two opposite sides, is
// filled from them: from the nearest pair that is one surface (a crack in it),
// else from the far side of a pair that is not (a sliver uncovered beside a
// moving edge, which is background). A wider hole -- a real disocclusion -- has
// reached pixels on one side at most and keeps the pre-fill as before.
//
// Not while rigid pixels are being skipped (orbit without a motion field):
// those are left to the pre-fill on purpose, and a thin part of the truck would
// look like a crack in the world around it.
static const int2 kPairs[8] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1),
                                int2(-1, -1), int2(1, 1), int2(1, -1), int2(-1, 1) };

bool fill_cracks()
{
    return hasMotion > 0.5f || !(rigidSkip > 0.5f && hasMask > 0.5f);
}

bool reached(int2 q, out uint key, out uint packed)
{
    key = 0u;
    packed = 0u;
    if (q.x < 0 || q.y < 0 || q.x >= (int)texSize.x || q.y >= (int)texSize.y) return false;
    packed = Idx[uint2(q)];
    if (packed == 0u) return false;
    key = Key[uint2(q)];
    return true;
}

// The reached neighbour a crack at p is filled from, and its packed source.
bool crack_source(int2 p, out int2 from, out uint packed)
{
    from = p;
    packed = 0u;
    uint sameKey = 0u, farKey = 0xFFFFFFFFu;
    int2 sameFrom = p, farFrom = p;
    uint samePacked = 0u, farPacked = 0u;
    [unroll] for (int i = 0; i < 8; i += 2) {
        const int2 a = p + kPairs[i], b = p + kPairs[i + 1];
        uint ka, kb, pa, pb;
        const bool ra = reached(a, ka, pa);
        const bool rb = reached(b, kb, pb);
        if (!ra || !rb) continue;
        const bool aNear = ka >= kb;
        const uint kn = aNear ? ka : kb, kf = aNear ? kb : ka;
        if (float(kn - kf) <= 0.03f * float(kn)) {
            if (kn > sameKey) { sameKey = kn; sameFrom = aNear ? a : b; samePacked = aNear ? pa : pb; }
        } else if (kf < farKey) {
            farKey = kf; farFrom = aNear ? b : a; farPacked = aNear ? pb : pa;
        }
    }
    if (samePacked != 0u) { from = sameFrom; packed = samePacked; return true; }
    if (farPacked  != 0u) { from = farFrom;  packed = farPacked;  return true; }
    return false;
}

uint2 unpack_src(uint packed)
{
    return uint2((packed & 0xFFFFu) - 1u, (packed >> 16) - 1u);
}

// SEE-THROUGH CRACKS. A surface coming closer is drawn larger than it was, so
// its scatter leaves a grid of one-pixel gaps across it -- and where the
// background beside it shifts less, background pixels land in those gaps and
// win them, nothing nearer having arrived. A passing pole then shows streaks of
// sky right through it (Tim's dump 003122, 2026-11-03). So a pixel that was
// reached, but lies between two pixels of one nearer surface on opposite sides,
// is a crack in that surface all the same: the nearest such pair, if its keys
// agree within 3% and both are nearer than this pixel's by more than that.
bool covered_crack(int2 p, uint keyHere, out int2 from, out uint packed)
{
    from = p;
    packed = 0u;
    uint best = 0u;
    [unroll] for (int i = 0; i < 8; i += 2) {
        const int2 a = p + kPairs[i], b = p + kPairs[i + 1];
        uint ka, kb, pa, pb;
        const bool ra = reached(a, ka, pa);
        const bool rb = reached(b, kb, pb);
        if (!ra || !rb) continue;
        const bool aNear = ka >= kb;
        const uint kn = aNear ? ka : kb, kf = aNear ? kb : ka;
        if (float(kn - kf) > 0.03f * float(kn)) continue;
        if (float(kf) <= 1.03f * float(keyHere)) continue;
        if (kn > best) { best = kn; from = aNear ? a : b; packed = aNear ? pa : pb; }
    }
    return packed != 0u;
}

// A crack's source: its neighbour's, stepped on by the same offset while that
// is still the same surface.
uint2 crack_step(uint2 src, int2 at, int2 from)
{
    uint2 stepped = src;
    const int2 s = int2(src) + (at - from);
    if (s.x >= 0 && s.y >= 0 && s.x < (int)texSize.x && s.y < (int)texSize.y) {
        const float dn = SrcDepth[depth_px(src)].r;
        const float ds = SrcDepth[depth_px(uint2(s))].r;
        if (abs(ds - dn) <= 0.03f * dn + 1.0e-7f) stepped = uint2(s);
    }
    return stepped;
}

// What destination pixel `at` shows of source pixel src: the source itself,
// except mirror glass, which shows what the rotation-only base has at `at` when
// that is glass in the retained frame too (housing_motion()).
float4 source_colour(uint2 src, uint2 at)
{
    uint2 from = src;
    if (hasMotion > 0.5f && rotH0.w > 0.5f && is_glass(depth_px(src))) {
        const float3 pt = float3((float2(at) + 0.5f) / texSize, 1.0f);
        const float w = dot(rotH2.xyz, pt);
        const float2 uv = float2(dot(rotH0.xyz, pt), dot(rotH1.xyz, pt)) / max(w, 1.0e-5f);
        const int2 s = int2(floor(uv * texSize));
        if (w > 1.0e-5f && s.x >= 0 && s.y >= 0 && s.x < (int)texSize.x && s.y < (int)texSize.y) {
            if (is_glass(depth_px(uint2(s)))) from = uint2(s);
        }
    }
    return SrcColor[from];
}

// THE MOTION-VECTOR OVERLAY (motion_overlay.hpp): the colour from source pixel
// src, tinted by the motion it was moved by -- relative to the head, as it shows
// from the new camera -- and by which of motion_how()'s cases supplied it. Only
// on the motion path; anything moved as static world keeps its colour.
float4 overlay(uint2 src, uint2 at, float4 colour)
{
    if (!(debugMotion > 0.5f) || !(hasMotion > 0.5f)) return colour;
    const uint2 mp = depth_px(src);
    const float d = SrcDepth[mp].r;
    if (!(d > minValidDepth)) return colour;
    uint how;
    float3 m;
    const bool moved = surface_motion(mp, d, m, how);
    if (!moved || how == 0u) return colour;
    const float2 uv = (float2(src) + 0.5f) / texSize;
    float4 world = mul(invOld, float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, d, 1.0f));
    if (!(abs(world.w) > 1e-12f)) return colour;
    world /= world.w;
    float3 a, b;
    float2 px = float2(1.0e4f, 0.0f);
    if (to_ndc(newVP, float4(world.xyz, 1.0f), a) &&
        to_ndc(newVP, float4(world.xyz + m - headMove, 1.0f), b))
        px = (b.xy - a.xy) * 0.5f * texSize;
    const float4 o = motion_colour(px, how, at);
    return float4(lerp(colour.rgb, o.rgb, o.a), colour.a);
}

[numthreads(8, 8, 1)]
void CS_Clear(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)texSize.x || tid.y >= (uint)texSize.y) return;
    Key[tid.xy] = 0u;
    Idx[tid.xy] = 0u;
}

[numthreads(8, 8, 1)]
void CS_ScatterDepth(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)texSize.x || tid.y >= (uint)texSize.y) return;
    int2 dst; uint key;
    if (!project(tid.xy, dst, key)) return;
    InterlockedMax(Key[uint2(dst)], key);
}

[numthreads(8, 8, 1)]
void CS_ScatterIndex(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)texSize.x || tid.y >= (uint)texSize.y) return;
    int2 dst; uint key;
    if (!project(tid.xy, dst, key)) return;
    // Only the winner of pass 1 gets to publish its coordinate. Storing +1 on
    // both axes keeps 0 meaning "no source reached this pixel".
    if (Key[uint2(dst)] != key) return;
    Idx[uint2(dst)] = ((tid.y + 1u) << 16) | (tid.x + 1u);
}

[numthreads(8, 8, 1)]
void CS_Resolve(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)texSize.x || tid.y >= (uint)texSize.y) return;
    uint packed = Idx[tid.xy];
    if (packed == 0u) {
        // COMPOSITE, NOT REPLACE: a destination no source reached keeps whatever
        // the caller already put there -- the rotation-warped image. That is what
        // makes this strictly an improvement rather than a different set of
        // holes. Except a crack (above), which takes its neighbour's source,
        // stepped on by the same offset while that is still the same surface.
        int2 from;
        if (!fill_cracks() || !crack_source(int2(tid.xy), from, packed)) {
            // THE OVERLAY'S HOLES: nothing landed here, so the pixel keeps the
            // caller's rotation-only image, a frame old. Marked magenta on every
            // other pixel, so that image still shows through.
            if (debugMotion > 0.5f && hasMotion > 0.5f && ((tid.x + tid.y) & 1u) == 0u)
                Dst[tid.xy] = float4(1.0f, 0.0f, 1.0f, 1.0f);
            return;
        }
        const uint2 src = crack_step(unpack_src(packed), int2(tid.xy), from);
        Dst[tid.xy] = overlay(src, tid.xy, source_colour(src, tid.xy));
        return;
    }
    uint2 src = unpack_src(packed);
    if (fill_cracks()) {
        int2 from;
        uint np;
        if (covered_crack(int2(tid.xy), Key[tid.xy], from, np))
            src = crack_step(unpack_src(np), int2(tid.xy), from);
    }
    Dst[tid.xy] = overlay(src, tid.xy, source_colour(src, tid.xy));
}
)";

struct State {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11ComputeShader> csClear, csDepth, csIndex, csResolve;
    ComPtr<ID3D11Buffer> cb;

    ComPtr<ID3D11Texture2D>           key, idx;
    ComPtr<ID3D11UnorderedAccessView> keyUav, idxUav;
    UINT w = 0, h = 0;

    GpuTimer timer;
} g;

struct CB {
    float invOld[16];
    float newVP[16];
    float newVPRigid[16];
    // Field order MUST match the HLSL cbuffer above, register for register.
    float texSize[2];
    float minValidDepth;
    float hasMask;
    float rigidSkip;
    float farDepth;
    float depthScale[2];
    float depthSize[2];
    float hasMotion;
    float hasMirror;
    float headMove[3];
    float debugMotion;
    float rotH[12];   // three rows of four: the homography, and rotH[3] its flag
};
static_assert(sizeof(CB) % 16 == 0, "constant buffer must be 16-byte aligned");

bool ensure_shaders(ID3D11Device* dev)
{
    if (g.csClear && g.csDepth && g.csIndex && g.csResolve && g.cb) return true;

    // The overlay's colour encoding goes in front: one definition for both eyes.
    const std::string src = std::string(motionoverlay::kColourHlsl) + kHlsl;
    auto build = [&](const char* entry, ComPtr<ID3D11ComputeShader>& out) -> bool {
        ComPtr<ID3DBlob> code, err;
        const HRESULT hr = D3DCompile(src.data(), src.size(), "reproject",
                                      nullptr, nullptr, entry, "cs_5_0", 0, 0,
                                      &code, &err);
        if (FAILED(hr)) {
            VRLOG("reproject: %s compile FAILED: %s", entry,
                  err ? (const char*)err->GetBufferPointer() : "(no message)");
            return false;
        }
        return SUCCEEDED(dev->CreateComputeShader(code->GetBufferPointer(),
                                                  code->GetBufferSize(), nullptr, &out));
    };
    if (!build("CS_Clear", g.csClear) || !build("CS_ScatterDepth", g.csDepth) ||
        !build("CS_ScatterIndex", g.csIndex) || !build("CS_Resolve", g.csResolve)) {
        g.csClear.Reset(); g.csDepth.Reset();
        g.csIndex.Reset(); g.csResolve.Reset();
        return false;
    }

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(CB);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &g.cb))) {
        VRLOG("reproject: constant buffer creation FAILED");
        g.csClear.Reset(); g.csDepth.Reset();
        g.csIndex.Reset(); g.csResolve.Reset();
        return false;
    }
    VRLOG("reproject: 6-DoF stale-eye shaders compiled");
    return true;
}

bool ensure_scratch(ID3D11Device* dev, UINT w, UINT h)
{
    if (g.key && g.idx && g.w == w && g.h == h) return true;
    g.keyUav.Reset(); g.idxUav.Reset(); g.key.Reset(); g.idx.Reset();
    g.w = g.h = 0;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &g.key)) ||
        FAILED(dev->CreateTexture2D(&td, nullptr, &g.idx)) ||
        FAILED(dev->CreateUnorderedAccessView(g.key.Get(), nullptr, &g.keyUav)) ||
        FAILED(dev->CreateUnorderedAccessView(g.idx.Get(), nullptr, &g.idxUav))) {
        VRLOG("reproject: scatter buffer creation FAILED (%ux%u)", w, h);
        g.keyUav.Reset(); g.idxUav.Reset(); g.key.Reset(); g.idx.Reset();
        return false;
    }
    g.w = w; g.h = h;
    return true;
}

} // namespace

bool composite(ID3D11DeviceContext* ctx,
               ID3D11ShaderResourceView* srcColorSrv,
               ID3D11ShaderResourceView* srcDepthSrv,
               ID3D11UnorderedAccessView* dstUav,
               unsigned width, unsigned height,
               const Params& params)
{
    if (!ctx || !srcColorSrv || !srcDepthSrv || !dstUav || !width || !height)
        return false;

    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    if (!ensure_shaders(dev.Get())) return false;
    if (!ensure_scratch(dev.Get(), width, height)) return false;

    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    CB* cb = static_cast<CB*>(m.pData);
    for (int i = 0; i < 16; ++i) cb->invOld[i] = params.invOldViewProj[i];
    for (int i = 0; i < 16; ++i) cb->newVP[i] = params.newViewProj[i];
    for (int i = 0; i < 16; ++i) cb->newVPRigid[i] = params.newViewProjRigid[i];
    cb->texSize[0] = (float)width;
    cb->texSize[1] = (float)height;
    cb->minValidDepth = params.minValidDepth;
    cb->farDepth      = params.farDepth;
    cb->hasMask = params.rigidMaskSrv ? 1.0f : 0.0f;
    cb->rigidSkip = params.rigidSkip ? 1.0f : 0.0f;
    cb->depthScale[0] = params.depthWidth  ? (float)params.depthWidth  / (float)width  : 1.0f;
    cb->depthScale[1] = params.depthHeight ? (float)params.depthHeight / (float)height : 1.0f;
    cb->depthSize[0]  = (float)(params.depthWidth  ? params.depthWidth  : width);
    cb->depthSize[1]  = (float)(params.depthHeight ? params.depthHeight : height);
    cb->hasMotion     = (params.motionSrv && params.motionOwnSrv) ? 1.0f : 0.0f;
    cb->hasMirror     = (cb->hasMotion > 0.5f && params.mirrorMaskSrv) ? 1.0f : 0.0f;
    for (int i = 0; i < 3; ++i) cb->headMove[i] = params.headMove[i];
    cb->debugMotion   = params.debugMotion ? 1.0f : 0.0f;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) cb->rotH[r * 4 + c] = params.rotH[r * 3 + c];
        cb->rotH[r * 4 + 3] = 0.0f;
    }
    cb->rotH[3] = params.haveRotH ? 1.0f : 0.0f;
    ctx->Unmap(g.cb.Get(), 0);

    // Save the compute stage, same contract as dibr::warp().
    ComPtr<ID3D11ComputeShader>       oldCs;
    ComPtr<ID3D11Buffer>              oldCb;
    ComPtr<ID3D11ShaderResourceView>  oldSrv[6];
    ComPtr<ID3D11UnorderedAccessView> oldUav[3];
    ctx->CSGetShader(&oldCs, nullptr, nullptr);
    ctx->CSGetConstantBuffers(0, 1, oldCb.GetAddressOf());
    ctx->CSGetShaderResources(0, 6, oldSrv[0].GetAddressOf());
    ctx->CSGetUnorderedAccessViews(0, 3, oldUav[0].GetAddressOf());

    // t2 may be null: the shader gates on hasMask rather than on the bind, so
    // a missing mask reads as "no truck anywhere" and the depth band alone
    // decides -- which is exactly the pre-mask behaviour.
    // t3/t4 likewise: hasMotion is only set when both are there.
    ID3D11ShaderResourceView*  srvs[6] = {srcDepthSrv, srcColorSrv,
                                          params.rigidMaskSrv, params.motionSrv,
                                          params.motionOwnSrv, params.mirrorMaskSrv};
    ID3D11UnorderedAccessView* uavs[3] = {g.keyUav.Get(), g.idxUav.Get(), dstUav};
    ID3D11Buffer* cbs[1] = {g.cb.Get()};
    ctx->CSSetConstantBuffers(0, 1, cbs);
    ctx->CSSetShaderResources(0, 6, srvs);
    ctx->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);

    const UINT gx = (width + 7) / 8, gy = (height + 7) / 8;

    g.timer.begin(ctx);

    // Cleared by a dispatch rather than ClearUnorderedAccessViewUint so both
    // scratch buffers are zeroed inside the same barrier chain as the scatters
    // that follow.
    ctx->CSSetShader(g.csClear.Get(), nullptr, 0);
    ctx->Dispatch(gx, gy, 1);
    ctx->CSSetShader(g.csDepth.Get(), nullptr, 0);
    ctx->Dispatch(gx, gy, 1);
    ctx->CSSetShader(g.csIndex.Get(), nullptr, 0);
    ctx->Dispatch(gx, gy, 1);
    ctx->CSSetShader(g.csResolve.Get(), nullptr, 0);
    ctx->Dispatch(gx, gy, 1);

    g.timer.end(ctx);

    ID3D11UnorderedAccessView* restoreUav[3] = {
        oldUav[0].Get(), oldUav[1].Get(), oldUav[2].Get()};
    ID3D11ShaderResourceView* restoreSrv[6] = {oldSrv[0].Get(), oldSrv[1].Get(),
                                               oldSrv[2].Get(), oldSrv[3].Get(),
                                               oldSrv[4].Get(), oldSrv[5].Get()};
    ID3D11Buffer* restoreCb[1] = {oldCb.Get()};
    ctx->CSSetUnorderedAccessViews(0, 3, restoreUav, nullptr);
    ctx->CSSetShaderResources(0, 6, restoreSrv);
    ctx->CSSetConstantBuffers(0, 1, restoreCb);
    ctx->CSSetShader(oldCs.Get(), nullptr, 0);
    return true;
}

void release()
{
    g.keyUav.Reset(); g.idxUav.Reset(); g.key.Reset(); g.idx.Reset();
    g.w = g.h = 0;
}

float gpu_ms() { return g.timer.last_ms(); }

float take_gpu_ms() { return g.timer.take_mean_ms(); }

} // namespace reproject
