#pragma once
#include <windows.h>
#include <cstdint>
#include <vector>

// SCANNING SNOWRUNNER.EXE IN MEMORY.
//
// Every game update relinks the exe and moves every function in it, so no
// address in this mod may be a hard-coded offset: each one is FOUND at startup,
// in the running image, and checked before anything is patched. See
// game_addresses.h for the targets and how each one is located.
//
// IN MEMORY ONLY. The exe is Steam-DRM wrapped -- its .text is encrypted on
// disk and decrypted in place by the stub before the game's own code runs -- so
// these scans are only meaningful once the game has started. By the first
// CreateDXGIFactory call it has. The exception directory (.pdata) and the
// unwind data it points at are NOT encrypted, which is what makes
// is_function_entry() usable as an independent check.
namespace hooks {

// Scans the executable sections of `module` for an IDA-style pattern, e.g.
// "48 8B 05 ?? ?? ?? ?? F3 0F 10 40 ?? C3". Every match, up to `cap` of them
// written to `out`; returns the TOTAL number of matches, which can exceed `cap`.
//
// There is deliberately no "first match" version. A signature is only trusted
// when this returns exactly 1: two matches mean the pattern no longer says
// which one it meant, and taking the first would be a guess.
int pattern_scan_all(HMODULE module, const char* ida_pattern, uint8_t** out, int cap);

// TRUE when `p` is the first byte of a function, according to the image's own
// exception directory: a RUNTIME_FUNCTION begins exactly here and its unwind
// data is not chained (a chained entry is a split-off fragment of another
// function, not an entry point).
//
// An address that falls INSIDE a RUNTIME_FUNCTION is rejected outright. That is
// the failure this exists for: a stale offset from an older build lands
// mid-function in the new one, and MinHook will happily overwrite five bytes of
// live code there.
//
// Leaf functions have no entry at all. For those it settles for the address
// being 16-byte aligned with int3/ret immediately before it -- the shape of an
// MSVC function boundary.
bool is_function_entry(const void* p);

// The primary chunk of the function containing `p`, as [begin, end). Follows
// chained unwind data back to the function's entry, so a call site found in a
// split-off fragment still reports the function it belongs to.
bool function_range(const void* p, uint8_t*& begin, uint8_t*& end);

// Every direct call (E8 rel32) to `callee` in the executable sections, as the
// address of each E8 byte. A byte scan, not a disassembly: an E8 that is really
// the middle of some other instruction would have to carry exactly this
// callee's displacement to be counted, and callers narrow the result by the
// function each site belongs to anyway (function_range).
std::vector<uint8_t*> find_calls_to(HMODULE module, const void* callee);

// The target of a rel32 operand: `insn` is the first byte of the instruction,
// `dispOffset` where in it the 32-bit displacement starts, and `insnLen` its
// total length -- rel32 is measured from the END of the instruction.
//   E8 xx xx xx xx            call rel32           -> (insn, 1, 5)
//   48 8B 05 xx xx xx xx      mov rax,[rip+disp32] -> (insn, 3, 7)
uint8_t* rel32_target(const uint8_t* insn, int dispOffset, int insnLen);

// Every instruction `opcode` disp32 in the executable sections whose
// RIP-relative target is `target`, as the address of its first byte. `opcode`
// is everything before the displacement -- {48 8D 15} for lea rdx,[rip+disp32]
// -- and the displacement must END the instruction, which is what lets the
// target be computed without a disassembler. A byte scan, like find_calls_to.
std::vector<uint8_t*> find_rip_refs(HMODULE module, const uint8_t* opcode, int opLen,
                                    const void* target);

// Every copy of the string `s` in the image's read-only data, matched WHOLE:
// starting after a NUL and ending on its own terminator, so "XInputGetState"
// finds neither "XInputGetStateEx" nor "MyXInputGetState".
std::vector<uint8_t*> find_cstrings(HMODULE module, const char* s);

// The import slot `module` calls `fn` through, found by NAME in its own import
// table -- so it is found even when the function itself is already detoured by
// somebody else. Any DLL when `dll` is null. Null when `fn` is not imported by
// name.
void** import_slot(HMODULE module, const char* dll, const char* fn);

// SnowRunner.exe's link timestamp and image size: together, the game build.
// Logged once at startup so every log says which build it came from.
uint32_t exe_timestamp();
uint32_t exe_image_size();
void log_game_build();

// DumpGameCode. Writes the whole mapped image, decrypted, beside the log as
// snowrunner_image_<timestamp>.bin -- file offset == RVA, so it loads in a
// disassembler as a raw binary at base 0x140000000 and every address in it
// matches the exe+offset values in the log. Once per game build: an existing
// file for this build is left alone.
//
// This is what new signatures are written from after a game update. Nothing
// else reads the decrypted code: the encrypted file on disk is useless for it.
void set_dump_game_code(bool on);
bool dump_game_code();
void maybe_dump_game_image();

} // namespace hooks
