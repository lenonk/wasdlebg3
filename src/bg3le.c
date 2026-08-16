/* bg3le — native Linux script extender for Baldur's Gate 3.
 *
 * Loaded with LD_PRELOAD. The input design rests on one verified fact:
 * SDL_PollEvent is the game's only event ingress. It imports no SDL_PeepEvents,
 * SDL_WaitEvent, SDL_AddEventWatch, SDL_SetEventFilter or SDL_GetKeyboardState,
 * so every keystroke, mouse motion and controller axis the game will ever see
 * passes through the one function below. That makes suppression and injection
 * exact rather than best-effort, and gives us a once-per-frame main-thread tick.
 *
 * Steam re-execs through a chain of helper processes and every one of them
 * inherits LD_PRELOAD, so the first thing we do is establish whether this
 * process is actually the game and go quiet if it is not.
 *
 * Configuration is by environment variable so it can be set from a Steam launch
 * option without a config file:
 *   BG3LE_LOG=<path>     append a log here (default: stderr)
 *   BG3LE_MOVE=0|1       drive movement from WASD          (default 1)
 *   BG3LE_SUPPRESS=0|1   hide WASD from the game's hotkeys (default 1)
 *   BG3LE_VERBOSE=0|1    also log from non-game processes  (default 0)
 *   BG3LE_FORCE=0|1      write even if the engine looks uninitialised (default 0)
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
#include <time.h>

#define HEARTBEAT_FRAMES 600

static int (*real_poll)(SDL_Event *);

static FILE *g_logf;
static int g_mirror_stderr;
static int cfg_suppress, cfg_move, cfg_verbose, cfg_force, cfg_hook, cfg_padmode;

/* The game checks this single byte before it will even ask for movement input.
 * In keyboard-and-mouse mode it is zero, so the whole direct-movement subsystem
 * is dormant — which is why our forced-input writes were being ignored. */
static volatile uint8_t *g_padflag;
static uint8_t g_padflag_orig;
static int g_padflag_saved;

/* Counts calls to the game's movement-input fetch. If this stays at zero while
 * keys are held, the game simply never asks for movement input in this mode —
 * which no amount of writing to the state block could fix. */
static bg3_hook g_hook;
static int g_hooked;

/* A gate on the same state block, checked by another caller of the input fetch:
 *   cmp dword [block+0xe98], 0 ; je <skip the call>                          */
#define GATE_OFF 0xe98

static int g_is_bg3;
static bg3_move_sig g_sig;
static int g_have_sig;
static bg3_move_ctl g_ctl;
static int g_have_ctl;

/* Live ECS type indices, read through the symbol table. They are zero until the
 * engine registers its component types, which makes them a reliable "is the game
 * actually up?" signal — and a gate on writing anything. */
static const volatile uint32_t *g_ti_character;
static const volatile uint32_t *g_ti_movement;

static unsigned long g_frames;
static int g_announced_live;

/* Held state of the movement keys, maintained even when we swallow the events. */
static struct { int w, a, s, d; } held;

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

    if (g_mirror_stderr) {
        va_start(ap, fmt);
        fputs("bg3le: ", stderr);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fputc('\n', stderr);
    }
}

struct exec_range { const uint8_t *code; size_t len; uintptr_t va; };

/* The first object dl_iterate_phdr reports is the main executable. We want its
 * executable PT_LOAD, live in memory — scanning that rather than the file means
 * the addresses we recover are already bias-adjusted. */
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
    return 1;
}

