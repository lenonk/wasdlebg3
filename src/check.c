/* bg3le-check — offline pre-flight against a BG3 executable.
 *
 * Everything bg3le does rests on one mechanism being present in the game binary.
 * Run this on your own copy first: it answers "will this work on my build?"
 * without launching anything, and prints the addresses it derived so a mismatch
 * is obvious rather than mysterious.
 */
#define _GNU_SOURCE
#include "sigscan.h"

#include <elf.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static const uint8_t *g_map;
static size_t g_len;

static const Elf64_Shdr *find_sec(const char *want, const Elf64_Ehdr *eh)
{
    const Elf64_Shdr *sh = (const void *)(g_map + eh->e_shoff);
    const char *str = (const char *)(g_map + sh[eh->e_shstrndx].sh_offset);
    for (unsigned i = 0; i < eh->e_shnum; i++)
        if (!strcmp(str + sh[i].sh_name, want))
            return &sh[i];
    return NULL;
}

static void print_build_id(const Elf64_Ehdr *eh)
{
    const Elf64_Shdr *s = find_sec(".note.gnu.build-id", eh);
    if (!s) return;
    const uint8_t *n = g_map + s->sh_offset;
    uint32_t namesz, descsz;
    memcpy(&namesz, n, 4);
    memcpy(&descsz, n + 4, 4);
    const uint8_t *desc = n + 16 + ((namesz + 3) & ~3u) - 4;
    printf("  build id      : ");
    for (uint32_t i = 0; i < descsz && i < 20; i++) printf("%02x", desc[i]);
    printf("\n");
}

/* The game's version string sits in .rodata as plain text like "4.1.1.7398727". */
static void print_version(const Elf64_Ehdr *eh)
{
    const Elf64_Shdr *ro = find_sec(".rodata", eh);
    if (!ro) return;
    const uint8_t *p = g_map + ro->sh_offset;
    for (size_t i = 0; i + 16 < ro->sh_size; i++) {
        if (p[i] == '4' && p[i + 1] == '.' && p[i + 2] == '1' && p[i + 3] == '.') {
            int ok = 1, digits = 0;
            for (int j = 4; j < 16; j++) {
                if (p[i + j] == 0) break;
                if (p[i + j] == '.') continue;
                if (p[i + j] < '0' || p[i + j] > '9') { ok = 0; break; }
                digits++;
            }
            if (ok && digits >= 7) {
                printf("  version       : %.20s\n", (const char *)p + i);
                return;
            }
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "usage: bg3le-check /path/to/Baldurs Gate 3/bin/bg3\n\n"
                "Checks whether this build of BG3 contains the movement override\n"
                "that bg3le drives, and prints the addresses it derives.\n");
        return 2;
    }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror(argv[1]); return 2; }
    struct stat st;
    fstat(fd, &st);
    g_len = st.st_size;
    g_map = mmap(NULL, g_len, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (g_map == MAP_FAILED) { perror("mmap"); return 2; }

    printf("\n=== %s ===\n", argv[1]);
    printf("  size          : %.1f MB\n", g_len / 1048576.0);

    if (g_len < sizeof(Elf64_Ehdr) || memcmp(g_map, ELFMAG, SELFMAG)) {
        printf("\n  NOT a Linux ELF binary.\n");
        printf("  If this is bg3.exe you have the WINDOWS build. bg3le needs the\n"
               "  native Linux executable — usually 'bg3' with no extension.\n\n");
        return 1;
    }
    const Elf64_Ehdr *eh = (const void *)g_map;
    print_build_id(eh);
    print_version(eh);

    const Elf64_Shdr *text = find_sec(".text", eh);
    const Elf64_Shdr *symtab = find_sec(".symtab", eh);
    const Elf64_Shdr *relatext = find_sec(".rela.text", eh);
    if (!text) { printf("\n  no .text — unusable.\n\n"); return 1; }

    printf("  .text         : %#lx, %.1f MB\n",
           (unsigned long)text->sh_addr, text->sh_size / 1048576.0);
    printf("  .symtab       : %s\n",
           symtab ? "present (internal symbols resolvable)" : "STRIPPED");
    printf("  .rela.text    : %s\n",
           relatext ? "present (exact xref graph available)" : "absent");

    bg3_move_sig sig;
    memset(&sig, 0, sizeof sig);
    int r = bg3_find_move_sig(g_map + text->sh_offset, text->sh_size, text->sh_addr, &sig);

    printf("\n--- movement override ---\n");
    if (r == 1) {
        printf("  FOUND at        %#lx\n", (unsigned long)sig.match_va);
        printf("  state pointer @ %#lx\n", (unsigned long)sig.global_slot);
        printf("  forced vec2   + %#x\n", sig.vec_off);
        printf("  enable flag   + %#x\n", sig.flag_off);
        printf("\n  GOOD — this build is supported. bg3le will drive movement\n"
               "  through the game's own analog-movement path.\n\n");
        return 0;
    }
    if (r < 0) {
        printf("  AMBIGUOUS — the pattern matched more than once.\n"
               "  bg3le will refuse to run rather than write to a guessed address.\n"
               "  Please send this output back so the signature can be tightened.\n\n");
        return 1;
    }
    printf("  NOT FOUND.\n\n"
           "  Either this is a different build than the one analysed, or Larian\n"
           "  changed the code. bg3le will refuse to touch memory in this state.\n"
           "  Send this output back and the signature can be re-derived.\n\n");
    return 1;
}
