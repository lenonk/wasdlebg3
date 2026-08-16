/* sigscan — locate BG3's forced-move-input override without hardcoded addresses.
 *
 * The global and the two field offsets we need belong to anonymous code, so they
 * move on every game patch. Rather than hardcode them we recover all three from
 * the instruction encodings of the override prologue itself, which is distinctive
 * enough to identify uniquely.
 */
#ifndef BG3LE_SIGSCAN_H
#define BG3LE_SIGSCAN_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uintptr_t global_slot;  /* address OF the pointer to the input state block */
    uint32_t vec_off;       /* forced-input vec2, within that block   (0x1394 in 4.1.1.7398727) */
    uint32_t flag_off;      /* forced-input enable byte               (0x139c) */
    uintptr_t match_va;     /* where the prologue was found           (0x2c75c6a) */
} bg3_move_sig;

/* Scans an executable range for the override prologue.
 * Returns 1 on a unique confident match, 0 if not found, and -1 if the pattern
 * matched more than once (ambiguous — refuse rather than guess). */
int bg3_find_move_sig(const uint8_t *code, size_t len, uintptr_t base_va, bg3_move_sig *out);

/* Live movement state, derived from a signature match. */
typedef struct {
    uintptr_t block;        /* base of the state block the game allocated */
    volatile float *vec;    /* [0] = strafe (right positive), [1] = forward (backward positive) */
    volatile uint8_t *flag; /* nonzero => the game uses vec instead of polling input */
} bg3_move_ctl;

/* Dereferences the global slot. Returns 0 until the game has allocated the block,
 * so callers must retry — it is null early in startup. */
int bg3_move_ctl_resolve(const bg3_move_sig *sig, bg3_move_ctl *out);

#endif
