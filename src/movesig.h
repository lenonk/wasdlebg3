/* Locating BG3's movement machinery.
 *
 * This is the knowledge that belongs to *this* mod rather than to the extender:
 * which bytes identify the movement-input fetch, which byte gates it, and which
 * branch skips it. Everything here is derived from instruction encodings at
 * runtime — no address is hardcoded, so a game patch that moves this code does
 * not silently corrupt anything, it simply fails to match and the plugin
 * declines to load.
 *
 * The functions are pure over (code, len, base_va) so they can be tested
 * offline against a copy of the game binary. In-process the plugin gets those
 * three values from bg3lese's api->image().
 */
#ifndef WASD_MOVESIG_H
#define WASD_MOVESIG_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uintptr_t global_slot;  /* address OF the pointer to the input state block */
    uint32_t vec_off;       /* forced-input vec2 within that block  (0x1394 on 4.1.1.7398727) */
    uint32_t flag_off;      /* forced-input enable byte             (0x139c) */
    uintptr_t match_va;     /* where the override prologue was found */
    uintptr_t fetch_fn;     /* the containing function's entry point */
} move_sig;

/* Returns 1 on a unique confident match, 0 if not found, -1 if ambiguous —
 * refusing rather than guessing which of several candidates was meant. */
int move_find_sig(const uint8_t *code, size_t len, uintptr_t base_va, move_sig *out);

/* The controller-mode flag: one byte the game checks before it will ask for
 * movement input at all. Found by consensus across the fetch's call sites, so
 * it is not sensitive to any single caller's shape. 0 if not found. */
uintptr_t move_find_padmode_flag(const uint8_t *code, size_t len, uintptr_t base_va,
                                 uintptr_t fetch_fn);

/* The `je` branches that skip the fetch when that flag is clear. Writing the
 * flag loses a per-frame race against the engine's input-mode arbiter, so these
 * are what actually get NOPed. Returns how many were found. */
size_t move_find_gates(const uint8_t *code, size_t len, uintptr_t base_va,
                       uintptr_t fetch_fn, uintptr_t flag,
                       uintptr_t *out, size_t max);

/* Live movement controls, from a signature match. */
typedef struct {
    uintptr_t block;
    volatile float *vec;    /* [0] strafe, right positive; [1] forward, W positive */
    volatile uint8_t *flag; /* nonzero => the game uses vec instead of polling */
} move_ctl;

/* 0 until the game has allocated the block, so callers must retry. */
int move_ctl_resolve(const move_sig *sig, move_ctl *out);

#endif
