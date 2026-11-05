#pragma once
#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

// AMD FSR, FOR THE UPSCALING TAB'S FSR 3 AND FSR 4 (render/dlss.hpp runs it).
//
// THE UPSCALER IS AMD'S OWN, in the two DLLs of AMD's FSR SDK that players put
// next to SnowRunner.exe themselves, as they do nvngx_dlss.dll for DLSS:
// amd_fidelityfx_loader_dx12.dll, and amd_fidelityfx_upscaler_dx12.dll, which
// carries FSR 4.x, 3.1.x and 2.x together. The mod builds against four MIT
// headers of the SDK (CMakeLists.txt fetches them) and ships nothing of AMD's.
// FSR 4 runs on the Radeon cards AMD lists for it; elsewhere -- NVIDIA and Intel
// included -- only FSR 3 is offered, and a request for 4 runs 3.
//
// DIRECTX 12 ONLY, and the game is DirectX 11. So FSR runs on a D3D12 device of
// its own, on the game's adapter, and the two meet in a few shared textures --
// made by D3D11, opened by D3D12 -- and one shared fence, which is what every
// DX11 integration of it does. Per upscale, all
// on the game's immediate context: copy the inputs into the shared textures,
// signal; the D3D12 queue waits for that, runs FSR and signals back; the D3D11
// context waits for that on the GPU -- the CPU never blocks -- and copies the
// result out. Nothing of the game's own D3D11 work is held up.
//
// The inputs are DLSS's, built by render/dlss.cpp the same way for both: the
// colour, the depth (reversed, R32), the motion vectors rebuilt per eye (render
// pixels, jitter-free) and the game's jitter. One FSR context per eye, each its
// own history, as DLSS keeps one feature per eye.
namespace fsr {

// This build has the FSR SDK's headers in it. False = no FSR in this DLL.
bool compiled_in();

// The D3D12 side and AMD's loader, brought up on first use against the game's
// device. False with the reason in why() -- no AMD DLLs beside the game, no
// D3D12 on this machine.
bool ensure(ID3D11Device* dev);
const char* why();

// A start that failed gets another go at the next ensure() -- the DLLs may have
// been put in place since. From any thread; the upscaling tab calls it when FSR
// is picked.
void retry();

// One eye's FSR context for this shape of work, created when anything differs
// (which empties its history -- `created` says so). `version` is 3 or 4; a 4
// this GPU does not offer runs as 3, and version_name() says which is running.
bool ensure_context(int eye, int version, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh,
                    bool hdr, bool* created);
const char* version_name(int eye);   // as AMD's DLL names it, e.g. "4.1.2"

struct Eval {
    int              eye = 0;
    ID3D11Texture2D* colour = nullptr;   // render size: the finished frame, or HDR at the resolve
    ID3D11Texture2D* depth = nullptr;    // render size, R32_FLOAT, reversed (1 near)
    ID3D11Texture2D* motion = nullptr;   // render size, R16G16_FLOAT, render pixels
    ID3D11Texture2D* output = nullptr;   // ow x oh, UAV-capable format
    uint32_t w = 0, h = 0, ow = 0, oh = 0;
    float    jitterX = 0.0f, jitterY = 0.0f;   // render pixels, x right, y down
    bool     reset = false;
    bool     hdr = false;                // linear HDR colour, else the gamma-encoded frame
    float    nearZ = 0.0f, farZ = 0.0f, vfovRad = 0.0f;
    float    frameMs = 0.0f;             // since this eye's last upscale
};
// On the game's immediate context. The context must already exist
// (ensure_context). False on any failure, logged.
bool evaluate(ID3D11DeviceContext* ctx, const Eval& e);

// The contexts and the shared textures -- FSR not in use. The D3D12 device and
// AMD's loader stay up for the next time.
void release();

// Before the process exits, while the game's device still exists.
void shutdown();

} // namespace fsr