/* The signature scan is the identifying test, and deliberately the only one:
 * it reads mapped memory and cannot fail for environmental reasons. Reading the
 * symbol table needs /proc/self/exe and a readable game file, which may not hold
 * inside Steam's container — so symbols are a bonus, never a gate. An earlier
 * version required both, and a symbol-table failure silently disabled everything. */
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
            g_logf = NULL;   /* stay out of the log entirely */
        return;
    }

    lg("BG3 identified by signature @%#lx", g_sig.match_va);
    lg("state ptr @%#lx, vec +%#x, flag +%#x",
       g_sig.global_slot, g_sig.vec_off, g_sig.flag_off);

    g_padflag = (volatile uint8_t *)bg3_find_padmode_flag(r.code, r.len, r.va,
                                                          g_sig.match_va - 10);
    if (g_padflag)
        lg("controller-mode flag @%p (currently %u)", (void *)g_padflag, *g_padflag);
    else
        lg("controller-mode flag NOT found — movement will stay dormant");

    if (cfg_hook) {
        /* The prologue starts 10 bytes before the signature match. We verify the
         * bytes before patching, so a wrong guess refuses rather than corrupts. */
        static const uint8_t prologue[5] = {0x55, 0x41, 0x57, 0x41, 0x56};
        uintptr_t fn = g_sig.match_va - 10;
        int rc2 = bg3_hook_count_calls(fn, prologue, &g_hook);
        g_hooked = (rc2 == BG3_HOOK_OK);
        lg("call counter on GetMoveInput @%#lx: %s (rc=%d)", fn,
           g_hooked ? "installed" : "NOT installed", rc2);
    }

    /* Best effort from here on. Resolve the ECS probes now but read them later:
     * at constructor time the engine has not registered its types yet, so every
     * index is still zero and tells us nothing. */
    char err[256] = {0};
    sr_ctx *c = sr_open_self(err, sizeof err);
    if (!c) {
        lg("symbols unavailable (%s) — continuing without the engine-ready gate", err);
        return;
    }
    lg("symbols: %zu, load bias %#lx", sr_count(c), sr_bias(c));

    sr_req probe[] = {
        {"_ZN2ls6TypeIdIN3ecl9CharacterEN3ecs22ComponentTypeIdContextEE11m_TypeIndexE", 0, 0},
        {"_ZN2ls6TypeIdIN3eoc17MovementComponentEN3ecs22ComponentTypeIdContextEE11m_TypeIndexE", 0, 0},
    };
    if (sr_resolve_many(c, probe, 2) == 0) {
        lg("ECS probe symbols not found — continuing without the engine-ready gate");
    } else {
        g_ti_character = (const volatile uint32_t *)probe[0].addr;
        g_ti_movement = (const volatile uint32_t *)probe[1].addr;
        lg("ECS probes @%#lx and @%#lx",
           (unsigned long)probe[0].addr, (unsigned long)probe[1].addr);
    }
    sr_close(c);
}

static int have_probes(void) { return g_ti_character || g_ti_movement; }

/* True once the engine has registered its component types. Without probes we
 * cannot tell, so we say yes and rely on the state-block pointer instead. */
static int engine_ready(void)
{
    if (!have_probes()) return 1;
    return (g_ti_character && *g_ti_character) || (g_ti_movement && *g_ti_movement);
}

/* Hand the game a movement vector directly, instead of letting it poll the four
 * CharacterMove* input actions. The engine treats this exactly like analog stick
 * deflection: it applies its own deadzone, normalises, clamps, and rotates the
 * vector into world space using the live camera. So what we write is
 * camera-relative — which is precisely what WASD means — and the character walks
 * through the ordinary locomotion, animation and collision path. */
