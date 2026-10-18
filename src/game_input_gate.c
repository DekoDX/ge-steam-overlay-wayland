/* Keeps keyboard and mouse input away from the game while the Steam overlay
 * is open. The game may hold a Wayland pointer lock, so the compositor keeps
 * delivering motion and clicks to it no matter what is stacked above its
 * surface. Native games sit on SDL, so the events are dropped at SDL's event
 * queue instead; the layer's own Wayland listeners still feed Steam. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "steam_overlay_bridge.h"

#define GE_MAX_SCANCODES 512

static const SDL_EventType gated_events[] =
{
    SDL_EVENT_KEY_DOWN,
    SDL_EVENT_KEY_UP,
    SDL_EVENT_TEXT_EDITING,
    SDL_EVENT_TEXT_INPUT,
    SDL_EVENT_MOUSE_MOTION,
    SDL_EVENT_MOUSE_BUTTON_DOWN,
    SDL_EVENT_MOUSE_BUTTON_UP,
    SDL_EVENT_MOUSE_WHEEL,
};

static struct
{
    void (*set_event_enabled)(Uint32, bool);
    bool (*push_event)(SDL_Event *);
    const bool *(*get_keyboard_state)(int *);
    SDL_MouseButtonFlags (*get_mouse_state)(float *, float *);
    SDL_Keycode (*get_key_from_scancode)(SDL_Scancode, SDL_Keymod, bool);
    SDL_Keymod (*get_mod_state)(void);
    SDL_Window *(*get_keyboard_focus)(void);
    bool (*set_cursor)(SDL_Cursor *);
    SDL_WindowID (*get_window_id)(SDL_Window *);
} sdl;

static int gated;
static bool keys_held[GE_MAX_SCANCODES];
static SDL_MouseButtonFlags buttons_held;

static int load_sdl(void)
{
    static int state; /* 0 = untried, 1 = ready, -1 = unavailable */

    if (state) return state > 0;
    sdl.set_event_enabled = dlsym(RTLD_DEFAULT, "SDL_SetEventEnabled");
    sdl.push_event = dlsym(RTLD_DEFAULT, "SDL_PushEvent");
    sdl.get_keyboard_state = dlsym(RTLD_DEFAULT, "SDL_GetKeyboardState");
    sdl.get_mouse_state = dlsym(RTLD_DEFAULT, "SDL_GetMouseState");
    sdl.get_key_from_scancode = dlsym(RTLD_DEFAULT, "SDL_GetKeyFromScancode");
    sdl.get_mod_state = dlsym(RTLD_DEFAULT, "SDL_GetModState");
    sdl.get_keyboard_focus = dlsym(RTLD_DEFAULT, "SDL_GetKeyboardFocus");
    sdl.get_window_id = dlsym(RTLD_DEFAULT, "SDL_GetWindowID");
    sdl.set_cursor = dlsym(RTLD_DEFAULT, "SDL_SetCursor");
    state = sdl.set_event_enabled && sdl.push_event && sdl.get_keyboard_state &&
            sdl.get_mouse_state && sdl.get_key_from_scancode &&
            sdl.get_mod_state && sdl.get_keyboard_focus && sdl.get_window_id &&
            sdl.set_cursor
            ? 1 : -1;
    return state > 0;
}

/* Events dropped while gated include releases, which would leave the game
 * believing a key or button is still held. Replay those releases. */
static void release_stale_input(void)
{
    SDL_Window *window = sdl.get_keyboard_focus();
    SDL_WindowID window_id = window ? sdl.get_window_id(window) : 0;
    SDL_Keymod mod = sdl.get_mod_state();
    SDL_MouseButtonFlags buttons = sdl.get_mouse_state(NULL, NULL);
    const bool *state;
    int count = 0, i;
    SDL_Event event;

    state = sdl.get_keyboard_state(&count);
    if (count > GE_MAX_SCANCODES) count = GE_MAX_SCANCODES;
    for (i = 0; state && i < count; ++i)
    {
        if (!keys_held[i] || state[i]) continue;
        memset(&event, 0, sizeof(event));
        event.key.type = SDL_EVENT_KEY_UP;
        event.key.windowID = window_id;
        event.key.scancode = (SDL_Scancode)i;
        event.key.key = sdl.get_key_from_scancode((SDL_Scancode)i, mod, false);
        event.key.mod = mod;
        sdl.push_event(&event);
    }
    for (i = 1; i <= 32; ++i)
    {
        SDL_MouseButtonFlags bit = SDL_BUTTON_MASK(i);

        if (!(buttons_held & bit) || (buttons & bit)) continue;
        memset(&event, 0, sizeof(event));
        event.button.type = SDL_EVENT_MOUSE_BUTTON_UP;
        event.button.windowID = window_id;
        event.button.button = (Uint8)i;
        sdl.push_event(&event);
    }
}

void ge_overlay_game_input_gate(int block)
{
    size_t i;
    int count = 0;

    block = !!block;
    if (block == gated || !load_sdl()) return;
    gated = block;

    if (block)
    {
        const bool *state = sdl.get_keyboard_state(&count);

        memset(keys_held, 0, sizeof(keys_held));
        if (count > GE_MAX_SCANCODES) count = GE_MAX_SCANCODES;
        if (state) memcpy(keys_held, state, (size_t)count);
        buttons_held = sdl.get_mouse_state(NULL, NULL);
    }
    for (i = 0; i < sizeof(gated_events) / sizeof(gated_events[0]); ++i)
        sdl.set_event_enabled(gated_events[i], !block);
    if (!block)
    {
        release_stale_input();
        /* The overlay replaced the pointer's cursor image. A NULL argument
         * makes SDL re-apply the game's own cursor state, which also hides
         * it again when the game is in relative mouse mode. */
        sdl.set_cursor(NULL);
    }
}
