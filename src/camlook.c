/* camlook — hold right mouse to rotate the camera.
 *
 * This mod deliberately implements no camera maths. The game already has smooth
 * drag-rotation: it imports no SDL_SetRelativeMouseMode and no SDL_CaptureMouse,
 * only SDL_WarpMouseInWindow and SDL_ShowCursor, which is the classic
 * hide-the-cursor-and-warp-it-back idiom. Whatever button that rotation is bound
 * to, the game hides the cursor, warps it, and reads the deltas itself.
 *
 * So all we do is make a right-drag look like that drag. We swallow the right
 * button and hand the game the look button in its place, then get out of the
 * way — the rotation, its acceleration curve and its cursor handling are the
 * game's own.
 *
 * The one piece of real work is telling a drag from a click, because right-click
 * opens context menus and stealing it would be worse than the feature is good.
 * So the press is held back until the mouse actually moves: move past the
 * threshold and it becomes a look-drag, release without moving and the original
 * click is handed over intact, just a few milliseconds late.
 */
#include "bg3lese.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CAMLOOK_VERSION "0.1.0"

static const bg3lese_api *api;

static int cfg_trigger;      /* the button you hold: right, by default */
static int cfg_look;         /* the button the game already rotates on */
static int cfg_threshold;    /* pixels of travel before it counts as a drag */
static int cfg_trace;

/* IDLE -> PENDING on press, then either DRAGGING or a replayed click. */
static enum { IDLE, PENDING, DRAGGING } state;
static SDL_Event g_press;    /* the held-back press, replayed if it was a click */
static int g_ax, g_ay;       /* accumulated travel since the press */

#define trace(...) do { if (cfg_trace) api->log(__VA_ARGS__); } while (0)

static int button_by_name(const char *name, int dflt)
{
    static const struct { const char *name; int b; } tbl[] = {
        {"left", SDL_BUTTON_LEFT}, {"middle", SDL_BUTTON_MIDDLE},
        {"right", SDL_BUTTON_RIGHT}, {"x1", SDL_BUTTON_X1}, {"x2", SDL_BUTTON_X2},
    };
    if (!name) return dflt;
    for (size_t i = 0; i < sizeof tbl / sizeof *tbl; i++)
        if (!strcasecmp(name, tbl[i].name)) return tbl[i].b;
    return dflt;
}

static const char *button_name(int b)
{
    switch (b) {
    case SDL_BUTTON_LEFT:   return "left";
    case SDL_BUTTON_MIDDLE: return "middle";
    case SDL_BUTTON_RIGHT:  return "right";
    case SDL_BUTTON_X1:     return "x1";
    case SDL_BUTTON_X2:     return "x2";
    default:                return "?";
    }
}

/* Hand the game a button event it never received, built from a real one so the
 * window, device and timestamp stay consistent. */
static void send_button(const SDL_Event *like, uint32_t type, int button, uint8_t st)
{
    SDL_Event e = *like;
    e.type = type;
    e.button.type = type;
    e.button.button = (uint8_t)button;
    e.button.state = st;
    e.button.clicks = 1;
    api->push_event(&e);
}

static void end_drag(const SDL_Event *like, const char *why)
{
    if (state == DRAGGING) {
        send_button(like, SDL_MOUSEBUTTONUP, cfg_look, SDL_RELEASED);
        trace("look ended (%s)", why);
    }
    state = IDLE;
    g_ax = g_ay = 0;
}

static int camlook_on_event(const SDL_Event *ev)
{
    /* Releasing the look button on focus loss matters more than usual here: the
     * game hides the cursor while rotating, and leaving it hidden would look
     * like a hang. */
    if (ev->type == SDL_WINDOWEVENT &&
        (ev->window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
         ev->window.event == SDL_WINDOWEVENT_MINIMIZED)) {
        end_drag(&g_press, "focus lost");
        return 0;
    }

    if (api->text_input_active()) {
        end_drag(&g_press, "text input");
        return 0;
    }

    switch (ev->type) {
    case SDL_MOUSEBUTTONDOWN:
        if (ev->button.button != cfg_trigger || state != IDLE) return 0;
        /* Hold the press back until we know whether this is a click or a drag. */
        g_press = *ev;
        g_ax = g_ay = 0;
        state = PENDING;
        return 1;

    case SDL_MOUSEMOTION:
        if (state != PENDING) return 0;   /* the game needs motion to rotate */
        g_ax += abs(ev->motion.xrel);
        g_ay += abs(ev->motion.yrel);
        if (g_ax + g_ay < cfg_threshold) return 0;

        state = DRAGGING;
        /* Consume this motion and re-inject it behind the press, rather than
         * letting it through and injecting the press after. Injected events are
         * delivered ahead of real ones, so passing it through here would hand
         * the game the motion first and the button-down second — backwards, and
         * the first frame of the drag would be dropped. */
        send_button(&g_press, SDL_MOUSEBUTTONDOWN, cfg_look, SDL_PRESSED);
        api->push_event(ev);
        trace("look started after %d px", g_ax + g_ay);
        return 1;

    case SDL_MOUSEBUTTONUP:
        if (ev->button.button != cfg_trigger) return 0;
        if (state == DRAGGING) {
            end_drag(ev, "released");
            return 1;
        }
        if (state == PENDING) {
            /* It was a click. Replay both halves in order — injecting only the
             * press and letting this release through would deliver them
             * backwards, since injected events are handed over first. */
            send_button(&g_press, SDL_MOUSEBUTTONDOWN, cfg_trigger, SDL_PRESSED);
            send_button(ev, SDL_MOUSEBUTTONUP, cfg_trigger, SDL_RELEASED);
            trace("click passed through");
            state = IDLE;
            return 1;
        }
        return 0;

    default:
        return 0;
    }
}

static int camlook_init(const bg3lese_api *a)
{
    api = a;
    cfg_trace = api->cfg_int("TRACE", 0);
    cfg_trigger = button_by_name(api->cfg("LOOK_TRIGGER", NULL), SDL_BUTTON_RIGHT);
    cfg_look = button_by_name(api->cfg("LOOK_BUTTON", NULL), SDL_BUTTON_MIDDLE);
    cfg_threshold = api->cfg_int("LOOK_THRESHOLD", 4);
    if (cfg_threshold < 1) cfg_threshold = 1;

    if (cfg_trigger == cfg_look) {
        api->log("LOOK_TRIGGER and LOOK_BUTTON are both %s — that would loop; "
                 "declining to load", button_name(cfg_look));
        return -1;
    }
    if (!api->cfg_int("LOOK", 1)) {
        api->log("disabled by BG3LE_LOOK=0");
        return -1;
    }

    /* Nothing is patched and no address is needed: this is pure event
     * translation, so it cannot be broken by a game update moving code. */
    api->log("ready — hold %s to rotate (sent as %s, %d px threshold)",
             button_name(cfg_trigger), button_name(cfg_look), cfg_threshold);
    return 0;
}

static void camlook_shutdown(void)
{
    end_drag(&g_press, "shutdown");
}

static const bg3lese_plugin camlook_plugin = {
    .abi = BG3LESE_ABI,
    .name = "camlook",
    .version = CAMLOOK_VERSION,
    /* After wasd: that one only takes keys, and if it ever declines to load we
     * still want the camera to work. */
    .priority = 20,
    .init = camlook_init,
    .on_event = camlook_on_event,
    .shutdown = camlook_shutdown,
};
BG3LESE_BUILTIN(camlook_plugin);
