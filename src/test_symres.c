/* Proves symres against two real ELFs: this test binary (live, PIE + ASLR, so
 * the bias must be applied correctly) and the BG3 executable (offline). */
#define _GNU_SOURCE
#include "symres.h"

#include <stdio.h>
#include <string.h>

static int fails;

static void ok(int cond, const char *what, const char *detail)
{
    printf("%s  %s%s%s\n", cond ? "PASS" : "FAIL", what,
           detail ? " — " : "", detail ? detail : "");
    if (!cond) fails++;
}

/* Resolved by name below; must not be static or inlined away. */
__attribute__((noinline)) int marker_fn(int x) { return x * 3 + 1; }

int marker_var = 0xC0FFEE;

static void test_self(void)
{
    char err[256] = {0};
    puts("\n=== live process (PIE + ASLR): bias must be applied ===");
    sr_ctx *c = sr_open_self(err, sizeof err);
    if (!c) { ok(0, "sr_open_self", err); return; }

    char buf[128];
    snprintf(buf, sizeof buf, "%zu symbols, bias %#lx", sr_count(c), sr_bias(c));
    ok(1, "opened self", buf);

    uintptr_t fn = sr_resolve(c, "marker_fn");
    snprintf(buf, sizeof buf, "resolved %#lx vs actual %#lx", fn, (uintptr_t)&marker_fn);
    ok(fn == (uintptr_t)&marker_fn, "resolve a function through the load bias", buf);

    /* The real proof: call through the resolved pointer. */
    if (fn) {
        int (*p)(int) = (int (*)(int))fn;
        snprintf(buf, sizeof buf, "marker_fn(14) = %d", p(14));
        ok(p(14) == 43, "call through the resolved pointer", buf);
    }

    uintptr_t var = sr_resolve(c, "marker_var");
    snprintf(buf, sizeof buf, "read %#x", var ? *(unsigned *)var : 0);
    ok(var && *(unsigned *)var == 0xC0FFEE, "resolve and read a global", buf);

    ok(sr_resolve(c, "definitely_not_a_symbol_xyz") == 0,
       "missing symbol returns 0", NULL);
    sr_close(c);
}

static void test_bg3(void)
{
    const char *path = "/project/uploads/bg3";
    char err[256] = {0}, buf[160];
    puts("\n=== BG3 executable (offline: link-time addresses) ===");
    sr_ctx *c = sr_open_file(path, err, sizeof err);
    if (!c) { ok(0, "sr_open_file", err); return; }

    snprintf(buf, sizeof buf, "%zu symbols", sr_count(c));
    ok(sr_count(c) > 150000, "symbol table is intact", buf);

    /* Addresses cross-checked against analysis/bg3.db. */
    sr_req reqs[] = {
        {"_start", 0, 0},
        {"_ZN2ls6TypeIdIN3ecl9CharacterEN3ecs22ComponentTypeIdContextEE11m_TypeIndexE", 0, 0},
        {"_ZN2ls6TypeIdIN3ecl4ItemEN3ecs22ComponentTypeIdContextEE11m_TypeIndexE", 0, 0},
        {"_ZN2ls6TypeIdIN3ecl22EquipmentVisualsSystemEN3ecs14SystemsContextEE11m_TypeIndexE", 0, 0},
    };
    const uintptr_t expect[] = {0x5152f50, 0x07c3b018, 0x07c3b028, 0x07c17e84};
    size_t n = sizeof reqs / sizeof *reqs;

    size_t found = sr_resolve_many(c, reqs, n);
    snprintf(buf, sizeof buf, "%zu/%zu", found, n);
    ok(found == n, "batch-resolved every requested symbol", buf);

    for (size_t i = 0; i < n; i++) {
        snprintf(buf, sizeof buf, "%#lx (expected %#lx) %.60s",
                 reqs[i].addr, expect[i], reqs[i].name);
        ok(reqs[i].addr == expect[i], "address matches the analysis DB", buf);
    }

    /* An ECS type index is a 4-byte static; sizes should say so. */
    ok(reqs[1].size == 4, "ECS type-index static is 4 bytes", NULL);
    sr_close(c);
}

int main(void)
{
    test_self();
    test_bg3();
    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASSED", fails);
    return fails != 0;
}
