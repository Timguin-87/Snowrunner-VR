#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include <cstdint>

// In-game settings overlay (Dear ImGui). Opens with a rebindable keyboard key
// (default Insert) or a rebindable gamepad button combo (default L3+R3);
// lets the user change the render options and orbit horizon lock live, and rebind
// both toggles. Every change is written straight to Snowrunner_VR_config.txt
// via vrcfg::save() -- no separate Apply step. Visible both on the desktop
// mirror AND, while open, in the headset as a separate XR quad composition
// layer (xr_mirror.cpp) -- the same already-built ImGui draw data feeds both
// targets, never rebuilt twice per Present.
namespace hooks {

// Lazy, idempotent install (WndProc subclass + ImGui/D3D11 backend). Call
// every Present; a no-op after the first successful call.
void install_menu_hook(IDXGISwapChain* swapchain);

// Called every Present, after install_menu_hook() and BEFORE
// xr::mirror_on_present() (see swapchain_hook.cpp): polls XInput once,
// edge-detects the keyboard/gamepad toggle (and drives an active rebind
// capture, if any), and -- while open -- builds this Present's ImGui draw
// data (NewFrame/Render) WITHOUT rendering it to any target yet. Must run
// before mirror_on_present() so the draw data is ready in time for that
// function to (conditionally) submit it as a headset-visible quad layer.
void menu_hook_update();

// Called every Present, after xr::mirror_on_present() (preserves the
// existing ordering guarantee: that function's AER desktop-eye-mirror trick
// does a raw CopyResource onto the backbuffer, which would stomp/flicker
// anything drawn before it). Renders the draw data menu_hook_update() already
// built this Present into the swapchain's current backbuffer, if the menu is
// open. No-op otherwise.
void menu_render_to_desktop(IDXGISwapChain* swapchain);

// True (with the desired pixel size) if the menu is open and has draw data
// ready to render this Present -- i.e. menu_hook_update() already ran. Lets
// xr_mirror.cpp decide whether to prepare a quad layer at all this Present
// without needing to know anything about ImGui itself.
bool menu_wants_quad(uint32_t& outW, uint32_t& outH);

// The settings panel's own current pixel rect within the full desktop-sized
// draw data (captured from ImGui::GetWindowPos()/GetWindowSize() in
// draw_settings_window()) -- lets the quad layer crop to just the panel
// instead of submitting a mostly-empty, desktop-window-sized texture. Only
// meaningful when menu_wants_quad() is true.
bool menu_panel_rect(int& x, int& y, int& w, int& h);

// Renders this Present's already-built draw data (see menu_hook_update())
// into an arbitrary render target -- e.g. an XR swapchain image wrapped in
// an RTV -- clearing it to transparent first (unlike menu_render_to_desktop(),
// there's no pre-existing frame content to preserve on a standalone overlay
// texture). Viewport/projection follow the draw data's own captured
// DisplaySize, so w/h should match the full desktop draw-data size returned
// by menu_wants_quad(), not the cropped panel rect.
void menu_render_to_extra_target(ID3D11RenderTargetView* rtv, ID3D11DeviceContext* ctx);

// Called from the ResizeBuffers detour: drops the cached backbuffer RTV so
// it's rebuilt lazily against the new size.
void menu_hook_on_resize();

bool is_menu_open();

// The rebindable menu-toggle key/combo (config-file-persisted).
int      menu_toggle_vk();
void     set_menu_toggle_vk(int vk);
uint32_t menu_toggle_gamepad_combo();
void     set_menu_toggle_gamepad_combo(uint32_t xinputButtonMask);

// Size of the mod's own settings window/font (1.0 = 100%, unscaled).
// Config-file / in-game-slider tunable; clamped to [0.5, 3.0].
float mod_ui_scale();
void  set_mod_ui_scale(float scale);

} // namespace hooks
