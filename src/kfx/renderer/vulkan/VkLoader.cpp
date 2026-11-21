/******************************************************************************/
// Endless Keeper - Vulkan renderer
/******************************************************************************/
/** @file VkLoader.cpp
 *     Vulkan entry points, loaded at run time through SDL.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "kfx/renderer/vulkan/VkLoader.h"
#include "bflib_basics.h"
#include "globals.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include "post_inc.h"

#define KFX_VK_DEFINE(name) PFN_##name name = nullptr;
extern "C" {
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
KFX_VK_GLOBAL_FUNCS(KFX_VK_DEFINE)
KFX_VK_INSTANCE_FUNCS(KFX_VK_DEFINE)
KFX_VK_INSTANCE_OPT_FUNCS(KFX_VK_DEFINE)
KFX_VK_DEVICE_FUNCS(KFX_VK_DEFINE)
KFX_VK_SWAPCHAIN_FUNCS(KFX_VK_DEFINE)
PFN_vkCmdBeginRenderingKHR kfx_vkCmdBeginRendering = nullptr;
PFN_vkCmdEndRenderingKHR   kfx_vkCmdEndRendering = nullptr;
}
#undef KFX_VK_DEFINE

static bool s_library_loaded = false;

bool VkLoader_LoadGlobal(const char** out_reason)
{
    if (vkCreateInstance != nullptr)
        return true;
    if (!s_library_loaded)
    {
        // SDL has to be initialised for its video subsystem's loader; the window system did that already.
        if (!SDL_Vulkan_LoadLibrary(nullptr))
        {
            if (out_reason) *out_reason = "no Vulkan loader (vulkan-1.dll / libvulkan.so)";
            SYNCLOG("Vulkan: loader not available: %s", SDL_GetError());
            return false;
        }
        s_library_loaded = true;
    }
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (vkGetInstanceProcAddr == nullptr)
    {
        if (out_reason) *out_reason = "the Vulkan loader has no vkGetInstanceProcAddr";
        return false;
    }
#define KFX_VK_LOAD_GLOBAL(name) name = (PFN_##name)vkGetInstanceProcAddr(VK_NULL_HANDLE, #name);
    KFX_VK_GLOBAL_FUNCS(KFX_VK_LOAD_GLOBAL)
#undef KFX_VK_LOAD_GLOBAL
    vkEnumerateInstanceVersion = (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    if (vkCreateInstance == nullptr || vkEnumerateInstanceExtensionProperties == nullptr)
    {
        if (out_reason) *out_reason = "the Vulkan loader is missing core functions";
        return false;
    }
    return true;
}

bool VkLoader_LoadInstance(VkInstance instance)
{
    bool ok = true;
#define KFX_VK_LOAD_INST(name) \
    name = (PFN_##name)vkGetInstanceProcAddr(instance, #name); \
    if (name == nullptr) { WARNLOG("Vulkan: instance function %s missing", #name); ok = false; }
    KFX_VK_INSTANCE_FUNCS(KFX_VK_LOAD_INST)
#undef KFX_VK_LOAD_INST
#define KFX_VK_LOAD_INST_OPT(name) name = (PFN_##name)vkGetInstanceProcAddr(instance, #name);
    KFX_VK_INSTANCE_OPT_FUNCS(KFX_VK_LOAD_INST_OPT)
#undef KFX_VK_LOAD_INST_OPT
    if (vkEnumerateInstanceVersion == nullptr)
        vkEnumerateInstanceVersion = (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    return ok;
}

bool VkLoader_LoadDevice(VkDevice device, bool core13, bool swapchain)
{
    bool ok = true;
#define KFX_VK_LOAD_DEV(name) \
    name = (PFN_##name)vkGetDeviceProcAddr(device, #name); \
    if (name == nullptr) { WARNLOG("Vulkan: device function %s missing", #name); ok = false; }
    KFX_VK_DEVICE_FUNCS(KFX_VK_LOAD_DEV)
    if (swapchain)
    {
        KFX_VK_SWAPCHAIN_FUNCS(KFX_VK_LOAD_DEV)
    }
#undef KFX_VK_LOAD_DEV
    if (core13)
    {
        kfx_vkCmdBeginRendering = (PFN_vkCmdBeginRenderingKHR)vkGetDeviceProcAddr(device, "vkCmdBeginRendering");
        kfx_vkCmdEndRendering = (PFN_vkCmdEndRenderingKHR)vkGetDeviceProcAddr(device, "vkCmdEndRendering");
    }
    if (kfx_vkCmdBeginRendering == nullptr || kfx_vkCmdEndRendering == nullptr)
    {
        kfx_vkCmdBeginRendering = (PFN_vkCmdBeginRenderingKHR)vkGetDeviceProcAddr(device, "vkCmdBeginRenderingKHR");
        kfx_vkCmdEndRendering = (PFN_vkCmdEndRenderingKHR)vkGetDeviceProcAddr(device, "vkCmdEndRenderingKHR");
    }
    if (kfx_vkCmdBeginRendering == nullptr || kfx_vkCmdEndRendering == nullptr)
    {
        WARNLOG("Vulkan: no vkCmdBeginRendering");
        ok = false;
    }
    return ok;
}

void VkLoader_Reset(void)
{
#define KFX_VK_CLEAR(name) name = nullptr;
    KFX_VK_INSTANCE_FUNCS(KFX_VK_CLEAR)
    KFX_VK_INSTANCE_OPT_FUNCS(KFX_VK_CLEAR)
    KFX_VK_DEVICE_FUNCS(KFX_VK_CLEAR)
    KFX_VK_SWAPCHAIN_FUNCS(KFX_VK_CLEAR)
#undef KFX_VK_CLEAR
    kfx_vkCmdBeginRendering = nullptr;
    kfx_vkCmdEndRendering = nullptr;
    // a global function: the next instance (the renderer's, after the probe's) needs it before it exists
    if (vkGetInstanceProcAddr != nullptr)
        vkEnumerateInstanceVersion = (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
}
