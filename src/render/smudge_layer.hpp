#pragma once
#include <d3d11.h>
#include <cstdint>

// The mud, rain and snow drawn ON the windscreen, captured on its own so it can
// be placed at the depth it actually occupies.
//
// THE PROBLEM. Splatter sits on the glass, an arm's length away, but the glass
// writes no depth -- so the depth buffer at those pixels holds the terrain far
// beyond the cab. DIBR then shifts the splatter by the terrain's disparity,
// which is nearly zero, while your eyes expect the disparity of something
// almost touching your face. The two eyes disagree by a large margin and never
// fuse: the splatter reads as permanently doubled. Hiding it (kCullWindowSmudge)
// solves the doubling by removing the effect, which works but costs the effect.
//
// THE FIX. Suppress the draw on the main target exactly as the cull does, but
// record it here first. The rendered eye then gets this layer composited back
// verbatim -- pixel-identical to what the game drew -- and the synthesized eye
// gets it composited with a UNIFORM horizontal shift equal to the disparity of
// the glass. Splatter is stuck to a surface at one roughly constant distance,
// so one shift describes all of it; no per-pixel depth is needed, which is
// fortunate because none exists.
//
// The scene behind the glass is unaffected either way: it is reprojected by its
// own real depth, with the splatter no longer sitting on top of it confusing
// the result.
//
// DEPTH VIEW KEPT, unlike uilayer. The UI is screen-space and wants no depth
// test, but splatter is geometry on a surface the dashboard and A-pillars
// genuinely occlude -- dropping the DSV would paint it over the cab interior.
// The game's own depth-write state comes along with the draw, which for glass
// is writes-off, so recording it here cannot disturb the scene depth.
//
// PREMULTIPLIED, for the same reason uilayer is: the capture target is
// transparent, so alpha has to accumulate as coverage (SrcBlendAlpha=ONE)
// rather than being squared by ordinary SrcAlpha blending. Composited as
// dst = s.rgb + dst * (1 - s.a).
namespace smudgelayer {

// True once the target exists and the feature is on. Every capture is inert
// otherwise, and the draw falls through to whatever the cull setting says.
bool active();

// ALWAYS ON where it applies, since 2026-08-24. It was a checkbox on the
// grounds that reprojecting the splatter is a behaviour change; it is not --
// without it the mud sits at the depth of the terrain BEYOND the cab, which is
// simply wrong, and the only reason to leave it off was to compare. active()
// above is the whole condition: the target exists and we are in gameplay.
// (Its caller adds one more -- xr::dibr_shift_enabled() -- since nothing else
// reprojects anything for this to correct.)

// (alpha_gamma() and brightness() lived here: an alpha curve and a colour
// multiply applied at composite time, exposed as two sliders, on the theory
// that the splatter is captured BEFORE the engine's colour grading and so
// lands too strong. Removed 2026-08-24 along with the shader terms. Nobody
// could say what the right values were, and a knob whose correct setting is
// unknown is a way to make the image wrong on purpose. The layer is now
// composited exactly as the game drew it.)

// How far the splatter sits from the captured glass depth, in metres, as a
// signed offset applied to the reprojection distance. Positive = farther.
//
// MEASURED IN THE HEADSET at -0.015 m, i.e. the mud reprojects 15 mm NEARER
// than the depth we captured for the pane. Note the sign came out opposite to
// the obvious "we keep the nearest surface, so we capture the inside of the
// glass" reasoning -- so this is an empirical constant, not a derived one, and
// whatever the 15 mm really is (pane modelling, a decal depth bias) it is not
// simply glass thickness.
//
// Without it the shift is slightly too large on the SYNTHESIZED eye only, so
// the error alternates with the rendered eye and reads as a small jitter on the
// smudge rather than a static offset. Was briefly a slider; tuned once, found
// to have a sharp null, and baked in.
float depth_offset();

// --- glass depth ---------------------------------------------------------
// The windscreen HAS no depth in the game's buffer: glass renders without
// depth writes, which is the root of the whole problem -- the depth at those
// pixels belongs to the terrain beyond the cab, so DIBR moves anything drawn on
// the glass by the terrain's disparity instead of the glass's.
//
// But the glass GEOMETRY is still submitted. So we draw it a second time into a
// depth target of our own with writes FORCED ON, and get the true per-pixel
// depth of the windscreen surface -- the thing the game declined to record.
// The splatter can then be displaced by real disparity that varies across the
// screen, instead of one guessed constant.
//
// It also DECIDES WHAT SURVIVES. Anything in the captured layer with no glass
// depth behind it is dropped: those pixels are the flat window planes and the
// overall tint, which no single distance can place (the left and right windows
// sit at genuinely different depths) and which comparison against AER shows are
// not drawn where the game puts them anyway. What is left is the splatter,
// resting on the surface we measured.
//
// Fed by BOTH the glass and the splatter shader roles, and driven from ahead of
// the cull, so the depth is captured even when the glass itself is hidden.
//
// Reverse-Z, cleared to 0 (infinitely far) and written with GREATER so the
// NEAREST surface wins where panes overlap. No colour target is bound, so this
// pass writes depth and nothing else.
struct Saved {
    ID3D11RenderTargetView* rtv[8] = {};
    UINT                    rtvCount = 0;
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11BlendState*       blend = nullptr;
    float                   blendFactor[4] = {};
    UINT                    sampleMask = 0;
    // Only used by the depth capture, which has to override the game's
    // depth-stencil state (glass draws with writes off, which is the point).
    ID3D11DepthStencilState* dss = nullptr;
    UINT                     stencilRef = 0;
    bool                    valid = false;
};

bool begin_capture(ID3D11DeviceContext* ctx, Saved& saved);
void end_capture(ID3D11DeviceContext* ctx, Saved& saved);

// The glass-depth pass described above.
bool begin_depth_capture(ID3D11DeviceContext* ctx, Saved& saved);
void end_depth_capture(ID3D11DeviceContext* ctx, Saved& saved);

// The captured windscreen depth, or null. Same projection as the scene, so
// z_view = projB / (d - projA) with the frame's own projection constants.
ID3D11ShaderResourceView* depth_srv();

// Clears the layer for the next frame. Call from Present AFTER both eyes have
// composited it.
void clear(ID3D11DeviceContext* ctx);

// The captured splatter, or null.
ID3D11ShaderResourceView* srv();

void update(ID3D11Device* dev, uint32_t canvasW, uint32_t canvasH, bool dibrActive);
void shutdown();

} // namespace smudgelayer
