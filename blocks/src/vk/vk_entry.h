/* src/vk/vk_entry.h — the Vulkan entry points, resolved at runtime.
 *
 * The renderer never links against the Vulkan loader: libvulkan is an optional
 * system component (Android ships it, desktop machines often do not), so it is
 * opened with dlopen() and the ~80 functions used below are fetched through
 * vkGetInstanceProcAddr/vkGetDeviceProcAddr on first use. A machine without a
 * loader therefore still builds the game — the Vulkan backend simply reports
 * why it cannot start and the software renderer takes over.
 *
 * The X-macro lists feed three things: the function-pointer table, the aliases
 * that module-private code expands the standard `vk*` names into, and the
 * resolver in vk_entry.c. Add a function to exactly one list. */
#ifndef ENJOER_VK_ENTRY_H
#define ENJOER_VK_ENTRY_H

#if defined(__ANDROID__)
#define VK_USE_PLATFORM_ANDROID_KHR 1
#define ENJOER_VK_ANDROID 1
#endif

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES 1
#endif
#include <vulkan/vulkan.h>

/* Instance level: filled right after VkInstance exists. */
#define ENJOER_VK_INSTANCE_ENTRIES(X) \
    X(vkGetInstanceProcAddr) X(vkCreateInstance) X(vkDestroyInstance) \
    X(vkEnumerateInstanceLayerProperties) \
    X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceFormatProperties) X(vkCreateDevice) \
    X(vkGetDeviceProcAddr) X(vkDestroySurfaceKHR) X(vkDestroySwapchainKHR)

/* Device level: filled right after VkDevice exists. The two surface/swapchain
 * entries are extensions and may stay NULL when they are not enabled. */
#define ENJOER_VK_DEVICE_ENTRIES(X) \
    X(vkAllocateCommandBuffers) X(vkAllocateDescriptorSets) X(vkAllocateMemory) \
    X(vkBeginCommandBuffer) X(vkBindBufferMemory) X(vkBindImageMemory) \
    X(vkCmdBeginRenderPass) X(vkCmdBindDescriptorSets) X(vkCmdBindIndexBuffer) \
    X(vkCmdBindPipeline) X(vkCmdBindVertexBuffers) X(vkCmdClearAttachments) \
    X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) \
    X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdEndRenderPass) \
    X(vkCmdPipelineBarrier) X(vkCmdPushConstants) X(vkCmdSetScissor) \
    X(vkCmdSetViewport) X(vkCreateBuffer) X(vkCreateCommandPool) \
    X(vkCreateDescriptorPool) X(vkCreateDescriptorSetLayout) X(vkCreateFence) \
    X(vkCreateFramebuffer) X(vkCreateGraphicsPipelines) X(vkCreateImage) \
    X(vkCreateImageView) X(vkCreatePipelineLayout) X(vkCreateRenderPass) \
    X(vkCreateSampler) X(vkCreateSemaphore) X(vkCreateShaderModule) \
    X(vkDestroyBuffer) X(vkDestroyCommandPool) X(vkDestroyDescriptorPool) \
    X(vkDestroyDescriptorSetLayout) X(vkDestroyDevice) X(vkDestroyFence) \
    X(vkDestroyFramebuffer) X(vkDestroyImage) X(vkDestroyImageView) \
    X(vkDestroyPipeline) X(vkDestroyPipelineLayout) X(vkDestroyRenderPass) \
    X(vkDestroySampler) X(vkDestroySemaphore) X(vkDestroyShaderModule) \
    X(vkDeviceWaitIdle) X(vkEndCommandBuffer) X(vkFreeCommandBuffers) \
    X(vkFreeMemory) X(vkGetBufferMemoryRequirements) X(vkGetDeviceQueue) \
    X(vkGetImageMemoryRequirements) X(vkMapMemory) X(vkQueueSubmit) \
    X(vkQueueWaitIdle) X(vkResetCommandBuffer) X(vkResetFences) \
    X(vkUnmapMemory) X(vkUpdateDescriptorSets) X(vkWaitForFences)

#define ENJOER_VK_DECLARE(name) PFN_##name name;
typedef struct {
    ENJOER_VK_INSTANCE_ENTRIES(ENJOER_VK_DECLARE)
    ENJOER_VK_DEVICE_ENTRIES(ENJOER_VK_DECLARE)
} EnjoerVkEntryPoints;
extern EnjoerVkEntryPoints enjoer_vk;

/* Every call site keeps using the standard names. */

/* vkGetInstanceProcAddr itself is the one function the loader must export. */
typedef PFN_vkGetInstanceProcAddr EnjoerVkGetInstanceProcAddr;

/* Opens libvulkan (or $ENJOER_VK_LIBRARY) and wires the instance entries.
 * Returns 0 and leaves a message in *error when no loader is present. */
int  vk_entry_load(void);
void vk_entry_load_instance(VkInstance instance);
void vk_entry_load_device(VkDevice device);

#endif
