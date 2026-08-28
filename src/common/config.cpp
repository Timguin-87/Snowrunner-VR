#include "common/config.h"
#include "common/log.h"
#include "hooks/viewbuild_hook.h"
#include "hooks/camera_hook.h"
#include "hooks/camera_fov.h"
#include "hooks/menu_hook.h"
#include "hooks/cbuffer_hook.h"
#include "hooks/ui_hook.h"
#include "hooks/shader_cull.h"
#include "hooks/resolution_hook.h"
#include "render/smudge_layer.hpp"
#include "xr/xr_mirror.h"

#include <windows.h>
#include <xinput.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace vrcfg {
namespace {

// --- path resolution (next to this DLL, same technique as frame_dump.cpp's
// dump_dir(): self-locate via the address of a function in this module
// rather than needing an HMODULE threaded in from DllMain) -------------

// Trailing backslash included. Empty on failure, which leaves every caller
// with a bare filename -- i.e. the process working directory, which is where
// the file would have gone before this module knew how to self-locate.
std::wstring dll_dir()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&dll_dir), &self);
    wchar_t path[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(self, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return L"";
    for (wchar_t* p = path + len; p > path; --p) {
        if (p[-1] == L'\\') { *p = L'\0'; break; }
    }
    return std::wstring(path);
}

std::wstring config_path() { return dll_dir() + L"Snowrunner_VR_config.txt"; }

// --- raw Win32 file I/O (no CRT FILE*, matching log.cpp's idiom) ------

std::string read_file(const std::wstring& path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return {};
    std::string data;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < 1'000'000) {
        data.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(h, data.data(), (DWORD)data.size(), &read, nullptr) || read != data.size())
            data.clear();
    }
    CloseHandle(h);
    return data;
}

bool write_file(const std::wstring& path, const std::string& data)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr) &&
              written == data.size();
    CloseHandle(h);
    return ok;
}

// --- key=value line parser ---------------------------------------------

std::string trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::map<std::string, std::string> parse_kv(const std::string& text)
{
    std::map<std::string, std::string> kv;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;

        std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';')
            continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = trim(t.substr(0, eq));
        std::string val = trim(t.substr(eq + 1));
        if (!key.empty())
            kv[key] = val;
    }
    return kv;
}

// Case-insensitive lookup -- friendlier for hand-editing than std::map's
// default ordering, and the map is tiny (a handful of keys) so a linear
// scan is fine.
bool kv_find(const std::map<std::string, std::string>& kv, const char* key, std::string& outVal)
{
    for (auto& [k, v] : kv) {
        if (_stricmp(k.c_str(), key) == 0) { outVal = v; return true; }
    }
    return false;
}

// "2688x2688" -> 2688,2688; "off"/"0" -> 0,0 (feature disabled). Anything
// else is rejected so a typo resets the file rather than silently spoofing a
// nonsense monitor size at the game.
bool parse_size(const std::string& s, int& outW, int& outH)
{
    if (s.empty()) return false;
    if (_stricmp(s.c_str(), "off") == 0 || s == "0") { outW = 0; outH = 0; return true; }
    int w = 0, h = 0;
    char tail = 0;
    if (sscanf_s(s.c_str(), "%dx%d%c", &w, &h, &tail, 1u) != 2) return false;
    if (w < 640 || h < 480 || w > 16384 || h > 16384) return false;
    outW = w; outH = h;
    return true;
}

std::string size_str(int w, int h)
{
    if (w <= 0 || h <= 0) return "off";
    char buf[32];
    snprintf(buf, sizeof(buf), "%dx%d", w, h);
    return buf;
}

// Each bad value is reported and skipped, leaving that one
// setting at its default and every other setting intact.
// See vrcfg::resolution_unset(). Module state rather than a Settings field
// because it has to outlive the load that read it.
std::atomic<bool> g_resolutionUnset{true};

void note_invalid(const char* key, const std::string& val)
{
    VRLOG("config: ignoring invalid value  %s=%s  -- keeping the default for it",
          key, val.c_str());
}

// Whole-string float parse (rejects "1.5garbage", trailing junk, empty).
bool parse_float(const std::string& s, float& out)
{
    if (s.empty()) return false;
    char* end = nullptr;
    float v = strtof(s.c_str(), &end);
    if (end != s.c_str() + s.size()) return false;
    out = v;
    return true;
}

// --- culled shader hashes ------------------------------------------------
// Comma-separated 16-digit hex, e.g. "3F2A1C0DE4B57896,00AB...". Each is the
// first 8 bytes of a pixel shader's DXBC container checksum -- see
// shader_cull.h for why that particular value is the identity that survives a
// restart. Whitespace around entries is tolerated because these get pasted in
// by hand from the log as often as they get written by the settings UI.
bool parse_hash_list(const std::string& s, uint64_t* out, int maxOut, int& outCount)
{
    outCount = 0;
    size_t i = 0;
    while (i <= s.size()) {
        size_t end = s.find(',', i);
        if (end == std::string::npos) end = s.size();
        std::string tok = s.substr(i, end - i);
        i = end + 1;
        size_t b = tok.find_first_not_of(" \t");
        size_t e = tok.find_last_not_of(" \t");
        if (b == std::string::npos) continue;          // blank entry, skip
        tok = tok.substr(b, e - b + 1);
        if (tok.size() > 2 && tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X'))
            tok = tok.substr(2);
        if (tok.empty() || tok.size() > 16) return false;
        char* stop = nullptr;
        const uint64_t v = strtoull(tok.c_str(), &stop, 16);
        if (!stop || *stop != '\0' || v == 0) return false;
        if (outCount < maxOut) out[outCount++] = v;
    }
    return true;
}

// Only the hashes past what the build ships -- what this install's own search
// found. See refresh_role_hashes() for why the built-in ones are not written.
std::string role_extra_hash_str(hooks::CullRole role)
{
    std::string out;
    const int n     = hooks::cull_role_hash_count(role);
    const int baked = hooks::cull_role_builtin_count(role);
    for (int i = baked; i < n; ++i) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%016llX",
                 (unsigned long long)hooks::cull_role_hash_at(role, i));
        if (!out.empty()) out += ',';
        out += buf;
    }
    return out;
}

