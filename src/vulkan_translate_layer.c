#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_USE_PLATFORM_XLIB_KHR
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include "surface_translation.h"

struct physical_device_data
{
    VkPhysicalDevice physical_device;
    struct physical_device_data *next;
};

struct instance_data
{
    VkInstance instance;
    PFN_vkGetInstanceProcAddr next_gipa;
    PFN_vkCreateWaylandSurfaceKHR create_wayland_surface;
    PFN_vkCreateXlibSurfaceKHR create_xlib_surface;
    PFN_vkGetPhysicalDeviceWaylandPresentationSupportKHR wayland_presentation_support;
    PFN_vkGetPhysicalDeviceXlibPresentationSupportKHR xlib_presentation_support;
    PFN_vkEnumeratePhysicalDevices enumerate_physical_devices;
    PFN_vkDestroyInstance destroy_instance;
    struct physical_device_data *physical_devices;
    Display *proxy_display;
    struct wl_display *wayland_display;
    struct instance_data *next;
};

static pthread_mutex_t instances_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct instance_data *instances;
static _Thread_local struct ge_steam_overlay_surface_target *surface_target;
static _Thread_local int synthetic_xlib_extension;

static int trace_enabled(void)
{
    const char *env = getenv("GE_WAYLAND_STEAM_OVERLAY_DEBUG");
    return env && atoi(env) != 0;
}

#define TRACE(fmt, ...) do { \
    if (trace_enabled()) \
        fprintf(stderr, "steam-overlay-wayland-translate: " fmt, ##__VA_ARGS__); \
} while (0)

static struct instance_data *instance_from_handle_locked(VkInstance instance)
{
    struct instance_data *data;

    for (data = instances; data; data = data->next)
        if (data->instance == instance) return data;
    return NULL;
}

static struct instance_data *instance_from_physical_device_locked(
        VkPhysicalDevice physical_device)
{
    struct physical_device_data *physical;
    struct instance_data *data;

    for (data = instances; data; data = data->next)
        for (physical = data->physical_devices; physical; physical = physical->next)
            if (physical->physical_device == physical_device) return data;
    return NULL;
}

