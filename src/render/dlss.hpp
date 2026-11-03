#pragma once

struct ID3D11DeviceContext;
struct ID3D11Texture2D;

// NVIDIA DLSS IN PLACE OF THE GAME'S OWN TAA RESOLVE.
//
// SnowRunner already renders everything DLSS asks for (measured from RenderDoc
// captures, docs/dlss_investigation.md): jittered HDR colour, reversed-Z depth,
// a velocity buffer for moving objects, and a camera-reprojection matrix for
// the rest -- all bound to its TAA resolve (PS "EDGE_AA"), which runs on the
// IMMEDIATE context. So the resolve's draw is taken over: the game's own
// inputs, turned into DLSS motion vectors by one compute pass, go through DLSS.
//
// TWO WAYS, by mode:
//
//   DLAA (render size = output size): DLSS runs AT the resolve, on the HDR
//   colour, and its result lands in the render target the resolve would have
//   written. Everything after it -- bloom, tonemapping, the UI -- runs
//   unchanged.
//
//   UPSCALING (Quality ... Ultra Performance; VR only): the game runs SMALLER
//   than the headset image -- the window is sized to the mode's render size
//   (resolution_hook.cpp), the one place the engine takes its size from, as the
//   game starts or live -- and DLSS brings each eye up to the full size at
//   PRESENT, on the finished frame. It cannot run at the resolve: the post chain
//   after it is sized to the render, so a larger image there would have nowhere
//   to go. At
//   the resolve the vectors and depth are still built, and the game's TAA blend
//   is skipped (the frame passes through unblended), because DLSS wants the raw
//   jittered image. The HUD is not in that frame -- in VR it is drawn into its
//   own layer (ui_layer.hpp) -- so DLSS sees only the scene, as it must.
//
// IN VR the game renders one eye per frame, so every "previous frame" it knows
// -- its velocity buffer, its previous camera -- is the other eye's. DLSS keeps
// one feature, one history, per eye there, and the motion vectors are rebuilt
// to point back to the SAME eye's last render (see kMvHlsl in dlss.cpp); the
// flat screen is eye 0.
//
// Needs the game's TAA switched ON (that is what renders the jitter and the
// velocity buffer), the DLSS SDK at build time (CMakeLists.txt fetches it) and
// nvngx_dlss.dll next to SnowRunner.exe, which players download themselves.
namespace dlss {

enum Mode : int {
    kOff = 0, kDlaa, kQuality, kBalanced, kPerformance, kUltraPerformance, kModeCount
};

int  mode();
void set_mode(int m);
bool mode_available(int m);
const char* mode_name(int m);                    // for the panel
const char* mode_key(int m);                     // config spelling: "off", "dlaa", ...
bool mode_from_key(const char* key, int& out);

// Quality and below: the game renders smaller than the headset image.
bool is_upscaling(int m);

// The size the game renders at for mode `m` and a headset image of outW x outH:
// the standard DLSS ratios (Quality 2/3, Balanced 0.58, Performance 1/2, Ultra
// Performance 1/3), each side rounded to a multiple of 4. Multiples of 4,
// because that is how the engine sizes its own render targets -- at 1902x977 it
// rendered 1904x980 and rescaled in its final pass -- and the vectors DLSS gets
// at Present have to be on the same grid as the frame. Off and DLAA return the
// output size.
void render_size_for(int m, int outW, int outH, int& rw, int& rh);

// THE DLSS MODEL ("render preset"). 0 = DLSS's own choice (K for DLAA in this
// runtime); otherwise the SDK's preset number -- J 10, K 11, L 12, M 13. The
// others are deprecated or reserved in SDK 310 and are not offered. Takes
// effect by recreating the DLSS feature, with a history reset.
enum Preset : int { kPresetDefault = 0, kPresetJ = 10, kPresetK = 11, kPresetL = 12,
                    kPresetM = 13 };
int  preset();
void set_preset(int p);
const char* preset_key(int p);                   // "default", "J", "K", "L", "M"
bool preset_from_key(const char* key, int& out);

// SHARPENING after DLSS, 0 - 1, 0 = off: AMD's contrast adaptive sharpening
// (render/sharpen.hpp) on DLSS's image -- after the upscale, or with DLAA on the
// finished frame, which has been through the game's tonemapping by then. Takes
// effect at the next Present.
float sharpness();
void  set_sharpness(float s);

// This build was made with the SDK. False = no DLSS in this DLL at all.
bool compiled_in();

// One line for the settings panel: what it is doing, or why it is not.
const char* status();

// Debug: flip the sign of the vertical jitter handed to DLSS. Session only. A
// wrong sign reads as a soft or faintly shaking image when standing still.
bool jitter_flip_y();
void set_jitter_flip_y(bool on);

// From ui_hook.cpp's Draw detour, when the bound pixel shader is the TAA
// resolve on the immediate context. True: the resolve's output has been
// written (DLAA's result, or the unblended frame when upscaling), so the
// caller must NOT draw. False: let the game's TAA draw as usual.
bool replace_taa_resolve(ID3D11DeviceContext* ctx, unsigned vertexCount);

// From xr_mirror.cpp, once per Present that shows a new render: DLSS's image of
// the frame in `backbuffer`, sharpened by sharpness() -- upscaled to the headset
// image size, or with DLAA the frame itself. Null when there is no image of
// DLSS's to show differently -- DLSS off, nothing it ran on this frame (no TAA:
// menus, loading), or DLAA with sharpening off -- and the caller shows the
// backbuffer as before. `eye` is the eye the caller is routing the frame to;
// the vectors were built for the eye the render itself recorded, and a
// disagreement is counted in the log. The texture stays valid until the next
// call.
ID3D11Texture2D* image_for_present(ID3D11DeviceContext* ctx, ID3D11Texture2D* backbuffer,
                                   int eye);

// Once per Present, after image_for_present(): the periodic log line, and
// releasing DLSS once it is off.
void on_present();

// The mod just resized the game. What failed at the old size gets another try.
void on_resized();

// Before the process exits, while the device still exists (see shutdown_xr).
void shutdown();

} // namespace dlss
