#pragma once
#include <cstdint>

struct IDXGISwapChain;

// Suppressing individual DRAW CALLS by the identity of the pixel shader they
// run. Built for the truck windscreen, which DIBR shift cannot handle by any amount
// of reprojection maths:
//
//   * The glass REFRACTS what is behind it but writes no depth, so the depth
//     buffer at those pixels holds the terrain far beyond the cab. DIBR then
//     shifts the refracted image by the terrain's disparity instead of the
//     glass's, and the warp lands in the wrong place.
//   * Mud splatters and rain sit ON the glass, an arm's length away, but
//     inherit that same far-away depth. Each eye disagrees about where they
//     are by a large margin, so they never fuse -- they read as permanently
//     doubled.
//
// No depth-side fix exists: the depth buffer is telling the truth about the
// geometry, the colour buffer just isn't showing that geometry. The only
// clean answer is to not draw the glass at all.
//
// MECHANISM. The Draw detours in ui_hook.cpp already sit on all four draw
// entry points of BOTH context vtables, so suppressing a draw is just not
// calling through. This module supplies the decision. Asking "is the bound
// pixel shader one we were told to cull" per draw would mean a PSGetShader
// (an AddRef/Release pair) on the hottest path in the process, so instead the
// answer is computed once per PSSetShader and cached in a thread-local: the
// draw path is one bool read.
//
// Deferred contexts are handled at RECORD time, which is the only time that
// can work -- a replayed command list has its draws baked in and never calls
// back into us (the same wall ui_hook.cpp hit with the HUD). The consequence
// is that a change to the cull set only becomes visible once the affected
// command list is re-recorded, which for this engine is frequent but not
// instant.
//
// IDENTITY. A shader's ID3D11PixelShader pointer is meaningless across
// sessions, so the cull set is keyed on the first 8 bytes of the DXBC
// container checksum, captured by hooking ID3D11Device::CreatePixelShader.
// That is the compiler's own hash of the bytecode: stable across runs, and
// the same value fxc/RenderDoc report, so a shader found once can be written
// to the config file and stay found.
namespace hooks {

// Hooks CreatePixelShader on the device vtable and PSSetShader on both the
// immediate and deferred context vtables. Idempotent; safe to call every
// Present until it succeeds.
bool install_shader_cull(IDXGISwapChain* swapchain);

// Rolls the per-frame draw counters shown in the settings UI.
void shader_cull_on_present();

// Which role the pixel shader bound on THIS thread belongs to, or -1. Valid
// between a PSSetShader and the draws that follow it, so it answers "what is
// about to be drawn" from inside a draw detour without a PSGetShader call.
// This is how ui_hook.cpp gates the HUD viewport shrink and tags a command
// list as containing UI, in both cases replacing a heuristic that could only
// guess.
int current_draw_role();

// The bound shader's stable bytecode hash, or 0. Same thread-local as
// current_draw_role(), for consumers that need to treat one member of a
// role differently from the rest.
uint64_t current_draw_hash();

// --- hot path, called from ui_hook.cpp's draw detours --------------------
// `elemCount` is the draw's index or vertex count. It is recorded per shader
// and is what separates real geometry from a fullscreen pass: tonemap,
// composite and copy passes draw a single triangle or quad (3-6 elements),
// and suppressing those is what turns the whole frame black. The search
// filters them out on exactly this.
bool cull_current_draw(unsigned elemCount);

// A replayed command list leaves whatever IT last bound as the current pixel
// shader, without going through our PSSetShader detour -- so the cached
// answer above is stale afterwards. Call this after ExecuteCommandList. It
// fails OPEN (nothing culled until the next explicit bind), which is the safe
// direction: a missed cull is a visible windscreen, an over-eager one is a
// hole in the frame.
void note_ps_state_lost();

// The mod's own rendering (mirror blit, stale-eye warp, ImGui menu) issues
// draws through the very same hooked vtables, and its shaders would otherwise
// be registered as candidates and become cullable -- a search step that
// happened to include the blit shader would black out the image being used to
// judge the search. Bracket our own draws with this and they are ignored
// entirely: not culled, not registered, not counted.
void set_self_render(bool on);

// --- named shaders -------------------------------------------------------
// Found once with the search below, and a DXBC hash is a property of the GAME
// rather than of a session or a machine -- so these ship as defaults and every
// user gets a working checkbox without ever running a search.
//
// BAKED IN, and for four of the five roles that is the whole story: there is no
// config key and the search cannot add to them. They name a closed set of
// effects -- the glass, the muck on it, the UI, the mirrors -- that was found
// once and is done, so a per-install override could only ever go stale against
// a later build or be broken by a mis-answered search. A game patch that
// recompiles one of them needs a new build, which is the honest cost.
//
// kCullRigid is the exception and stays open: it is a map of every vehicle in
// the game, it is genuinely unfinished, and its config key holds ONLY what a
// given install found beyond the built-in set (see cull_role_builtin_count).
//
// These are independent of the master switch below, which governs only the
// experimental list and the search. A named toggle is a normal setting.
// Each role owns a SET of hashes, not one. A game compiles permutations of the
// same material -- rain, mud and snow on the glass are different shaders, as
// are quality variants -- so one name can legitimately cover several. This is
// also why the identity is the bytecode hash rather than the shader pointer:
// the same bytecode created as several distinct COM objects collapses to one
// entry by construction.
// Not every role is about CULLING. A role is really "these shaders are this
// thing"; whether that identity is used to suppress the draw, gate a viewport
// rewrite, tag a command list or duplicate the draw elsewhere is up to the
// consumer. kCullUi and kCullMirror are identification-first: they default to
// enabled=false and are read by ui_hook.cpp and the DIBR shift path.
enum CullRole {
    kCullWindows = 0,     // the cab glass itself, refraction and all
    kCullWindowSmudge,    // the mud, rain and snow sitting ON that glass
    kCullUi,              // every in-game UI shader: HUD, pause, map, garage, menu
    kCullMirror,          // the mirror surfaces -- reflected content, no usable depth
    // Everything RIGID WITH THE CAMERA: truck body, cab interior, trailers and
    // their loads. Identification-only -- these draws are never suppressed.
    //
    // The 6-DoF stale-eye reprojection moves every pixel by the camera's own
    // travel, which is right for the world and wrong for anything bolted to the
    // camera. A depth threshold used to guess at that, which only worked
    // inside a cab and only because nothing in the WORLD is within a metre or
    // two there; it was removed 2026-08-20. This role answers the same question
    // exactly, from the renderer's own idea of what the truck is, and keeps
    // working in exterior views where the depth heuristic could not.
    kCullRigid,
    // The winch anchor-point markers. Identification only -- these draws are
    // never suppressed and, as of 2026-08-24, nothing reads this role.
    //
    // It is kept because the hashes are the expensive part. A size slider was
    // built on them and removed the same day: the only lever a draw detour has
    // over a draw it does not understand is the viewport, and scaling that
    // moves each icon away from the screen position it exists to mark. Doing
    // it properly needs the vertex-side size constant, and whoever finds that
    // should not have to find these again first.
    kCullWinchMarker,
    kCullRoleCount
};

// Sized for kCullRigid, which is the only role that is a MAP rather than a
// handful of named shaders: every vehicle, trailer and cab interior in the game
// contributes, and the list grows as more are driven. The other roles use a
// dozen at most and cost nothing for the headroom.
//
// The whole table is g_roleHash[kCullRoleCount][kMaxRoleHashes] of atomics --
// 8 KB at this size, static. role_of_hash() scans it linearly, but only when a
// pixel shader is seen for the FIRST time (and on retag), never per draw or per
// bind: the per-draw path reads one cached thread-local bool.
constexpr int kMaxRoleHashes = 200;

bool     cull_role_enabled(CullRole role);
void     set_cull_role_enabled(CullRole role, bool on);
int      cull_role_hash_count(CullRole role);
uint64_t cull_role_hash_at(CullRole role, int index);

// How many of that count SHIPPED IN THE BUILD. Anything at or past this index
// was found by this install's own search, and is the only part worth writing to
// the config -- persisting the built-in ones too would freeze a stale copy of
// them into every user's file and quietly override the next build's list.
int      cull_role_builtin_count(CullRole role);

// PER-HASH MUTE -- session only, never saved. A muted entry keeps its place in
// the list but stops answering to the role, so it drops out of everything that
// reads the role at once: the 6-DoF reprojection's rigid mask and the hide-
// rigid switch alike. For deciding whether an entry belongs in the list at all.
bool     cull_role_hash_muted(CullRole role, int index);
void     set_cull_role_hash_muted(CullRole role, int index, bool muted);

// Whether that hash has been bound at all this session, asked by HASH so it
// still answers for a muted entry -- "has this ever drawn" is the question you
// need while deciding whether to mute it.
bool     cull_role_hash_seen(CullRole role, int index);

// Whether the search may add to this role at all. Only kCullRigid: the other
// four name a closed set of effects that was found once and is done, so the
// hashes are baked and there is no config key for them. add_cull_role_hash()
// refuses (and logs) for the rest.
bool     cull_role_extensible(CullRole role);
void     add_cull_role_hash(CullRole role, uint64_t hash);
// How many of the role's hashes have actually been seen bound this session.
// Zero while the role is enabled means the checkbox can do nothing -- the
// hashes are stale after a game patch, or nothing has drawn them yet.
int      cull_role_seen_count(CullRole role);
const char* cull_role_key(CullRole role);   // "windows" / "window smudge"

// --- screens -------------------------------------------------------------
// (THE SCREEN PROBE lived here: a per-shader record of which screens each
// pixel shader had been seen drawing on, a sidecar file of the evidence, and a
// tab for collecting it. Removed 2026-08-26, its job done -- the map's five
// markers are baked into shader_cull.cpp and the answer does not need
// re-deriving. A DXBC checksum is the same number tomorrow, on another machine,
// after a reinstall, which is exactly why this was worth probing where the
// screen-state OBJECT hunt was not: that searched heap allocations with no
// identity past a restart, so every session had to find it again.)
constexpr int kScreenProbeCount = 4;
enum { kScreenMenu = 0, kScreenGarage = 1, kScreenMap = 2, kScreenGameplay = 3 };

// "MENU", "GARAGE", "MAP", "GAMEPLAY" -- one spelling, shared by the log and
// the UI so a screen cannot be called two things in two places.
const char* screen_name(int screen);


// --- the fingerprint gate ------------------------------------------------
// WHICH SCREEN WE ARE ON, answered from the probe's result instead of from the
// camera's pose.
//
// The pose classifier (hooks::map_by_pose) has a known failure it cannot fix:
// it calls the map at any camera pitched -45 deg near the world origin, which
// gameplay can reach. It is a heuristic about where the camera IS. This asks a
// different question -- what is the game DRAWING -- and a shader that only ever
// draws on one screen answers it directly.
//
// THE MAP IS THE ONLY SCREEN WITH A MARKER SET, and its five hashes are baked
// -- see kMapMarkers in shader_cull.cpp for how they were arrived at, and why
// the menu's set was withdrawn rather than kept. A screen is claimed only when
// EVERY shader in its list is drawing, which makes the length of the list the
// strength of the claim, and makes a bad member show up as a screen that never
// fires rather than as an occasional wrong answer.
//
// Off by default: a screen the gate cannot see falls back to garage, which is a
// visible change mid-drive. Legacy stays the shipping behaviour until this has
// hours on it.
bool screen_gate_direct();
void set_screen_gate_direct(bool on);

// One of kScreenMenu/kScreenGarage/kScreenMap/kScreenGameplay, updated once per
// Present.
//
// Only MAP is ever claimed. GARAGE IS THE FALLBACK -- what you get when nothing
// claimed you -- and the menu and gameplay are never claimed here at all.
//
// None of the three can be, and every route was tried. No shader draws only in
// the garage, because it shares its interface with the menus and its world with
// gameplay. The engine's own DRIVE_CAMERA mode enum, which MudRunner's PDB names
// and which would have settled it, is unreadable in SnowRunner's garage: that
// screen does not tick the drive camera at all (see camera_hook.h). And the
// menu's one exclusive marker turned out to draw inside a level too.
//
// Callers separate the two with hooks::logic_camera_idle_ms() instead. The drive
// camera falling silent is not a proxy for the garage -- it is the same fact
// measured directly, and it needs nothing discovered first.
//
// Use direct_screen_positive() to tell a real answer from the fallback.
//
// ALWAYS ANSWERS, whether or not the gate is switched on, so the log line keeps
// reporting what it WOULD say while the gate is running on legacy.
int direct_screen();

// TRUE when the current answer was positively identified -- a marker list fully
// satisfied, or the drive camera's mode field read -- rather than the garage
// fallback. Anything that switches the render mode should require this: "failed
// to identify a screen" and "identified a static screen" are different claims,
// and only the second is safe to act on.
bool direct_screen_positive();

// --- master switch (experimental list + search only) ---------------------
bool shader_cull_enabled();
void set_shader_cull_enabled(bool on);

// --- browsing what has been seen ----------------------------------------
struct CullShaderInfo {
    void*    ps = nullptr;
    uint64_t hash = 0;          // 0 = shader predates our hook, cannot be saved
    uint32_t drawsPerFrame = 0; // last completed frame
    uint32_t maxElems = 0;      // largest index/vertex count seen
    bool     culled = false;
    bool     inSearchRange = false;
    bool     isCandidate = false;   // passes the current search filter
};

int  cull_shader_count();
bool cull_shader_info(int index, CullShaderInfo& out);   // false if out of range

// Sets the session flag AND, when the hash is known, adds/removes it from the
// persisted set -- so ticking a box in the UI is all that's needed for the
// setting to survive vrcfg::save().
void set_cull_shader_culled(int index, bool culled);

// --- search filter -------------------------------------------------------
// The first version of this searched EVERY shader and was unusable: a step
// that happened to include the final composite pass blacked out the whole
// frame, and "is the windscreen gone" cannot be answered against a black
// screen. Two properties keep the frame alive without ever excluding the
// glass:
//
//   minElems      - a fullscreen pass draws 3-6 elements. Real world geometry
//                   does not. Everything at or below this is left alone, which
//                   covers tonemap, composite, blur and the final copy.
//   maxDrawsFrame - terrain, foliage and shadow casters run to hundreds of
//                   draws a frame. A windscreen is a handful.
//
// Both are adjustable from the settings UI, because they are heuristics: if a
// search runs out of candidates without finding the glass, widening them is
// the next thing to try.
void set_search_filter(unsigned minElems, unsigned maxDrawsFrame);
unsigned search_min_elems();
unsigned search_max_draws_frame();
int      search_candidate_count();   // how many shaders pass the filter now

// --- bisect search -------------------------------------------------------
// Stepping one-at-a-time through even the filtered list is slow in a headset,
// so the search halves it per answer: suppress half the candidates, the user
// says whether the windscreen went away, keep whichever half contained it.
//
// The candidate list is a snapshot taken when the search starts -- shaders
// first seen mid-search would otherwise renumber the range out from under it.
struct CullSearch {
    bool     active = false;
    int      lo = 0, hi = 0;           // the target is somewhere in [lo, hi)
    int      testCount = 0;            // how many shaders are suppressed right now
    int      total = 0;                // candidate count when the search started
    int      step = 0;
    bool     found = false;
    uint64_t foundHash = 0;
    int      foundIndex = -1;          // index into the FULL list, for the table
};

CullSearch cull_search_state();
void cull_search_begin();
void cull_search_answer(bool windscreenGone);
void cull_search_cancel();

// Draws suppressed during the last completed frame -- the honest answer to
// "is this doing anything at all", and the quickest way to tell a cull that
// never reaches the geometry from one that reaches it and does not help.
uint32_t suppressed_draws_last_frame();

// --- persisted set (config.cpp) -----------------------------------------
int      culled_hash_count();
uint64_t culled_hash_at(int index);
void     add_culled_hash(uint64_t hash);
void     clear_culled_hashes();

} // namespace hooks
