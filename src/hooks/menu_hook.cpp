#include "hooks/menu_hook.h"
#include "hooks/viewbuild_hook.h"
#include "hooks/camera_fov.h"
#include "hooks/camera_hook.h"
#include "hooks/ui_hook.h"
#include "hooks/shader_cull.h"
#include "hooks/depth_probe.h"
#include "hooks/cbuffer_hook.h"
#include "hooks/resolution_hook.h"
#include "hooks/input_block.h"
#include "common/log.h"
#include "common/config.h"
#include "render/dibr.hpp"
#include "render/frame_dump.hpp"
#include "render/ui_layer.hpp"
#include "render/smudge_layer.hpp"
#include "xr/xr_mirror.h"

#include <d3d11.h>
#include <wrl/client.h>
#include <windows.h>
#include <xinput.h>

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include <MinHook.h>

#include <atomic>
#include <algorithm>
#include <cfloat>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <string>

using Microsoft::WRL::ComPtr;

// imgui_impl_win32.h intentionally comments this declaration out (to avoid
// pulling <windows.h> into a header meant to be included from anywhere) and
// says to copy it into the .cpp that calls it.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace hooks {
namespace {

// --- install state ---------------------------------------------------

std::atomic<bool> g_installed{false};
HWND    g_hwnd = nullptr;
WNDPROC g_origWndProc = nullptr;
ComPtr<ID3D11Device>           g_device;
ComPtr<ID3D11DeviceContext>    g_ctx;
ComPtr<ID3D11RenderTargetView> g_backbufferRtv;

// Guards the shared ImGuiContext against being touched from both the
// message-pump thread (WndProc) and the Present thread at once -- Dear
// ImGui's own FAQ requires a lock for cross-thread same-context use, and
// this codebase already has the identical hazard elsewhere (xr_mirror.cpp's
// g_poseLock, for the same reason: the camera-hook thread differs from the
// Present thread).
//
// CRITICAL_SECTION, not this project's usual SRWLOCK: ImGui_ImplWin32_WndProcHandler
// calls SetCapture()/ReleaseCapture() on mouse button messages, which
// synchronously sends WM_CAPTURECHANGED back through this SAME window's
// WndProc on the SAME thread before the original call returns -- i.e.
// Detour_WndProc legitimately re-enters itself. SRWLOCK's exclusive mode is
// NOT reentrant (a thread re-acquiring it deadlocks itself -- this is
// exactly what caused every mouse click to freeze the game).
// CRITICAL_SECTION supports same-thread reentrancy by design while still
// blocking a genuinely different thread, which is the actual property
// needed here.
CRITICAL_SECTION g_imguiLock;

std::atomic<bool> g_menuOpen{false};

// FIRST-RUN RESOLUTION, and the one thing in this file that acts on its own.
//
// The render resolution is consumed at CreateWindowExW, long before any OpenXR
// runtime exists -- so the config cannot be born with the right value, and
// nothing can apply a new one to the running game (see the note where
// apply_resolution_now() used to live in resolution_hook.cpp). The best that
// can be done is to fill it in the moment a runtime IS reachable, write it, and
// say plainly that the game has to be restarted for it to take.
//
// Runs from the Present tick rather than from a draw, because the menu is
// almost certainly closed when this fires and ImGui only builds draw data while
// it is open -- so this opens the menu itself. That is deliberate: a notice the
// player has to already be looking at the settings to see would never be seen
// on the launch it matters for.
std::atomic<bool> g_firstRunNotice{false};   // popup owed
bool              g_firstRunPopupShown = false;
uint32_t          g_firstRunW = 0, g_firstRunH = 0;

void check_first_run_resolution()
{
    if (!vrcfg::resolution_unset()) return;

    uint32_t nativeW = 0, nativeH = 0;
    if (!xr::native_eye_size(nativeW, nativeH) || !nativeW) return;

    // 100% of the headset's own per-eye WIDTH, squared and rounded to 8 the
    // same way the slider does -- so the slider reads exactly 100% afterwards
    // instead of 99 or 101 from a value it did not produce itself.
    uint32_t e = (nativeW + 7u) & ~7u;
    if (e < 640u) e = 640u;

    hooks::set_force_resolution((int)e, (int)e);
    vrcfg::save();              // clears resolution_unset() on the way through
    g_firstRunW = e; g_firstRunH = e;
    g_firstRunNotice.store(true);
    g_menuOpen.store(true);     // nothing draws while it is closed
    VRLOG("resolution: first run -- wrote %ux%u to the config from the headset's "
          "native %ux%u per eye; a restart is needed for the game to be created "
          "at that size", e, e, nativeW, nativeH);
}

// --- input blocking while the menu is open --------------------------------
// SnowRunner reads gameplay input by direct polling, not window messages --
// confirmed in testing: keys and gamepad buttons still reached the game while
// the menu was open despite Detour_WndProc already swallowing
// WM_KEY*/WM_MOUSE* messages below.
//
// THE GAMEPAD HALF IS NOT DONE HERE. input_block.cpp owns it, and its header
// carries what was actually measured about how this game reaches the pad.
// This file only POLLS the pad, for menu navigation and the toggle combo, and
// it does so through hooks::xinput_poll_self() so its own reads pass through
// the block that is silencing the game.
//
// THE KEYBOARD HALF IS UNSOLVED. The game polls GetAsyncKeyState, and hooking
// that process-wide crashed the game on startup three times -- twice with
// Steam Input and the Steam overlay ruled out, so RTSS (confirmed present and
// hooking this process; swapchain_hook.cpp has logged RTSSHooks64.dll as a
// ResizeBuffers caller in the same session) is the remaining suspect. MinHook
// reported success every time, so it was never a resolve or create failure.
//
// Those detours are gone rather than left switched off. Whatever revisits this
// needs a diagnostic first -- dump the target's first bytes before patching, to
// see whether something has already redirected it -- and a dead #if 0 block was
// not helping anyone do that. Keyboard input reaches the game while the menu is
// open until then.

// --- rebindable toggle -------------------------------------------------

std::atomic<int>      g_menuToggleVk{VK_INSERT};
std::atomic<uint32_t> g_menuToggleCombo{XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB};

// --- mod UI scale (font + widget size) ----------------------------------

std::atomic<float> g_modUiScale{1.0f};
// Un-rescaled style, captured once right after StyleColorsDark() in
// install_menu_hook(); every scale change rebuilds GetStyle() from this
// baseline (ScaleAllSizes() is NOT idempotent -- reapplying it to an
// already-scaled style every frame would compound multiplicatively).
ImGuiStyle g_baseStyle;
float g_lastAppliedUiScale = -1.0f;   // force-apply on the first present

// PRESENT RATE. Measured here rather than read from ImGui's io.Framerate
// because menu_hook_update() runs on EVERY Present, including while the menu is
// closed -- so this is the game's real frame rate, not the rate with the
// overlay up and ImGui's own frame timing running.
std::atomic<float> g_fps{0.0f};

void tick_fps()
{
    static LARGE_INTEGER freq{}, last{};
    static int frames = 0;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (last.QuadPart == 0) { last = now; frames = 0; return; }
    ++frames;
    const double dt = double(now.QuadPart - last.QuadPart) / double(freq.QuadPart);
    if (dt >= 0.5) {          // twice a second: settled enough to read, quick to react
        g_fps.store((float)(frames / dt));
        last = now;
        frames = 0;
    }
}

void apply_ui_scale_if_changed()
{
    const float s = g_modUiScale.load();
    if (s == g_lastAppliedUiScale)
        return;
    ImGui::GetStyle() = g_baseStyle;
    ImGui::GetStyle().ScaleAllSizes(s);
    ImGui::GetIO().FontGlobalScale = s;
    g_lastAppliedUiScale = s;
}

// --- rebind capture state (touched only from the Present-hook thread,
// inside menu_hook_update -- no lock needed) ------------------------

enum class RebindTarget { kNone, kKeyboard, kGamepad };
RebindTarget g_rebindTarget = RebindTarget::kNone;
bool         g_captureBaseline[256] = {};   // keys already down when kKeyboard capture began
unsigned     g_captureComboSeen = 0;        // buttons OR'd together since kGamepad capture began

void begin_keyboard_capture()
{
    for (int vk = 0; vk < 256; ++vk)
        g_captureBaseline[vk] = (::GetAsyncKeyState(vk) & 0x8000) != 0;
    g_rebindTarget = RebindTarget::kKeyboard;
}

void begin_gamepad_capture()
{
    g_captureComboSeen = 0;
    g_rebindTarget = RebindTarget::kGamepad;
}

// --------------------------------------------------------------------------
// WndProc subclass
// --------------------------------------------------------------------------

LRESULT CALLBACK Detour_WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    const bool open = g_menuOpen.load();

