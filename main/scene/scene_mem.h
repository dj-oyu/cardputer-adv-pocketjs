#pragma once
#include <stdbool.h>
#include <stddef.h>

// Scratch for the home screen's background scenes, in two tiers.
//
// No two scenes can be live at once -- `mode` picks one of seven backgrounds --
// and main.c's loop paints no screen at all while a guest owns the display. As
// separate .bss arrays they were nonetheless resident from boot to power-off:
// 30,671 bytes for solar_sail and 7,845 for flower, on a board where an app
// cannot bring the radio up because esp_wifi_init is reached with 9 KB free and
// needs about 48.
//
// The two tiers exist because "shared" and "large" want opposite things:
//
//   core  a modest block every scene borrows in turn. Shared, so it is sized to
//         the largest core rather than to the sum, and it never shrinks -- the
//         churn of freeing and retaking a few KB on every mode change buys
//         nothing.
//
//   bulk  the part that swells for one particular mode. Owned by one scene at a
//         time and thrown away the moment another scene takes the core, so
//         picking solar_sail once does not leave its 23 KB sitting behind the
//         next background for the rest of the boot.
//
// Both are given back when an app takes the display. The memory is then simply
// in the heap again, which is where the JS guest, the font atlas and the Wi-Fi
// driver take theirs: sharing with the guest needs no arena inside the guest,
// only that this one is not being held.
//
// Both are taken at the home screen, where about 228 KiB is free, so the
// contiguity even a 23 KB bulk request needs is not in doubt. Neither is ever
// taken while an app runs.

// Both blocks are 16-byte aligned. That is part of the contract rather than
// something each scene arranges, because of how the failure looks: the vector
// rows load 128 bits at a time, and per the TRM a 128-bit load forces the low
// four address bits to zero rather than faulting. A block on malloc's usual
// 8-byte boundary would therefore not crash -- it would read eight bytes
// earlier than asked and draw a plausible picture, on a board with no debugger
// attached. A defect that produces a believable frame is one nobody reports
// and nobody bisects, so it is made impossible here instead of being a rule
// every future scene has to remember. The cost is fifteen bytes per block.
//
// A block handed back with *rebuild set is also zeroed. That is not a substitute
// for honouring the flag -- a scene that forgets to rebuild a cache reads zeros
// instead of somebody else's data, which is still wrong -- it is here for the
// one thing a scene deliberately does not write: the ocean's column block ends
// in a 32-byte pad that its vector row loads and discards, and in .bss those
// bytes were zero. Nobody should have to reason about registers filled from
// uninitialised memory because an array moved to the heap.

// Borrows the shared core. `owner` is any stable address unique to the caller;
// the address of one of its own statics will do.
//
// *rebuild is set when the contents are not the caller's: another scene had the
// block, or it was released and taken again. The caller must then redo whatever
// it caches there. Making that an output rather than something each scene
// remembers is deliberate -- a "have I initialised" flag kept outside the block
// survives the recycling that invalidates it, which is the one bug this shape
// cannot have.
//
// Taking the core also evicts any bulk belonging to somebody else, which is
// what makes a mode change give the large allocation back without every scene
// having to know the others exist.
//
// Returns NULL when the block cannot be had. A scene must draw something
// without it rather than fail; nothing here is worth a reboot on the home
// screen.
void *scene_mem(const void *owner, size_t bytes, bool *rebuild);

// Borrows the bulk slot, which holds one owner's allocation at a time. Call it
// after scene_mem(), and treat NULL as "draw the cheap version": a scene that
// needs bulk should still produce a picture without it where it can.
void *scene_bulk(const void *owner, size_t bytes, bool *rebuild);

// Gives both back. Called when an app takes the display, and safe to call when
// nothing is held. The next scene to ask takes a fresh block and is told to
// rebuild.
void scene_mem_release(void);
