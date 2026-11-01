/* Regression probe for SDL X11 popup synchronization. Creates only temporary windows.
 * From the engine root, link against the SDL archive being tested:
 * cc -O2 -I Engine/Source/ThirdParty/SDL3/SDL-gui-backend/include \
 *   "$FIXES_ROOT/tests/SDLPopupSyncTest.c" /path/to/libSDL3_fPIC.a \
 *   -lm -pthread -ldl -o /tmp/sdl-popup-sync-test
 */
#include <SDL3/SDL.h>
#include <stdio.h>

static int failures;

static void exercise(SDL_Window *window, const char *kind, int x, int y)
{
    SDL_SyncWindow(window);
    for (int i = 0; i < 6; ++i) {
        SDL_SetWindowSize(window, 150 + (i % 2), 100);
        SDL_SetWindowPosition(window, x, y);
        Uint64 start = SDL_GetTicksNS();
        bool ok = SDL_SyncWindow(window);
        double ms = (SDL_GetTicksNS() - start) / 1000000.0;
        int actual_x, actual_y, w, h;
        SDL_GetWindowPosition(window, &actual_x, &actual_y);
        SDL_GetWindowSize(window, &w, &h);
        printf("kind=%s trial=%d ok=%d elapsed_ms=%.3f relative=%d,%d size=%d,%d\n",
               kind, i, ok, ms, actual_x, actual_y, w, h);
        if (!ok || actual_x != x || actual_y != y || w != 150 + (i % 2) || h != 100) {
            ++failures;
        }
    }
}

int main(void)
{
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "%s\n", SDL_GetError()); return 2; }
    SDL_Window *parent = SDL_CreateWindow("SDL popup sync test", 480, 360,
        SDL_WINDOW_BORDERLESS | SDL_WINDOW_NOT_FOCUSABLE);
    if (!parent) { fprintf(stderr, "%s\n", SDL_GetError()); return 2; }
    SDL_SetWindowPosition(parent, 420, 180);
    SDL_SyncWindow(parent);
    SDL_Window *menu = SDL_CreatePopupWindow(parent, 15, 20, 150, 100,
        SDL_WINDOW_POPUP_MENU | SDL_WINDOW_NOT_FOCUSABLE);
    SDL_Window *tooltip = SDL_CreatePopupWindow(parent, 30, 40, 150, 100,
        SDL_WINDOW_TOOLTIP | SDL_WINDOW_NOT_FOCUSABLE);
    SDL_Window *nested = menu ? SDL_CreatePopupWindow(menu, 25, 30, 150, 100,
        SDL_WINDOW_TOOLTIP | SDL_WINDOW_NOT_FOCUSABLE) : NULL;
    if (!menu || !tooltip || !nested) { fprintf(stderr, "%s\n", SDL_GetError()); return 2; }
    exercise(parent, "toplevel", 420, 180);
    exercise(menu, "menu", 15, 20);
    exercise(tooltip, "tooltip", 30, 40);
    exercise(nested, "nested-tooltip", 25, 30);
    SDL_DestroyWindow(nested);
    SDL_DestroyWindow(tooltip);
    SDL_DestroyWindow(menu);
    SDL_DestroyWindow(parent);
    SDL_Quit();
    printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