    // Only feed ImGui while actually open. ImGui_ImplWin32_WndProcHandler
    // queues input events internally (mouse position/buttons, key state)
    // and NewFrame() is what drains that queue -- but NewFrame() only ever
    // runs while the menu is open (menu_hook_update). Feeding it
    // unconditionally meant every ordinary game click/keypress while the
    // menu was closed sat in ImGui's queue, stale, and got played back
    // against whatever widget happened to be under that position the
    // instant the menu reopened -- exactly the "changes apply as soon as I
    // press Insert" bug. Not feeding it at all while closed means the queue
    // starts clean every time the menu opens.
    if (open) {
        EnterCriticalSection(&g_imguiLock);
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
        LeaveCriticalSection(&g_imguiLock);
    }

    // Modal while open: swallow keyboard/mouse from the game entirely.
    // Everything else (WM_SIZE, WM_ACTIVATE, WM_DEVICECHANGE, ...) always
    // passes through so window management keeps working regardless of menu
    // state. NOTE: if SnowRunner reads mouse-look via raw input (WM_INPUT)
    // rather than WM_MOUSEMOVE, that path isn't covered here -- not
    // confirmed to be in use, so not preemptively handled.
    if (open &&
        ((msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
         (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST)))
        return 0;

    return CallWindowProcW(g_origWndProc, hwnd, msg, wParam, lParam);
}

// --------------------------------------------------------------------------
// ImGui / D3D11 backend lifecycle
// --------------------------------------------------------------------------

bool ensure_backbuffer_rtv(IDXGISwapChain* swapchain)
{
    if (g_backbufferRtv)
        return true;
    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)))
        return false;
    return SUCCEEDED(g_device->CreateRenderTargetView(bb.Get(), nullptr, &g_backbufferRtv));
}

// --------------------------------------------------------------------------
// Gamepad -> ImGui navigation. Targets the current, non-deprecated API
// (io.AddKeyEvent/AddKeyAnalogEvent + ImGuiKey_Gamepad*), not the pre-1.88
// io.NavInputs[] array -- mirrors imgui_impl_win32.cpp's own (disabled, see
// IMGUI_IMPL_WIN32_DISABLE_GAMEPAD in CMakeLists.txt) UpdateGamepads().
// --------------------------------------------------------------------------

