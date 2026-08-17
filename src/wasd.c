/* wasdlebg3 — WASD movement, as a bg3lese plugin.
 *
 * Two facts about the game carry this, both verified against the binary:
 *
 *   The movement-input fetch checks a forced-input override *before* it polls
 *   CharacterMoveForward/Backward/Left/Right, and that override is consumed
 *   upstream of the deadzone, normalise and camera rotation. So the vector we
 *   write is camera-relative exactly like analog stick deflection — and because
 *   magnitudes below 1.0 survive the normalise step, a shorter vector is a real
 *   walk rather than a clamped run.
 *
 *   That whole path is skipped by one `cmp`/`je` on a controller-mode flag.
 *   Writing the flag loses a per-frame race against the engine's input-mode
 *   arbiter — measured, it reset ours on every frame without exception — so the
 *   six-byte branch is NOPed instead.
 *
 * The host owns SDL_PollEvent, the patch ledger and text-input state; this file
 * owns only what is specific to moving a character.
 */
#include "bg3lese.h"
#include "movesig.h"

#include <string.h>
#include <strings.h>

#define WASD_VERSION "0.2.0"
#define MAX_GATES 8

static const bg3lese_api *api;

static move_sig g_sig;
static move_ctl g_ctl;
static int g_have_ctl;
static int g_ready;              /* input filtering is live */
static int g_can_move;           /* signature matched AND gates opened */

static int cfg_suppress, cfg_move, cfg_trace;
static double cfg_walk_speed;
static SDL_Scancode cfg_walk_key;

static struct { int w, a, s, d, walk; } held;
static unsigned long g_frames;

#define trace(...) do { if (cfg_trace) api->log(__VA_ARGS__); } while (0)

/* A small table beats SDL_GetScancodeFromName for the handful of keys anyone
 * actually binds a walk modifier to. */
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

/* ---------------------------------------------------------------- movement */

static void release_movement(const char *why)
{
    int had = held.w || held.a || held.s || held.d;
    memset(&held, 0, sizeof held);
    if (g_have_ctl && *g_ctl.flag)
        *g_ctl.flag = 0;
    if (had) trace("movement released (%s)", why);
}

static void drive_movement(void)
{
    if (!cfg_move || !g_can_move || !g_have_ctl) return;

    /* Y is positive-forward. SDL's stick convention is the opposite and
     * following it sent the character backwards, so this is not SDL-signed. */
    int x = held.d - held.a, y = held.w - held.s;

    if (!x && !y) {
        /* A stale vector with the flag still set walks the character forever. */
        if (*g_ctl.flag) {
            *g_ctl.flag = 0;
            trace("movement released (frame %lu)", g_frames);
        }
        return;
    }

    double mag = held.walk ? cfg_walk_speed : 1.0;
    if (x && y) mag *= 0.70710678;   /* a diagonal must not outrun a cardinal */

    g_ctl.vec[0] = (float)(x * mag);
    g_ctl.vec[1] = (float)(y * mag);
    if (!*g_ctl.flag)
        trace("movement engaged (frame %lu) vec=(%.3f, %.3f)%s", g_frames,
              (double)g_ctl.vec[0], (double)g_ctl.vec[1], held.walk ? " walking" : "");
    *g_ctl.flag = 1;
}

/* ------------------------------------------------------------- plugin body */

