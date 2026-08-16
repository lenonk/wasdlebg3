/* symres — resolve BG3's unexported internal symbols at runtime.
 *
 * The game's own functions and its ECS type-index statics live in .symtab, which
 * the loader never maps and dlsym() cannot see. So we find the executable's load
 * bias from the running process, then read .symtab back off disk and add the bias.
 *
 * Everything here is deliberately dependency-free: this runs inside the game's
 * address space from an LD_PRELOAD constructor, before we can assume anything.
 */
#define _GNU_SOURCE
#include "symres.h"

#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>

struct sr_ctx {
    void *map;          /* mmap of the on-disk executable */
    size_t maplen;
    uintptr_t bias;     /* runtime load bias of the main executable */
    const Elf64_Sym *sym;
    size_t nsym;
    const char *str;
    size_t strsz;
};

/* dl_iterate_phdr yields the main executable first, with an empty name.
 * dlpi_addr is exactly the bias we need, and works under ASLR and PIE without
 * parsing /proc/self/maps or guessing which mapping is the executable. */
static int first_object_cb(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size;
    *(uintptr_t *)data = (uintptr_t)info->dlpi_addr;
    return 1; /* stop after the first */
}

static uintptr_t find_bias(void)
{
    uintptr_t bias = (uintptr_t)-1;
    dl_iterate_phdr(first_object_cb, &bias);
    return bias;
}

static sr_ctx *sr_open(const char *path, int apply_bias, char *err, size_t errlen)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        snprintf(err, errlen, "open %s: %m", path);
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) < 0) {
        snprintf(err, errlen, "fstat: %m");
        close(fd);
        return NULL;
    }
    /* The file is ~214 MB but we only touch the symbol/string tables, so a
     * private read-only mapping costs address space, not resident memory. */
    void *map = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        snprintf(err, errlen, "mmap: %m");
        return NULL;
    }

    const unsigned char *b = map;
    if (st.st_size < (off_t)sizeof(Elf64_Ehdr) || memcmp(b, ELFMAG, SELFMAG) ||
        b[EI_CLASS] != ELFCLASS64) {
        snprintf(err, errlen, "not a 64-bit ELF");
        munmap(map, st.st_size);
        return NULL;
    }

    const Elf64_Ehdr *eh = map;
    if (eh->e_shoff == 0 || eh->e_shnum == 0) {
        snprintf(err, errlen, "no section headers (binary stripped?)");
        munmap(map, st.st_size);
        return NULL;
    }
    const Elf64_Shdr *sh = (const void *)(b + eh->e_shoff);

    const Elf64_Shdr *symtab = NULL;
    for (unsigned i = 0; i < eh->e_shnum; i++)
        if (sh[i].sh_type == SHT_SYMTAB) { symtab = &sh[i]; break; }
    if (!symtab) {
        snprintf(err, errlen, "no .symtab — this build is stripped");
        munmap(map, st.st_size);
        return NULL;
    }
    const Elf64_Shdr *strtab = &sh[symtab->sh_link];

    sr_ctx *c = calloc(1, sizeof(*c));
    if (!c) { munmap(map, st.st_size); snprintf(err, errlen, "oom"); return NULL; }
    c->map = map;
    c->maplen = st.st_size;
    c->sym = (const void *)(b + symtab->sh_offset);
    c->nsym = symtab->sh_size / sizeof(Elf64_Sym);
    c->str = (const char *)(b + strtab->sh_offset);
    c->strsz = strtab->sh_size;
    c->bias = apply_bias ? find_bias() : 0;
    if (c->bias == (uintptr_t)-1) {
        snprintf(err, errlen, "could not determine load bias");
        munmap(map, st.st_size);
        free(c);
        return NULL;
    }
    return c;
}

sr_ctx *sr_open_self(char *err, size_t errlen)
{
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n < 0) {
        snprintf(err, errlen, "readlink /proc/self/exe: %m");
        return NULL;
    }
    self[n] = 0;
    return sr_open(self, 1, err, errlen);
}

sr_ctx *sr_open_file(const char *path, char *err, size_t errlen)
{
    return sr_open(path, 0, err, errlen);
}

uintptr_t sr_bias(const sr_ctx *c) { return c->bias; }
size_t sr_count(const sr_ctx *c) { return c->nsym; }

/* Linear scan. 152k symbols is ~5 ms; callers that resolve many names should
 * use sr_resolve_many() so the table is walked exactly once. */
uintptr_t sr_resolve(const sr_ctx *c, const char *name)
{
    for (size_t i = 0; i < c->nsym; i++) {
        Elf64_Word off = c->sym[i].st_name;
        if (off == 0 || off >= c->strsz) continue;
        if (c->sym[i].st_value == 0) continue;
        if (!strcmp(c->str + off, name))
            return c->bias + c->sym[i].st_value;
    }
    return 0;
}

size_t sr_resolve_many(const sr_ctx *c, sr_req *reqs, size_t n)
{
    size_t found = 0;
    for (size_t i = 0; i < n; i++) reqs[i].addr = 0;
    for (size_t i = 0; i < c->nsym; i++) {
        Elf64_Word off = c->sym[i].st_name;
        if (off == 0 || off >= c->strsz || c->sym[i].st_value == 0) continue;
        const char *nm = c->str + off;
        for (size_t j = 0; j < n; j++) {
            if (reqs[j].addr) continue;
            if (!strcmp(nm, reqs[j].name)) {
                reqs[j].addr = c->bias + c->sym[i].st_value;
                reqs[j].size = c->sym[i].st_size;
                found++;
                break;
            }
        }
    }
    return found;
}

void sr_close(sr_ctx *c)
{
    if (!c) return;
    munmap(c->map, c->maplen);
    free(c);
}