void feed_gamepad_nav(const XINPUT_GAMEPAD& gp, bool connected)
{
    ImGuiIO& io = ImGui::GetIO();
    if (connected) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    else           io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    if (!connected)
        return;

    auto btn = [&](ImGuiKey key, WORD mask) { io.AddKeyEvent(key, (gp.wButtons & mask) != 0); };
    btn(ImGuiKey_GamepadStart,     XINPUT_GAMEPAD_START);
    btn(ImGuiKey_GamepadBack,      XINPUT_GAMEPAD_BACK);
    btn(ImGuiKey_GamepadFaceLeft,  XINPUT_GAMEPAD_X);
    btn(ImGuiKey_GamepadFaceRight, XINPUT_GAMEPAD_B);
    btn(ImGuiKey_GamepadFaceUp,    XINPUT_GAMEPAD_Y);
    btn(ImGuiKey_GamepadFaceDown,  XINPUT_GAMEPAD_A);
    btn(ImGuiKey_GamepadDpadLeft,  XINPUT_GAMEPAD_DPAD_LEFT);
    btn(ImGuiKey_GamepadDpadRight, XINPUT_GAMEPAD_DPAD_RIGHT);
    btn(ImGuiKey_GamepadDpadUp,    XINPUT_GAMEPAD_DPAD_UP);
    btn(ImGuiKey_GamepadDpadDown,  XINPUT_GAMEPAD_DPAD_DOWN);
    // L1/R1 deliberately NOT forwarded: they drive the tab strip (see
    // cycle_tab()). ImGui maps them to its tweak-slow/tweak-fast modifiers,
    // which would otherwise change the value of whatever slider is focused on
    // the very press that is switching away from it.
    btn(ImGuiKey_GamepadL3,        XINPUT_GAMEPAD_LEFT_THUMB);
    btn(ImGuiKey_GamepadR3,        XINPUT_GAMEPAD_RIGHT_THUMB);

    auto analogBtn = [&](ImGuiKey key, BYTE v, int thresh) {
        io.AddKeyAnalogEvent(key, v > thresh, v > thresh ? (v - thresh) / float(255 - thresh) : 0.0f);
    };
    analogBtn(ImGuiKey_GamepadL2, gp.bLeftTrigger,  XINPUT_GAMEPAD_TRIGGER_THRESHOLD);
    analogBtn(ImGuiKey_GamepadR2, gp.bRightTrigger, XINPUT_GAMEPAD_TRIGGER_THRESHOLD);

    auto stick = [&](ImGuiKey keyLo, ImGuiKey keyHi, SHORT v, int deadzone) {
        float f = (std::abs((int)v) > deadzone)
            ? (std::abs((int)v) - deadzone) / float(32767 - deadzone) : 0.0f;
        io.AddKeyAnalogEvent(keyLo, v < -deadzone, v < -deadzone ? f : 0.0f);
        io.AddKeyAnalogEvent(keyHi, v >  deadzone, v >  deadzone ? f : 0.0f);
    };
    stick(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, gp.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
    stick(ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadLStickUp,    gp.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
}

// --------------------------------------------------------------------------
// Settings window content
// --------------------------------------------------------------------------

// Render resolution, expressed as a percentage of the OpenXR runtime's own
// recommended per-eye WIDTH and applied as a SQUARE window (the vertical
// recommendation is deliberately ignored).
//
// Square is a deliberate choice, not an oversight, and an aspect-preserving
// variant was tried and reverted 2026-08-14: a square canvas is a known,
// controllable quantity that this mod has been developed and tested against on
// every screen (gameplay, map, garage, menus), whereas letting the window take
// an arbitrary per-headset aspect would ship an untested render and UI shape to
// anyone whose headset reports a different one.
//
// The consequence, now that eye_frustum_half_angles() declares the frustum from
// the HORIZONTAL FOV, is visible and intended: the image always spans the
// display's full width and is never cropped left/right, while vertically it
// letterboxes on a portrait-ish per-eye FOV (Quest 3) and is cropped on a
// landscape one. That is the honest result of rendering a square -- we have no
// content for a portrait headset's extra vertical angle, and the old formula's
// only way of filling it was to declare a window the content was never drawn
// for.
//
// This is the one setting here that cannot take effect live. The pixel size is
// consumed at CreateWindowExW, during startup, before an OpenXR instance
// exists -- so the slider cannot compute it in time for the run you are
// sitting in. What it does instead is write the resolved pixel size into the
// config (ForceGameResolution), where the next launch reads it early enough.
// The percentage is therefore not stored at all: it is derived back from
// pixels / native width each time the menu opens, so the slider always shows
// what is really configured rather than a number that could drift out of
// agreement with it (e.g. after switching headsets, where the same pixel size
// is a different percentage).
void draw_resolution_scale()
{
    uint32_t nativeW = 0, nativeH = 0;
    const bool haveNative = xr::native_eye_size(nativeW, nativeH);

    // Snapped to a multiple of 8 (safe for RT alignment / block formats) and
    // floored at the config parser's own minimum, so every value this writes
    // survives a save/load round trip.
    auto edge_for = [&](int pct) -> uint32_t {
        uint32_t e = (uint32_t)((float)nativeW * (float)pct / 100.0f + 0.5f);
        e = (e + 7u) & ~7u;
        return e < 640u ? 640u : e;
    };

    // Derived once, the first frame the runtime's native size is known, and
    // owned by the slider from then on. Re-deriving every frame would fight
    // the drag, since the config only holds the last committed value.
    static int s_pct = -1;
    if (haveNative && s_pct < 0) {
        const int cur = hooks::force_resolution_w();
        s_pct = (cur > 0) ? (int)((float)cur * 100.0f / (float)nativeW + 0.5f) : 100;
        s_pct = (s_pct < 50) ? 50 : (s_pct > 150 ? 150 : s_pct);
    }
    int pct = (s_pct < 0) ? 100 : s_pct;

    ImGui::BeginDisabled(!haveNative);
    if (ImGui::SliderInt("Render resolution", &pct, 50, 150, "%d%%")) {
        s_pct = pct;
        const uint32_t e = edge_for(pct);
        hooks::set_force_resolution((int)e, (int)e);
        vrcfg::save();
    }
    ImGui::EndDisabled();

    // AllowWhenDisabled so the explanation is still reachable in the state
    // where the user most needs it -- no runtime up yet.
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (!haveNative) {
            ImGui::SetTooltip("No OpenXR runtime reachable yet.\n"
                              "Start the headset/runtime and reopen this menu.");
        } else {
            const uint32_t e = edge_for(pct);
            // The "currently rendering" line is the only way to see at a
            // glance whether a restart is still owed for the chosen value.
            //
            // Compared against what the CONFIG actually asks for, not against
            // the square size this slider would write. ForceGameResolution is a
            // free WxH -- hand-editing it to a non-square (e.g. the headset's
            // own aspect, which the frustum declaration handles correctly) is a
            // supported thing to do, and testing against `e` would have told
            // anyone who did that they still owed a restart forever.
            const int cfgW = hooks::force_resolution_w();
            const int cfgH = hooks::force_resolution_h();
            uint32_t curW = 0, curH = 0;
            char now[80];
            if (xr::render_canvas_wh(curW, curH)) {
                const bool matches = (cfgW > 0 && cfgH > 0)
                    ? ((int)curW == cfgW && (int)curH == cfgH)
                    : (curW == e && curH == e);
                snprintf(now, sizeof(now), "%ux%u%s", curW, curH,
                         matches ? "  (matches)" : "  (restart pending)");
            } else {
                snprintf(now, sizeof(now), "(unknown)");
            }

            ImGui::SetTooltip("Headset native: %ux%u per eye (width is what scales).\n"
                              "Game window becomes: %ux%u\n"
                              "Currently rendering: %s\n"
                              "\n"
                              "A RESTART IS REQUIRED. The game builds its window\n"
                              "once, at launch, and sizes its swapchain from it --\n"
                              "it never re-reads that size, so there is no way to\n"
                              "apply a new resolution to the running game. Moving\n"
                              "this slider saves to Snowrunner_VR_config.txt and\n"
                              "takes effect the next time you launch.\n"
                              "\n"
                              "Square by design. The image always spans the full\n"
                              "width of the display; vertically it letterboxes if\n"
                              "your headset's per-eye view is taller than wide.",
                              nativeW, nativeH, e, e, now);
        }
    }

}

void draw_settings_tab()
{
    // Re-snaps the recenter origin (yaw AND position) to wherever the head is
    // now. The position half is the one that matters here: the startup origin
    // is captured on the first tracked frame, which is often while the headset
    // is still on a desk or being put on, and every frame after is measured as
    // an offset from it -- fed to the camera as a lateral/forward LEAN, i.e. a
    // view that is slid off-centre rather than rotated. The runtime's own
    // recenter does not reset this: it redefines the runtime's LOCAL space,
    // while this origin is our own snapshot taken inside it.
    // Pitch and roll are deliberately untouched (see update_head_pose): they
    // should always read as the headset's true absolute tilt.
    ImGui::TextDisabled("%.0f FPS", g_fps.load());

    if (ImGui::Button("Recenter view", ImVec2(-1.0f, 0.0f)))
        xr::request_recenter();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Face the direction you want as forward, then click.\n"
                          "Sets the game's forward yaw to where you are looking,\n"
                          "re-centres your position, and re-pins the UI plane if\n"
                          "it is set to Forward. Pitch and roll always follow the\n"
                          "headset, so the horizon stays put. Same as the HOME key.\n\n"
                          "Nothing else moves the yaw: a camera change or truck\n"
                          "swap re-centres your position only.");
    ImGui::Separator();

    // THE TWO RENDER OPTIONS, and there is no longer a third thing choosing
    // between them. The path is always AER -- one eye rendered per Present --
    // and these decide what the OTHER eye is made of:
    //
    //   the warp    brings that eye's own last real render to now
    //   the shift   reprojects the eye that WAS rendered into it, by depth
    //
    // They compose. The shift lands where it has depth and a source; the warp
    // fills every pixel it could not reach. So each is a checkbox with its own
    // detail control beside it, rather than a mode that excludes the other.
    {
        bool warpOn = xr::warp_enabled();
        if (ImGui::Checkbox("Stale eye warp", &warpOn)) {
            xr::set_warp_enabled(warpOn);
            vrcfg::save();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Bring the eye that was not rendered this frame to the\n"
                              "current instant, instead of resubmitting it unchanged.\n"
                              "\n"
                              "On by default. Turning it off is how you tell 'the warp\n"
                              "is wrong' from 'what it is warping is wrong' -- if a\n"
                              "judder survives with this off, the warp is not it.\n"
                              "\n"
                              "SAVED, so a measurement left switched off stays off on\n"
                              "the next launch. That is deliberate: a switch that puts\n"
                              "itself back is harder to notice than one left wrong.");

        if (warpOn) {
            ImGui::SameLine();
            int wtype = xr::warp_type();
            const char* warpLabels[xr::kWarpTypeCount] = {
                "Headset rotation only",
                "Headset + game rotation",
                "Headset + game 6dof" };
            ImGui::SetNextItemWidth(230.0f);
            if (ImGui::Combo("Warp type", &wtype, warpLabels, xr::kWarpTypeCount)) {
                xr::set_warp_type(wtype);
                vrcfg::save();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "A ladder, not three separate things -- each level contains\n"
                    "the one before it.\n"
                    "\n"
                    "HEADSET ROTATION ONLY is the headset pose at capture against\n"
                    "now. It is blind to the game camera, so stick look and vehicle\n"
                    "turns move the rendered eye and leave the stale one behind.\n"
                    "\n"
                    "HEADSET + GAME ROTATION builds it from the game's own view\n"
                    "matrix instead, which already has your head composed into it --\n"
                    "so it covers both, with no decomposition to get wrong. COCKPIT\n"
                    "ONLY: the orbit camera swings around the truck rather than\n"
                    "turning in place, which a rotation warp cannot represent, so\n"
                    "orbit always falls back to the headset pose.\n"
                    "\n"
                    "HEADSET + GAME 6DOF adds a reprojection by the camera's\n"
                    "TRANSLATION, using the retained frame's own depth -- rotation\n"
                    "is a homography and needs none, but a translation moves near\n"
                    "pixels further than far ones. In a driving game that is most of\n"
                    "what is left. It supersedes the rotation warp wherever it lands,\n"
                    "so the level above is what fills the pixels it cannot reach.\n"
                    "\n"
                    "Costs a second reprojection pass and the depth capture that\n"
                    "feeds it.");
        }

        bool shift = xr::dibr_shift_enabled();
        if (ImGui::Checkbox("DIBR shift", &shift)) {
            xr::set_dibr_shift_enabled(shift);
            vrcfg::save();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Reproject the eye that WAS rendered into the other one\n"
                              "using scene depth, so both eyes carry this frame's\n"
                              "content rather than one carrying the last frame's.\n"
                              "\n"
                              "Off by default: it is the only thing that pays for the\n"
                              "scene-depth capture, and where it cannot see a surface\n"
                              "it leaves a disocclusion hole -- which the stale eye\n"
                              "warp above then fills.");

        if (shift) {
            ImGui::SameLine();
            // The LIST order is not the mode numbering. Modes are numbered in
            // dibr.hpp by the order they were implemented; the dropdown is
            // ordered by how it is used -- the two fills you would actually play
            // with first, then the two diagnostics. kFillOrder[i] is the mode
            // the i-th entry selects, so going through it in both directions
            // means the stored value and the numbering do not change with the
            // presentation.
            static const int kFillOrder[4] = { 1, 2, 0, 3 };
            const char* fillLabels[4] = {
                "Stale eye, warped (default)",
                "Background stretch",
                "Magenta (show the holes)",
                "Disparity debug view" };
            const int current = dibr::fill_mode();
            int item = 0;
            for (int i = 0; i < 4; ++i)
                if (kFillOrder[i] == current) { item = i; break; }
            ImGui::SetNextItemWidth(230.0f);
            if (ImGui::Combo("Hole fill", &item, fillLabels, 4))
                dibr::set_fill_mode(kFillOrder[item]);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Not saved to the config -- it is a diagnostic, and two of the\n"
                    "four are unusable to actually play with.\n"
                    "\n"
                    "MAGENTA shows exactly which pixels the reprojection could not\n"
                    "reach. Speckle over near geometry means the disparity step\n"
                    "between neighbouring pixels is being read as a silhouette;\n"
                    "large clean bands beside objects are real disocclusions. It is\n"
                    "what separates the two things ghosting can be: holes in the\n"
                    "wrong PLACE, or holes filled with the wrong CONTENT. If the\n"
                    "ghosting persists with magenta on, look at the warp; if magenta\n"
                    "looks clean and only the filled image ghosts, it is the fill.\n"
                    "\n"
                    "STALE EYE is the default, and the only source that actually SAW\n"
                    "the disoccluded surface. It is one frame old and brought to now\n"
                    "by whatever Warp type is selected above.\n"
                    "\n"
                    "STRETCH never leaves the current frame so it cannot drift, but\n"
                    "it invents the surface it fills with. It is also what stale eye\n"
                    "falls back to when no retained eye is available.");
        }

    }

    ImGui::Separator();

    // The cab glass and the muck on it, by pixel-shader identity (shader_cull.h).
    // Both hurt most with DIBR shift on, but the glass is worth removing either
    // way: it is a refracting surface a few centimetres from the eyes, which is
    // awkward in stereo however the frame was produced. The smudge is specific
    // to the shift -- without it the other eye is a warped copy of a frame that
    // already had the splatters in the right place, so they fuse.
    bool disableWindows = hooks::cull_role_enabled(hooks::kCullWindows);
    if (ImGui::Checkbox("Disable windows", &disableWindows)) {
        hooks::set_cull_role_enabled(hooks::kCullWindows, disableWindows);
        vrcfg::save();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Skips the cab glass entirely. It refracts the scene behind it but\n"
            "writes no depth, so any reprojection moves the refracted image by\n"
            "the depth of the terrain beyond the cab instead of the glass's own.");
    if (disableWindows) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%d of %d shaders seen)",
                            hooks::cull_role_seen_count(hooks::kCullWindows),
                            hooks::cull_role_hash_count(hooks::kCullWindows));
    }

    // NO SMUDGE CONTROLS ANY MORE, and the reasoning is the same for all three
    // that used to be here.
    //
    // "Disable window smudge" hid the mud and rain because, under DIBR shift,
    // they inherit the depth of whatever is behind the glass and so land in the
    // wrong place in the synthesized eye. That was a workaround for a problem
    // the smudge layer now solves properly -- captured to its own target and
    // put back at the glass's own depth -- so hiding real content the game drew
    // is no longer a trade anyone needs to make. The role is baked and simply
    // never enabled.
    //
    // "Reproject window smudge" was the switch for that layer. It is not a
    // preference: with the shift on, not reprojecting puts the mud at the
    // terrain's depth, which is wrong rather than different. It is on whenever
    // the shift is.
    //
    // The alpha-curve and brightness sliders are gone with the shader terms
    // behind them -- see smudge_layer.hpp.
    ImGui::Separator();

    bool horizonLock = hooks::horizon_lock_enabled();
    if (ImGui::Checkbox("Orbit camera horizon lock", &horizonLock)) {
        hooks::set_horizon_lock_enabled(horizonLock);
        vrcfg::save();
    }

    // Cockpit counterparts to the orbit horizon lock above, levelled against
    // the TRUCK rather than the world -- the truck's tilt on hills is kept,
    // only the stick's own look input is dropped on the locked axis.
    bool truckLevelLock = hooks::truck_level_lock_enabled();
    if (ImGui::Checkbox("Cockpit lock truck level", &truckLevelLock)) {
        hooks::set_truck_level_lock_enabled(truckLevelLock);
        vrcfg::save();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Stick look-PITCH stops moving the view; head pitch\n"
                          "becomes the only source. Truck tilt on hills is kept.");

    bool truckYawLock = hooks::truck_yaw_lock_enabled();
    if (ImGui::Checkbox("Cockpit lock truck yaw", &truckYawLock)) {
        hooks::set_truck_yaw_lock_enabled(truckYawLock);
        vrcfg::save();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Stick look-YAW stops moving the view; head yaw\n"
                          "becomes the only source.");

    ImGui::Separator();

    // THE CULLING FOV, declared on combine::CAMERA itself (hooks/camera_fov.h).
    //
    // ALWAYS ON, and there is no switch any more. It replaced writing the game's
    // own FOV fields, which no longer happens at all: that value needed ~200 deg
    // before geometry stopped vanishing at the periphery and was already culling
    // small objects dead ahead by ~150, so no setting satisfied both ends.
    // m_fFOV is the field the engine genuinely builds its cull frustum from, and
    // at 1.00x the periphery is clear and what forward culling remains has moved
    // further out. Off would mean the game's own value, which is far too narrow
    // for a headset -- not a state worth being able to reach.
    //
    // The search that finds the camera looks after itself: it waits for
    // gameplay, takes one attempt per visit, and re-arms on its own if the
    // object is ever replaced. Its progress reads out on the Advanced tab.
    {
        float ff = hooks::camera_fov_factor();
        ImGui::SetNextItemWidth(230.0f);
        if (ImGui::SliderFloat("Culling FOV", &ff, 0.5f, 2.5f, "%.2fx")) {
            hooks::set_camera_fov_factor(ff);
            vrcfg::save();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Multiplier on the RENDERED FOV, written into\n"
                "combine::CAMERA::m_fFOV (+0x108, radians) -- the field the\n"
                "engine derives its cull frustum from. The game's own FOV\n"
                "fields are never touched, and what the renderer DRAWS is\n"
                "unaffected either way: PROJ LOCK owns the projection matrix\n"
                "independently of this.\n"
                "\n"
                "1.00x declares exactly what is drawn -- currently %.1f deg.\n"
                "Higher keeps more geometry alive at the edges of vision and\n"
                "costs draw calls.",
                hooks::camera_fov_result_deg());
    }

    ImGui::Separator();

    float modUiPct = mod_ui_scale() * 100.0f;
    if (ImGui::SliderFloat("Mod UI size", &modUiPct, 50.0f, 250.0f, "%.0f%%")) {
        set_mod_ui_scale(modUiPct / 100.0f);
        vrcfg::save();
    }

    // --- UI plane -----------------------------------------------------------
    // The game's HUD/menus/map are no longer rendered into the eye images at
    // all: they are captured to their own layer and submitted as a quad, which
    // is what these three controls place. The MAP ignores all of them by
    // design -- it is forced head-locked at full size so its screen-space
    // markers stay on the map features they point at.
    ImGui::TextDisabled("UI plane");

    // First, because it is the switch for whether the other three place
    // anything at all: the plane is where the game's UI goes, and this is
    // whether the game draws one.
    bool hideHud = hooks::hide_hud_enabled();
    if (ImGui::Checkbox("Hide HUD", &hideHud)) {
        hooks::set_hide_hud_enabled(hideHud);
        vrcfg::save();
    }

    float uiSize = xr::ui_plane_size();
    if (ImGui::SliderFloat("In-game UI size", &uiSize, 0.2f, 10.0f, "%.2f m")) {
        xr::set_ui_plane_size(uiSize);
        vrcfg::save();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Width of the UI plane, as a real size in metres.");

    float uiDist = xr::ui_plane_distance();
    if (ImGui::SliderFloat("UI distance", &uiDist, 0.3f, 20.0f, "%.2f m")) {
        xr::set_ui_plane_distance(uiDist);
        vrcfg::save();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How far away the plane sits, and therefore where the\n"
                          "UI converges. It is a real distance, so moving it back\n"
                          "makes it LOOK smaller too -- raise the size above to\n"
                          "get back to the same apparent size at a comfier depth.\n\n"
                          "The map screen follows this too, but only in DEPTH: its\n"
                          "size stays locked to the rendered view so the markers\n"
                          "keep lining up.");

    int uiMode = xr::ui_plane_mode();
    const char* uiModeLabels[] = { "Head locked", "Forward (pinned)" };
    if (ImGui::Combo("UI placement", &uiMode, uiModeLabels, 2)) {
        xr::set_ui_plane_mode(uiMode);
        vrcfg::save();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Head locked: follows you everywhere (the original\n"
                          "behaviour). Forward: stays where you put it in the\n"
                          "world, so you can look away from it.");

    ImGui::Separator();

    // BASE render size. Shrinks the rendered FOV and the compositor window
    // together, so lowering it draws less scene into fewer pixels with no
    // stretch -- the same mechanism the map screen uses, applied everywhere.
    // Everything downstream re-derives from it automatically (it lives inside
    // composited_fov_scale), including the map slider below, which is a
    // fraction OF this rather than of the headset's native FOV.
    float renderPct = xr::render_fov_scale() * 100.0f;
    if (ImGui::SliderFloat("Render size (FOV)", &renderPct, 30.0f, 100.0f, "%.0f%%")) {
        xr::set_render_fov_scale(renderPct / 100.0f);
        vrcfg::save();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Renders a narrower FOV into a smaller window.\n\n"
            "Cheaper than it looks: fewer pixels AND less scene, with no\n"
            "stretch, because the submitted FOV and the rendered FOV stay\n"
            "locked to each other. Costs peripheral vision, not sharpness.");

    float mapPct = xr::map_window_shrink() * 100.0f;
    if (ImGui::SliderFloat("Map screen size", &mapPct, 0.0f, 100.0f, "%.0f%%")) {
        xr::set_map_window_shrink(mapPct / 100.0f);
        vrcfg::save();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A fraction of the render size above, not of the\n"
                          "headset's native FOV -- the two multiply.");

    ImGui::Separator();
    draw_resolution_scale();

    ImGui::Separator();
    ImGui::Text("Menu key: %s", vrcfg::vk_name(menu_toggle_vk()).c_str());
    ImGui::SameLine();
    if (ImGui::Button("Rebind##key"))
        begin_keyboard_capture();

    ImGui::Text("Menu gamepad combo: %s", vrcfg::gamepad_combo_name(menu_toggle_gamepad_combo()).c_str());
    ImGui::SameLine();
    if (ImGui::Button("Rebind##pad"))
        begin_gamepad_capture();
}

// THE ADVANCED TAB: the controls that are diagnostics or one-time
// calibrations rather than things you would reach for while playing, plus the
// readouts that tell you whether the machinery underneath is working.
//
// Nothing here is second-guessed elsewhere -- these are the only copies of
// these controls, moved out of Settings to stop that tab reading like a list
// of everything the mod can do rather than a list of what you would change.
void draw_advanced_tab()
{
    // WHICH SCREEN THE MOD THINKS YOU ARE ON. It drives the render FOV, the HUD
    // shrink bypass and whether DIBR shift runs, so a wrong answer is visible
    // immediately -- which is why both classifiers stay live and only the one
    // that is READ changes here.
    {
        const char* gateLabels[2] = { "Legacy (camera pose)", "Direct markers" };
        int gate = hooks::screen_gate_direct() ? 1 : 0;
        ImGui::SetNextItemWidth(230.0f);
        if (ImGui::Combo("Screen gate", &gate, gateLabels, 2)) {
            hooks::set_screen_gate_direct(gate != 0);
            vrcfg::save();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "LEGACY reads the main camera's POSE: pitched -45 degrees near the\n"
                "world origin means the map. It needs nothing to be found first and\n"
                "works from the first frame, but gameplay can reach that pose, and\n"
                "when it does the map's reduced FOV arrives mid-drive.\n"
                "\n"
                "DIRECT MARKERS reads the game's own state instead, from two\n"
                "sources.\n"
                "\n"
                "THE MAP comes from five pixel shaders that draw on it and nowhere\n"
                "else, found with a probe and then baked in. All five must be\n"
                "drawing for the map to be selected, so a bad member shows up as a\n"
                "map that never fires rather than as an occasional wrong answer.\n"
                "\n"
                "EVERYTHING ELSE is separated by the drive-camera idle timer. No\n"
                "shader can tell the garage from gameplay -- the garage shares its\n"
                "interface with the menus and its world with gameplay -- and the\n"
                "engine's own camera-mode enum is no help either, because\n"
                "SnowRunner's garage does not tick the drive camera at all. That\n"
                "silence IS the signal, and the timer reads it directly.\n"
                "\n"
                "The menu had a marker of its own until it turned out to draw inside\n"
                "a level too, so it is answered the same way now.\n"
                "\n"
                "Both gates run either way, and the log reports what this one would\n"
                "say while the setting is still on Legacy.");
        ImGui::SameLine();
        ImGui::TextDisabled("-> %s%s", hooks::screen_name(hooks::direct_screen()),
                            hooks::direct_screen_positive() ? "" : " (fallback)");
        if (gate != 0)
            ImGui::TextDisabled("Map from its baked marker list; everything else "
                                "from the drive-camera idle timer.");
    }

    ImGui::Separator();

    // Degrees for the UI; xr::vertical_recenter()/set_vertical_recenter() are
    // radians, clamped to [-0.5, 0.5] there (matched here so the slider can't
    // show a range it'd immediately get clamped away from).
    constexpr float kRad2Deg = 180.0f / 3.14159265f;
    constexpr float kDeg2Rad = 3.14159265f / 180.0f;
    float vertRecenterDeg = xr::vertical_recenter() * kRad2Deg;
    if (ImGui::SliderFloat("Vertical recenter", &vertRecenterDeg, -0.5f * kRad2Deg, 0.5f * kRad2Deg, "%.1f deg")) {
        xr::set_vertical_recenter(vertRecenterDeg * kDeg2Rad);
        vrcfg::save();
    }
    // What the slider ADDS TO, so 0 is not mistaken for "no correction". The
    // headset's own vertical FOV asymmetry is applied automatically; this used
    // to be folded into the slider's -8 deg default, which was only ever right
    // for the machine it was tuned on.
    {
        const float autoDeg = xr::fov_center_pitch() * kRad2Deg;
        ImGui::TextDisabled("  + %.1f deg auto (headset FOV centre) = %.1f total",
                            autoDeg, autoDeg + vertRecenterDeg);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Your runtime reports its per-eye frustum, and it is not\n"
                "symmetric: this headset shows more below the optical axis\n"
                "than above it, so the middle of the panel is not straight\n"
                "ahead. That offset is read once and applied for you.\n"
                "\n"
                "0.0 here means the runtime has not reported a frustum yet.\n"
                "\n"
                "The slider above is preference on top. If you find yourself\n"
                "needing a large value again, the automatic term is wrong --\n"
                "check the fov_center_pitch line in the log.");
    }

    ImGui::Separator();

    // THE TWO CAPTURES, and they answer different questions -- which is why
    // both are here rather than one being the other with a number changed.
    //
    // The single shot keeps every pixel, because a question about SHARPNESS
    // (soft edges, UI legibility, compositor upscaling) cannot be asked of an
    // image that was point-decimated 2:1 on the way to disk -- the answer comes
    // back as an artefact of the dump. The burst halves resolution and takes 20
    // frames, because a question about MOTION needs consecutive frames far more
    // than it needs detail, and 20 full-res stereo pairs is a lot of disk for
    // one press.
    if (ImGui::Button("Stereo screenshot (full res)"))
        framedump::request(1, true);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Writes ONE full-resolution stereo pair, left eye on the\n"
                          "left, to dibr_dump/<HHMMSS>/f0_LR.jpg beside the DLL --\n"
                          "the exact images handed to the headset.");

    ImGui::SameLine();
    if (ImGui::Button("20-frame burst"))
        framedump::request(20);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Twenty consecutive stereo pairs at HALF resolution, to\n"
                          "dibr_dump/<HHMMSS>/f0_LR.jpg .. f19_LR.jpg. The same\n"
                          "thing the PageUp key does, which still works.\n"
                          "\n"
                          "This is the one for judder, ghosting and anything else\n"
                          "that only exists between frames: half resolution is\n"
                          "plenty to see an eye lagging the other one, and it\n"
                          "keeps a burst to a sane size on disk.\n"
                          "\n"
                          "This menu does not appear in them: it is submitted as\n"
                          "its own quad layer, and what gets captured is the eye\n"
                          "images underneath it.");

    ImGui::Separator();

    // Test knob, not a preference. Headsets with outward-angled panels (Index,
    // Bigscreen Beyond, some Pimax) report a per-eye ROTATION in XrView.pose
    // on top of the +/-IPD/2 translation; we read and apply it automatically,
    // but that path can't be exercised on a parallel-panel headset. This fakes
    // one. Because the same cant is applied to the render AND declared in the
    // submitted pose, the compositor undoes exactly what the render added:
    // dragging this slider should change NOTHING visually. If the world shifts
    // or the HUD stops fusing, something in the chain has a sign backwards.
    float cantDeg = xr::debug_eye_cant_deg();
    if (ImGui::SliderFloat("Eye cant (TEST)", &cantDeg, -15.0f, 15.0f, "%.1f deg")) {
        xr::set_debug_eye_cant_deg(cantDeg);
        vrcfg::save();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Diagnostic only -- leave at 0.\n"
                          "Simulates a canted-panel headset. A correct build\n"
                          "looks identical at every value.");

    ImGui::Separator();

    ImGui::Separator();

    // THE READOUTS. None of these is a control; they are the three places the
    // mod can be wrong about something without saying so on screen.
    ImGui::TextDisabled("Readouts");

    // Which screen the camera POSE says we are on, and whether the world is
    // actually being played -- the two booleans the legacy gate is built from,
    // shown raw so a disagreement with the marker gate above is visible.
    ImGui::TextDisabled("Map = %d      Gameplay = %d",
                        (int)hooks::map_by_pose(), (int)hooks::in_gameplay());

    // Where the stale-eye warp got its rotation from, counted since the last
    // reset. Only shown when a warp type that reads the game camera is
    // selected -- with headset-rotation-only there is no second source to
    // disagree with.
    if (xr::warp_uses_game_rot()) {
        xr::WarpSourceStats ws;
        xr::warp_source_stats(ws);
        // stale = the camera had not rebuilt since the retained eye's
        //         capture; a large share means the camera snapshot is
        //         slower than the submit rate.
        // orbit = not the cockpit camera, where a rotation-only warp is
        //         worse than none.
        // delta   should stay under a degree or so; a steady large value
        //         means the two sources disagree about which way the view
        //         turned, i.e. a wrong axis convention.
        ImGui::TextDisabled("  camera %u   stale %u   orbit %u   max delta %.2f deg",
                            ws.used, ws.staleSnapshot, ws.orbitDeclined,
                            ws.worstDisagreeDeg);
    }

    // How the culling-FOV camera search is getting on (hooks/camera_fov.h).
    // It runs itself -- waits for gameplay, one attempt per visit, re-arms if
    // the object is replaced -- so this is the only place that says whether it
    // ever succeeded.
    ImGui::TextDisabled("Culling FOV: %s", hooks::camera_fov_status());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "The camera object lives on the heap under ASLR, so there is no\n"
            "fixed address to record and it is found by sweeping for\n"
            "m_tmViewProj == m_tmView * m_tmProj -- sixteen floats that must\n"
            "be the exact product of thirty-two others.\n"
            "\n"
            "It only exists IN GAMEPLAY, so the search waits for the drive\n"
            "camera rather than wasting an attempt on a menu. Every write\n"
            "re-checks the object first, so a freed or recycled block starts\n"
            "a fresh search by itself.");
}