static void tick(void)
{
    if (!g_is_bg3) return;
    g_frames++;

    /* Re-resolve every frame rather than caching: if the game ever reallocates
     * this block we would otherwise spend the rest of the session writing into
     * freed memory and wondering why nothing moved. */
    if (g_have_sig) {
        bg3_move_ctl now;
        if (bg3_move_ctl_resolve(&g_sig, &now)) {
            if (!g_have_ctl) {
                g_have_ctl = 1;
                g_ctl = now;
                lg("state block resolved on frame %lu: block %#lx, flag currently %u",
                   g_frames, now.block, *now.flag);
            } else if (now.block != g_ctl.block) {
                lg("state block MOVED on frame %lu: %#lx -> %#lx",
                   g_frames, g_ctl.block, now.block);
                g_ctl = now;
            }
        } else if (g_have_ctl) {
            lg("state block pointer went NULL on frame %lu", g_frames);
            g_have_ctl = 0;
        }
    }

    if (!g_announced_live && have_probes() && engine_ready()) {
        g_announced_live = 1;
        lg("engine initialised on frame %lu — ECS indices: Character=%u Movement=%u",
           g_frames, g_ti_character ? *g_ti_character : 0,
           g_ti_movement ? *g_ti_movement : 0);
    }

    if (g_frames % HEARTBEAT_FRAMES == 0) {
        unsigned gate = g_have_ctl ? *(volatile uint32_t *)(g_ctl.block + GATE_OFF) : 0;
        lg("frame %lu: block=%s pad=%u gate=%u calls=%llu readback vec=(%.2f,%.2f) flag=%u keys(w%d a%d s%d d%d)",
           g_frames, g_have_ctl ? "live" : "null",
           g_padflag ? *g_padflag : 0, gate,
           g_hooked ? (unsigned long long)*g_hook.counter : 0ULL,
           g_have_ctl ? (double)g_ctl.vec[0] : 0.0,
           g_have_ctl ? (double)g_ctl.vec[1] : 0.0,
           g_have_ctl ? *g_ctl.flag : 0,
           held.w, held.a, held.s, held.d);
    }

    if (!cfg_move || !g_have_ctl)
        return;
    /* Refuse to write into a game that has not finished starting up. */
    if (!engine_ready() && !cfg_force)
        return;

    int x = held.d - held.a, y = held.s - held.w;

    /* Open the gate. Setting the raw byte rather than calling the game's own
     * ActivateControllerMode is deliberate: that function also drives a wholesale
     * Noesis UI reload into the controller theme, which we do not want. */
    if (g_padflag && cfg_padmode) {
        if (!g_padflag_saved) {
            g_padflag_orig = *g_padflag;
            g_padflag_saved = 1;
        }
        int want = (cfg_padmode == 2) ? (x || y) : 1;
        if (want && !*g_padflag) {
            *g_padflag = 1;
            lg("opened the controller-mode gate on frame %lu", g_frames);
        } else if (!want && *g_padflag && cfg_padmode == 2) {
            *g_padflag = g_padflag_orig;
        }
    }

    if (!x && !y) {
        /* Leaving the flag set with a stale vector walks the character forever. */
        if (*g_ctl.flag) {
            *g_ctl.flag = 0;
            lg("movement released (frame %lu)", g_frames);
        }
        return;
    }
    /* Magnitude 1.0 in any direction; the engine's own clamp handles the rest. */
    double mag = (x && y) ? 0.70710678 : 1.0;
    g_ctl.vec[0] = (float)(x * mag);
    g_ctl.vec[1] = (float)(y * mag);
    if (!*g_ctl.flag)
        lg("movement engaged (frame %lu) vec=(%.3f, %.3f)",
           g_frames, (double)g_ctl.vec[0], (double)g_ctl.vec[1]);
    *g_ctl.flag = 1;
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
        lg("key %s %s -> vector (%+d,%+d)",
           SDL_GetScancodeName(ev->key.keysym.scancode), down ? "down" : "up",
           held.d - held.a, held.s - held.w);
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
            /* The game drains events until this returns 0, so here we are exactly
             * once per frame, on the main thread, before the frame is simulated. */
            tick();
            return 0;
        }
        if (intercept(ev))
            continue;  /* swallowed — hand the game the next event instead */
        return 1;
    }
}

/* Losing focus with keys held would otherwise leave the character walking. */
__attribute__((destructor)) static void bg3le_fini(void)
{
    if (g_have_ctl && *g_ctl.flag) {
        *g_ctl.flag = 0;
        lg("cleared movement flag on unload");
    }
    if (g_padflag && g_padflag_saved) {
        *g_padflag = g_padflag_orig;
        lg("restored the controller-mode flag to %u", g_padflag_orig);
    }
    if (g_hooked) {
        lg("GetMoveInput was called %llu times this session",
           (unsigned long long)*g_hook.counter);
        bg3_hook_remove(&g_hook);
    }
}

/* Steam's container runtime may not give us a writable $HOME, and if the log
 * cannot be opened we would vanish without trace. Fall back through candidates,
 * and mirror to stderr so something survives even when every file fails. */
static void open_log(void)
{
    const char *want = getenv("BG3LE_LOG");
    const char *tries[3];
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
    g_mirror_stderr = 0;   /* already stderr; do not double up */
    fprintf(stderr, "bg3le: no writable log file, logging to stderr\n");
}

__attribute__((constructor)) static void bg3le_init(void)
{
    open_log();
    cfg_suppress = envflag("BG3LE_SUPPRESS", 1);
    cfg_move = envflag("BG3LE_MOVE", 1);
    cfg_verbose = envflag("BG3LE_VERBOSE", 0);
    cfg_force = envflag("BG3LE_FORCE", 0);
    cfg_hook = envflag("BG3LE_HOOK", 1);
    {   /* 0 = leave alone, 1 = hold the gate open, 2 = only while moving */
        const char *v = getenv("BG3LE_PADMODE");
        cfg_padmode = v ? atoi(v) : 1;
    }
    if (cfg_verbose && g_logf != stderr) g_mirror_stderr = 1;

    identify_host();
    if (g_is_bg3)
        lg("armed (move=%d suppress=%d padmode=%d hook=%d)",
           cfg_move, cfg_suppress, cfg_padmode, cfg_hook);
}