std::string hash_list_str()
{
    std::string out;
    const int n = hooks::culled_hash_count();
    for (int i = 0; i < n; ++i) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%016llX", (unsigned long long)hooks::culled_hash_at(i));
        if (!out.empty()) out += ',';
        out += buf;
    }
    return out;
}

// --- VK <-> friendly name -----------------------------------------------

struct VkName { int vk; const char* name; };
constexpr VkName kVkNames[] = {
    { VK_INSERT, "Insert" }, { VK_HOME, "Home" }, { VK_END, "End" },
    { VK_DELETE, "Delete" }, { VK_PRIOR, "PageUp" }, { VK_NEXT, "PageDown" },
    { VK_SCROLL, "ScrollLock" }, { VK_OEM_4, "[" }, { VK_OEM_6, "]" },
    { VK_SPACE, "Space" }, { VK_TAB, "Tab" },
    { VK_F1, "F1" }, { VK_F2, "F2" }, { VK_F3, "F3" }, { VK_F4, "F4" },
    { VK_F5, "F5" }, { VK_F6, "F6" }, { VK_F7, "F7" }, { VK_F8, "F8" },
    { VK_F9, "F9" }, { VK_F10, "F10" }, { VK_F11, "F11" }, { VK_F12, "F12" },
};

// --- gamepad digital-button <-> friendly name ---------------------------

struct GpButton { WORD mask; const char* name; };
constexpr GpButton kGpButtons[] = {
    { XINPUT_GAMEPAD_DPAD_UP,        "DpadUp" },
    { XINPUT_GAMEPAD_DPAD_DOWN,      "DpadDown" },
    { XINPUT_GAMEPAD_DPAD_LEFT,      "DpadLeft" },
    { XINPUT_GAMEPAD_DPAD_RIGHT,     "DpadRight" },
    { XINPUT_GAMEPAD_START,          "Start" },
    { XINPUT_GAMEPAD_BACK,           "Back" },
    { XINPUT_GAMEPAD_LEFT_THUMB,     "L3" },
    { XINPUT_GAMEPAD_RIGHT_THUMB,    "R3" },
    { XINPUT_GAMEPAD_LEFT_SHOULDER,  "LB" },
    { XINPUT_GAMEPAD_RIGHT_SHOULDER, "RB" },
    { XINPUT_GAMEPAD_A, "A" }, { XINPUT_GAMEPAD_B, "B" },
    { XINPUT_GAMEPAD_X, "X" }, { XINPUT_GAMEPAD_Y, "Y" },
};

// --- defaults + the live struct load()/save() work with ----------------

// DIBR shift off by default: it is the only thing that pays for the scene-depth
// capture, and its failure mode is disocclusion artefacts rather than a clean
// fallback. The stale-eye warp alone is the safe starting point.
constexpr bool     kDefaultDibrShift   = false;
// Orbit horizon lock off, cockpit truck-level lock on (further down). The
// pair used to be the other way round, which had it stabilising the camera you
// are not usually in and leaving the one you are alone.
constexpr bool     kDefaultHorizonLock = false;
constexpr int      kDefaultMenuVk      = VK_INSERT;
constexpr unsigned kDefaultMenuCombo   = XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;
constexpr float kDefaultModUiScale      = 1.0f;
// THE UI PLANE, retuned 2026-08-27 from what the numbers had to be while the
// HUD was still painted into the eye images. 1.9 m at 1.6 m away reproduced
// the old fraction-of-the-view sizing, which was the right thing to preserve
// then and is no longer what anyone wants: it is a HUD pressed against your
// face. 8 m at 7 m subtends about the same angle, but at a distance the eyes
// can converge on comfortably instead of crossing to read it.
//
// Forward rather than head-locked for the same reason. Head-locked reproduced
// where the HUD used to sit; a plane 7 m out that follows your head is a wall
// you cannot look away from, while a pinned one is a thing in the world you
// glance at.
constexpr float kDefaultUiPlaneSize     = 8.00f;
constexpr int   kDefaultUiPlaneMode     = xr::kUiPlaneForward;
constexpr float kDefaultUiPlaneDistance = 7.00f;
constexpr float kDefaultRenderFovScale  = 1.0f;
// The map/garage window, as a fraction of the render size. Small enough to sit
// inside the sweet spot of the lenses rather than needing eye movement to read
// its corners -- it is a flat screen being looked AT, not a view being looked
// THROUGH.
constexpr float kDefaultMapWindowShrink = 0.4f;
constexpr bool     kDefaultHideHud = false;
// Matches xr_mirror.cpp's g_vertOffset initializer (the pre-existing
// hand-tuned, headset-confirmed constant).
// 0 since 2026-08-20: the vertical FOV asymmetry this used to absorb is now
// read from the runtime per headset (xr::fov_center_pitch()). This is the
// user's own preference on top of that.
constexpr float kDefaultVerticalRecenter = 0.0f;
// The stale-eye warp itself, on. Off resubmits the eye that was not rendered
// exactly as it was, one frame stale, which is the thing this mod exists to
// avoid -- so off is a measurement ("is the warp what is wrong, or is what it
// is warping wrong?"), not a way anyone should be playing. It persists anyway:
// a switch that silently un-flips itself on the next launch is worse than one
// that can be left in a bad place, because the bad place is at least visible.
constexpr bool  kDefaultWarpEnabled = true;
// Stale-eye warp source: the whole ladder, headset + game 6-DoF. It was
// headset-pose-only for as long as the stronger levels were unproven -- their
// failure mode is worse ghosting rather than an obvious break, which is a bad
// thing to have on by default while it is still being got right. That is no
// longer the state they are in, and in a driving game the camera's own motion
// is most of what the warp has to account for.
constexpr int   kDefaultWarpType = 2;   // xr::kWarp6Dof
// The cab band for that reprojection, in view metres. Sized from a cockpit:
// dashboard ~0.8, windscreen ~1.2, and the nearest world geometry a bumper
// reaches is a few metres out.
// Synthetic per-eye display cant (degrees) -- a TEST knob for the canting path
// on parallel-panel hardware, not a preference. 0 = off, which is what any
// real headset should be left at (a genuine cant is read from OpenXR itself).
constexpr float kDefaultDebugEyeCantDeg = 0.0f;
// Cockpit truck-relative look locks. PITCH is locked by default: in a headset
// the stick moving the view up and down fights the head doing the same job,
// and pitch is the axis where that reads as the world tilting. YAW is left to
// the stick, because looking behind you is a thing you actually need and neck
// travel does not reach it.
constexpr bool kDefaultTruckLevelLock = true;
constexpr bool kDefaultTruckYawLock   = false;
// Mode 4 only. Off = the original behaviour, so an existing config that predates
// this key reads exactly as it did.
// Off by default: the game keeps its own resolution, exactly as it would
// without the mod, until someone sets a size.
constexpr int  kDefaultForceResW = 0;
constexpr int  kDefaultForceResH = 0;

