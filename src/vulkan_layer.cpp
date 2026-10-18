#define VK_USE_PLATFORM_XLIB_KHR
#define VK_USE_PLATFORM_WAYLAND_KHR

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <link.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <wayland-client.h>

#include "steam_overlay_bridge.h"
#include "surface_translation.h"
#include "vkroots.h"

namespace ge_steam_overlay
{

struct surface_state
{
    VkInstance instance;
    ge_overlay_wayland_surface *wayland;
};

struct translator_search
{
    std::string path;
};

static std::mutex surface_mutex;
static std::unordered_map<VkSurfaceKHR, surface_state> surfaces;
static std::once_flag translator_once;
static void *translator_handle;
static ge_steam_overlay_register_surface_fn register_surface;
static ge_steam_overlay_set_synthetic_xlib_fn set_synthetic_xlib;

static bool debug_enabled()
{
    const char *env = std::getenv("GE_WAYLAND_STEAM_OVERLAY_DEBUG");
    return env && std::atoi(env) != 0;
}

static void trace(const char *message)
{
    if (debug_enabled())
        std::fprintf(stderr, "steam-overlay-wayland: %s\n", message);
}

static int find_translator(struct dl_phdr_info *info, size_t size, void *arg)
{
    auto *search = static_cast<translator_search *>(arg);
    const char *name;

    (void)size;
    if (!info->dlpi_name || !*info->dlpi_name) return 0;
    name = std::strrchr(info->dlpi_name, '/');
    name = name ? name + 1 : info->dlpi_name;
    if (std::strcmp(name, GE_STEAM_OVERLAY_BACK_LIBRARY)) return 0;
    search->path = info->dlpi_name;
    return 1;
}

static void resolve_translator()
{
    translator_search search;

    if (!dl_iterate_phdr(find_translator, &search) || search.path.empty())
    {
        trace("surface translation layer is not loaded");
        return;
    }

    translator_handle = dlopen(search.path.c_str(),
                               RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
    if (!translator_handle)
    {
        trace("could not open the loaded surface translation layer");
        return;
    }

    register_surface = reinterpret_cast<ge_steam_overlay_register_surface_fn>(
        dlsym(translator_handle, GE_STEAM_OVERLAY_REGISTER_SURFACE_SYMBOL));
    if (!register_surface)
        trace("surface translation registration entry point is unavailable");

    set_synthetic_xlib =
        reinterpret_cast<ge_steam_overlay_set_synthetic_xlib_fn>(
            dlsym(translator_handle, GE_STEAM_OVERLAY_SYNTHETIC_XLIB_SYMBOL));
    if (!set_synthetic_xlib)
        trace("surface translation extension entry point is unavailable");
}

static void remember_surface(VkInstance instance, VkSurfaceKHR surface,
                             const VkWaylandSurfaceCreateInfoKHR *create_info)
{
    ge_overlay_wayland_surface *wayland;

    if (!surface || !create_info ||
        !(wayland = ge_overlay_wayland_surface_create(create_info->display,
                                                       create_info->surface)))
        return;

    std::lock_guard lock(surface_mutex);
    surfaces.emplace(surface, surface_state{instance, wayland});
}

static void destroy_surface_state(surface_state state)
{
    ge_overlay_wayland_surface_destroy(state.wayland);
}

static bool extension_enabled(const VkInstanceCreateInfo *create_info,
                              const char *name)
{
    for (uint32_t i = 0; i < create_info->enabledExtensionCount; ++i)
        if (!std::strcmp(create_info->ppEnabledExtensionNames[i], name))
            return true;
    return false;
}

class instance_overrides
{
public:
    static VkResult CreateInstance(
        PFN_vkCreateInstance create_instance,
        const VkInstanceCreateInfo *create_info,
        const VkAllocationCallbacks *allocator,
        VkInstance *instance)
    {
        VkInstanceCreateInfo info;
        std::vector<const char *> extensions;
        bool injected_xlib = false;
        VkResult result;

        if (!create_info)
            return create_instance(create_info, allocator, instance);

        std::call_once(translator_once, resolve_translator);
        info = *create_info;
        if (set_synthetic_xlib &&
            !extension_enabled(create_info,
                               VK_KHR_XLIB_SURFACE_EXTENSION_NAME))
        {
            if (create_info->enabledExtensionCount)
                extensions.assign(create_info->ppEnabledExtensionNames,
                                  create_info->ppEnabledExtensionNames +
                                      create_info->enabledExtensionCount);
            extensions.push_back(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
            info.enabledExtensionCount = extensions.size();
            info.ppEnabledExtensionNames = extensions.data();
            injected_xlib = true;
        }

        if (injected_xlib) set_synthetic_xlib(1);
        result = create_instance(&info, allocator, instance);
        if (injected_xlib) set_synthetic_xlib(0);
        if (result == VK_SUCCESS)
            ge_overlay_focus_proxy_instance_created();
        return result;
    }

    static VkResult CreateWaylandSurfaceKHR(
        const vkroots::VkInstanceDispatch& dispatch,
        VkInstance instance,
        const VkWaylandSurfaceCreateInfoKHR *create_info,
        const VkAllocationCallbacks *allocator,
        VkSurfaceKHR *surface)
    {
        VkResult result;
        void *x_display = nullptr;
        unsigned long x_window = 0;

        std::call_once(translator_once, resolve_translator);
        if (create_info && surface && register_surface &&
            ge_overlay_bridge_get_xlib_proxy(&x_display, &x_window))
        {
            ge_steam_overlay_surface_target target{
                .size = sizeof(target),
                .version = GE_STEAM_OVERLAY_TRANSLATION_VERSION,
                .instance = instance,
                .x_display = x_display,
                .x_window = x_window,
                .wayland_display = create_info->display,
                .wayland_surface = create_info->surface,
                .p_next = create_info->pNext,
                .flags = create_info->flags,
                .translated = 0,
            };
            VkXlibSurfaceCreateInfoKHR xlib_info{
                .sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
                .pNext = nullptr,
                .flags = 0,
                .dpy = static_cast<Display *>(x_display),
                .window = x_window,
            };

            if (register_surface(&target))
            {
                result = dispatch.CreateXlibSurfaceKHR(instance, &xlib_info,
                                                       allocator, surface);
                register_surface(nullptr);
                if (result == VK_SUCCESS && target.translated)
                {
                    remember_surface(instance, *surface, create_info);
                    return result;
                }

                if (result == VK_SUCCESS)
                {
                    dispatch.DestroySurfaceKHR(instance, *surface, allocator);
                    *surface = VK_NULL_HANDLE;
                }
                trace("surface translation failed, using Wayland directly");
            }
        }

        result = dispatch.CreateWaylandSurfaceKHR(instance, create_info,
                                                  allocator, surface);
        if (result == VK_SUCCESS && create_info && surface && *surface)
            remember_surface(instance, *surface, create_info);
        return result;
    }

    static void DestroySurfaceKHR(
        const vkroots::VkInstanceDispatch& dispatch,
        VkInstance instance,
        VkSurfaceKHR surface,
        const VkAllocationCallbacks *allocator)
    {
        surface_state state{};

        {
            std::lock_guard lock(surface_mutex);
            if (auto entry = surfaces.find(surface); entry != surfaces.end())
            {
                state = entry->second;
                surfaces.erase(entry);
            }
        }

        if (state.wayland) destroy_surface_state(state);
        dispatch.DestroySurfaceKHR(instance, surface, allocator);
    }

    static void DestroyInstance(
        const vkroots::VkInstanceDispatch& dispatch,
        VkInstance instance,
        const VkAllocationCallbacks *allocator)
    {
        std::vector<surface_state> stale_surfaces;

        {
            std::lock_guard lock(surface_mutex);
            for (auto entry = surfaces.begin(); entry != surfaces.end();)
            {
                if (entry->second.instance != instance)
                {
                    ++entry;
                    continue;
                }
                stale_surfaces.push_back(entry->second);
                entry = surfaces.erase(entry);
            }
        }

        for (auto state : stale_surfaces) destroy_surface_state(state);
        dispatch.DestroyInstance(instance, allocator);
        ge_overlay_focus_proxy_instance_destroyed();
    }
};

class device_overrides
{
public:
    static VkResult CreateSwapchainKHR(
        const vkroots::VkDeviceDispatch& dispatch,
        VkDevice device,
        const VkSwapchainCreateInfoKHR* create_info,
        const VkAllocationCallbacks* allocator,
        VkSwapchainKHR* swapchain)
    {
        if (create_info)
        {
            if (debug_enabled())
                std::fprintf(stderr,
                    "steam-overlay-wayland: swapchain %ux%u\n",
                    create_info->imageExtent.width,
                    create_info->imageExtent.height);
            ge_overlay_wayland_set_swapchain_extent(
                create_info->imageExtent.width,
                create_info->imageExtent.height);
        }

        return dispatch.CreateSwapchainKHR(
            device, create_info, allocator, swapchain);
    }

    static VkResult QueuePresentKHR(
        const vkroots::VkQueueDispatch& dispatch,
        VkQueue queue,
        const VkPresentInfoKHR *present_info)
    {
        ge_overlay_wayland_surface *wayland = nullptr;

        {
            std::lock_guard lock(surface_mutex);
            if (!surfaces.empty())
                wayland = ge_overlay_wayland_surface_ref(
                    surfaces.begin()->second.wayland);
        }

        if (wayland)
        {
            ge_overlay_wayland_surface_dispatch(wayland);
            ge_overlay_wayland_surface_destroy(wayland);
        }
        return dispatch.QueuePresentKHR(queue, present_info);
    }
};

} /* namespace ge_steam_overlay */

VKROOTS_DEFINE_LAYER_INTERFACES(ge_steam_overlay::instance_overrides,
                                ge_steam_overlay::device_overrides)
