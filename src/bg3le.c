/* bg3le — native Linux WASD movement for Baldur's Gate 3.
 *
 * Loaded with LD_PRELOAD. Three verified facts carry the whole design:
 *
 *   1. SDL_PollEvent is the game's ONLY input ingress — it imports no
 *      SDL_PeepEvents, SDL_WaitEvent, SDL_AddEventWatch, SDL_SetEventFilter and
 *      no SDL_GetKeyboardState. Interposing it gives exact control of every
 *      keystroke, plus a once-per-frame tick on the main thread.
 *
 *   2. The game's movement-input fetch checks a forced-input override before it
 *      polls anything, and that override is consumed upstream of the deadzone,
 *      normalise and camera rotation. So the vector we write is camera-relative,
 *      exactly like analog stick deflection, and magnitudes below 1.0 pass
 *      through unscaled — which is what gives us a walk speed for free.
 *
 *   3. That whole path is skipped by one `cmp`/`je` on a controller-mode flag.
 *      Writing the flag loses a per-frame race against the engine's input-mode
 *      arbiter (measured: it reset ours on every frame without exception), so
 *      the branch is NOPed instead.
 *
 * Nothing is hardcoded. Every address is derived at load time from instruction
 * encodings, and the library refuses to touch memory if anything fails to match.
 *
 * Steam re-execs through a chain of helpers and each inherits LD_PRELOAD, so the
 * first thing we do is decide whether this process is the game and go quiet if
 * it is not. Configuration is by environment variable so it fits in a Steam
 * launch option; see README.md for the full list.
 */
#define _GNU_SOURCE
#include "hook.h"
#include "sigscan.h"
#include "symres.h"

#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define BG3LE_VERSION "0.1.0"
#define MAX_GATES 8
#define HEARTBEAT_FRAMES 1800

static int (*real_poll)(SDL_Event *);

/* ---------------------------------------------------------------- logging */

static FILE *g_logf;
static int g_mirror_stderr;
static int cfg_verbose;   /* log from non-game processes too */
static int cfg_trace;     /* per-keystroke and heartbeat detail */

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

    if (g_mirror_stderr) {
        va_start(ap, fmt);
        fputs("bg3le: ", stderr);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fputc('\n', stderr);
    }
}

#define trace(...) do { if (cfg_trace) lg(__VA_ARGS__); } while (0)

/* ----------------------------------------------------------- configuration */

static int cfg_suppress, cfg_move, cfg_force, cfg_hook, cfg_gate;
static double cfg_walk_speed;
static SDL_Scancode cfg_walk_key;

static int envflag(const char *k, int dflt)
{
    const char *v = getenv(k);
    return v ? (*v != '0') : dflt;
}

/* A small name table beats dlsym'ing SDL_GetScancodeFromName for the handful of
 * keys anyone actually binds a walk modifier to. */
static SDL_Scancode scancode_by_name(const char *name, SDL_Scancode dflt)
{
    static const struct { const char *name; SDL_Scancode sc; } tbl[] = {
        {"lshift", SDL_SCANCODE_LSHIFT}, {"rshift", SDL_SCANCODE_RSHIFT},
        {"lctrl",  SDL_SCANCODE_LCTRL},  {"rctrl",  SDL_SCANCODE_RCTRL},
        {"lalt",   SDL_SCANCODE_LALT},   {"ralt",   SDL_SCANCODE_RALT},
        {"capslock", SDL_SCANCODE_CAPSLOCK}, {"tab", SDL_SCANCODE_TAB},
        {"none",   SDL_SCANCODE_UNKNOWN},
    };
    if (!name) return dflt;
    for (size_t i = 0; i < sizeof tbl / sizeof *tbl; i++)
        if (!strcasecmp(name, tbl[i].name)) return tbl[i].sc;
    return dflt;
}

/* ------------------------------------------------------------- game state */

static int g_is_bg3;
static bg3_move_sig g_sig;
static int g_have_sig;
static bg3_move_ctl g_ctl;
static int g_have_ctl;

static volatile uint8_t *g_padflag;
static bg3_patch g_gates[MAX_GATES];
static size_t g_ngates;

static bg3_hook g_hook;
static int g_hooked;

/* Live ECS type indices, read through the symbol table. Zero until the engine
 * registers its component types, which makes them an "is the game up?" signal
 * and a gate on writing anything. */
static const volatile uint32_t *g_ti_character;
static const volatile uint32_t *g_ti_movement;

static unsigned long g_frames;
static int g_announced_live;

