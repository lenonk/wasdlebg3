#include "sigscan.h"

#include <string.h>

/* The override prologue, verbatim from 4.1.1.7398727 @ 0x2c75c6a:
 *
 *   48 8b 05 <rel32>          mov   rax, [rip+disp]      ; the state-block pointer
 *   48 8b 0d <rel32>          mov   rcx, [rip+disp]      ; player -> input index map
 *   80 b8 <disp32> 00         cmp   byte [rax+disp], 0   ; the enable flag
 *   74 <i8>                   je    <poll normally>
 *   f2 0f 10 a8 <disp32>      movsd xmm5, [rax+disp]     ; the forced vec2
 *
 * Only four fields vary, and each of the three values we need is encoded right
 * here — so a match yields the address and both offsets with no further work.
 */
#define WILD 0x100
static const int PAT[] = {
    0x48, 0x8b, 0x05, WILD, WILD, WILD, WILD,
    0x48, 0x8b, 0x0d, WILD, WILD, WILD, WILD,
    0x80, 0xb8, WILD, WILD, WILD, WILD, 0x00,
    0x74, WILD,
    0xf2, 0x0f, 0x10, 0xa8, WILD, WILD, WILD, WILD,
};
#define PATLEN ((int)(sizeof PAT / sizeof *PAT))

/* Guard against a coincidental encoding match: the real thing is followed within
 * a short window by the four movement actions being polled, in this order. */
static const uint8_t ACTIONS[4] = {0xa0, 0x9f, 0x9d, 0x9e};
#define ACTION_WINDOW 256

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static int actions_follow(const uint8_t *code, size_t len, size_t from)
{
    size_t end = from + ACTION_WINDOW;
    if (end > len) end = len;
    int want = 0;
    for (size_t i = from; i + 5 <= end && want < 4; i++) {
        /* mov esi, imm32 */
        if (code[i] == 0xbe && code[i + 2] == 0 && code[i + 3] == 0 && code[i + 4] == 0) {
            if (code[i + 1] == ACTIONS[want]) want++;
        }
    }
    return want == 4;
}

int bg3_find_move_sig(const uint8_t *code, size_t len, uintptr_t base_va, bg3_move_sig *out)
{
    int hits = 0;
    if (len < (size_t)PATLEN) return 0;

    for (size_t i = 0; i + PATLEN <= len; i++) {
        /* Cheap reject on the three fixed lead bytes before the full compare. */
        if (code[i] != 0x48 || code[i + 1] != 0x8b || code[i + 2] != 0x05)
            continue;

        int ok = 1;
        for (int j = 3; j < PATLEN; j++) {
            if (PAT[j] == WILD) continue;
            if (code[i + j] != (uint8_t)PAT[j]) { ok = 0; break; }
        }
        if (!ok || !actions_follow(code, len, i + PATLEN))
            continue;

        if (++hits > 1)
            return -1;  /* ambiguous: refuse rather than pick one */

        /* mov rax,[rip+disp] — RIP is the address of the NEXT instruction. */
        uintptr_t rip = base_va + i + 7;
        out->global_slot = rip + (int32_t)rd32(code + i + 3);
        out->flag_off = rd32(code + i + 16);
        out->vec_off = rd32(code + i + 27);
        out->match_va = base_va + i;
    }
    return hits == 1;
}

/* How far back from a call site to look for its guarding mode check. */
#define GUARD_WINDOW 0x300
#define MAX_CAND 16

uintptr_t bg3_find_padmode_flag(const uint8_t *code, size_t len, uintptr_t base_va,
                                uintptr_t fetch_fn)
{
    uintptr_t cand[MAX_CAND];
    int count[MAX_CAND], n = 0;

    for (size_t i = 0; i + 5 <= len; i++) {
        if (code[i] != 0xE8) continue;
        int32_t rel;
        memcpy(&rel, code + i + 1, 4);
        if (base_va + i + 5 + rel != fetch_fn) continue;

        /* Walk back over the caller looking for `cmp byte [rip+disp32], 0`. */
        size_t from = i > GUARD_WINDOW ? i - GUARD_WINDOW : 0;
        for (size_t j = from; j + 7 <= i; j++) {
            if (code[j] != 0x80 || code[j + 1] != 0x3D || code[j + 6] != 0x00)
                continue;
            int32_t d;
            memcpy(&d, code + j + 2, 4);
            uintptr_t flag = base_va + j + 7 + d;
            int k = 0;
            for (; k < n; k++)
                if (cand[k] == flag) { count[k]++; break; }
            if (k == n && n < MAX_CAND) { cand[n] = flag; count[n] = 1; n++; }
        }
    }
    int best = -1;
    for (int k = 0; k < n; k++)
        if (best < 0 || count[k] > count[best]) best = k;
    return best >= 0 ? cand[best] : 0;
}

size_t bg3_find_move_gates(const uint8_t *code, size_t len, uintptr_t base_va,
                           uintptr_t fetch_fn, uintptr_t flag,
                           uintptr_t *out, size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i + 5 <= len && n < max; i++) {
        if (code[i] != 0xE8) continue;
        int32_t rel;
        memcpy(&rel, code + i + 1, 4);
        if (base_va + i + 5 + rel != fetch_fn) continue;

        size_t from = i > GUARD_WINDOW ? i - GUARD_WINDOW : 0;
        for (size_t j = from; j + 13 <= i && n < max; j++) {
            /* cmp byte [rip+disp32], 0 */
            if (code[j] != 0x80 || code[j + 1] != 0x3D || code[j + 6] != 0x00)
                continue;
            int32_t d;
            memcpy(&d, code + j + 2, 4);
            if (base_va + j + 7 + d != flag)
                continue;
            /* the six-byte `je rel32` immediately after it is the gate */
            if (code[j + 7] != 0x0F || code[j + 8] != 0x84)
                continue;
            uintptr_t je = base_va + j + 7;
            int dup = 0;
            for (size_t k = 0; k < n; k++)
                if (out[k] == je) { dup = 1; break; }
            if (!dup) out[n++] = je;
        }
    }
    return n;
}

int bg3_move_ctl_resolve(const bg3_move_sig *sig, bg3_move_ctl *out)
{
    uintptr_t block = *(uintptr_t *)sig->global_slot;
    if (!block) return 0;   /* not allocated yet — caller retries */
    out->block = block;
    out->vec = (volatile float *)(block + sig->vec_off);
    out->flag = (volatile uint8_t *)(block + sig->flag_off);
    return 1;
}
