/* bg3le — native Linux script extender for Baldur's Gate 3.
 *
 * Loaded with LD_PRELOAD. The whole input design rests on one verified fact:
 * SDL_PollEvent is the game's only event ingress. It imports no SDL_PeepEvents,
 * SDL_WaitEvent, SDL_AddEventWatch, SDL_SetEventFilter or SDL_GetKeyboardState,
 * so every keystroke, mouse motion and controller axis the game will ever see
 * passes through the one function below. That makes suppression and injection
 * exact rather than best-effort.
 *
 * Configuration is by environment variable so it can be set from a Steam launch
 * option without a config file:
 *   BG3LE_LOG=<path>     append a log here (default: stderr)
 *   BG3LE_SUPPRESS=0|1   swallow WASD before the game's hotkey layer sees it
 *   BG3LE_INJECT=0|1     synthesise left-stick axis motion from held WASD keys
 */
#define _GNU_SOURCE
#include "symres.h"

#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_INJECT 16

static int (*real_poll)(SDL_Event *);

static FILE *g_logf;
static int cfg_suppress, cfg_inject;

/* Held-state of the movement keys, maintained even when we swallow the events. */
static struct { int w, a, s, d; } held;

static SDL_Event inject_q[MAX_INJECT];
static int inject_n;

static int envflag(const char *k, int dflt)
{
    const char *v = getenv(k);
    return v ? (*v != '0') : dflt;
}

__attribute__((format(printf, 1, 2)))
static void lg(const char *fmt, ...)
{
    if (!g_logf) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(g_logf, "[bg3le %7ld.%03ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_logf, fmt, ap);
    va_end(ap);
    fputc('\n', g_logf);
    fflush(g_logf);
}

/* Returns 1 if the event should be hidden from the game. */
static int intercept(const SDL_Event *ev)
{
    if (ev->type != SDL_KEYDOWN && ev->type != SDL_KEYUP)
        return 0;
    if (ev->key.repeat)
        return cfg_suppress && held.w + held.a + held.s + held.d > 0;

    int down = (ev->type == SDL_KEYDOWN);
    int *slot = NULL;
    switch (ev->key.keysym.scancode) {
    case SDL_SCANCODE_W: slot = &held.w; break;
    case SDL_SCANCODE_A: slot = &held.a; break;
    case SDL_SCANCODE_S: slot = &held.s; break;
    case SDL_SCANCODE_D: slot = &held.d; break;
    default: return 0;
    }
    if (*slot != down) {
        *slot = down;
        lg("movement key %s %s   -> vector (%+d,%+d)",
           SDL_GetScancodeName(ev->key.keysym.scancode), down ? "down" : "up",
           held.d - held.a, held.s - held.w);
    }
    return cfg_suppress;
}

/* Turn held keys into a left-stick deflection. This is the prototype of the
 * virtual-gamepad path: the game already knows how to move a character from
 * analog stick input, so if this reaches its controller code we get real
 * direct movement without touching any internal game state. */
static void queue_axis_events(void)
{
    if (!cfg_inject) return;
    int x = held.d - held.a, y = held.s - held.w;
    /* Normalise a diagonal so it is not faster than a cardinal. */
    double mag = (x && y) ? 0.7071 : 1.0;
    Sint16 ax = (Sint16)(x * 32767 * mag), ay = (Sint16)(y * 32767 * mag);

    static Sint16 last_x, last_y;
    if (ax == last_x && ay == last_y) return;
    last_x = ax;
    last_y = ay;

    struct { Uint8 axis; Sint16 val; } out[2] = {
        {SDL_CONTROLLER_AXIS_LEFTX, ax},
        {SDL_CONTROLLER_AXIS_LEFTY, ay},
    };
    for (int i = 0; i < 2 && inject_n < MAX_INJECT; i++) {
        SDL_Event e;
        memset(&e, 0, sizeof e);
        e.type = SDL_CONTROLLERAXISMOTION;
        e.caxis.timestamp = SDL_GetTicks();
        e.caxis.which = 0;
        e.caxis.axis = out[i].axis;
        e.caxis.value = out[i].val;
        inject_q[inject_n++] = e;
    }
    lg("injecting left-stick (%d,%d)", ax, ay);
}

int SDL_PollEvent(SDL_Event *ev)
{
    if (!real_poll) {
        real_poll = dlsym(RTLD_NEXT, "SDL_PollEvent");
        if (!real_poll) {
            lg("FATAL: no real SDL_PollEvent behind us: %s", dlerror());
            return 0;
        }
    }

    for (;;) {
        if (inject_n > 0) {
            *ev = inject_q[0];
            memmove(inject_q, inject_q + 1, (size_t)(--inject_n) * sizeof *inject_q);
            return 1;
        }
        int r = real_poll(ev);
        if (!r)
            return 0;
        if (intercept(ev)) {
            queue_axis_events();
            continue; /* swallowed — hand the game the next event instead */
        }
        queue_axis_events();
        return 1;
    }
}

__attribute__((constructor)) static void bg3le_init(void)
{
    const char *path = getenv("BG3LE_LOG");
    g_logf = path ? fopen(path, "ae") : stderr;
    if (!g_logf) g_logf = stderr;
    cfg_suppress = envflag("BG3LE_SUPPRESS", 1);
    cfg_inject = envflag("BG3LE_INJECT", 0);

    lg("loaded (suppress=%d inject=%d)", cfg_suppress, cfg_inject);

    char err[256] = {0};
    sr_ctx *c = sr_open_self(err, sizeof err);
    if (!c) {
        lg("symres unavailable: %s  (fine outside the game)", err);
        return;
    }
    lg("host: %zu symbols, load bias %#lx", sr_count(c), sr_bias(c));

    /* If we are inside BG3 these resolve; anywhere else they do not, which is
     * exactly how we detect the host without hardcoding a path. */
    sr_req probe[] = {
        {"_ZN2ls6TypeIdIN3ecl9CharacterEN3ecs22ComponentTypeIdContextEE11m_TypeIndexE", 0, 0},
        {"_ZN2ls6TypeIdIN3eoc17MovementComponentEN3ecs22ComponentTypeIdContextEE11m_TypeIndexE", 0, 0},
    };
    size_t n = sizeof probe / sizeof *probe;
    if (sr_resolve_many(c, probe, n) == 0) {
        lg("host is not BG3 — input shim active, game hooks idle");
    } else {
        for (size_t i = 0; i < n; i++)
            if (probe[i].addr)
                lg("ECS type index @%#lx = %u  (%.70s)", probe[i].addr,
                   *(unsigned *)probe[i].addr, probe[i].name);
    }
    sr_close(c);
}
