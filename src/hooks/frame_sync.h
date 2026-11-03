#pragma once
#include <cstdint>

// SYNCHED SEQUENTIAL'S CLOCK: every second game update with its time stopped.
//
// The engine runs a DXUT-style loop on its main thread -- OnFrameMove(time,
// elapsed) updates the game, then OnFrameRender(device, time, elapsed) renders
// and presents -- and hands both their time explicitly. Stopping that time on
// every second update makes two consecutive frames render the same moment,
// which is what lets the two eyes of a pair agree. Measured 2026-10-27 before
// any of this was built (docs/same_frame_stereo.md): with the eye offset
// zeroed, every frozen frame came back pixel-identical to the one before it, on
// the menus and in-game while driving.
namespace hooks {

// Hooks both callbacks at the addresses game_addresses() resolved. From the
// first CreateDXGIFactory, after the resolver.
void install_frame_sync();

// Both callbacks hooked. False means this game build hid one of them, and
// Synched Sequential is not available.
bool frame_sync_ready();

// While on, updates alternate: a REAL one carrying all the time since the last
// real one, then a FROZEN repeat with the same time and nothing elapsed. The
// game's clock keeps up with the wall clock; it just moves in pairs. Off is the
// engine untouched.
void set_frame_freeze(bool on);
bool frame_freeze_on();

// The update (OnFrameMove) most recently run, counted from 1, and whether a
// recent one -- within the last 32 -- was a frozen repeat. The next update's
// kind is known in advance, since the alternation is ours.
uint32_t frame_update_index();
bool     frame_update_frozen(uint32_t index);
bool     frame_next_update_frozen();

// How many updates behind a render is the update it shows: 0 on the menus,
// 1 in-game (both measured). OnFrameRender is handed the time of the update
// it renders, so render-side animation stops together with the world it is
// drawn on -- without it, foliage kept moving inside an in-game pair.
void set_frame_render_lag(int lag);

} // namespace hooks