constexpr bool kDefaultCullShaders = false;
// The cab glass, skipped by default. It refracts the scene behind it but writes
// no depth, so it is wrong under any reprojection and awkward in stereo even
// without one -- a refracting surface a few centimetres from the eyes.
constexpr bool kDefaultDisableWindows = true;

struct Settings {
    bool     dibrShift          = kDefaultDibrShift;
    bool     horizonLock        = kDefaultHorizonLock;
    int      menuVk             = kDefaultMenuVk;
    unsigned menuCombo          = kDefaultMenuCombo;
    float    modUiScale         = kDefaultModUiScale;
    float    uiPlaneSize        = kDefaultUiPlaneSize;
    int      uiPlaneMode        = kDefaultUiPlaneMode;
    float    uiPlaneDistance    = kDefaultUiPlaneDistance;
    float    renderFovScale     = kDefaultRenderFovScale;
    float    mapWindowShrink    = kDefaultMapWindowShrink;
    bool     hideHud            = kDefaultHideHud;
    float    verticalRecenter   = kDefaultVerticalRecenter;
    bool     warpEnabled        = kDefaultWarpEnabled;
    int      warpType           = kDefaultWarpType;
    float    debugEyeCantDeg    = kDefaultDebugEyeCantDeg;
    bool     truckLevelLock     = kDefaultTruckLevelLock;
    bool     truckYawLock       = kDefaultTruckYawLock;
    int      forceResW          = kDefaultForceResW;
    int      forceResH          = kDefaultForceResH;
    // No resolution has ever been chosen -- see vrcfg::resolution_unset().
    bool     resolutionUnset    = true;
    bool     cullShaders        = kDefaultCullShaders;
    std::string cullShaderHashes;   // comma-separated 16-digit hex, "" = none
    bool     disableWindows       = kDefaultDisableWindows;
    bool     screenGateDirect     = false;
    float    cameraFovFactor       = 1.0f;
    // Empty = keep the built-in hash. Only written out once the module has
    // one, which it always does, so these round-trip as documentation of what
    // the build currently targets.
    // Only roles that hooks::cull_role_extensible() allows have a config slot.
    // The rest are baked into the build with no key at all -- see shader_cull.h.
    std::string rigidShaderHash;
};

// Where each role's hash list lives in Settings.
//
// ONE mapping, used by load()'s parse table AND by the write-back before every
// save. They used to be separate -- the load side table-driven, the two
// write-back sides hand-written -- and kCullRigid was duly added to the first
// and missed by both of the others, so RigidShaderHash was written empty on
// every launch while the compiled defaults quietly kept working in memory.
//
// The static_assert is the actual guard: adding a role breaks the build here
// rather than silently dropping that role's hashes from the file.
static_assert(hooks::kCullRoleCount == 6,
              "a CullRole was added -- give it a row in role_dst()");
std::string* role_dst(Settings& s, int r)
{
    switch ((hooks::CullRole)r) {
        case hooks::kCullRigid:        return &s.rigidShaderHash;
        // Baked in, no key: kCullWindows, kCullWindowSmudge, kCullUi,
        // kCullMirror, kCullWinchMarker.
        default:                       return nullptr;
    }
}

// Every persisted role's hashes, read back out of the module. Must run before
// the file is written: add_cull_role_hash() runs at runtime, so `s` as the
// caller filled it is not authoritative.
//
// ONLY THE PART THIS INSTALL FOUND. Writing the built-in hashes out too would
// freeze a copy of them into the user's file, and the next build's updated list
// would then be silently overridden by that stale copy -- exactly what baking
// them in is meant to prevent. So the string starts at the built-in count, and
// a fresh install writes an empty value.
void refresh_role_hashes(Settings& s)
{
    for (int r = 0; r < hooks::kCullRoleCount; ++r)
        if (std::string* d = role_dst(s, r))
            *d = role_extra_hash_str((hooks::CullRole)r);
}