void draw_cull_tab()
{
    bool enabled = hooks::shader_cull_enabled();
    if (ImGui::Checkbox("Enable shader culling", &enabled)) {
        hooks::set_shader_cull_enabled(enabled);
        vrcfg::save();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(off = draw path untouched)");

    ImGui::TextWrapped(
        "Removes the windscreen so DIBR shift stops reprojecting the glass, and the "
        "mud/rain on it, by the depth of the terrain behind the cab -- which is "
        "what makes splatters sit at the wrong distance and refuse to fuse. "
        "Sit in the cockpit with the windscreen clearly in view before "
        "searching. Changes take a moment to show: the game bakes draws into "
        "command lists and re-records them continuously, so a suppressed shader "
        "keeps drawing until its list is rebuilt.");
    ImGui::Separator();

    const hooks::CullSearch s = hooks::cull_search_state();
    const int count = hooks::cull_shader_count();

    if (!s.active) {
        // The filter is the whole reason the search is usable. Searching every
        // shader blacks the frame out on most steps -- a step that includes the
        // final composite pass leaves nothing to judge "is the windscreen gone"
        // against, in the headset or on the desktop.
        int minElems  = (int)hooks::search_min_elems();
        int maxDraws  = (int)hooks::search_max_draws_frame();
        bool changed = false;
        ImGui::SetNextItemWidth(160.0f);
        changed |= ImGui::SliderInt("Min elements/draw", &minElems, 1, 64);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Excludes fullscreen passes (tonemap, composite, copy):\n"
                              "they draw 3-6 elements, real geometry draws more.\n"
                              "This is what stops a search step blacking the frame out.");
        ImGui::SetNextItemWidth(160.0f);
        changed |= ImGui::SliderInt("Max draws/frame", &maxDraws, 1, 512);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Excludes bulk geometry (terrain, foliage, shadow casters).\n"
                              "A windscreen is a handful of draws per frame.");
        if (changed) hooks::set_search_filter((unsigned)minElems, (unsigned)maxDraws);

        ImGui::Text("Shaders seen: %d    passing the filter: %d",
                    count, hooks::search_candidate_count());
        if (count <= 0) {
            ImGui::TextUnformatted("Nothing seen yet -- get into the game first.");
        } else if (ImGui::Button("Start search")) {
            hooks::cull_search_begin();
        }
    } else if (s.found) {
        ImGui::Text("Converged after %d answers: candidate #%d", s.step, s.foundIndex);
        ImGui::Text("Hash: %016llX", (unsigned long long)s.foundHash);
        if (s.foundHash == 0)
            ImGui::TextWrapped("This shader was created before the mod attached, so it "
                               "has no hash and cannot be saved -- it can still be ticked "
                               "below for this session.");
        ImGui::TextUnformatted("Only this one is suppressed now. Is the effect gone?");
        // Saving into a NAMED setting is the normal outcome, not the exception:
        // a game compiles permutations, so the second and third shader found
        // for the same effect belong alongside the first rather than in the
        // anonymous list, where they would answer to a different switch.
        //
        // Only the roles that are still OPEN are offered. The glass, its muck,
        // the UI and the mirrors are baked into the build and closed (see
        // shader_cull.h) -- add_cull_role_hash() would refuse anyway, so a
        // button for them could only mislead.
        if (s.foundHash != 0) {
            ImGui::TextUnformatted("Save it to:");
            bool firstRole = true;
            for (int r = 0; r < hooks::kCullRoleCount; ++r) {
                const hooks::CullRole role = (hooks::CullRole)r;
                if (!hooks::cull_role_extensible(role)) continue;
                char label[64];
                snprintf(label, sizeof(label), "%s (%d)",
                         hooks::cull_role_key(role), hooks::cull_role_hash_count(role));
                if (!firstRole) ImGui::SameLine();
                firstRole = false;
                if (ImGui::Button(label)) {
                    hooks::add_cull_role_hash(role, s.foundHash);
                    hooks::set_cull_role_enabled(role, true);
                    hooks::cull_search_cancel();
                    vrcfg::save();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Experimental list")) {
                hooks::set_cull_shader_culled(s.foundIndex, true);
                hooks::cull_search_cancel();
                vrcfg::save();
            }
        }
        if (ImGui::Button("Discard / search again")) hooks::cull_search_cancel();
    } else {
        ImGui::Text("Step %d -- suppressing %d of %d candidates (%d still in play)",
                    s.step + 1, s.testCount, s.total, s.hi - s.lo);
        ImGui::TextUnformatted("Is the effect gone?");
        if (ImGui::Button("Yes, gone"))     hooks::cull_search_answer(true);
        ImGui::SameLine();
        if (ImGui::Button("No, still there")) hooks::cull_search_answer(false);
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))        hooks::cull_search_cancel();
    }

    ImGui::Separator();
    // Zero here while a search is running means the cull is not reaching the
    // geometry at all -- a different problem from picking the wrong shader,
    // and worth being able to tell apart at a glance.
    ImGui::Text("Draws suppressed last frame: %u", hooks::suppressed_draws_last_frame());

    // The full list, for pinning something found another way (a RenderDoc
    // capture, or a hash from someone else's config). Sorted by nothing --
    // first-bind order is what the search indexes into, so re-ordering the
    // rows here would make the two disagree about what "#37" means.
    static bool showAll = false;
    ImGui::Checkbox("Show every shader", &showAll);
    ImGui::SameLine();
    ImGui::TextDisabled("(otherwise: candidates, culled and in-search rows)");

    if (ImGui::BeginTable("cull_table", 6,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_SizingStretchProp,
            ImVec2(0.0f, 220.0f))) {
        ImGui::TableSetupColumn("Cull");
        ImGui::TableSetupColumn("#");
        ImGui::TableSetupColumn("Hash (stable)");
        ImGui::TableSetupColumn("Pointer (session)");
        ImGui::TableSetupColumn("Draws/frame");
        ImGui::TableSetupColumn("Max elems");
        ImGui::TableHeadersRow();

        for (int i = 0; i < count; ++i) {
            hooks::CullShaderInfo info;
            if (!hooks::cull_shader_info(i, info)) continue;
            // Culled and in-search rows always show, so a mistake is never
            // hidden by the filter that was on when it was made.
            if (!showAll && !info.isCandidate && !info.culled && !info.inSearchRange) continue;

            ImGui::PushID(i);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            bool culled = info.culled;
            if (ImGui::Checkbox("##cull", &culled)) {
                hooks::set_cull_shader_culled(i, culled);
                vrcfg::save();
            }

            ImGui::TableSetColumnIndex(1);
            if (info.inSearchRange) ImGui::TextDisabled("%d*", i);
            else                    ImGui::Text("%d", i);

            ImGui::TableSetColumnIndex(2);
            if (info.hash) ImGui::Text("%016llX", (unsigned long long)info.hash);
            else           ImGui::TextDisabled("(pre-hook)");

            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%p", info.ps);

            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%u", info.drawsPerFrame);

            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%u", info.maxElems);

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// Settings panel's own pixel rect within the full desktop draw data, captured
// fresh each Present -- see menu_panel_rect() / the headset quad layer, which
// crops to just this instead of the whole (mostly empty) desktop-sized
// texture. AlwaysAutoResize means GetWindowSize() already reflects this
// frame's real laid-out size by the time we read it here, right before End().
std::atomic<int> g_panelX{0}, g_panelY{0}, g_panelW{0}, g_panelH{0};

// A POPUP IS ITS OWN WINDOW, and the headset quad crops to the settings
// panel's bounds -- so a popup laid out anywhere else is simply not in the
// picture. That is exactly what happened to the first-run notice: ImGui
// centres a modal on the VIEWPORT, which here is the whole desktop-sized
// texture, and only its bottom-left corner fell inside the crop.
//
// Two halves to the fix and both are needed. The popup is positioned over the
// panel (so it is inside the crop to begin with), and menu_panel_rect() unions
// this in (so a popup larger than the panel, or nudged off it, still cannot be
// cut off). Zeroed every frame the popup is not up.
std::atomic<int> g_popupX{0}, g_popupY{0}, g_popupW{0}, g_popupH{0};

// --- tab strip -----------------------------------------------------------
// Table-driven so the shoulder-button cycling in menu_hook_update() and the
// strip itself can never disagree about how many tabs exist or their order.
// The rigid-shader list, one checkbox per hash. A hand-built map of every
// vehicle in the game is going to contain wrong entries -- that is the expected
// failure, not a hypothetical one -- and the only way to find them is to take
// one out and look. Unticking mutes that hash: it keeps its place in the list
// but stops answering to the role, so it drops out of the 6-DoF reprojection's
// rigid mask AND out of the hide-rigid switch together, because both read the
// same role.
//
// Session only, deliberately. Nothing here is written to the config -- a value
// that survived a restart would turn an experiment into a setting, and the
// point is to decide what belongs in the BAKED list, which is a code change.
void draw_rigid_tab()
{
    constexpr hooks::CullRole kRole = hooks::kCullRigid;
    const int n     = hooks::cull_role_hash_count(kRole);
    const int baked = hooks::cull_role_builtin_count(kRole);

    ImGui::Text("%d shaders -- %d built in, %d found this install", n, baked, n - baked);
    ImGui::TextDisabled("Session only: none of this is saved. Untick to take a shader");
    ImGui::TextDisabled("out of the rigid role and see what changes.");

    ImGui::Separator();

    // THE ONLY COPY of this toggle now -- Settings had a second one with its
    // own tooltip, and the two are merged here rather than one being dropped.
    bool hideRigid = hooks::cull_role_enabled(kRole);
    if (ImGui::Checkbox("Hide rigid shaders (mask check)", &hideRigid))
        hooks::set_cull_role_enabled(kRole, hideRigid);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Suppresses every un-muted shader below, so what VANISHES is\n"
            "exactly what the 6-DoF reprojection treats as rigid -- and what\n"
            "is LEFT is what the mask does not cover.\n"
            "\n"
            "That second reading is the one that matters: an unmapped truck\n"
            "part looks exactly like a correctly-masked one, it just warps a\n"
            "little. Sit in the cockpit and look for truck that refuses to\n"
            "disappear -- that is the geometry still getting the camera's\n"
            "translation. Then find its shader in the list below and tick it in.\n"
            "\n"
            "While it is on the mask itself is empty (the culled draws never\n"
            "reach the marking pass), so the warp behaves as if nothing were\n"
            "rigid. Untick it to judge the result.\n"
            "\n"
            "NOT SAVED: it resets on the next launch, so leaving it on cannot\n"
            "cost you an invisible truck later.");

    ImGui::SameLine();
    ImGui::TextDisabled("(%d seen)", hooks::cull_role_seen_count(kRole));

    ImGui::SameLine();
    if (ImGui::Button("All on"))
        for (int i = 0; i < n; ++i) hooks::set_cull_role_hash_muted(kRole, i, false);
    ImGui::SameLine();
    if (ImGui::Button("All off"))
        for (int i = 0; i < n; ++i) hooks::set_cull_role_hash_muted(kRole, i, true);

    ImGui::Separator();

    // "seen" is the column that makes this usable: an entry never bound this
    // session cannot be the cause of anything on screen, so it needs no test.
    ImGui::TextDisabled("hash                 seen   origin");
    ImGui::BeginChild("rigidlist", ImVec2(0.0f, 0.0f), true);
    for (int i = 0; i < n; ++i) {
        const uint64_t h = hooks::cull_role_hash_at(kRole, i);
        bool on = !hooks::cull_role_hash_muted(kRole, i);
        char label[64];
        snprintf(label, sizeof(label), "%016llX##rigid%d", (unsigned long long)h, i);
        if (ImGui::Checkbox(label, &on))
            hooks::set_cull_role_hash_muted(kRole, i, !on);
        ImGui::SameLine(220.0f);
        if (hooks::cull_role_hash_seen(kRole, i)) ImGui::TextUnformatted("seen");
        else                                      ImGui::TextDisabled("-");
        ImGui::SameLine(280.0f);
        ImGui::TextDisabled(i < baked ? "built in" : "found");
    }
    ImGui::EndChild();
}

struct MenuTab { const char* name; void (*draw)(); };
constexpr MenuTab kTabs[] = {
    { "Settings",    &draw_settings_tab },
    { "Advanced",    &draw_advanced_tab },
    { "Shader Cull", &draw_cull_tab },
    { "Rigid",       &draw_rigid_tab },
};
constexpr int kTabCount = (int)(sizeof(kTabs) / sizeof(kTabs[0]));

// g_activeTab is learned from ImGui rather than assumed, so a tab picked with
// the mouse stays the reference point for the next LB/RB press. Both are
// touched only from the Present-hook thread (menu_hook_update drives the poll
// and the draw), so no lock -- same rule as g_rebindTarget above.
int g_activeTab  = 0;
int g_pendingTab = -1;   // -1 = no switch requested

void cycle_tab(int delta)
{
    g_pendingTab = ((g_activeTab + delta) % kTabCount + kTabCount) % kTabCount;
}

void draw_settings_window()
{
    ImGui::SetNextWindowPos(ImVec2(60, 60), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("SnowRunner VR Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

    // The first-run resolution notice, as a MODAL -- the whole reason the menu
    // was opened, so it should not be possible to miss it behind a tab.
    //
    // CENTRED ON THIS PANEL, not on the viewport, which is what ImGui would do
    // by default. The viewport here is the desktop-sized texture the menu is
    // drawn into, and the headset only ever sees the crop around this window
    // (menu_panel_rect), so a viewport-centred popup lands almost entirely
    // outside the picture. Read straight from the window we are inside: the
    // position is exact and the size is last frame's, which under
    // AlwaysAutoResize is this frame's unless the content just changed.
    g_popupW.store(0); g_popupH.store(0);   // re-published below while it is up
    if (g_firstRunNotice.load() && !g_firstRunPopupShown) {
        g_firstRunPopupShown = true;
        ImGui::OpenPopup("First-time setup");
    }
    if (g_firstRunNotice.load()) {
        const ImVec2 wp = ImGui::GetWindowPos();
        const ImVec2 ws = ImGui::GetWindowSize();
        ImGui::SetNextWindowPos(ImVec2(wp.x + ws.x * 0.5f, wp.y + ws.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f, 0.0f), ImVec2(520.0f, FLT_MAX));
    }
    if (ImGui::BeginPopupModal("First-time setup", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            "First time startup detected and resolution set in config, please "
            "restart game to launch with corrected resolution.");
        ImGui::Spacing();
        ImGui::TextWrapped("Render resolution set to %ux%u, from your headset's "
                           "own native size.", g_firstRunW, g_firstRunH);
        ImGui::TextDisabled("The game builds its window once, at launch, and never "
                            "re-reads the size -- so this cannot be applied now.");
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(120.0f, 0.0f))) {
            g_firstRunNotice.store(false);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();

        const ImVec2 pp = ImGui::GetWindowPos();
        const ImVec2 ps = ImGui::GetWindowSize();
        g_popupX.store((int)pp.x); g_popupY.store((int)pp.y);
        g_popupW.store((int)ps.x); g_popupH.store((int)ps.y);
        ImGui::EndPopup();
    }

    if (g_rebindTarget == RebindTarget::kKeyboard) {
        ImGui::TextUnformatted("Press any key to bind... (Esc cancels)");
    } else if (g_rebindTarget == RebindTarget::kGamepad) {
        ImGui::TextUnformatted("Hold the gamepad buttons, then release... (Esc cancels)");
    } else if (ImGui::BeginTabBar("tabs")) {
        // Consumed here, once: SetSelected has to be passed on exactly the
        // frame the switch is wanted, or the tab would be forced open again
        // every frame and become impossible to leave by clicking.
        const int want = g_pendingTab;
        g_pendingTab = -1;
        for (int i = 0; i < kTabCount; ++i) {
            const ImGuiTabItemFlags flags = (i == want) ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(kTabs[i].name, nullptr, flags)) {
                g_activeTab = i;   // only the visible tab reports true
                kTabs[i].draw();
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }

    ImVec2 pos = ImGui::GetWindowPos();
    ImVec2 size = ImGui::GetWindowSize();
    g_panelX.store((int)pos.x);
    g_panelY.store((int)pos.y);
    g_panelW.store((int)size.x);
    g_panelH.store((int)size.y);

    ImGui::End();
}

} // namespace

// --------------------------------------------------------------------------
// Public API
// --------------------------------------------------------------------------

void install_menu_hook(IDXGISwapChain* swapchain)
{
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true))
        return;

    DXGI_SWAP_CHAIN_DESC scd{};
    if (FAILED(swapchain->GetDesc(&scd)) || !scd.OutputWindow) { g_installed = false; return; }
    if (FAILED(swapchain->GetDevice(__uuidof(ID3D11Device), (void**)&g_device))) { g_installed = false; return; }
    g_device->GetImmediateContext(&g_ctx);

    // Must be ready before the subclass below can start routing messages.
    InitializeCriticalSection(&g_imguiLock);

    g_hwnd = scd.OutputWindow;
    g_origWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Detour_WndProc)));
    if (!g_origWndProc) { g_installed = false; return; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    g_baseStyle = ImGui::GetStyle();
    g_lastAppliedUiScale = -1.0f;   // force apply_ui_scale_if_changed() to run on the next present

    if (!ImGui_ImplWin32_Init(g_hwnd) || !ImGui_ImplDX11_Init(g_device.Get(), g_ctx.Get())) {
        VRLOG("menu hook: ImGui backend init FAILED");
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));
        g_installed = false;
        return;
    }

    VRLOG("menu hook installed (toggle: %s / %s)",
          vrcfg::vk_name(g_menuToggleVk.load()).c_str(),
          vrcfg::gamepad_combo_name(g_menuToggleCombo.load()).c_str());
}