/* Held state of the movement keys, maintained even when we hide the events. */
static struct { int w, a, s, d, walk; } held;

/* True while the game has a text field focused, e.g. naming a save. Typing
 * "was" into a save name must not walk your character across the room. */
static int g_text_input;

/* --------------------------------------------------------- identification */

struct exec_range { const uint8_t *code; size_t len; uintptr_t va; };

static int exec_range_cb(struct dl_phdr_info *info, size_t sz, void *data)
{
    (void)sz;
    struct exec_range *r = data;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *p = &info->dlpi_phdr[i];
        if (p->p_type == PT_LOAD && (p->p_flags & PF_X) && p->p_memsz > r->len) {
            r->va = info->dlpi_addr + p->p_vaddr;
            r->code = (const uint8_t *)r->va;
            r->len = p->p_memsz;
        }
    }
    return 1;   /* the first object is the main executable */
}

static void patch_gates(const struct exec_range *r, uintptr_t fetch)
{
    uintptr_t gates[MAX_GATES];
    size_t ng = bg3_find_move_gates(r->code, r->len, r->va, fetch,
                                    (uintptr_t)g_padflag, gates, MAX_GATES);
    if (!ng) {
        lg("no controller-mode gate found — movement will stay dormant");
        return;
    }
    for (size_t i = 0; i < ng; i++) {
        uint8_t expect[6];
        memcpy(expect, (const void *)gates[i], 6);
        if (expect[0] != 0x0F || expect[1] != 0x84)
            continue;
        int rc = bg3_patch_nop(gates[i], 6, expect, &g_gates[g_ngates]);
        if (rc == BG3_HOOK_OK) g_ngates++;
        else lg("gate @%#lx NOT patched (rc=%d)", gates[i], rc);
    }
    lg("opened %zu of %zu movement gate(s)", g_ngates, ng);
}

/* The signature scan is the identifying test, and deliberately the only one: it
 * reads mapped memory and cannot fail for environmental reasons. Reading the
 * symbol table needs /proc/self/exe and a readable game file, neither guaranteed
 * inside Steam's container, so symbols are a bonus and never a gate. */
static void identify_host(void)
{
    struct exec_range r = {0};
    dl_iterate_phdr(exec_range_cb, &r);
    int rc = r.code ? bg3_find_move_sig(r.code, r.len, r.va, &g_sig) : 0;

    g_have_sig = (rc == 1);
    g_is_bg3 = g_have_sig;
    if (!g_is_bg3) {
        if (cfg_verbose)
            lg("not the game (%zu KB of code, signature rc=%d) — idle", r.len / 1024, rc);
        else
            g_logf = NULL;   /* Steam re-execs through many helpers; stay silent */
        return;
    }

    lg("bg3le " BG3LE_VERSION " — BG3 identified, override @%#lx", g_sig.match_va);

    uintptr_t fetch = g_sig.match_va - 10;
    g_padflag = (volatile uint8_t *)bg3_find_padmode_flag(r.code, r.len, r.va, fetch);
    if (!g_padflag)
        lg("controller-mode flag NOT found — movement will stay dormant");
    else if (cfg_gate)
        patch_gates(&r, fetch);

    if (cfg_hook) {
        /* The prologue starts 10 bytes before the signature match. The bytes are
         * verified before patching, so a wrong guess refuses rather than corrupts. */
        static const uint8_t prologue[5] = {0x55, 0x41, 0x57, 0x41, 0x56};
        int rc2 = bg3_hook_count_calls(fetch, prologue, &g_hook);
        g_hooked = (rc2 == BG3_HOOK_OK);
        if (!g_hooked) lg("call counter not installed (rc=%d)", rc2);
    }

    char err[256] = {0};
    sr_ctx *c = sr_open_self(err, sizeof err);
    if (!c) {
        lg("symbols unavailable (%s) — continuing without the engine-ready gate", err);
        return;
    }
    sr_req probe[] = {
        {"_ZN2ls6TypeIdIN3ecl9CharacterEN3ecs22ComponentTypeIdContextEE11m_TypeIndexE", 0, 0},
        {"_ZN2ls6TypeIdIN3eoc17MovementComponentEN3ecs22ComponentTypeIdContextEE11m_TypeIndexE", 0, 0},
    };
    if (sr_resolve_many(c, probe, 2) == 0)
        lg("ECS probe symbols not found — continuing without the engine-ready gate");
    else {
        g_ti_character = (const volatile uint32_t *)probe[0].addr;
        g_ti_movement = (const volatile uint32_t *)probe[1].addr;
    }
    sr_close(c);
}