std::string format_file(const Settings& s)
{
    const char* wtKey = xr::warp_type_key(s.warpType);
    if (!wtKey) wtKey = "Headset";   // defensive; warpType should always be in range here

    // Sized with headroom, and the result is length-checked below: a silent
    // snprintf truncation here is invisible at write time but shows up much
    // later as "that setting doesn't persist" (the truncated keys read back as
    // missing on the next launch and get quietly reset to defaults). That has
    // already happened once -- three keys were added to the argument list
    // while the format string kept its old, shorter conversion list.
    // Sized with headroom on purpose: this is a single snprintf of the whole
    // file, so overflowing it does not fail loudly -- it truncates, and every
    // setting past the cut stops persisting. That happened at 8263 bytes when
    // the rigid-band keys were added. Grow this before adding blocks, not after
    // noticing settings no longer save.
    char buf[24576];
    const int written = snprintf(buf, sizeof(buf),
        "# Snowrunner_VR_config.txt -- auto-generated. Comments (#) and blank\n"
        "# lines are ignored. A key with an invalid value resets this whole\n"
        "# file to defaults on next launch; a key that's simply missing (e.g.\n"
        "# this file predates a newer setting) only fills in that one default.\n"
        "\n"
        "# DIBRShift: true, false. Reproject the rendered eye into the other one\n"
        "# using scene depth, instead of leaving that eye to the stale-eye warp\n"
        "# alone. The two compose: the shift lands where it has depth and a\n"
        "# source, the warp fills every pixel it could not reach.\n"
        "DIBRShift=%s\n"
        "# HorizonLock: true, false (orbit camera only)\n"
        "HorizonLock=%s\n"
        "# HideHud: true, false\n"
        "HideHud=%s\n"
        "# MenuKey: keyboard key that opens/closes the mod settings menu\n"
        "MenuKey=%s\n"
        "# MenuGamepadCombo: gamepad buttons (held together) that open/close the menu\n"
        "MenuGamepadCombo=%s\n"
        "# ModUIScale: size of this settings window/font (0.5 - 3.0, 1.0 = 100%%)\n"
        "ModUIScale=%.2f\n"
        "# UIPlaneSize: width of the in-game UI plane, in metres (0.2 - 10). The\n"
        "# UI is a composition layer, not part of the render, so this is a real\n"
        "# size at a real distance -- pushing the plane back with UIPlaneDistance\n"
        "# makes it look smaller, and this is what compensates.\n"
        "UIPlaneSize=%.2f\n"
        "# UIPlaneMode: headlocked, forward. Where that layer sits -- following\n"
        "# the head, or pinned in the world where you were looking at the last\n"
        "# recenter (the Recenter view button, or HOME).\n"
        "# The map screen ignores this: it is always head-locked at full size, the\n"
        "# only placement where its screen-space markers still line up with the map.\n"
        "UIPlaneMode=%s\n"
        "# UIPlaneDistance: metres from the eye (0.3 - 20). Where the UI converges.\n"
        "# The MAP screen follows this too, but in DEPTH only: its plane always\n"
        "# subtends the rendered view exactly and is hung on the eye its capture\n"
        "# came from, so that eye overlays its own render at any distance and the\n"
        "# other eye sees it shifted by roughly IPD/distance.\n"
        "UIPlaneDistance=%.2f\n"
        "# RenderFovScale: base render size (0.30 - 1.00). Lowers the rendered\n"
        "# FOV and the compositor window together, so less scene is drawn into\n"
        "# fewer pixels with no stretch. Everything else derives from this.\n"
        "RenderFovScale=%.2f\n"
        "# MapWindowShrink: map/garage window size as a fraction of the render\n"
        "# size above (0.0 - 1.0) -- not of the headset native FOV.\n"
        "MapWindowShrink=%.2f\n"
        "# VerticalRecenter: pose-pitch vertical image recenter, radians (-0.5 - 0.5)\n"
        "VerticalRecenter=%.4f\n"
        "# WarpEnabled: true, false. Bring the eye that was NOT rendered this\n"
        "# frame to the current instant, rather than resubmitting it a frame\n"
        "# stale. Off is a diagnostic: if a judder survives with this false,\n"
        "# the warp is not what is causing it.\n"
        "WarpEnabled=%s\n"
        "# WarpType: Headset, GameRotation, Game6Dof. What the stale-eye warp is\n"
        "# built from -- a ladder, each level containing the one before it.\n"
        "#   Headset       the headset pose at capture vs now. Blind to the game\n"
        "#                 camera, so stick look and vehicle turns pass through\n"
        "#                 uncorrected.\n"
        "#   GameRotation  the game's own view matrix instead. The rendered view\n"
        "#                 is (game camera x head) and it carries both, so it\n"
        "#                 covers the game camera too. Cockpit only -- an orbit\n"
        "#                 camera swings around the truck rather than turning in\n"
        "#                 place, which a rotation warp cannot represent, so\n"
        "#                 orbit falls back to the pose.\n"
        "#   Game6Dof      that, plus a reprojection by the camera's TRANSLATION\n"
        "#                 using the retained frame's own depth. Rotation is a\n"
        "#                 homography and needs none; a translation moves near\n"
        "#                 pixels further than far ones, which is why it does.\n"
        "WarpType=%s\n"
        "# DebugEyeCantDeg: synthetic per-eye display cant for TESTING (-15 - 15).\n"
        "# Leave at 0. Real canting is read from the OpenXR runtime automatically;\n"
        "# this only exists to exercise that path on a headset that has none, where\n"
        "# a correct implementation must look identical at every value.\n"
        "DebugEyeCantDeg=%.2f\n"
        "# TruckLevelLock: cockpit only -- stick look-PITCH stops moving the view;\n"
        "# head pitch becomes the only source. The truck's own tilt on hills is kept.\n"
        "TruckLevelLock=%s\n"
        "# TruckYawLock: cockpit only -- same, for the stick's look-YAW.\n"
        "TruckYawLock=%s\n"
        "\n"
        "# ForceGameResolution: WxH the game window is CREATED at, overriding\n"
        "# whatever resolution the game itself picked (it clamps its own to the\n"
        "# desktop work area). The engine sizes its swapchain from the window,\n"
        "# so this sets the render resolution directly, with no video.dat edit.\n"
        "# Normally driven by the settings menu's \"Render resolution\" slider.\n"
        "# Applied at startup only -- changing it needs a game restart. There is\n"
        "# no way to apply it live: the engine reads the window size once, at\n"
        "# creation, and does not re-derive it from a later resize.\n"
        "# 'off' = leave the game's own resolution alone.\n"
        "# 'auto' = not chosen yet. The mod fills this in from the headset's\n"
        "# own native width the first time a runtime is reachable, then asks you\n"
        "# to restart so the game is created at that size.\n"
        "ForceGameResolution=%s\n"
        "\n"
        "# ScreenGate: legacy, direct -- how the mod decides which screen you are\n"
        "# on (map / garage / gameplay), which drives the render FOV, the HUD\n"
        "# shrink bypass and whether DIBR shift runs at all.\n"
        "# 'legacy' reads the main camera's POSE: pitched -45 deg near the world\n"
        "# origin means the map. Known failure: gameplay can reach that pose.\n"
        "# 'direct' reads the game's own state instead. The map comes from five\n"
        "# pixel shaders that draw on it and nowhere else, baked in; all five\n"
        "# must be drawing for it to be selected. Everything else is separated\n"
        "# by the drive-camera idle timer -- the garage does not tick that\n"
        "# camera, which is a direct reading rather than a guess, and needs\n"
        "# nothing discovered first.\n"
        "ScreenGate=%s\n"
        "\n"
        "# CameraFovFactor: the culling FOV. Writes the rendered FOV, times\n"
        "# this factor, into combine::CAMERA::m_fFOV (+0x108, radians) -- the\n"
        "# field the engine builds its cull frustum from. The game's own FOV\n"
        "# fields are never touched. Always declared; there is no off switch,\n"
        "# because off is the game's own value and that is far too narrow for a\n"
        "# headset. 0.5 - 2.5, 1.0 = declare exactly what is rendered; higher\n"
        "# keeps more geometry alive at the edges of vision and costs draw\n"
        "# calls.\n"
        "CameraFovFactor=%.2f\n"
        "\n"
        "# DisableWindows: true, false -- skip the cab glass entirely. The glass\n"
        "# refracts the scene but writes no depth, so DIBR shift reprojects the\n"
        "# refracted image by the depth of the terrain BEYOND the cab and it\n"
        "# lands in the wrong place. Useful whether or not DIBR shift is on.\n"
        "DisableWindows=%s\n"
        "# Which pixel shaders the two settings above skip is BAKED INTO THE\n"
        "# BUILD and has no key here, as are the UI and mirror shader sets. They\n"
        "# name a closed set of effects that was found once and is done, so a\n"
        "# per-install override could only go stale against a later build or be\n"
        "# broken by a mis-answered search. A game patch that recompiles one of\n"
        "# them needs a new build of the mod.\n"
        "\n"
        "# RigidShaderHash: everything rigid with the camera -- truck body, cab\n"
        "# interior, trailers. Never suppressed; used to mask those pixels out of\n"
        "# the 6-DoF stale-eye reprojection, which would otherwise move them by\n"
        "# the camera's own travel and shred the cockpit at near depth.\n"
        "# The only shader set still open, because it is a map of every vehicle in\n"
        "# the game rather than a handful of effects, and is not finished. This\n"
        "# holds ONLY what this install's own search found ON TOP of the built-in\n"
        "# list -- so it starts empty, and the built-in list keeps updating with\n"
        "# the mod instead of being frozen by a copy in here. Add to it from the\n"
        "# \"Shader Cull\" tab's search.\n"
        "RigidShaderHash=%s\n"
        "\n"
        "# CullShaders: true, false -- master switch for the EXPERIMENTAL list\n"
        "# below and the \"Shader Cull\" tab's search. The two named settings\n"
        "# above are independent of it. Off means the draw path behaves exactly\n"
        "# as it did before this feature existed.\n"
        "CullShaders=%s\n"
        "# CullShaderHashes: comma-separated pixel shaders to skip, each the\n"
        "# first 8 bytes of its DXBC checksum in hex -- stable across sessions\n"
        "# and across machines, unlike the pointers shown in the settings UI.\n"
        "# Filled in by the search when it finds something that has no name yet.\n"
        "# Empty = none.\n"
        "CullShaderHashes=%s\n",
        s.dibrShift ? "true" : "false",
        s.horizonLock ? "true" : "false", s.hideHud ? "true" : "false",
        vk_name(s.menuVk).c_str(), gamepad_combo_name(s.menuCombo).c_str(),
        s.modUiScale, s.uiPlaneSize,
        s.uiPlaneMode == xr::kUiPlaneForward ? "forward" : "headlocked",
        s.uiPlaneDistance,
        s.renderFovScale, s.mapWindowShrink,
        s.verticalRecenter,
        s.warpEnabled ? "true" : "false",
        wtKey,
        s.debugEyeCantDeg,
        s.truckLevelLock ? "true" : "false", s.truckYawLock ? "true" : "false",
        s.resolutionUnset ? "auto" : size_str(s.forceResW, s.forceResH).c_str(),
        s.screenGateDirect ? "direct" : "legacy",
        s.cameraFovFactor,
        s.disableWindows ? "true" : "false",
        s.rigidShaderHash.c_str(),
        s.cullShaders ? "true" : "false", s.cullShaderHashes.c_str());
    if (written < 0 || written >= (int)sizeof(buf))
        VRLOG("config: FORMAT TRUNCATED (%d bytes needed) -- settings past the cut "
              "will not persist; grow buf[] in format_file()", written);
    return buf;
}

