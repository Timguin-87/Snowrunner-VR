#pragma once
#include <dxgi.h>

struct ID3D11Texture2D;   // pre_hud_texture(); avoids pulling d3d11.h in here

// Locating the HUD without editing the game's own draw calls -- and, since the
// UI moved onto its own composition layer, taking it out of the render at the
// draw. See render/ui_layer.hpp for where those pixels go.
//
// The HUD's rendering is baked into a command list. An early reading of this
// was that the list is recorded ONCE, before this DLL can observe it, with zero
// FinishCommandList calls ever seen -- SUPERSEDED: with the deferred vtable
// hooked separately from the immediate one (they are distinct, see the note in
// ui_hook.cpp), 15000+ FinishCommandList calls per session are observed and the
// engine re-records constantly. That is what makes shader-identity tagging of a
// recording possible at all. What IS still live and
// mutable, though, are the RESOURCES that recording binds -- constant buffers,
// render targets -- since those are GPU objects, not baked values. If
// ExecuteCommandList runs with restoreState=FALSE (the common case), the
// device context still holds whatever the HUD's draws left bound immediately
// after the replay, inspectable via plain Get-calls with no interception of
// the original Set-calls needed. A pixel-diff extraction approach was tried
// and abandoned: under AER, consecutive frames render DIFFERENT eyes, so
// diffing them picks up the whole scene's parallax, not just the HUD.
namespace hooks {

bool install_ui_hook(IDXGISwapChain* swapchain);

// Call once per frame from the Present hook.
void ui_hook_on_present(IDXGISwapChain* swapchain);


// The backbuffer as it stood just before the HUD was drawn -- the frame without
// its UI. Null when DIBR shift is off, before the first capture,
// on any frame where the capture landed in the wrong place (see the
// retrospective validation in ui_hook_on_present), and -- the common case now --
// whenever the UI is being redirected to its own quad layer, since a backbuffer
// the UI never reached IS the pre-HUD frame. Callers must handle null by
// falling back to the finished backbuffer.
//
// DIBR shift reprojects this so the HUD is not dragged sideways by the scene depth
// behind it. See the long note at the capture site for why the capture POINT is
// the difficult part.
ID3D11Texture2D* pre_hud_texture();


// Hides the HUD entirely (known-good fallback for e.g. screenshots). Toggled
// at runtime from the in-game settings UI, and by the config file.
bool hide_hud_enabled();
void set_hide_hud_enabled(bool on);

} // namespace hooks
