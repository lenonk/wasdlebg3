#include "movesig.h"

#include "scan.h"   /* bg3lese's generic pattern primitives */

#include <string.h>

/* The override prologue, verbatim from 4.1.1.7398727 @ 0x2c75c6a:
 *
 *   48 8b 05 <rel32>          mov   rax, [rip+disp]      ; the state-block pointer
 *   48 8b 0d <rel32>          mov   rcx, [rip+disp]      ; player -> input index map
 *   80 b8 <disp32> 00         cmp   byte [rax+disp], 0   ; the enable flag
 *   74 <i8>                   je    <poll normally>
 *   f2 0f 10 a8 <disp32>      movsd xmm5, [rax+disp]     ; the forced vec2
 *
 * Only four fields vary, and each value we need is encoded right here — so a
 * match yields the global and both offsets with no further work.
 */
#define PROLOGUE \
    "48 8b 05 ?? ?? ?? ?? 48 8b 0d ?? ?? ?? ?? 80 b8 ?? ?? ?? ?? 00 74 ?? " \
    "f2 0f 10 a8 ?? ?? ?? ??"
#define PROLOGUE_LEN 31

/* The fetch's entry point sits ten bytes before the prologue: four pushes and
 * a `sub rsp, 0x48`. Verified against the binary. */
#define FETCH_BACKOFF 10

/* Guard against a coincidental encoding match: the real thing polls the four
 * movement actions, in this order, shortly after. */
static const uint8_t ACTIONS[4] = {0xa0, 0x9f, 0x9d, 0x9e};
#define ACTION_WINDOW 256

/* How far back from a call site to look for its guarding mode check. */
#define GUARD_WINDOW 0x300
#define MAX_CALLERS 32
#define MAX_CAND 16

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
    for (size_t i = from; i + 5 <= end && want < 4; i++)
        if (code[i] == 0xbe && code[i + 2] == 0 && code[i + 3] == 0 &&
            code[i + 4] == 0 && code[i + 1] == ACTIONS[want])
            want++;
    return want == 4;
}

int move_find_sig(const uint8_t *code, size_t len, uintptr_t base_va, move_sig *out)
{
    scan_pat pat;
    if (scan_parse(PROLOGUE, &pat) != 0) return 0;

    uintptr_t hits[8];
    size_t n = scan_find(&pat, code, len, base_va, 0, 0, hits, 8);
    if (!n) return 0;

    int found = 0;
    for (size_t i = 0; i < n && i < 8; i++) {
        size_t off = hits[i] - base_va;
        if (!actions_follow(code, len, off + PROLOGUE_LEN))
            continue;
        if (++found > 1) return -1;   /* ambiguous: refuse rather than guess */

        const uint8_t *p = code + off;
        /* mov rax,[rip+disp] — RIP is the address of the next instruction. */
        out->global_slot = hits[i] + 7 + (int32_t)rd32(p + 3);
        out->flag_off = rd32(p + 16);
        out->vec_off = rd32(p + 27);
        out->match_va = hits[i];
        out->fetch_fn = hits[i] - FETCH_BACKOFF;
    }
    /* More matches than the buffer held means we cannot claim uniqueness. */
    if (n > 8) return -1;
    return found == 1;
}

uintptr_t move_find_padmode_flag(const uint8_t *code, size_t len, uintptr_t base_va,
                                 uintptr_t fetch_fn)
{
    uintptr_t callers[MAX_CALLERS];
    size_t nc = scan_callers(fetch_fn, code, len, base_va, callers, MAX_CALLERS);
    if (nc > MAX_CALLERS) nc = MAX_CALLERS;

    scan_pat cmp_pat;
    if (scan_parse("80 3d ?? ?? ?? ?? 00", &cmp_pat) != 0) return 0;

    uintptr_t cand[MAX_CAND];
    int count[MAX_CAND], n = 0;

    for (size_t c = 0; c < nc; c++) {
        uintptr_t hi = callers[c];
        uintptr_t lo = hi > base_va + GUARD_WINDOW ? hi - GUARD_WINDOW : base_va;

        uintptr_t sites[32];
        size_t ns = scan_find(&cmp_pat, code, len, base_va, lo, hi, sites, 32);
        if (ns > 32) ns = 32;

        for (size_t s = 0; s < ns; s++) {
            const uint8_t *p = code + (sites[s] - base_va);
            uintptr_t flag = sites[s] + 7 + (int32_t)rd32(p + 2);
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

size_t move_find_gates(const uint8_t *code, size_t len, uintptr_t base_va,
                       uintptr_t fetch_fn, uintptr_t flag,
                       uintptr_t *out, size_t max)
{
    uintptr_t callers[MAX_CALLERS];
    size_t nc = scan_callers(fetch_fn, code, len, base_va, callers, MAX_CALLERS);
    if (nc > MAX_CALLERS) nc = MAX_CALLERS;

    /* cmp byte [rip+disp32], 0 followed immediately by a six-byte `je rel32`. */
    scan_pat pat;
    if (scan_parse("80 3d ?? ?? ?? ?? 00 0f 84", &pat) != 0) return 0;

    size_t n = 0;
    for (size_t c = 0; c < nc; c++) {
        uintptr_t hi = callers[c];
        uintptr_t lo = hi > base_va + GUARD_WINDOW ? hi - GUARD_WINDOW : base_va;

        uintptr_t sites[32];
        size_t ns = scan_find(&pat, code, len, base_va, lo, hi, sites, 32);
        if (ns > 32) ns = 32;

        for (size_t s = 0; s < ns; s++) {
            const uint8_t *p = code + (sites[s] - base_va);
            if (sites[s] + 7 + (int32_t)rd32(p + 2) != flag)
                continue;
            uintptr_t je = sites[s] + 7;
            int dup = 0;
            for (size_t k = 0; k < n && k < max; k++)
                if (out[k] == je) { dup = 1; break; }
            if (!dup && n < max) out[n++] = je;
        }
    }
    return n;
}

int move_ctl_resolve(const move_sig *sig, move_ctl *out)
{
    uintptr_t block = *(uintptr_t *)sig->global_slot;
    if (!block) return 0;   /* not allocated yet — caller retries */
    out->block = block;
    out->vec = (volatile float *)(block + sig->vec_off);
    out->flag = (volatile uint8_t *)(block + sig->flag_off);
    return 1;
}