static int have_probes(void) { return g_ti_character || g_ti_movement; }

static int engine_ready(void)
{
    if (!have_probes()) return 1;   /* cannot tell; rely on the block pointer */
    return (g_ti_character && *g_ti_character) || (g_ti_movement && *g_ti_movement);
}

/* -------------------------------------------------------------- movement */

static void release_movement(const char *why)
{
    int had = held.w || held.a || held.s || held.d;
    memset(&held, 0, sizeof held);
    if (g_have_ctl && *g_ctl.flag)
        *g_ctl.flag = 0;
    if (had)
        trace("movement released (%s)", why);
}

/* Hand the game a movement vector instead of letting it poll the four
 * CharacterMove* actions. The engine applies its own deadzone, normalise, clamp
 * and camera rotation, so this is camera-relative — "forward" means away from
 * the camera, which is exactly what WASD should mean. */
static void drive_movement(void)
{
    if (!cfg_move || !g_have_ctl) return;
    if (!engine_ready() && !cfg_force) return;

    /* Y is positive-forward. SDL's stick convention is the opposite and
     * following it sent the character backwards, so this axis is not SDL-signed. */
    int x = held.d - held.a, y = held.w - held.s;

    if (!x && !y) {
        /* Leaving the flag set with a stale vector walks the character forever. */
        if (*g_ctl.flag) {
            *g_ctl.flag = 0;
            trace("movement released (frame %lu)", g_frames);
        }
        return;
    }

    /* Magnitudes below 1.0 survive the engine's normalise step untouched, so a
     * shorter vector really is a slower walk rather than a clamped run. */
    double mag = held.walk ? cfg_walk_speed : 1.0;
    if (x && y) mag *= 0.70710678;   /* a diagonal must not outrun a cardinal */

    g_ctl.vec[0] = (float)(x * mag);
    g_ctl.vec[1] = (float)(y * mag);
    if (!*g_ctl.flag)
        trace("movement engaged (frame %lu) vec=(%.3f, %.3f)%s", g_frames,
              (double)g_ctl.vec[0], (double)g_ctl.vec[1], held.walk ? " walking" : "");
    *g_ctl.flag = 1;
}

static void tick(void)
{
    if (!g_is_bg3) return;
    g_frames++;

    /* Re-resolve rather than cache: if the game ever reallocates this block we
     * would otherwise spend the session writing into freed memory. */
    if (g_have_sig) {
        bg3_move_ctl now;
        if (bg3_move_ctl_resolve(&g_sig, &now)) {
            if (!g_have_ctl) {
                g_have_ctl = 1;
                g_ctl = now;
                lg("movement control live (frame %lu)", g_frames);
            } else if (now.block != g_ctl.block) {
                lg("state block moved: %#lx -> %#lx", g_ctl.block, now.block);
                g_ctl = now;
            }
        } else if (g_have_ctl) {
            lg("state block pointer went NULL on frame %lu", g_frames);
            g_have_ctl = 0;
        }
    }

    if (!g_announced_live && have_probes() && engine_ready()) {
        g_announced_live = 1;
        lg("engine up (frame %lu) — ready", g_frames);
    }

    if (cfg_trace && g_frames % HEARTBEAT_FRAMES == 0)
        lg("frame %lu: block=%s calls=%llu keys(w%d a%d s%d d%d walk%d) text=%d",
           g_frames, g_have_ctl ? "live" : "null",
           g_hooked ? (unsigned long long)*g_hook.counter : 0ULL,
           held.w, held.a, held.s, held.d, held.walk, g_text_input);

    drive_movement();
}

/* ----------------------------------------------------------- input filter */

