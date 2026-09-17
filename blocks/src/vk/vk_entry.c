/* src/vk/vk_entry.c — dlopen-based resolution of the Vulkan entry points.
 *
 * See vk_entry.h for why the game does not link libvulkan directly. */
#include "vk_entry.h"

#include <dlfcn.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

EnjoerVkEntryPoints enjoer_vk;

/* Set by vk_entry_load(); used to resolve the rest. */
static PFN_vkGetInstanceProcAddr get_instance_proc;
static PFN_vkGetDeviceProcAddr get_device_proc;
static void *loader;

int vk_entry_load(void) {
    memset(&enjoer_vk, 0, sizeof(enjoer_vk));
    loader = NULL;
    const char *wanted = getenv("ENJOER_VK_LIBRARY");
    const char *candidates[3] = { wanted, "libvulkan.so.1", "libvulkan.so" };
    for (int i = 0; i < 3; i++) {
        if (!candidates[i] || !candidates[i][0]) continue;
        loader = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
        if (loader) break;
    }
    if (!loader) return 0;
    get_instance_proc = (PFN_vkGetInstanceProcAddr)dlsym(loader, "vkGetInstanceProcAddr");
    if (!get_instance_proc) { dlclose(loader); loader = NULL; return 0; }
    /* vkCreateInstance and friends below the instance level resolve through the
     * loader's own vkGetInstanceProcAddr(NULL, ...) path, which is exactly what
     * the loader supports for global commands. */
    vk_entry_load_instance(VK_NULL_HANDLE);
    return 1;
}

void vk_entry_load_instance(VkInstance instance) {
    if (!get_instance_proc) return;
#define ENJOER_VK_RESOLVE(name) \
    enjoer_vk.name = (PFN_##name)get_instance_proc(instance, "vk" #name);
    ENJOER_VK_INSTANCE_ENTRIES(ENJOER_VK_RESOLVE)
#undef ENJOER_VK_RESOLVE
    get_device_proc = (PFN_vkGetDeviceProcAddr)get_instance_proc(instance, "vkGetDeviceProcAddr");
}

void vk_entry_load_device(VkDevice device) {
    if (!get_device_proc || !device) return;
#define ENJOER_VK_RESOLVE(name) \
    enjoer_vk.name = (PFN_##name)get_device_proc(device, "vk" #name);
    ENJOER_VK_DEVICE_ENTRIES(ENJOER_VK_RESOLVE)
#undef ENJOER_VK_RESOLVE
}
