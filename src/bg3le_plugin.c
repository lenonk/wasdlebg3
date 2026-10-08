/* wasdlebg3 as a bg3le plugin: src/wasd.c's movement code (wasdlebg3 and
 * bg3lese by Brian Calvert, MIT) on bg3le's plugin API. Keys arrive on the main
 * thread, the vector is written on the client's game thread, and the options
 * are bg3le settings; bg3lese's scan.c, patch.c and host.c do the scanning and
 * patching bg3le does not offer. */
#include "bg3le_plugin.h"
#include "host.h"
#include "movesig.h"
#include "patch.h"

#include <SDL2/SDL.h>

#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

#define WASD_VERSION "1.0"
#define MAX_GATES 8

enum { KEY_W = 1, KEY_A = 2, KEY_S = 4, KEY_D = 8, KEY_WALK = 16 };
#define MOVE_KEYS (KEY_W | KEY_A | KEY_S | KEY_D)

static const bg3le_host *host;
static bg3le_plugin *self;

/* Settings: bg3le writes these directly, so they are read fresh each use. */
static int cfg_move = 1, cfg_suppress = 1, cfg_trace = 0, cfg_gate = 1, cfg_input_only = 0;
static int cfg_walk_key = SDL_SCANCODE_LSHIFT;   /* 0 = no walk modifier */
static float cfg_walk_speed = 0.5f;

static move_sig g_sig;
static move_ctl g_ctl;          /* client game thread only */
static int g_have_ctl;
static int g_ready;             /* input filtering is live */
static int g_can_move;          /* signature matched and the gate opened */
static unsigned long g_frames;

/* Written by the event handler (main thread), read by the frame handler. */
static _Atomic unsigned g_keys;

static SDL_bool (*sdl_text_input_active)(void);

#define trace(...) do { if (cfg_trace) host->log(self, __VA_ARGS__); } while (0)

static void set_key(unsigned bit, int down)
{
    if (down) atomic_fetch_or(&g_keys, bit);
    else atomic_fetch_and(&g_keys, ~bit);
}

/* ---------------------------------------------------------------- movement */

static void drive_movement(void)
{
    unsigned k = atomic_load(&g_keys);
    int x = !!(k & KEY_D) - !!(k & KEY_A), y = !!(k & KEY_W) - !!(k & KEY_S);

    if (!cfg_move || (!x && !y)) {
        /* A stale vector with the flag still set walks the character forever. */
        if (*g_ctl.flag) {
            *g_ctl.flag = 0;
            trace("movement released (frame %lu)", g_frames);
        }
        return;
    }

    /* Y is positive-forward: SDL's stick convention sent the character backwards. */
    double mag = (k & KEY_WALK) ? cfg_walk_speed : 1.0;
    if (x && y) mag *= 0.70710678;   /* a diagonal must not outrun a cardinal */

    g_ctl.vec[0] = (float)(x * mag);
    g_ctl.vec[1] = (float)(y * mag);
    if (!*g_ctl.flag)
        trace("movement engaged (frame %lu) vec=(%.3f, %.3f)%s", g_frames,
              (double)g_ctl.vec[0], (double)g_ctl.vec[1], (k & KEY_WALK) ? " walking" : "");
    *g_ctl.flag = 1;
}

/* Client game thread, once per frame. */
static void wasd_on_frame(void *user, double dt)
{
    (void)user;
    (void)dt;
    if (!g_can_move) return;
    g_frames++;

    /* Re-resolved each frame: if the game reallocates this block, a cached
     * pointer would spend the session writing into freed memory. */
    move_ctl now;
    if (move_ctl_resolve(&g_sig, &now)) {
        if (!g_have_ctl) {
            g_have_ctl = 1;
            g_ctl = now;
            host->log(self, "movement control live (frame %lu)", g_frames);
        } else if (now.block != g_ctl.block) {
            host->log(self, "state block moved: %#lx -> %#lx", g_ctl.block, now.block);
            g_ctl = now;
        }
    } else if (g_have_ctl) {
        host->log(self, "state block pointer went NULL on frame %lu", g_frames);
        g_have_ctl = 0;
    }

    if (g_have_ctl) drive_movement();
}

/* Main thread, for every event the game is about to receive. */
static int wasd_on_event(void *user, SDL_Event *ev)
{
    (void)user;
    if (!g_ready) return 0;

    /* Alt-tabbing away with a key held would otherwise walk forever. */
    if (ev->type == SDL_WINDOWEVENT &&
        (ev->window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
         ev->window.event == SDL_WINDOWEVENT_MINIMIZED)) {
        if (atomic_exchange(&g_keys, 0) & MOVE_KEYS) trace("movement released (focus lost)");
        return 0;
    }

    if (ev->type != SDL_KEYDOWN && ev->type != SDL_KEYUP) return 0;

    /* While a text field is focused every key belongs to the game: typing
     * "Wasteland" into a save name must not walk the character across the room. */
    if (sdl_text_input_active != NULL && sdl_text_input_active()) {
        if (atomic_fetch_and(&g_keys, ~(unsigned)MOVE_KEYS) & MOVE_KEYS)
            trace("movement released (text input)");
        return 0;
    }

    unsigned k = atomic_load(&g_keys);
    if (ev->key.repeat) return cfg_suppress && (k & MOVE_KEYS);

    int down = ev->type == SDL_KEYDOWN;
    int sc = ev->key.keysym.scancode;

    /* The walk modifier is observed but never consumed: it is a real binding in
     * the game, and stealing it would break whatever it is bound to. */
    if (cfg_walk_key != 0 && sc == cfg_walk_key) {
        set_key(KEY_WALK, down);
        return 0;
    }

    unsigned bit;
    switch (sc) {
    case SDL_SCANCODE_W: bit = KEY_W; break;
    case SDL_SCANCODE_A: bit = KEY_A; break;
    case SDL_SCANCODE_S: bit = KEY_S; break;
    case SDL_SCANCODE_D: bit = KEY_D; break;
    default: return 0;   /* jump, interact and everything else pass through */
    }
    if (!!(k & bit) != down) {
        set_key(bit, down);
        k = atomic_load(&g_keys);
        trace("key %s %s -> vector (%+d,%+d)", sc == SDL_SCANCODE_W ? "W" : sc == SDL_SCANCODE_A ? "A"
              : sc == SDL_SCANCODE_S ? "S" : "D", down ? "down" : "up",
              !!(k & KEY_D) - !!(k & KEY_A), !!(k & KEY_W) - !!(k & KEY_S));
    }
    return cfg_suppress;
}