static VkResult remember_physical_devices(struct instance_data *data, uint32_t count,
                                          const VkPhysicalDevice *physical_devices)
{
    struct physical_device_data *physical, **tail;
    uint32_t i;

    pthread_mutex_lock(&instances_mutex);
    tail = &data->physical_devices;
    while (*tail) tail = &(*tail)->next;
    for (i = 0; i < count; ++i)
    {
        for (physical = data->physical_devices; physical; physical = physical->next)
            if (physical->physical_device == physical_devices[i]) break;
        if (physical) continue;
        if (!(physical = malloc(sizeof(*physical))))
        {
            pthread_mutex_unlock(&instances_mutex);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        physical->physical_device = physical_devices[i];
        physical->next = NULL;
        *tail = physical;
        tail = &physical->next;
    }
    pthread_mutex_unlock(&instances_mutex);
    return VK_SUCCESS;
}

__attribute__((visibility("default")))
int ge_steam_overlay_register_wayland_surface_v1(
        struct ge_steam_overlay_surface_target *target)
{
    struct instance_data *data;

    if (!target)
    {
        surface_target = NULL;
        return 1;
    }
    if (surface_target || target->size < sizeof(*target) ||
        target->version != GE_STEAM_OVERLAY_TRANSLATION_VERSION ||
        !target->instance || !target->x_display || !target->x_window ||
        !target->wayland_display || !target->wayland_surface)
        return 0;

    pthread_mutex_lock(&instances_mutex);
    data = instance_from_handle_locked((VkInstance)target->instance);
    if (data)
    {
        data->proxy_display = target->x_display;
        data->wayland_display = target->wayland_display;
    }
    pthread_mutex_unlock(&instances_mutex);
    if (!data) return 0;

    target->translated = 0;
    surface_target = target;
    return 1;
}

__attribute__((visibility("default")))
void ge_steam_overlay_set_synthetic_xlib_extension_v1(int enabled)
{
    synthetic_xlib_extension = !!enabled;
}

static VKAPI_ATTR VkResult VKAPI_CALL translate_CreateXlibSurfaceKHR(
        VkInstance instance, const VkXlibSurfaceCreateInfoKHR *create_info,
        const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface)
{
    PFN_vkCreateWaylandSurfaceKHR create_wayland_surface = NULL;
    PFN_vkCreateXlibSurfaceKHR create_xlib_surface = NULL;
    struct ge_steam_overlay_surface_target *target = surface_target;
    VkWaylandSurfaceCreateInfoKHR wayland_info;
    struct instance_data *data;

    pthread_mutex_lock(&instances_mutex);
    if ((data = instance_from_handle_locked(instance)))
    {
        create_wayland_surface = data->create_wayland_surface;
        create_xlib_surface = data->create_xlib_surface;
    }
    pthread_mutex_unlock(&instances_mutex);

    if (target && create_info && target->instance == instance &&
        target->x_display == create_info->dpy &&
        target->x_window == create_info->window)
    {
        if (!create_wayland_surface) return VK_ERROR_EXTENSION_NOT_PRESENT;

        wayland_info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
        wayland_info.pNext = target->p_next;
        wayland_info.flags = target->flags;
        wayland_info.display = target->wayland_display;
        wayland_info.surface = target->wayland_surface;
        target->translated = 1;
        TRACE("translated Xlib window %#lx to Wayland surface %p\n",
              create_info->window, target->wayland_surface);
        return create_wayland_surface(instance, &wayland_info, allocator, surface);
    }

    if (create_xlib_surface)
        return create_xlib_surface(instance, create_info, allocator, surface);
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL
translate_GetPhysicalDeviceXlibPresentationSupportKHR(
        VkPhysicalDevice physical_device, uint32_t queue_family, Display *display,
        VisualID visual_id)
{
    PFN_vkGetPhysicalDeviceWaylandPresentationSupportKHR wayland_support = NULL;
    PFN_vkGetPhysicalDeviceXlibPresentationSupportKHR xlib_support = NULL;
    struct wl_display *wayland_display = NULL;
    struct instance_data *data;

    pthread_mutex_lock(&instances_mutex);
    if ((data = instance_from_physical_device_locked(physical_device)))
    {
        xlib_support = data->xlib_presentation_support;
        if (data->proxy_display == display)
        {
            wayland_support = data->wayland_presentation_support;
            wayland_display = data->wayland_display;
        }
    }
    pthread_mutex_unlock(&instances_mutex);

    if (wayland_support && wayland_display)
        return wayland_support(physical_device, queue_family, wayland_display);
    if (xlib_support)
        return xlib_support(physical_device, queue_family, display, visual_id);
    return VK_FALSE;
}

static VKAPI_ATTR VkResult VKAPI_CALL translate_EnumeratePhysicalDevices(
        VkInstance instance, uint32_t *count, VkPhysicalDevice *physical_devices)
{
    PFN_vkEnumeratePhysicalDevices enumerate = NULL;
    struct instance_data *data;
    VkResult result;

    pthread_mutex_lock(&instances_mutex);
    if ((data = instance_from_handle_locked(instance)))
        enumerate = data->enumerate_physical_devices;
    pthread_mutex_unlock(&instances_mutex);
    if (!enumerate) return VK_ERROR_INITIALIZATION_FAILED;

    result = enumerate(instance, count, physical_devices);
    if (physical_devices && (result == VK_SUCCESS || result == VK_INCOMPLETE))
    {
        VkResult remember_result = remember_physical_devices(data, *count,
                                                              physical_devices);
        if (remember_result != VK_SUCCESS) return remember_result;
    }
    return result;
}

static void free_instance_data(struct instance_data *data)
{
    struct physical_device_data *physical;

    while ((physical = data->physical_devices))
    {
        data->physical_devices = physical->next;
        free(physical);
    }
    free(data);
}

static VKAPI_ATTR void VKAPI_CALL translate_DestroyInstance(
        VkInstance instance, const VkAllocationCallbacks *allocator)
{
    PFN_vkDestroyInstance destroy_instance = NULL;
    struct instance_data **entry, *data = NULL;

    pthread_mutex_lock(&instances_mutex);
    for (entry = &instances; *entry; entry = &(*entry)->next)
    {
        if ((*entry)->instance != instance) continue;
        data = *entry;
        *entry = data->next;
        destroy_instance = data->destroy_instance;
        break;
    }
    pthread_mutex_unlock(&instances_mutex);

    if (!data) return;
    free_instance_data(data);
    if (destroy_instance) destroy_instance(instance, allocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL translate_CreateInstance(
        const VkInstanceCreateInfo *create_info,
        const VkAllocationCallbacks *allocator, VkInstance *instance)
{
    VkLayerInstanceCreateInfo *link;
    VkInstanceCreateInfo next_info;
    PFN_vkGetInstanceProcAddr next_gipa;
    PFN_vkCreateInstance next_create;
    const char **extensions = NULL;
    struct instance_data *data;
    int strip_xlib;
    uint32_t i, count = 0;
    VkResult result;

    if (!create_info) return VK_ERROR_INITIALIZATION_FAILED;
    link = (VkLayerInstanceCreateInfo *)create_info->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                     link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerInstanceCreateInfo *)link->pNext;
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;

    next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    next_create = (PFN_vkCreateInstance)next_gipa(NULL, "vkCreateInstance");
    if (!next_create) return VK_ERROR_INITIALIZATION_FAILED;

    strip_xlib = synthetic_xlib_extension;
    next_info = *create_info;
    if (strip_xlib && create_info->enabledExtensionCount)
    {
        extensions = malloc(create_info->enabledExtensionCount * sizeof(*extensions));
        if (!extensions) return VK_ERROR_OUT_OF_HOST_MEMORY;
        for (i = 0; i < create_info->enabledExtensionCount; ++i)
        {
            if (!strcmp(create_info->ppEnabledExtensionNames[i],
                        VK_KHR_XLIB_SURFACE_EXTENSION_NAME))
                continue;
            extensions[count++] = create_info->ppEnabledExtensionNames[i];
        }
        next_info.enabledExtensionCount = count;
        next_info.ppEnabledExtensionNames = extensions;
    }

    result = next_create(&next_info, allocator, instance);
    free(extensions);
    if (result != VK_SUCCESS) return result;

    if (!(data = calloc(1, sizeof(*data))))
    {
        PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)
            next_gipa(*instance, "vkDestroyInstance");
        if (destroy) destroy(*instance, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    data->instance = *instance;
    data->next_gipa = next_gipa;
    data->create_wayland_surface = (PFN_vkCreateWaylandSurfaceKHR)
        next_gipa(*instance, "vkCreateWaylandSurfaceKHR");
    if (!strip_xlib)
        data->create_xlib_surface = (PFN_vkCreateXlibSurfaceKHR)
            next_gipa(*instance, "vkCreateXlibSurfaceKHR");
    data->wayland_presentation_support =
        (PFN_vkGetPhysicalDeviceWaylandPresentationSupportKHR)
        next_gipa(*instance, "vkGetPhysicalDeviceWaylandPresentationSupportKHR");
    if (!strip_xlib)
        data->xlib_presentation_support =
            (PFN_vkGetPhysicalDeviceXlibPresentationSupportKHR)
            next_gipa(*instance, "vkGetPhysicalDeviceXlibPresentationSupportKHR");
    data->enumerate_physical_devices = (PFN_vkEnumeratePhysicalDevices)
        next_gipa(*instance, "vkEnumeratePhysicalDevices");
    data->destroy_instance = (PFN_vkDestroyInstance)
        next_gipa(*instance, "vkDestroyInstance");

    pthread_mutex_lock(&instances_mutex);
    data->next = instances;
    instances = data;
    pthread_mutex_unlock(&instances_mutex);
    return VK_SUCCESS;
}

__attribute__((visibility("default")))
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
        VkInstance instance, const char *name)
{
    PFN_vkGetInstanceProcAddr next_gipa = NULL;
    struct instance_data *data;

    if (!name) return NULL;
    if (!strcmp(name, "vkGetInstanceProcAddr"))
        return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if (!strcmp(name, "vkCreateInstance"))
        return (PFN_vkVoidFunction)translate_CreateInstance;
    if (!strcmp(name, "vkDestroyInstance"))
        return (PFN_vkVoidFunction)translate_DestroyInstance;
    if (!strcmp(name, "vkEnumeratePhysicalDevices"))
        return (PFN_vkVoidFunction)translate_EnumeratePhysicalDevices;
    if (!strcmp(name, "vkCreateXlibSurfaceKHR"))
        return (PFN_vkVoidFunction)translate_CreateXlibSurfaceKHR;
    if (!strcmp(name, "vkGetPhysicalDeviceXlibPresentationSupportKHR"))
        return (PFN_vkVoidFunction)
            translate_GetPhysicalDeviceXlibPresentationSupportKHR;

    pthread_mutex_lock(&instances_mutex);
    if ((data = instance_from_handle_locked(instance))) next_gipa = data->next_gipa;
    pthread_mutex_unlock(&instances_mutex);
    return next_gipa ? next_gipa(instance, name) : NULL;
}

__attribute__((visibility("default")))
VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(
        VkNegotiateLayerInterface *interface)
{
    if (!interface) return VK_ERROR_INITIALIZATION_FAILED;
    if (interface->loaderLayerInterfaceVersion > 2)
        interface->loaderLayerInterfaceVersion = 2;
    interface->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    interface->pfnGetDeviceProcAddr = NULL;
    interface->pfnGetPhysicalDeviceProcAddr = NULL;
    return VK_SUCCESS;
}
