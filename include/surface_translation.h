#ifndef GE_STEAM_OVERLAY_SURFACE_TRANSLATION_H
#define GE_STEAM_OVERLAY_SURFACE_TRANSLATION_H

#include <stdint.h>

#define GE_STEAM_OVERLAY_TRANSLATION_VERSION 1
#define GE_STEAM_OVERLAY_REGISTER_SURFACE_SYMBOL \
    "ge_steam_overlay_register_wayland_surface_v1"
#define GE_STEAM_OVERLAY_SYNTHETIC_XLIB_SYMBOL \
    "ge_steam_overlay_set_synthetic_xlib_extension_v1"

struct ge_steam_overlay_surface_target
{
    uint32_t size;
    uint32_t version;
    void *instance;
    void *x_display;
    unsigned long x_window;
    void *wayland_display;
    void *wayland_surface;
    const void *p_next;
    uint32_t flags;
    uint32_t translated;
};

typedef int (*ge_steam_overlay_register_surface_fn)(
    struct ge_steam_overlay_surface_target *target);
typedef void (*ge_steam_overlay_set_synthetic_xlib_fn)(int enabled);

#endif
