/* A stand-in for the game: pushes known input, then reports exactly what
 * SDL_PollEvent handed back. Run with and without the shim preloaded and the
 * difference is the shim's effect. Uses SDL's dummy drivers, so it needs no
 * display and no copy of BG3.
 *
 *   sdl_harness            six key events, four of them WASD
 *   sdl_harness modifier   left shift plus W
 *   sdl_harness textinput  WASD while a text field is focused
 *   sdl_harness focus      W held, then the window loses focus
 *   sdl_harness rmbclick   right press and release without moving
 *   sdl_harness rmbdrag    right press, mouse moves, release
 *   sdl_harness lmbdrag    left press, mouse moves, release
 */
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>

static void push_key(SDL_Scancode sc, int down)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.keysym.scancode = sc;
    e.key.keysym.sym = SDL_GetKeyFromScancode(sc);
    if (SDL_PushEvent(&e) < 0)
        fprintf(stderr, "push failed: %s\n", SDL_GetError());
}

static void push_button(int button, int down, int x, int y)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    e.button.button = (Uint8)button;
    e.button.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.button.clicks = 1;
    e.button.x = x;
    e.button.y = y;
    SDL_PushEvent(&e);
}

static void push_motion(int dx, int dy)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_MOUSEMOTION;
    e.motion.xrel = dx;
    e.motion.yrel = dy;
    SDL_PushEvent(&e);
}

static const char *bname(Uint8 b)
{
    switch (b) {
    case SDL_BUTTON_LEFT: return "left";
    case SDL_BUTTON_MIDDLE: return "middle";
    case SDL_BUTTON_RIGHT: return "right";
    default: return "?";
    }
}

static void push_focus_lost(void)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_WINDOWEVENT;
    e.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    SDL_PushEvent(&e);
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "default";
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 2;
    }

    if (!strcmp(mode, "modifier")) {
        push_key(SDL_SCANCODE_LSHIFT, 1);
        push_key(SDL_SCANCODE_W, 1);
        push_key(SDL_SCANCODE_W, 0);
        push_key(SDL_SCANCODE_LSHIFT, 0);
    } else if (!strcmp(mode, "textinput")) {
        SDL_StartTextInput();          /* the shim interposes this */
        push_key(SDL_SCANCODE_W, 1);
        push_key(SDL_SCANCODE_A, 1);
        push_key(SDL_SCANCODE_S, 1);
        push_key(SDL_SCANCODE_D, 1);
    } else if (!strcmp(mode, "focus")) {
        push_key(SDL_SCANCODE_W, 1);
        push_focus_lost();
        push_key(SDL_SCANCODE_X, 1);
    } else if (!strcmp(mode, "rmbclick")) {
        push_button(SDL_BUTTON_RIGHT, 1, 100, 100);
        push_button(SDL_BUTTON_RIGHT, 0, 100, 100);
    } else if (!strcmp(mode, "rmbdrag")) {
        push_button(SDL_BUTTON_RIGHT, 1, 100, 100);
        push_motion(10, 0);
        push_motion(10, 0);
        push_button(SDL_BUTTON_RIGHT, 0, 120, 100);
    } else if (!strcmp(mode, "lmbdrag")) {
        push_button(SDL_BUTTON_LEFT, 1, 100, 100);
        push_motion(10, 0);
        push_button(SDL_BUTTON_LEFT, 0, 110, 100);
    } else {
        const struct { SDL_Scancode sc; int down; } script[] = {
            {SDL_SCANCODE_W, 1}, {SDL_SCANCODE_X, 1},
            {SDL_SCANCODE_D, 1}, {SDL_SCANCODE_W, 0},
            {SDL_SCANCODE_SPACE, 1}, {SDL_SCANCODE_D, 0},
        };
        for (size_t i = 0; i < sizeof script / sizeof *script; i++)
            push_key(script[i].sc, script[i].down);
    }

    int keys = 0, other = 0, motions = 0;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
            keys++;
            printf("  GAME SAW key %-6s %s\n",
                   SDL_GetScancodeName(e.key.keysym.scancode),
                   e.type == SDL_KEYDOWN ? "down" : "up");
        } else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
            printf("  GAME SAW btn %-6s %s\n", bname(e.button.button),
                   e.type == SDL_MOUSEBUTTONDOWN ? "down" : "up");
        } else if (e.type == SDL_MOUSEMOTION) {
            motions++;
            printf("  GAME SAW motion %+d\n", e.motion.xrel);
        } else {
            other++;
        }
    }
    printf("RESULT keys=%d other=%d motions=%d\n", keys, other, motions);
    SDL_Quit();
    return 0;
}
