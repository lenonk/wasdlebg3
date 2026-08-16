/* Exercises the inline hook against a local function that reproduces BG3's
 * prologue byte-for-byte, so the mechanism is proven before it ever touches
 * the game: the counter must count, and the hooked function must still return
 * correct results. */
#define _GNU_SOURCE
#include "hook.h"

#include <stdio.h>
#include <string.h>

static int fails;

static void ok(int cond, const char *what, const char *detail)
{
    printf("%s  %s%s%s\n", cond ? "PASS" : "FAIL", what,
           detail ? " — " : "", detail ? detail : "");
    if (!cond) fails++;
}

/* Same first five bytes as BG3's movement-input fetch: 55 41 57 41 56 */
__attribute__((naked, aligned(16))) static int probe_fn(int x __attribute__((unused)))
{
    __asm__ volatile(
        "push %rbp\n\t"
        "push %r15\n\t"
        "push %r14\n\t"
        "push %rbx\n\t"
        "sub  $0x48, %rsp\n\t"
        "mov  %edi, %eax\n\t"
        "imul $3, %eax, %eax\n\t"
        "add  $1, %eax\n\t"
        "add  $0x48, %rsp\n\t"
        "pop  %rbx\n\t"
        "pop  %r14\n\t"
        "pop  %r15\n\t"
        "pop  %rbp\n\t"
        "ret\n\t");
}

/* Deliberately different prologue, to prove the safety check bites. */
__attribute__((noinline)) static int other_fn(int x) { return x + 7; }

int main(void)
{
    const uint8_t expect[5] = {0x55, 0x41, 0x57, 0x41, 0x56};
    char buf[160];

    puts("\n=== inline hook ===");

    uintptr_t t = (uintptr_t)&probe_fn;
    snprintf(buf, sizeof buf, "prologue %02x %02x %02x %02x %02x at %#lx",
             ((uint8_t *)t)[0], ((uint8_t *)t)[1], ((uint8_t *)t)[2],
             ((uint8_t *)t)[3], ((uint8_t *)t)[4], t);
    ok(memcmp((void *)t, expect, 5) == 0, "probe reproduces BG3's prologue", buf);
    ok(probe_fn(5) == 16, "probe works before hooking", NULL);

    bg3_hook h;
    int rc = bg3_hook_count_calls(t, expect, &h);
    snprintf(buf, sizeof buf, "rc=%d island=%p (%+ld MB from target)", rc, h.island,
             h.island ? ((long)h.island - (long)t) / (1024 * 1024) : 0L);
    ok(rc == BG3_HOOK_OK, "installed", buf);
    if (rc != BG3_HOOK_OK) { printf("\nFAILED\n"); return 1; }

    ok((intptr_t)h.island - (intptr_t)t < 0x7fffffffL &&
       (intptr_t)t - (intptr_t)h.island < 0x7fffffffL,
       "island is within rel32 range", NULL);

    int wrong = 0;
    for (int i = 0; i < 1000; i++)
        if (probe_fn(i) != i * 3 + 1) wrong++;

    snprintf(buf, sizeof buf, "%d wrong results out of 1000", wrong);
    ok(wrong == 0, "hooked function still returns correct results", buf);

    snprintf(buf, sizeof buf, "counter=%llu (expected 1000)",
             (unsigned long long)*h.counter);
    ok(*h.counter == 1000, "counter counted every call", buf);

    bg3_hook_remove(&h);
    ok(memcmp((void *)t, expect, 5) == 0, "prologue restored on removal", NULL);
    ok(probe_fn(11) == 34, "function still works after removal", NULL);

    /* The safety check is the whole reason this is not reckless. */
    bg3_hook h2;
    int rc2 = bg3_hook_count_calls((uintptr_t)&other_fn, expect, &h2);
    snprintf(buf, sizeof buf, "rc=%d", rc2);
    ok(rc2 == BG3_HOOK_MISMATCH, "refuses a function whose prologue differs", buf);
    ok(other_fn(1) == 8, "the refused function is untouched", NULL);

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASSED", fails);
    return fails != 0;
}