void menu_hook_update()
{
    tick_fps();   // every Present, menu open or not -- see g_fps

    if (!g_installed.load())
        return;

    // hooks::xinput_poll_self(), NOT XInputGetState directly: input_block.cpp
    // silences the gamepad for everyone while the menu is open, and this is
    // the call that marks itself as ours so it reads the real pad anyway.
    // Without it the menu cannot be navigated -- or even closed -- with the
    // controller that opened it.
    XINPUT_STATE state{};
    const bool gamepadConnected = (hooks::xinput_poll_self(0, &state) == ERROR_SUCCESS);
    const XINPUT_GAMEPAD& gp = state.Gamepad;

    // Rebind capture takes over the toggle entirely while active.
    if (g_rebindTarget == RebindTarget::kKeyboard) {
        if (::GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            g_rebindTarget = RebindTarget::kNone;
        } else {
            for (int vk = 0; vk < 256; ++vk) {
                if (g_captureBaseline[vk]) continue;
                if ((::GetAsyncKeyState(vk) & 0x8000) != 0) {
                    g_menuToggleVk.store(vk);
                    g_rebindTarget = RebindTarget::kNone;
                    vrcfg::save();
                    break;
                }
            }
        }
    } else if (g_rebindTarget == RebindTarget::kGamepad) {
        if (::GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            g_rebindTarget = RebindTarget::kNone;
        } else if (gamepadConnected) {
            g_captureComboSeen |= gp.wButtons;
            if (gp.wButtons == 0 && g_captureComboSeen != 0) {
                g_menuToggleCombo.store(g_captureComboSeen);
                g_rebindTarget = RebindTarget::kNone;
                vrcfg::save();
            }
        }
    } else {
        // Normal toggle edge-detect -- key OR full combo held. A single
        // gated store (not two independent ones) so a coincident key+combo
        // edge in the same frame still only flips the menu once.
        static bool prevKeyHeld = false;
        static bool prevComboHeld = false;
        const bool keyHeld = (::GetAsyncKeyState(g_menuToggleVk.load()) & 0x8000) != 0;
        const uint32_t combo = g_menuToggleCombo.load();
        const bool comboHeld = gamepadConnected && combo != 0 && (gp.wButtons & combo) == combo;

        if ((keyHeld && !prevKeyHeld) || (comboHeld && !prevComboHeld))
            g_menuOpen.store(!g_menuOpen.load());
        prevKeyHeld = keyHeld;
        prevComboHeld = comboHeld;
    }

    // LB/RB cycle the tab strip. Edge-detected from our own poll rather than
    // through ImGui, which has no gamepad binding for tab bars at all. Held
    // state is tracked unconditionally, including while the menu is closed, so
    // a shoulder button already down when the menu opens is not read as a
    // fresh press on the first frame.
    {
        static bool prevLB = false, prevRB = false;
        const bool lb = gamepadConnected && (gp.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
        const bool rb = gamepadConnected && (gp.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
        if (g_menuOpen.load() && g_rebindTarget == RebindTarget::kNone) {
            if (lb && !prevLB) cycle_tab(-1);
            if (rb && !prevRB) cycle_tab(+1);
        }
        prevLB = lb;
        prevRB = rb;
    }

    check_first_run_resolution();

    if (!g_menuOpen.load())
        return;

    EnterCriticalSection(&g_imguiLock);

    apply_ui_scale_if_changed();
    feed_gamepad_nav(gp, gamepadConnected);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    draw_settings_window();
    ImGui::Render();

    LeaveCriticalSection(&g_imguiLock);
}

void menu_render_to_desktop(IDXGISwapChain* swapchain)
{
    if (!g_installed.load() || !g_menuOpen.load())
        return;

    EnterCriticalSection(&g_imguiLock);
    if (ensure_backbuffer_rtv(swapchain)) {
        ID3D11RenderTargetView* rtv = g_backbufferRtv.Get();
        // Bind only -- do NOT clear, the game's already-rendered frame is there.
        g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
    LeaveCriticalSection(&g_imguiLock);
}

bool menu_wants_quad(uint32_t& outW, uint32_t& outH)
{
    if (!g_installed.load() || !g_menuOpen.load())
        return false;
    ImVec2 size = ImGui::GetIO().DisplaySize;
    if (size.x <= 0 || size.y <= 0)
        return false;
    outW = (uint32_t)size.x;
    outH = (uint32_t)size.y;
    return true;
}

bool menu_panel_rect(int& x, int& y, int& w, int& h)
{
    if (!g_installed.load() || !g_menuOpen.load())
        return false;
    x = g_panelX.load(); y = g_panelY.load();
    w = g_panelW.load(); h = g_panelH.load();
    if (w <= 0 || h <= 0) return false;

    // Grow to cover any popup as well -- see g_popupX.
    const int pw = g_popupW.load(), ph = g_popupH.load();
    if (pw > 0 && ph > 0) {
        const int px = g_popupX.load(), py = g_popupY.load();
        const int x1 = (x < px) ? x : px;
        const int y1 = (y < py) ? y : py;
        const int x2 = ((x + w) > (px + pw)) ? (x + w) : (px + pw);
        const int y2 = ((y + h) > (py + ph)) ? (y + h) : (py + ph);
        x = x1; y = y1; w = x2 - x1; h = y2 - y1;
    }
    return true;
}

void menu_render_to_extra_target(ID3D11RenderTargetView* rtv, ID3D11DeviceContext* ctx)
{
    if (!g_installed.load() || !g_menuOpen.load() || !rtv || !ctx)
        return;

    EnterCriticalSection(&g_imguiLock);
    const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    ctx->ClearRenderTargetView(rtv, clear);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    LeaveCriticalSection(&g_imguiLock);
}

void menu_hook_on_resize()
{
    g_backbufferRtv.Reset();
}

bool is_menu_open() { return g_menuOpen.load(); }

int      menu_toggle_vk() { return g_menuToggleVk.load(); }
void     set_menu_toggle_vk(int vk) { g_menuToggleVk.store(vk); }
uint32_t menu_toggle_gamepad_combo() { return g_menuToggleCombo.load(); }
void     set_menu_toggle_gamepad_combo(uint32_t mask) { g_menuToggleCombo.store(mask); }

float mod_ui_scale() { return g_modUiScale.load(); }
void  set_mod_ui_scale(float scale)
{
    g_modUiScale.store((std::max)(0.5f, (std::min)(3.0f, scale)));
}

} // namespace hooks
