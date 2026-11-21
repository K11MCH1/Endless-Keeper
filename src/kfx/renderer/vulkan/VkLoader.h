/******************************************************************************/
// Endless Keeper - Vulkan renderer
/******************************************************************************/
/** @file VkLoader.h
 *     Vulkan entry points, loaded at run time.
 * The game never links vulkan-1.dll / libvulkan.so: the loader is opened through SDL when the Vulkan renderer
 * starts, so the exe still starts on Windows 7 and on machines without Vulkan (it then falls back to OpenGL).
 */
/******************************************************************************/
#ifndef RENDERER_VULKAN_VKLOADER_H
#define RENDERER_VULKAN_VKLOADER_H

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

// Vulkan structs are filled the usual way, `T x = { VK_STRUCTURE_TYPE_... };` (the rest zeroed)
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

// X-macro lists: VKF(name) declares/loads PFN_name name.
#define KFX_VK_GLOBAL_FUNCS(VKF) \
    VKF(vkCreateInstance) \
    VKF(vkEnumerateInstanceExtensionProperties) \
    VKF(vkEnumerateInstanceLayerProperties)

#define KFX_VK_INSTANCE_FUNCS(VKF) \
    VKF(vkDestroyInstance) \
    VKF(vkEnumeratePhysicalDevices) \
    VKF(vkGetPhysicalDeviceProperties) \
    VKF(vkGetPhysicalDeviceFeatures) \
    VKF(vkGetPhysicalDeviceQueueFamilyProperties) \
    VKF(vkGetPhysicalDeviceMemoryProperties) \
    VKF(vkGetPhysicalDeviceFormatProperties) \
    VKF(vkEnumerateDeviceExtensionProperties) \
    VKF(vkCreateDevice) \
    VKF(vkGetDeviceProcAddr) \
    VKF(vkDestroySurfaceKHR) \
    VKF(vkGetPhysicalDeviceSurfaceSupportKHR) \
    VKF(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    VKF(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    VKF(vkGetPhysicalDeviceSurfacePresentModesKHR)

// Optional instance functions (may stay null).
#define KFX_VK_INSTANCE_OPT_FUNCS(VKF) \
    VKF(vkEnumerateInstanceVersion) \
    VKF(vkGetPhysicalDeviceFeatures2) \
    VKF(vkGetPhysicalDeviceFeatures2KHR) \
    VKF(vkCreateDebugUtilsMessengerEXT) \
    VKF(vkDestroyDebugUtilsMessengerEXT)

#define KFX_VK_DEVICE_FUNCS(VKF) \
    VKF(vkDestroyDevice) \
    VKF(vkGetDeviceQueue) \
    VKF(vkDeviceWaitIdle) \
    VKF(vkQueueSubmit) \
    VKF(vkQueueWaitIdle) \
    VKF(vkCreateCommandPool) \
    VKF(vkDestroyCommandPool) \
    VKF(vkResetCommandPool) \
    VKF(vkAllocateCommandBuffers) \
    VKF(vkFreeCommandBuffers) \
    VKF(vkBeginCommandBuffer) \
    VKF(vkEndCommandBuffer) \
    VKF(vkCreateFence) \
    VKF(vkDestroyFence) \
    VKF(vkWaitForFences) \
    VKF(vkResetFences) \
    VKF(vkGetFenceStatus) \
    VKF(vkCreateSemaphore) \
    VKF(vkDestroySemaphore) \
    VKF(vkCreateImage) \
    VKF(vkDestroyImage) \
    VKF(vkCreateImageView) \
    VKF(vkDestroyImageView) \
    VKF(vkGetImageMemoryRequirements) \
    VKF(vkBindImageMemory) \
    VKF(vkCreateBuffer) \
    VKF(vkDestroyBuffer) \
    VKF(vkGetBufferMemoryRequirements) \
    VKF(vkBindBufferMemory) \
    VKF(vkAllocateMemory) \
    VKF(vkFreeMemory) \
    VKF(vkMapMemory) \
    VKF(vkUnmapMemory) \
    VKF(vkFlushMappedMemoryRanges) \
    VKF(vkInvalidateMappedMemoryRanges) \
    VKF(vkCreateSampler) \
    VKF(vkDestroySampler) \
    VKF(vkCreateShaderModule) \
    VKF(vkDestroyShaderModule) \
    VKF(vkCreatePipelineLayout) \
    VKF(vkDestroyPipelineLayout) \
    VKF(vkCreateGraphicsPipelines) \
    VKF(vkDestroyPipeline) \
    VKF(vkCreatePipelineCache) \
    VKF(vkDestroyPipelineCache) \
    VKF(vkGetPipelineCacheData) \
    VKF(vkCreateDescriptorSetLayout) \
    VKF(vkDestroyDescriptorSetLayout) \
    VKF(vkCreateDescriptorPool) \
    VKF(vkDestroyDescriptorPool) \
    VKF(vkResetDescriptorPool) \
    VKF(vkAllocateDescriptorSets) \
    VKF(vkUpdateDescriptorSets) \
    VKF(vkCmdPipelineBarrier) \
    VKF(vkCmdCopyBuffer) \
    VKF(vkCmdCopyBufferToImage) \
    VKF(vkCmdCopyImageToBuffer) \
    VKF(vkCmdCopyImage) \
    VKF(vkCmdBlitImage) \
    VKF(vkCmdClearColorImage) \
    VKF(vkCmdClearDepthStencilImage) \
    VKF(vkCmdClearAttachments) \
    VKF(vkCmdBindPipeline) \
    VKF(vkCmdBindDescriptorSets) \
    VKF(vkCmdBindVertexBuffers) \
    VKF(vkCmdBindIndexBuffer) \
    VKF(vkCmdPushConstants) \
    VKF(vkCmdSetViewport) \
    VKF(vkCmdSetScissor) \
    VKF(vkCmdSetStencilReference) \
    VKF(vkCmdDraw) \
    VKF(vkCmdDrawIndexed)

// Swapchain functions: loaded only when VK_KHR_swapchain was enabled.
#define KFX_VK_SWAPCHAIN_FUNCS(VKF) \
    VKF(vkCreateSwapchainKHR) \
    VKF(vkDestroySwapchainKHR) \
    VKF(vkGetSwapchainImagesKHR) \
    VKF(vkAcquireNextImageKHR) \
    VKF(vkQueuePresentKHR)

#define KFX_VK_DECLARE(name) extern PFN_##name name;
extern "C" {
extern PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
KFX_VK_GLOBAL_FUNCS(KFX_VK_DECLARE)
KFX_VK_INSTANCE_FUNCS(KFX_VK_DECLARE)
KFX_VK_INSTANCE_OPT_FUNCS(KFX_VK_DECLARE)
KFX_VK_DEVICE_FUNCS(KFX_VK_DECLARE)
KFX_VK_SWAPCHAIN_FUNCS(KFX_VK_DECLARE)
// vkCmdBeginRendering / vkCmdEndRendering: the 1.3 core names or the KHR extension's, whichever the device has.
extern PFN_vkCmdBeginRenderingKHR kfx_vkCmdBeginRendering;
extern PFN_vkCmdEndRenderingKHR   kfx_vkCmdEndRendering;
}
#undef KFX_VK_DECLARE

/** Opens the Vulkan loader (SDL) and the global functions. False (with a reason) if there is none. */
bool VkLoader_LoadGlobal(const char** out_reason);
/** Loads the instance-level functions. */
bool VkLoader_LoadInstance(VkInstance instance);
/** Loads the device-level functions. */
bool VkLoader_LoadDevice(VkDevice device, bool core13, bool swapchain);
/** Clears the instance + device tables (the loader library stays open for SDL). */
void VkLoader_Reset(void);

#endif // RENDERER_VULKAN_VKLOADER_H
