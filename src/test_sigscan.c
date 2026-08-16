/* Runs the signature scanner over the real BG3 .text and checks it recovers the
 * global and both field offsets that were read by hand out of the disassembly. */
#define _GNU_SOURCE
#include "sigscan.h"

#include <elf.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;

static void ok(int cond, const char *what, const char *detail)
{
    printf("%s  %s%s%s\n", cond ? "PASS" : "FAIL", what,
           detail ? " — " : "", detail ? detail : "");
    if (!cond) fails++;
}

int main(void)
{
    const char *path = "/project/uploads/bg3";
    puts("\n=== signature scan over BG3 .text ===");

    int fd = open(path, O_RDONLY);
    if (fd < 0) { ok(0, "open bg3", strerror(errno)); return 1; }
    struct stat st;
    fstat(fd, &st);
    const uint8_t *m = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) { ok(0, "mmap bg3", "failed"); return 1; }

    const Elf64_Ehdr *eh = (const void *)m;
    const Elf64_Shdr *sh = (const void *)(m + eh->e_shoff);
    const char *shstr = (const char *)(m + sh[eh->e_shstrndx].sh_offset);
    const Elf64_Shdr *text = NULL;
    for (unsigned i = 0; i < eh->e_shnum; i++)
        if (!strcmp(shstr + sh[i].sh_name, ".text")) { text = &sh[i]; break; }
    if (!text) { ok(0, "find .text", NULL); return 1; }

    char buf[192];
    snprintf(buf, sizeof buf, ".text va %#lx size %#lx",
             (unsigned long)text->sh_addr, (unsigned long)text->sh_size);
    ok(1, "located .text", buf);

    bg3_move_sig sig;
    memset(&sig, 0, sizeof sig);
    int r = bg3_find_move_sig(m + text->sh_offset, text->sh_size, text->sh_addr, &sig);

    snprintf(buf, sizeof buf, "returned %d", r);
    ok(r == 1, "found exactly one match in 90 MB of code", buf);
    if (r != 1) return 1;

    snprintf(buf, sizeof buf, "%#lx (expected 0x2c75c6a)", (unsigned long)sig.match_va);
    ok(sig.match_va == 0x2c75c6a, "match is at the hand-verified address", buf);

    snprintf(buf, sizeof buf, "%#lx (expected 0x7d9d198)", (unsigned long)sig.global_slot);
    ok(sig.global_slot == 0x7d9d198, "recovered the global slot from the rel32", buf);

    snprintf(buf, sizeof buf, "%#x (expected 0x1394)", sig.vec_off);
    ok(sig.vec_off == 0x1394, "recovered the forced-vector offset", buf);

    snprintf(buf, sizeof buf, "%#x (expected 0x139c)", sig.flag_off);
    ok(sig.flag_off == 0x139c, "recovered the enable-flag offset", buf);

    /* The controller-mode flag must be derived, not hardcoded. */
    uintptr_t fetch = sig.match_va - 10;
    uintptr_t flag = bg3_find_padmode_flag(m + text->sh_offset, text->sh_size,
                                           text->sh_addr, fetch);
    snprintf(buf, sizeof buf, "%#lx (expected 0x7d9d108)", (unsigned long)flag);
    ok(flag == 0x7d9d108, "derived the controller-mode flag from call sites", buf);

    uintptr_t gates[8];
    size_t ng = bg3_find_move_gates(m + text->sh_offset, text->sh_size, text->sh_addr,
                                    fetch, flag, gates, 8);
    snprintf(buf, sizeof buf, "%zu found, first %#lx (expected 0x290a3e2)", ng,
             ng ? (unsigned long)gates[0] : 0UL);
    ok(ng >= 1 && gates[0] == 0x290a3e2, "located the controller-mode gate branch", buf);

    /* A scanner that matches anything is worthless — check it rejects noise. */
    static uint8_t noise[1 << 20];
    for (size_t i = 0; i < sizeof noise; i++) noise[i] = (uint8_t)(i * 31 + (i >> 8));
    bg3_move_sig junk;
    ok(bg3_find_move_sig(noise, sizeof noise, 0x400000, &junk) == 0,
       "rejects 1 MB of non-matching bytes", NULL);

    /* And that the action cross-check is actually load-bearing: the encoding
     * pattern alone, without the four polled actions following, must not match. */
    static uint8_t stub[256];
    memset(stub, 0x90, sizeof stub);
    memcpy(stub, (const uint8_t[]){0x48,0x8b,0x05,0,0,0,0, 0x48,0x8b,0x0d,0,0,0,0,
                                   0x80,0xb8,0x9c,0x13,0,0,0x00, 0x74,0x0d,
                                   0xf2,0x0f,0x10,0xa8,0x94,0x13,0,0}, 31);
    ok(bg3_find_move_sig(stub, sizeof stub, 0x400000, &junk) == 0,
       "encoding match without the four polled actions is rejected", NULL);

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASSED", fails);
    return fails != 0;
}