/* Returns 1 if the event should be hidden from the game. */
static int intercept(const SDL_Event *ev)
{
    /* Alt-tabbing away with a key held would otherwise walk forever. */
    if (ev->type == SDL_WINDOWEVENT &&
        (ev->window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
         ev->window.event == SDL_WINDOWEVENT_MINIMIZED)) {
        release_movement("focus lost");
        return 0;
    }

    if (ev->type != SDL_KEYDOWN && ev->type != SDL_KEYUP)
        return 0;

    /* While a text field is focused every key belongs to the game. */
    if (g_text_input) {
        if (held.w || held.a || held.s || held.d)
            release_movement("text input");
        return 0;
    }

    if (ev->key.repeat)
        return cfg_suppress && (held.w || held.a || held.s || held.d);

    int down = (ev->type == SDL_KEYDOWN);
    SDL_Scancode sc = ev->key.keysym.scancode;

    /* The walk modifier is observed but never swallowed — it is a real binding
     * in the game and stealing it would break whatever it is bound to. */
    if (cfg_walk_key != SDL_SCANCODE_UNKNOWN && sc == cfg_walk_key) {
        held.walk = down;
        return 0;
    }

    int *slot = NULL;
    switch (sc) {
    case SDL_SCANCODE_W: slot = &held.w; break;
    case SDL_SCANCODE_A: slot = &held.a; break;
    case SDL_SCANCODE_S: slot = &held.s; break;
    case SDL_SCANCODE_D: slot = &held.d; break;
    default: return 0;   /* jump, interact and everything else pass through */
    }
    if (*slot != down) {
        *slot = down;
        trace("key %s %s -> vector (%+d,%+d)", SDL_GetScancodeName(sc),
              down ? "down" : "up", held.d - held.a, held.w - held.s);
    }
    return cfg_suppress;
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
        int r = real_poll(ev);
        if (!r) {
            /* The game drains events until this returns 0, so we are here
             * exactly once per frame, on the main thread, before simulation. */
            tick();
            return 0;
        }
        if (intercept(ev))
            continue;   /* swallowed — hand the game the next event instead */
        return 1;
    }
}

/* The game tells us when a text field takes focus; we only have to listen. */
void SDL_StartTextInput(void)
{
    static void (*real_start)(void);
    if (!real_start) real_start = dlsym(RTLD_NEXT, "SDL_StartTextInput");
    g_text_input = 1;
    release_movement("text input started");
    if (real_start) real_start();
}

void SDL_StopTextInput(void)
{
    static void (*real_stop)(void);
    if (!real_stop) real_stop = dlsym(RTLD_NEXT, "SDL_StopTextInput");
    g_text_input = 0;
    if (real_stop) real_stop();
}

/* ------------------------------------------------------------- lifecycle */

static void open_log(void)
{
    const char *want = getenv("BG3LE_LOG");
    const char *tries[2];
    int n = 0;
    if (want) tries[n++] = want;
    tries[n++] = "/tmp/bg3le.log";

    for (int i = 0; i < n; i++) {
        g_logf = fopen(tries[i], "ae");
        if (g_logf) {
            if (i > 0) {
                fprintf(stderr, "bg3le: could not write %s, using %s\n", want, tries[i]);
                g_mirror_stderr = 1;
            }
            return;
        }
    }
    g_logf = stderr;
    fprintf(stderr, "bg3le: no writable log file, logging to stderr\n");
}

__attribute__((destructor)) static void bg3le_fini(void)
{
    if (g_have_ctl && *g_ctl.flag) *g_ctl.flag = 0;
    for (size_t i = 0; i < g_ngates; i++)
        bg3_patch_restore(&g_gates[i]);
    if (g_ngates) lg("restored %zu gate branch(es)", g_ngates);
    if (g_hooked) bg3_hook_remove(&g_hook);
}

__attribute__((constructor)) static void bg3le_init(void)
{
    open_log();
    cfg_verbose = envflag("BG3LE_VERBOSE", 0);
    cfg_trace = envflag("BG3LE_TRACE", 0);
    cfg_suppress = envflag("BG3LE_SUPPRESS", 1);
    cfg_move = envflag("BG3LE_MOVE", 1);
    cfg_force = envflag("BG3LE_FORCE", 0);
    cfg_hook = envflag("BG3LE_HOOK", cfg_trace);   /* only needed for diagnostics */
    cfg_gate = envflag("BG3LE_GATE", 1);
    cfg_walk_key = scancode_by_name(getenv("BG3LE_WALK_KEY"), SDL_SCANCODE_LSHIFT);
    {
        const char *v = getenv("BG3LE_WALK_SPEED");
        cfg_walk_speed = v ? atof(v) : 0.5;
        if (cfg_walk_speed <= 0.0 || cfg_walk_speed > 1.0) cfg_walk_speed = 0.5;
    }
    if (cfg_verbose && g_logf != stderr) g_mirror_stderr = 1;

    identify_host();
    if (g_is_bg3)
        lg("ready (move=%d suppress=%d gates=%zu walk=%s@%.2f)",
           cfg_move, cfg_suppress, g_ngates,
           cfg_walk_key == SDL_SCANCODE_UNKNOWN ? "off" : SDL_GetScancodeName(cfg_walk_key),
           cfg_walk_speed);
}
