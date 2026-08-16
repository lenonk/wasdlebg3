/* symres — resolve unexported internal symbols of the running executable.
 *
 * BG3's own functions and its ECS type-index statics are in .symtab, which the
 * dynamic loader never maps, so dlsym() cannot reach them. sr_open_self()
 * re-reads the executable from disk and applies the process's load bias.
 */
#ifndef BG3LE_SYMRES_H
#define BG3LE_SYMRES_H

#include <stddef.h>
#include <stdint.h>

typedef struct sr_ctx sr_ctx;

typedef struct {
    const char *name;   /* in: mangled symbol name */
    uintptr_t addr;     /* out: runtime address, or 0 if not found */
    uint64_t size;      /* out: st_size */
} sr_req;

/* Open the running executable; addresses come back bias-adjusted and live. */
sr_ctx *sr_open_self(char *err, size_t errlen);

/* Open an ELF file for offline inspection; addresses come back as link-time
 * virtual addresses with no bias applied. Used by tests and build tooling. */
sr_ctx *sr_open_file(const char *path, char *err, size_t errlen);

uintptr_t sr_bias(const sr_ctx *);
size_t sr_count(const sr_ctx *);

/* Single lookup: walks the whole symbol table. */
uintptr_t sr_resolve(const sr_ctx *, const char *name);

/* Batch lookup: walks the symbol table once for all requests. Returns the
 * number resolved. Prefer this — with 152k symbols the walk dominates. */
size_t sr_resolve_many(const sr_ctx *, sr_req *reqs, size_t n);

void sr_close(sr_ctx *);

#endif
