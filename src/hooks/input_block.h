#pragma once

// Stops game input while the mod's settings menu is open.
//
// Measured, not assumed (the log lines this module emits are what established
// all of it):
//
//  - The exe has NO XInput import, yet blocking XInput is what stops the
//    gamepad reaching the game -- so it resolves XInput at runtime, which is
//    invisible to the import table.
//  - It binds DINPUT8.dll and creates DirectInput devices constantly, but only
//    ever KEYBOARD and MOUSE ones -- never a joystick. So DirectInput carries
//    the keyboard, not the pad.
//  - The mod's own menu polling also goes through XInput, so a blocked call
//    has to be told apart from ours. That CANNOT be done by looking at the
//    return address: the Steam overlay hooks XInputGetState too, and hooked it
//    after we did, so every caller -- the game and this mod alike -- arrives
//    through Steam's trampoline presenting the same return address inside
//    gameoverlayrenderer64.dll. Our own polling is flagged explicitly instead,
//    via xinput_poll_self() below.
// <windows.h> first: xinput.h has no includes of its own and fails with
// "No Target Architecture" if it lands in a translation unit ahead of it.
#include <windows.h>
#include <xinput.h>
namespace hooks {

// Hooks DirectInput8Create so the device objects the game creates can be
// intercepted, and whatever XInput DLL is loaded so far. MUST run before the
// game creates its devices, i.e. from DllMain, after hooks::init() (MinHook).
// Idempotent.
void install_input_block();

// Call every Present. Retries the XInput scan until it finds something: the
// game has no XInput import and resolves it at runtime, so the DLL it uses may
// appear long after DllMain. Costs nothing once a hook has landed.
void input_block_on_present();

// The ONLY way the mod should read the gamepad. Marks the call as ours for the
// duration, so it passes through the block that is silencing everyone else
// while the menu is open -- which is what keeps the menu drivable by the same
// controller it is silencing. Identical semantics to XInputGetState().
DWORD xinput_poll_self(DWORD userIndex, XINPUT_STATE* state);

} // namespace hooks
