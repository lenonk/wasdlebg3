/* A stand-in for the game: pushes known input events, then reports exactly what
 * SDL_PollEvent handed back. Run it with and without the shim preloaded and the
 * difference is the shim's effect. Uses SDL's dummy drivers so it needs no display. */
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>

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

int main(void)
{
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 2;
    }

    const struct { SDL_Scancode sc; int down; } script[] = {
        {SDL_SCANCODE_W, 1}, {SDL_SCANCODE_X, 1},
        {SDL_SCANCODE_D, 1}, {SDL_SCANCODE_W, 0},
        {SDL_SCANCODE_SPACE, 1}, {SDL_SCANCODE_D, 0},
    };
    for (size_t i = 0; i < sizeof script / sizeof *script; i++)
        push_key(script[i].sc, script[i].down);

    int keys = 0, axes = 0, other = 0;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
            keys++;
            printf("  GAME SAW key %-6s %s\n",
                   SDL_GetScancodeName(e.key.keysym.scancode),
                   e.type == SDL_KEYDOWN ? "down" : "up");
        } else if (e.type == SDL_CONTROLLERAXISMOTION) {
            axes++;
            printf("  GAME SAW axis %d = %d\n", e.caxis.axis, e.caxis.value);
        } else {
            other++;
        }
    }
    printf("RESULT keys=%d axes=%d other=%d\n", keys, axes, other);
    SDL_Quit();
    return 0;
}
