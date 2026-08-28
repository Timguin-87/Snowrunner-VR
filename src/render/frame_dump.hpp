#pragma once
#include <d3d11.h>
#include <cstdint>

// Dumps the exact per-eye images being handed to the headset, so eye content
// can be compared frame by frame outside the headset. PageUp captures a
// 20-frame HALF-res burst (bound in xr_mirror.cpp); the Advanced tab's button
// takes a single FULL-res stereo pair. One request captures the next N frames
// for BOTH eyes, written side by side, left eye on the left.
//
// JPEG via WIC, which ships with Windows, so no encoder dependency. Bursts
// halve the resolution to keep them a sane size on disk -- see fullRes below
// for when that is the wrong trade.
namespace framedump {

// Arms a burst of `frames` frames (both eyes each). Safe to call while a burst
// is already running -- it restarts it.
//
// fullRes keeps every pixel instead of point-decimating 2:1. The halved image is
// fine for comparing the two eyes against each other, which is what bursts are
// for, but it both destroys and ALIASES fine detail -- so any question about
// SHARPNESS (soft edges, UI legibility, compositor upscaling) has to be asked at
// full resolution or the answer is an artefact of the dump. Pair it with a small
// frame count: one stereo pair at a 2688-square canvas is ~43 MB uncompressed.
void request(int frames = 5, bool fullRes = false);

// True while a burst is in progress.
bool active();

// Index of the frame currently being captured -- the N in "fN_LR.jpg". Lets
// other subsystems log their per-frame decisions under the same number, so a
// visible artefact in one image can be joined to the branch that produced it
// instead of being reasoned about backwards. Meaningless unless active().
int frame_index();

// Records `src` as this frame's content for `eye`. No-op unless a burst is
// active. Call once per eye per frame, with the image actually submitted for
// that eye. Eye buffers persist across frames, so an eye that is not blitted
// this frame keeps the image the headset is still showing for it.
void capture(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, uint32_t eye);

// Call once per frame after both eyes have been captured. Writes the stereo
// pair side by side (left eye left, right eye right) as
// "<dll dir>/dibr_dump/<HHMMSS>/f<frame>_LR.jpg" and advances the burst.
void end_frame();

} // namespace framedump