void write_defaults_or_current(const Settings& s)
{
    if (!write_file(config_path(), format_file(s)))
        VRLOG("config: failed to write Snowrunner_VR_config.txt");
}

} // namespace

std::string vk_name(int vk)
{
    if (vk >= 'A' && vk <= 'Z') return std::string(1, (char)vk);
    if (vk >= '0' && vk <= '9') return std::string(1, (char)vk);
    for (auto& e : kVkNames)
        if (e.vk == vk) return e.name;
    char buf[8];
    snprintf(buf, sizeof(buf), "0x%02X", vk);
    return buf;
}

bool vk_from_name(const std::string& name, int& outVk)
{
    if (name.size() == 1) {
        char c = (char)std::toupper((unsigned char)name[0]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) { outVk = c; return true; }
    }
    for (auto& e : kVkNames)
        if (_stricmp(e.name, name.c_str()) == 0) { outVk = e.vk; return true; }
    if (name.size() > 2 && name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
        char* end = nullptr;
        long v = strtol(name.c_str() + 2, &end, 16);
        if (end && *end == '\0' && v > 0 && v < 256) { outVk = (int)v; return true; }
    }
    return false;
}

std::string gamepad_combo_name(unsigned int mask)
{
    std::string out;
    for (auto& e : kGpButtons) {
        if (mask & e.mask) {
            if (!out.empty()) out += '+';
            out += e.name;
        }
    }
    return out;
}

