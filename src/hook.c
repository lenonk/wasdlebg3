/* A minimal, deliberately narrow inline hook: count how often a function runs.
 *
 * We need to answer one empirical question — is BG3's movement-input fetch called
 * at all in keyboard-and-mouse mode? — and no amount of static analysis settles it.
 *
 * The target's first five bytes are three pushes, so they relocate trivially, and
 * the binary has no Intel CET (verified: zero endbr64 in 90 MB of .text), so there
 * is no landing-pad requirement. The patch is written as one aligned 8-byte atomic
 * store, so a thread executing the prologue concurrently can never see a torn
 * instruction.
 */
#define _GNU_SOURCE
#include "hook.h"

#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* island layout:
 *   +0   uint64_t counter
 *   +8   48 ff 05 <rel32>   inc qword [rip+disp]  -> counter
 *   +15  <stolen 5 bytes>
 *   +20  e9 <rel32>         jmp target+5
 */
#define ISLAND_SZ 64
#define STUB_OFF 8

static uintptr_t page_of(uintptr_t a) { return a & ~(uintptr_t)(sysconf(_SC_PAGESIZE) - 1); }

/* A rel32 jump only reaches +-2 GB, and our library lands terabytes away from the
 * game's code, so the trampoline has to live near the target. */
static void *alloc_island_near(uintptr_t target)
{
    size_t pg = sysconf(_SC_PAGESIZE);
    for (uintptr_t delta = pg; delta < 0x60000000; delta += 16 * pg) {
        for (int dir = 0; dir < 2; dir++) {
            uintptr_t at = dir ? target + delta : target - delta;
            at = page_of(at);
            if (!at) continue;
            void *p = mmap((void *)at, ISLAND_SZ, PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p != MAP_FAILED)
                return p;
        }
    }
    return NULL;
}

static int unprotect(uintptr_t addr, size_t len)
{
    uintptr_t start = page_of(addr);
    size_t span = (addr + len) - start;
    return mprotect((void *)start, span, PROT_READ | PROT_WRITE | PROT_EXEC);
}

int bg3_hook_count_calls(uintptr_t target, const uint8_t *expect, bg3_hook *h)
{
    memset(h, 0, sizeof *h);

    /* Refuse unless the prologue is byte-for-byte what we analysed. */
    if (memcmp((const void *)target, expect, 5) != 0)
        return BG3_HOOK_MISMATCH;
    if (target & 7)
        return BG3_HOOK_UNALIGNED;   /* the atomic patch below needs alignment */

    uint8_t *island = alloc_island_near(target);
    if (!island)
        return BG3_HOOK_NO_ISLAND;

    uint8_t *stub = island + STUB_OFF;
    /* inc qword [rip+rel32] -> the counter at island+0 */
    stub[0] = 0x48; stub[1] = 0xFF; stub[2] = 0x05;
    int32_t to_counter = (int32_t)((intptr_t)island - (intptr_t)(stub + 7));
    memcpy(stub + 3, &to_counter, 4);
    /* the stolen prologue */
    memcpy(stub + 7, expect, 5);
    /* jmp back to target+5 */
    stub[12] = 0xE9;
    int32_t back = (int32_t)((intptr_t)(target + 5) - (intptr_t)(island + STUB_OFF + 17));
    memcpy(stub + 13, &back, 4);

    if (unprotect(target, 8) != 0) {
        munmap(island, ISLAND_SZ);
        return BG3_HOOK_MPROTECT;
    }

    /* Build the replacement 8 bytes: a 5-byte jmp plus the three original bytes
     * that follow, so the store is a single aligned write and never tears. */
    uint8_t patch[8];
    memcpy(patch, (const void *)target, 8);
    patch[0] = 0xE9;
    int32_t to_stub = (int32_t)((intptr_t)stub - (intptr_t)(target + 5));
    memcpy(patch + 1, &to_stub, 4);

    memcpy(h->original, (const void *)target, 8);
    h->target = target;
    h->island = island;
    h->counter = (volatile uint64_t *)island;

    uint64_t want;
    memcpy(&want, patch, 8);
    __atomic_store_n((uint64_t *)target, want, __ATOMIC_SEQ_CST);
    return BG3_HOOK_OK;
}

/* Canonical multi-byte NOPs, indexed by length. */
static const uint8_t NOPS[9][8] = {
    {0}, {0x90},
    {0x66, 0x90},
    {0x0F, 0x1F, 0x00},
    {0x0F, 0x1F, 0x40, 0x00},
    {0x0F, 0x1F, 0x44, 0x00, 0x00},
    {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00},
    {0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00},
    {0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
};

int bg3_patch_nop(uintptr_t addr, size_t len, const uint8_t *expect, bg3_patch *p)
{
    memset(p, 0, sizeof *p);
    if (len < 2 || len > 8)
        return BG3_HOOK_UNALIGNED;
    if (memcmp((const void *)addr, expect, len) != 0)
        return BG3_HOOK_MISMATCH;

    uintptr_t word = addr & ~(uintptr_t)7;
    if (addr + len > word + 8)
        return BG3_HOOK_UNALIGNED;   /* straddles two words; refuse rather than tear */

    if (unprotect(word, 8) != 0)
        return BG3_HOOK_MPROTECT;

    uint8_t buf[8];
    memcpy(buf, (const void *)word, 8);
    memcpy(&p->original, buf, 8);
    memcpy(buf + (addr - word), NOPS[len], len);

    uint64_t want;
    memcpy(&want, buf, 8);
    p->word = word;
    p->active = 1;
    __atomic_store_n((uint64_t *)word, want, __ATOMIC_SEQ_CST);
    return BG3_HOOK_OK;
}

void bg3_patch_restore(bg3_patch *p)
{
    if (!p->active) return;
    __atomic_store_n((uint64_t *)p->word, p->original, __ATOMIC_SEQ_CST);
    p->active = 0;
}

void bg3_hook_remove(bg3_hook *h)
{
    if (!h->target) return;
    uint64_t orig;
    memcpy(&orig, h->original, 8);
    __atomic_store_n((uint64_t *)h->target, orig, __ATOMIC_SEQ_CST);
    if (h->island) munmap(h->island, ISLAND_SZ);
    h->target = 0;
    h->island = NULL;
    h->counter = NULL;
}