/* ------------------------------------------------------------------- setup */

static int open_gates(const host_image *img)
{
    uintptr_t flag = move_find_padmode_flag(img->code, img->len, img->base_va, g_sig.fetch_fn);
    if (!flag) {
        host->warn(self, "controller-mode flag not found; movement would stay dormant");
        return -1;
    }

    uintptr_t gates[MAX_GATES];
    size_t ng = move_find_gates(img->code, img->len, img->base_va, g_sig.fetch_fn, flag,
                                gates, MAX_GATES);
    if (!ng) {
        host->warn(self, "no movement gate found; movement would stay dormant");
        return -1;
    }

    size_t opened = 0;
    for (size_t i = 0; i < ng; i++) {
        uint8_t expect[6];
        memcpy(expect, (const void *)gates[i], sizeof expect);
        if (expect[0] != 0x0F || expect[1] != 0x84) continue;
        int rc = patch_nop(gates[i], 6, expect);
        if (rc == PATCH_OK) opened++;
        else host->log(self, "gate @%#lx not patched (rc=%d)", gates[i], rc);
    }
    if (!opened) return -1;
    host->log(self, "opened %zu of %zu movement gate(s)", opened, ng);
    return 0;
}

/* The gate NOPs live only in memory; put them back as the game exits. */
__attribute__((destructor)) static void wasd_shutdown(void)
{
    if (g_have_ctl && *g_ctl.flag) *g_ctl.flag = 0;
    patch_restore_all();
}

BG3LE_PLUGIN_EXPORT int bg3le_plugin_init(const bg3le_host *h, bg3le_plugin *p)
{
    host = h;
    self = p;
    host->describe(self, "LinuxNativeWASD", WASD_VERSION);

    if (host->size < offsetof(bg3le_host, add_frame_handler) + sizeof host->add_frame_handler) {
        host->warn(self, "needs bg3le's add_frame_handler (newer than v0.3.4); not loading");
        return -1;
    }

    host->add_setting(self, "move", BG3LE_SETTING_BOOL, &cfg_move, 0, 0);
    host->add_setting(self, "suppress", BG3LE_SETTING_BOOL, &cfg_suppress, 0, 0);
    host->add_setting(self, "walk_key", BG3LE_SETTING_INT, &cfg_walk_key, 0, 511);
    host->add_setting(self, "walk_speed", BG3LE_SETTING_FLOAT, &cfg_walk_speed, 0.05, 1.0);
    host->add_setting(self, "gate", BG3LE_SETTING_BOOL, &cfg_gate, 0, 0);
    host->add_setting(self, "input_only", BG3LE_SETTING_BOOL, &cfg_input_only, 0, 0);
    host->add_setting(self, "trace", BG3LE_SETTING_BOOL, &cfg_trace, 0, 0);

    sdl_text_input_active = (SDL_bool (*)(void))host->sdl_function("SDL_IsTextInputActive");
    if (sdl_text_input_active == NULL)
        host->warn(self, "SDL_IsTextInputActive not found; typing W/A/S/D in text fields will move");

    host_image img;
    if (!host_probe(&img)) {
        host->warn(self, "this process does not look like Baldur's Gate 3; not loading");
        return -1;
    }

    /* Taking WASD while unable to move would be worse than not loading: the keys
     * would be swallowed and the character would stand still. So an unrecognised
     * build is a clean decline, and the game keeps its keys. input_only overrides
     * that, for diagnosing key handling on a build this cannot yet drive. */
    int rc = move_find_sig(img.code, img.len, img.base_va, &g_sig);
    int can_move = 0;
    if (rc != 1) {
        const char *why = rc < 0 ? "movement signature is ambiguous"
                                 : "movement signature not found; this game build is not one I recognise";
        if (!cfg_input_only) {
            host->warn(self, "%s; not loading", why);
            return -1;
        }
        host->log(self, "%s; input_only is set, so filtering input but not moving", why);
    } else {
        host->log(self, "override @%#lx, state ptr @%#lx, vec +%#x, flag +%#x",
                  g_sig.match_va, g_sig.global_slot, g_sig.vec_off, g_sig.flag_off);
        if (cfg_gate && open_gates(&img) != 0) {
            patch_restore_all();
            if (!cfg_input_only) return -1;
            host->log(self, "input_only is set, so filtering input but not moving");
        } else {
            can_move = 1;
        }
    }

    if (host->add_event_handler(self, wasd_on_event, NULL) != 0
        || host->add_frame_handler(self, wasd_on_frame, NULL) != 0) {
        patch_restore_all();
        host->warn(self, "could not register its handlers; not loading");
        return -1;
    }

    host->log(self, "ready (move=%d suppress=%d walk_key=%d@%.2f)", cfg_move, cfg_suppress,
              cfg_walk_key, (double)cfg_walk_speed);
    g_ready = 1;
    g_can_move = can_move;
    return 0;
}