bool gamepad_combo_from_name(const std::string& name, unsigned int& outMask)
{
    unsigned int mask = 0;
    size_t pos = 0;
    while (pos <= name.size()) {
        size_t plus = name.find('+', pos);
        std::string tok = trim(name.substr(pos, plus == std::string::npos ? std::string::npos : plus - pos));
        pos = (plus == std::string::npos) ? name.size() + 1 : plus + 1;
        if (tok.empty())
            continue;
        bool found = false;
        for (auto& e : kGpButtons) {
            if (_stricmp(e.name, tok.c_str()) == 0) { mask |= e.mask; found = true; break; }
        }
        if (!found)
            return false;
    }
    if (mask == 0)
        return false;
    outMask = mask;
    return true;
}

void init()
{
    Settings s;
    const std::string text = read_file(config_path());
    const bool fileExisted = !text.empty();
    const std::map<std::string, std::string> kv = parse_kv(text);

    bool anyMissing = false;
    std::string v;

    // Indexed by CullRole, so adding a role means adding one row here
    // rather than extending a chain of ternaries.
    // nullptr = baked into the build, no config key. Indexed by CullRole.
    const char* kRoleKeys[hooks::kCullRoleCount] = {
        nullptr, nullptr, nullptr, nullptr, "RigidShaderHash", nullptr };
    std::string* kRoleDst[hooks::kCullRoleCount] = {};
    for (int r = 0; r < hooks::kCullRoleCount; ++r) kRoleDst[r] = role_dst(s, r);

    if (kv_find(kv, "DIBRShift", v)) {
        if (_stricmp(v.c_str(), "true") == 0 || v == "1")       s.dibrShift = true;
        else if (_stricmp(v.c_str(), "false") == 0 || v == "0") s.dibrShift = false;
        else note_invalid("DIBRShift", v);
    } else anyMissing = true;

    if (kv_find(kv, "HorizonLock", v)) {
        if (_stricmp(v.c_str(), "true") == 0 || v == "1")      s.horizonLock = true;
        else if (_stricmp(v.c_str(), "false") == 0 || v == "0") s.horizonLock = false;
        else note_invalid("HorizonLock", v);
    } else anyMissing = true;

    if (kv_find(kv, "HideHud", v)) {
        if (_stricmp(v.c_str(), "true") == 0 || v == "1")      s.hideHud = true;
        else if (_stricmp(v.c_str(), "false") == 0 || v == "0") s.hideHud = false;
        else note_invalid("HideHud", v);
    } else anyMissing = true;

    if (kv_find(kv, "MenuKey", v)) {
        int vk;
        // VK_ESCAPE is reserved (cancels a rebind-in-progress), never a valid binding.
        if (vk_from_name(v, vk) && vk != VK_ESCAPE) s.menuVk = vk; else note_invalid("MenuKey", v);
    } else anyMissing = true;

    if (kv_find(kv, "MenuGamepadCombo", v)) {
        unsigned mask;
        if (gamepad_combo_from_name(v, mask)) s.menuCombo = mask; else note_invalid("MenuGamepadCombo", v);
    } else anyMissing = true;

    if (kv_find(kv, "ModUIScale", v)) {
        float f;
        if (parse_float(v, f) && f >= 0.5f && f <= 3.0f) s.modUiScale = f; else note_invalid("ModUIScale", v);
    } else anyMissing = true;

    if (kv_find(kv, "UIPlaneSize", v)) {
        float f;
        if (parse_float(v, f) && f >= 0.2f && f <= 10.0f) s.uiPlaneSize = f;
        else note_invalid("UIPlaneSize", v);
    } else anyMissing = true;

    if (kv_find(kv, "UIPlaneMode", v)) {
        if (_stricmp(v.c_str(), "headlocked") == 0)   s.uiPlaneMode = xr::kUiPlaneHeadLocked;
        else if (_stricmp(v.c_str(), "forward") == 0) s.uiPlaneMode = xr::kUiPlaneForward;
        else note_invalid("UIPlaneMode", v);
    } else anyMissing = true;

    if (kv_find(kv, "UIPlaneDistance", v)) {
        float f;
        if (parse_float(v, f) && f >= 0.3f && f <= 20.0f) s.uiPlaneDistance = f;
        else note_invalid("UIPlaneDistance", v);
    } else anyMissing = true;

    if (kv_find(kv, "RenderFovScale", v)) {
        float f;
        if (parse_float(v, f) && f >= 0.30f && f <= 1.00f) s.renderFovScale = f;
        else note_invalid("RenderFovScale", v);
    } else anyMissing = true;

    if (kv_find(kv, "MapWindowShrink", v)) {
        float f;
        if (parse_float(v, f) && f >= 0.05f && f <= 1.0f) s.mapWindowShrink = f; else note_invalid("MapWindowShrink", v);
    } else anyMissing = true;

    if (kv_find(kv, "VerticalRecenter", v)) {
        float f;
        if (parse_float(v, f) && f >= -0.5f && f <= 0.5f) s.verticalRecenter = f; else note_invalid("VerticalRecenter", v);
    } else anyMissing = true;

    if (kv_find(kv, "WarpEnabled", v)) {
        if (_stricmp(v.c_str(), "true") == 0 || v == "1")       s.warpEnabled = true;
        else if (_stricmp(v.c_str(), "false") == 0 || v == "0") s.warpEnabled = false;
        else note_invalid("WarpEnabled", v);
    } else anyMissing = true;

    if (kv_find(kv, "WarpType", v)) {
        int t;
        if (xr::warp_type_from_key(v.c_str(), t)) s.warpType = t;
        else note_invalid("WarpType", v);
    } else anyMissing = true;

    if (kv_find(kv, "DebugEyeCantDeg", v)) {
        float f;
        if (parse_float(v, f) && f >= -15.0f && f <= 15.0f) s.debugEyeCantDeg = f; else note_invalid("DebugEyeCantDeg", v);
    } else anyMissing = true;

    if (kv_find(kv, "TruckLevelLock", v)) {
        if (_stricmp(v.c_str(), "true") == 0 || v == "1")      s.truckLevelLock = true;
        else if (_stricmp(v.c_str(), "false") == 0 || v == "0") s.truckLevelLock = false;
        else note_invalid("TruckLevelLock", v);
    } else anyMissing = true;

    if (kv_find(kv, "TruckYawLock", v)) {
        if (_stricmp(v.c_str(), "true") == 0 || v == "1")      s.truckYawLock = true;
        else if (_stricmp(v.c_str(), "false") == 0 || v == "0") s.truckYawLock = false;
        else note_invalid("TruckYawLock", v);
    } else anyMissing = true;

    if (kv_find(kv, "ForceGameResolution", v)) {
        int w = 0, h = 0;
        if (_stricmp(v.c_str(), "auto") == 0) {
            s.resolutionUnset = true;   // still waiting for a runtime to size it
        } else if (parse_size(v, w, h)) {
            s.forceResW = w; s.forceResH = h;
            s.resolutionUnset = false;  // includes an explicit "off"
        } else {
            note_invalid("ForceGameResolution", v);
        }
    } else anyMissing = true;   // absent -> resolutionUnset keeps its default, true

    if (kv_find(kv, "ScreenGate", v)) {
        // "fingerprint" was this option's name until the garage/gameplay half
        // stopped coming from shaders. Still accepted so an existing config
        // keeps working; it is written back as "direct".
        if (_stricmp(v.c_str(), "direct") == 0 ||
            _stricmp(v.c_str(), "fingerprint") == 0)   s.screenGateDirect = true;
        else if (_stricmp(v.c_str(), "legacy") == 0)   s.screenGateDirect = false;
        else note_invalid("ScreenGate", v);
    } else anyMissing = true;

    if (kv_find(kv, "CameraFovFactor", v)) {
        float f;
        if (parse_float(v, f) && f >= 0.5f && f <= 2.5f) s.cameraFovFactor = f;
        else note_invalid("CameraFovFactor", v);
    } else anyMissing = true;

    if (kv_find(kv, "DisableWindows", v)) {
        if (_stricmp(v.c_str(), "true") == 0 || v == "1")      s.disableWindows = true;
        else if (_stricmp(v.c_str(), "false") == 0 || v == "0") s.disableWindows = false;
        else note_invalid("DisableWindows", v);
    } else anyMissing = true;

    // Reuses the list parser for a single value: same hex form, same rules,
    // and it already rejects the mistakes worth catching (empty, too long,
    // trailing junk, zero).
    for (int r = 0; r < hooks::kCullRoleCount; ++r) {
        const char* key = kRoleKeys[r];
        if (!key || !kRoleDst[r]) continue;   // baked-in role, no key to read
        std::string& dst = *kRoleDst[r];
        if (kv_find(kv, key, v)) {
            uint64_t probe[hooks::kMaxRoleHashes];
            int got = 0;
            if (parse_hash_list(v, probe, hooks::kMaxRoleHashes, got) && got >= 1) dst = v;
            else note_invalid(key, v);
        } else anyMissing = true;
    }

    if (kv_find(kv, "CullShaders", v)) {
        if (_stricmp(v.c_str(), "true") == 0 || v == "1")      s.cullShaders = true;
        else if (_stricmp(v.c_str(), "false") == 0 || v == "0") s.cullShaders = false;
        else note_invalid("CullShaders", v);
    } else anyMissing = true;

    if (kv_find(kv, "CullShaderHashes", v)) {
        // Validated by parsing it here; the values themselves are handed to
        // the module below rather than kept in Settings, since that is where
        // the live set lives.
        uint64_t probe[64];
        int probeCount = 0;
        if (parse_hash_list(v, probe, 64, probeCount)) s.cullShaderHashes = v;
        else note_invalid("CullShaderHashes", v);
    } else anyMissing = true;

    if (anyMissing) {
        VRLOG("config: Snowrunner_VR_config.txt is missing one or more keys -- filling defaults");
    } else if (fileExisted) {
        VRLOG("config: loaded Snowrunner_VR_config.txt");
    }

    xr::set_dibr_shift_enabled(s.dibrShift);
    hooks::set_horizon_lock_enabled(s.horizonLock);
    hooks::set_hide_hud_enabled(s.hideHud);
    hooks::set_menu_toggle_vk(s.menuVk);
    hooks::set_menu_toggle_gamepad_combo(s.menuCombo);
    hooks::set_mod_ui_scale(s.modUiScale);
    xr::set_ui_plane_size(s.uiPlaneSize);
    xr::set_ui_plane_mode(s.uiPlaneMode);
    xr::set_ui_plane_distance(s.uiPlaneDistance);
    xr::set_render_fov_scale(s.renderFovScale);
    xr::set_map_window_shrink(s.mapWindowShrink);
    xr::set_vertical_recenter(s.verticalRecenter);
    xr::set_warp_enabled(s.warpEnabled);
    xr::set_warp_type(s.warpType);
    xr::set_debug_eye_cant_deg(s.debugEyeCantDeg);
    hooks::set_truck_level_lock_enabled(s.truckLevelLock);
    hooks::set_truck_yaw_lock_enabled(s.truckYawLock);
    hooks::set_screen_gate_direct(s.screenGateDirect);
    hooks::set_camera_fov_factor(s.cameraFovFactor);
    hooks::set_force_resolution(s.forceResW, s.forceResH);
    g_resolutionUnset.store(s.resolutionUnset);

    // Hashes first: add_culled_hash() checks each against the named shaders,
    // so the roles have to be pointed at the right values before it runs or a
    // config override would be missed and the hash would land in the anonymous
    // list instead.
    // ADDED TO the built-in set, never replacing it: the value holds only what
    // this install found on top (see refresh_role_hashes). add_cull_role_hash
    // skips duplicates and refuses non-extensible roles, so a hand-edited file
    // cannot corrupt a baked list.
    for (int r = 0; r < hooks::kCullRoleCount; ++r) {
        if (!kRoleDst[r]) continue;
        const std::string& src = *kRoleDst[r];
        uint64_t hs[hooks::kMaxRoleHashes];
        int got = 0;
        if (src.empty() || !parse_hash_list(src, hs, hooks::kMaxRoleHashes, got) || got < 1)
            continue;
        for (int i = 0; i < got; ++i)
            hooks::add_cull_role_hash((hooks::CullRole)r, hs[i]);
    }
    hooks::set_cull_role_enabled(hooks::kCullWindows, s.disableWindows);

    hooks::clear_culled_hashes();
    {
        uint64_t hashes[64];
        int n = 0;
        parse_hash_list(s.cullShaderHashes, hashes, 64, n);
        for (int i = 0; i < n; ++i) hooks::add_culled_hash(hashes[i]);
        if (n > 0) VRLOG("config: %d shader hash(es) marked for culling", n);
    }
    hooks::set_shader_cull_enabled(s.cullShaders);

    // Re-read the cull settings from the module before writing: add_culled_hash()
    // moves a hash the search found into whichever named setting owns it, which
    // happens AFTER `s` was filled in. Without this the file would be written
    // back in its pre-migration form and migrate again on every single launch.
    s.disableWindows         = hooks::cull_role_enabled(hooks::kCullWindows);
    s.cullShaderHashes       = hash_list_str();
    refresh_role_hashes(s);

    if (!fileExisted || anyMissing)
        write_defaults_or_current(s);
}

