#ifndef BG3LE_HOOK_H
#define BG3LE_HOOK_H

#include <stdint.h>

enum {
    BG3_HOOK_OK = 0,
    BG3_HOOK_MISMATCH = -1,   /* prologue is not what we analysed — refuse */
    BG3_HOOK_NO_ISLAND = -2,  /* no free page within rel32 range of the target */
    BG3_HOOK_MPROTECT = -3,
    BG3_HOOK_UNALIGNED = -4,
};

typedef struct {
    uintptr_t target;
    uint8_t original[8];
    void *island;
    volatile uint64_t *counter;  /* incremented once per call to target */
} bg3_hook;

/* Installs a call counter on `target`, whose first five bytes must equal `expect`.
 * Non-destructive to behaviour: the original prologue still runs. */
int bg3_hook_count_calls(uintptr_t target, const uint8_t *expect, bg3_hook *h);

void bg3_hook_remove(bg3_hook *h);

#endif
