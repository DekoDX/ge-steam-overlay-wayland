# Standalone Wayland Steam overlay bridge

This project makes Steam's native Vulkan overlay usable by native Wayland
applications and Wine Wayland without modifying Wine, Proton, Steam, or the
game. It uses a Vulkan meta-layer with three ordered components:

```
application
  -> VK_LAYER_GE_wayland_steam_overlay_front
  -> VK_LAYER_VALVE_steam_overlay
  -> VK_LAYER_GE_wayland_steam_overlay_back
  -> Vulkan driver
```

The front component intercepts `vkCreateWaylandSurfaceKHR`, creates a hidden
X11 proxy window, and calls the next layer through `vkCreateXlibSurfaceKHR`.
Steam therefore sees a normal X11 window and initializes its existing XIM/XIC
text-input path without any Steam-specific ABI calls. The back component
recognizes that exact proxy request and creates the Vulkan surface from the
application's original `wl_surface`. Swapchain creation and presentation stay
on Wayland.

The layer does not scan Steam instructions, access private vtables, patch Steam
binaries, preload a Steam symbol interposer, or inject private character-event
records. If either translation component is unavailable or rejects a request,
the front component falls back to the application's original Wayland surface
creation call.

## Input

The front component creates keyboard and pointer objects from the Wayland seat
and forwards their events through Steam's normal X11 event path. Steam's XIC
converts keyboard events according to the active layout and handles modifiers,
compose sequences, and text input.

This standalone input path is independent of an application's own Wayland
input path. Under Wine, the layer cannot tell Wine that Steam consumed an
event, so the game may also receive input while the overlay is active. A
Wine-integrated bridge can avoid that limitation, but would no longer be
independent of the Proton or Wine version.

The layer uses one process-wide input context for the Wayland connection and
dispatches its private event queue once before each presentation.

## Activation

Both 64-bit and 32-bit components must be installed for mixed-architecture
games. At build time, CMake locates Steam's native overlay library and creates
a uniquely named explicit registration for it. Steam must still make
`gameoverlayrenderer.so` available to the game process.

Enable the compatibility sandwich and suppress Steam's separate implicit copy
of the same component layer:

```sh
WINE_WAYLAND_STEAM_OVERLAY_LAYER=1
DISABLE_VK_LAYER_VALVE_steam_overlay_1=1
```

The meta-layer activates the explicit Steam component in the middle. The
disable variable suppresses only Steam's original implicit registration, so it
cannot be inserted outside the ordered sandwich.

Disable this project explicitly with:

```sh
DISABLE_WINE_WAYLAND_STEAM_OVERLAY_LAYER=1
```

For diagnostics, set `GE_WAYLAND_STEAM_OVERLAY_DEBUG=1`. Vulkan layer discovery
and ordering can additionally be inspected with `VK_LOADER_DEBUG=layer`.

If Steam is installed in a nonstandard location, pass its library explicitly
when configuring each architecture:

```sh
-DSTEAM_OVERLAY_LIBRARY=/path/to/steamoverlayvulkanlayer.so
```

## Build

The build requires CMake, Ninja, Vulkan headers, vkroots, Xlib, Wayland client
and cursor libraries, pthreads, and `wayland-scanner`.

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DVULKAN_HEADERS_INCLUDE_DIR=/path/to/Vulkan-Headers/include \
  -DVKROOTS_INCLUDE_DIR=/path/to/vkroots
cmake --build build
cmake --install build
```

Build and install a second time with a 32-bit toolchain and 32-bit dependencies
to support 32-bit applications.