bool resolution_unset() { return g_resolutionUnset.load(); }

void save()
{
    Settings s;
    s.dibrShift       = xr::dibr_shift_enabled();
    s.horizonLock     = hooks::horizon_lock_enabled();
    s.hideHud         = hooks::hide_hud_enabled();
    s.menuVk          = hooks::menu_toggle_vk();
    s.menuCombo       = hooks::menu_toggle_gamepad_combo();
    s.modUiScale      = hooks::mod_ui_scale();
    s.uiPlaneSize     = xr::ui_plane_size();
    s.uiPlaneMode     = xr::ui_plane_mode();
    s.uiPlaneDistance = xr::ui_plane_distance();
    s.renderFovScale  = xr::render_fov_scale();
    s.mapWindowShrink = xr::map_window_shrink();
    s.verticalRecenter = xr::vertical_recenter();
    s.warpEnabled      = xr::warp_enabled();
    s.warpType         = xr::warp_type();
    s.debugEyeCantDeg = xr::debug_eye_cant_deg();
    s.truckLevelLock = hooks::truck_level_lock_enabled();
    s.truckYawLock = hooks::truck_yaw_lock_enabled();
    s.screenGateDirect = hooks::screen_gate_direct();
    s.cameraFovFactor    = hooks::camera_fov_factor();
    // Answering the question is what clears it, and setting a real size is the
    // only way to answer it -- so this is the one place that needs to remember.
    if (hooks::force_resolution_w() > 0) g_resolutionUnset.store(false);
    s.resolutionUnset = g_resolutionUnset.load();
    s.forceResW = hooks::force_resolution_w();
    s.forceResH = hooks::force_resolution_h();
    s.cullShaders = hooks::shader_cull_enabled();
    s.cullShaderHashes = hash_list_str();
    s.disableWindows      = hooks::cull_role_enabled(hooks::kCullWindows);
    refresh_role_hashes(s);
    write_defaults_or_current(s);
}

} // namespace vrcfg