static int wasd_on_event(const SDL_Event *ev)
{
    if (!g_ready) return 0;

    /* Alt-tabbing away with a key held would otherwise walk forever. */
    if (ev->type == SDL_WINDOWEVENT &&
        (ev->window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
         ev->window.event == SDL_WINDOWEVENT_MINIMIZED)) {
        release_movement("focus lost");
        return 0;
    }

    if (ev->type != SDL_KEYDOWN && ev->type != SDL_KEYUP)
        return 0;

    /* While a text field is focused every key belongs to the game — typing
     * "Wasteland" into a save name must not walk the character across the room.
     * The host tracks this so every plugin agrees on the answer. */
    if (api->text_input_active()) {
        if (held.w || held.a || held.s || held.d)
            release_movement("text input");
        return 0;
    }

    if (ev->key.repeat)
        return cfg_suppress && (held.w || held.a || held.s || held.d);

    int down = (ev->type == SDL_KEYDOWN);
    SDL_Scancode sc = ev->key.keysym.scancode;

    /* The walk modifier is observed but never consumed: it is a real binding in
     * the game and stealing it would break whatever it is bound to. */
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

static void wasd_on_frame(void)
{
    if (!g_can_move) return;
    g_frames++;

    /* Re-resolve rather than cache: if the game ever reallocates this block we
     * would otherwise spend the session writing into freed memory. */
    move_ctl now;
    if (move_ctl_resolve(&g_sig, &now)) {
        if (!g_have_ctl) {
            g_have_ctl = 1;
            g_ctl = now;
            api->log("movement control live (frame %lu)", g_frames);
        } else if (now.block != g_ctl.block) {
            api->log("state block moved: %#lx -> %#lx", g_ctl.block, now.block);
            g_ctl = now;
        }
    } else if (g_have_ctl) {
        api->log("state block pointer went NULL on frame %lu", g_frames);
        g_have_ctl = 0;
    }

    drive_movement();
}

static int open_gates(const uint8_t *code, size_t len, uintptr_t base_va)
{
    uintptr_t flag = move_find_padmode_flag(code, len, base_va, g_sig.fetch_fn);
    if (!flag) {
        api->log("controller-mode flag not found — movement would stay dormant");
        return -1;
    }

    uintptr_t gates[MAX_GATES];
    size_t ng = move_find_gates(code, len, base_va, g_sig.fetch_fn, flag,
                                gates, MAX_GATES);
    if (!ng) {
        api->log("no movement gate found — movement would stay dormant");
        return -1;
    }

    size_t opened = 0;
    for (size_t i = 0; i < ng; i++) {
        uint8_t expect[6];
        memcpy(expect, (const void *)gates[i], sizeof expect);
        if (expect[0] != 0x0F || expect[1] != 0x84) continue;
        int rc = api->patch_nop(gates[i], 6, expect);
        if (rc == BG3LESE_OK) opened++;
        else api->log("gate @%#lx not patched (rc=%d)", gates[i], rc);
    }
    if (!opened) return -1;
    api->log("opened %zu of %zu movement gate(s)", opened, ng);
    return 0;
}

static int wasd_init(const bg3lese_api *a)
{
    api = a;
    cfg_suppress = api->cfg_int("SUPPRESS", 1);
    cfg_move = api->cfg_int("MOVE", 1);
    cfg_trace = api->cfg_int("TRACE", 0);
    cfg_walk_key = scancode_by_name(api->cfg("WALK_KEY", NULL), SDL_SCANCODE_LSHIFT);
    cfg_walk_speed = api->cfg_num("WALK_SPEED", 0.5);
    if (cfg_walk_speed <= 0.0 || cfg_walk_speed > 1.0) cfg_walk_speed = 0.5;

    const uint8_t *code;
    size_t len;
    uintptr_t base_va;
    api->image(&code, &len, &base_va);

    /* Handling input while unable to actually move would be worse than not
     * loading: WASD would be swallowed and the character would stand still. So
     * an unrecognised build is a clean decline, and the game keeps its keys.
     * INPUT_ONLY overrides that for testing the filter without the game, and
     * for diagnosing key handling on a build we cannot yet drive. */
    int input_only = api->cfg_int("INPUT_ONLY", 0);

    int rc = move_find_sig(code, len, base_va, &g_sig);
    if (rc != 1) {
        const char *why = rc < 0
            ? "movement signature is ambiguous"
            : "movement signature not found — this game build is not one I recognise";
        if (!input_only) {
            api->log("%s; declining to load", why);
            return -1;
        }
        api->log("%s; INPUT_ONLY set, so filtering input but not moving", why);
        g_ready = 1;
        return 0;
    }
    api->log("override @%#lx, state ptr @%#lx, vec +%#x, flag +%#x",
             g_sig.match_va, g_sig.global_slot, g_sig.vec_off, g_sig.flag_off);

    if (api->cfg_int("GATE", 1) && open_gates(code, len, base_va) != 0) {
        if (!input_only) return -1;
        api->log("INPUT_ONLY set, so filtering input but not moving");
        g_ready = 1;
        return 0;
    }

    api->log("ready (move=%d suppress=%d walk=%s@%.2f)", cfg_move, cfg_suppress,
             cfg_walk_key == SDL_SCANCODE_UNKNOWN ? "off"
                                                  : SDL_GetScancodeName(cfg_walk_key),
             cfg_walk_speed);
    g_ready = 1;
    g_can_move = 1;
    return 0;
}

static void wasd_shutdown(void)
{
    if (g_have_ctl && *g_ctl.flag) *g_ctl.flag = 0;
    /* The gate NOPs are the host's to revert; it does that from its ledger. */
}

static const bg3lese_plugin wasd_plugin = {
    .abi = BG3LESE_ABI,
    .name = "wasd",
    .version = WASD_VERSION,
    /* Early: this consumes movement keys, and a plugin that merely observes
     * them should still see everything we decline to take. */
    .priority = 10,
    .init = wasd_init,
    .on_event = wasd_on_event,
    .on_frame = wasd_on_frame,
    .shutdown = wasd_shutdown,
};
BG3LESE_BUILTIN(wasd_plugin);
