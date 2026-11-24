/******************************************************************************/
// Endless Keeper - Vulkan renderer
/******************************************************************************/
/** @file VkCore.cpp
 *     Vulkan device, frames in flight, uploads, render passes, swapchain and readback.
 * @par Coordinates
 *     Images are stored the way OpenGL stores them: row 0 is the BOTTOM of the picture. Viewports and scissors
 *     take OpenGL's window coordinates unchanged (positive height), so NDC y=-1 lands in row 0 as in OpenGL,
 *     gl_FragCoord has the same values, and every pass that samples a captured image reads it as OpenGL does.
 *     Only the swapchain blit and the readbacks turn the picture over (as OpenGL's blit and glReadPixels do).
 */
/******************************************************************************/
#include "pre_inc.h"
#include "kfx/renderer/vulkan/VkCore.h"
#include "bflib_basics.h"
#include "globals.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include "kfx/renderer/vulkan/VkPrerotate_gen.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include "post_inc.h"

static inline uint64_t vk_now_ns()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static const VkDeviceSize kRingSize = 8u * 1024u * 1024u;

/******************************************************************************/
/* instance + device selection (shared by the probe and Init)                 */
/******************************************************************************/

static bool env_on(const char* name)
{
    const char* v = SDL_getenv(name);
    return v != nullptr && v[0] != '\0' && v[0] != '0';
}

static VKAPI_ATTR VkBool32 VKAPI_CALL vk_debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/, const VkDebugUtilsMessengerCallbackDataEXT* data, void* /*user*/)
{
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        ERRORLOG("[VK] %s", data->pMessage);
    else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        WARNLOG("[VK] %s", data->pMessage);
    return VK_FALSE;
}

static bool has_ext(const std::vector<VkExtensionProperties>& list, const char* name)
{
    for (const VkExtensionProperties& e : list)
        if (strcmp(e.extensionName, name) == 0)
            return true;
    return false;
}

static bool create_instance(bool validation, VkInstance& out, std::string& reason, bool* out_debug_utils)
{
    const char* why = nullptr;
    if (!VkLoader_LoadGlobal(&why))
    {
        reason = why ? why : "no Vulkan loader";
        return false;
    }
    uint32_t inst_version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion != nullptr)
        vkEnumerateInstanceVersion(&inst_version);
    if (inst_version < VK_API_VERSION_1_1)
    {
        reason = "the Vulkan loader is only version 1.0";
        return false;
    }

    Uint32 sdl_count = 0;
    char const* const* sdl_exts = SDL_Vulkan_GetInstanceExtensions(&sdl_count);
    if (sdl_exts == nullptr)
    {
        reason = std::string("SDL has no Vulkan surface support: ") + SDL_GetError();
        return false;
    }
    uint32_t avail_count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &avail_count, nullptr);
    std::vector<VkExtensionProperties> avail(avail_count);
    vkEnumerateInstanceExtensionProperties(nullptr, &avail_count, avail.data());

    std::vector<const char*> exts;
    for (Uint32 i = 0; i < sdl_count; i++)
    {
        if (!has_ext(avail, sdl_exts[i]))
        {
            reason = std::string("instance extension missing: ") + sdl_exts[i];
            return false;
        }
        exts.push_back(sdl_exts[i]);
    }
    std::vector<const char*> layers;
    bool debug_utils = false;
    if (validation)
    {
        uint32_t lc = 0;
        vkEnumerateInstanceLayerProperties(&lc, nullptr);
        std::vector<VkLayerProperties> lp(lc);
        vkEnumerateInstanceLayerProperties(&lc, lp.data());
        for (const VkLayerProperties& l : lp)
            if (strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0)
                layers.push_back("VK_LAYER_KHRONOS_validation");
        if (layers.empty())
            WARNLOG("Vulkan: KFX_VK_VALIDATION set but the validation layer is not installed");
        if (has_ext(avail, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
        {
            exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            debug_utils = true;
        }
    }

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "Endless Keeper";
    app.applicationVersion = 1;
    app.pEngineName = "KeeperFX";
    app.engineVersion = 1;
    app.apiVersion = (inst_version >= VK_API_VERSION_1_3) ? VK_API_VERSION_1_3 : VK_API_VERSION_1_1;

    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = (uint32_t)exts.size();
    ci.ppEnabledExtensionNames = exts.data();
    ci.enabledLayerCount = (uint32_t)layers.size();
    ci.ppEnabledLayerNames = layers.data();
    VkResult r = vkCreateInstance(&ci, nullptr, &out);
    if (r != VK_SUCCESS)
    {
        reason = "vkCreateInstance failed (" + std::to_string((int)r) + ")";
        return false;
    }
    if (!VkLoader_LoadInstance(out))
    {
        reason = "the Vulkan instance lacks functions";
        PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)vkGetInstanceProcAddr(out, "vkDestroyInstance");
        if (destroy) destroy(out, nullptr);
        out = VK_NULL_HANDLE;
        return false;
    }
    if (out_debug_utils) *out_debug_utils = debug_utils;
    return true;
}

struct VkPick {
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    uint32_t qfi = 0;
    uint32_t api = 0;
    bool core13 = false;
    std::vector<const char*> exts;
    VkFormat depth = VK_FORMAT_UNDEFINED;
    std::string name;
};

static bool format_has(VkPhysicalDevice pd, VkFormat f, VkFormatFeatureFlags want)
{
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(pd, f, &fp);
    return (fp.optimalTilingFeatures & want) == want;
}

static bool device_suitable(VkInstance inst, VkPhysicalDevice pd, VkPick& out, std::string& why)
{
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(pd, &p);
    out.name = p.deviceName;
    out.api = p.apiVersion;
    if (p.apiVersion < VK_API_VERSION_1_1) { why = "Vulkan 1.0 only"; return false; }
    if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU && !env_on("KFX_VK_ALLOW_CPU")) { why = "a CPU (software) device"; return false; }

    uint32_t qc = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qc, nullptr);
    std::vector<VkQueueFamilyProperties> qp(qc);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qc, qp.data());
    bool found = false;
    for (uint32_t i = 0; i < qc; i++)
    {
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && SDL_Vulkan_GetPresentationSupport(inst, pd, i))
        {
            out.qfi = i;
            found = true;
            break;
        }
    }
    if (!found) { why = "no graphics queue that can present"; return false; }

    uint32_t ec = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &ec, nullptr);
    std::vector<VkExtensionProperties> ep(ec);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &ec, ep.data());
    if (!has_ext(ep, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) { why = "no swapchain extension"; return false; }
    out.exts.clear();
    out.exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    out.core13 = p.apiVersion >= VK_API_VERSION_1_3;
    if (!out.core13)
    {
        if (!has_ext(ep, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME)) { why = "no dynamic rendering"; return false; }
        out.exts.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
        if (p.apiVersion < VK_API_VERSION_1_2)
        {
            if (!has_ext(ep, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME) || !has_ext(ep, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME))
            { why = "no depth/stencil resolve"; return false; }
            out.exts.push_back(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);
            out.exts.push_back(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
        }
    }
    PFN_vkGetPhysicalDeviceFeatures2 gpdf2 = vkGetPhysicalDeviceFeatures2 ? vkGetPhysicalDeviceFeatures2 : vkGetPhysicalDeviceFeatures2KHR;
    if (gpdf2 == nullptr) { why = "no vkGetPhysicalDeviceFeatures2"; return false; }
    VkPhysicalDeviceDynamicRenderingFeaturesKHR dr = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR };
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    f2.pNext = &dr;
    gpdf2(pd, &f2);
    if (!dr.dynamicRendering) { why = "dynamic rendering not supported"; return false; }

    const VkFormatFeatureFlags ds = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (format_has(pd, VK_FORMAT_D24_UNORM_S8_UINT, ds)) out.depth = VK_FORMAT_D24_UNORM_S8_UINT;
    else if (format_has(pd, VK_FORMAT_D32_SFLOAT_S8_UINT, ds)) out.depth = VK_FORMAT_D32_SFLOAT_S8_UINT;
    else { why = "no depth/stencil format"; return false; }
    const VkFormatFeatureFlags rt = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT
                                  | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT;
    if (!format_has(pd, VK_FORMAT_R8G8B8A8_UNORM, rt)) { why = "RGBA8 render targets not supported"; return false; }
    if (!format_has(pd, VK_FORMAT_R8_UNORM, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) { why = "R8 textures not supported"; return false; }
    return true;
}

static int device_score(VkPhysicalDevice pd)
{
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(pd, &p);
    switch (p.deviceType)
    {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 1000;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 500;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 100;
    default:                                     return 1;
    }
}

static bool pick_device(VkInstance inst, VkPick& out, std::string& reason)
{
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    if (n == 0) { reason = "no Vulkan device"; return false; }
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(inst, &n, pds.data());
    const char* force = SDL_getenv("KFX_VK_DEVICE");
    int best = -1;
    VkPick best_pick;
    std::string whys;
    for (uint32_t i = 0; i < n; i++)
    {
        if (force != nullptr && force[0] != '\0' && atoi(force) != (int)i)
            continue;
        VkPick pick;
        std::string why;
        if (!device_suitable(inst, pds[i], pick, why))
        {
            SYNCLOG("Vulkan: device %u (%s) not usable: %s", i, pick.name.c_str(), why.c_str());
            if (!whys.empty()) whys += "; ";
            whys += pick.name + ": " + why;
            continue;
        }
        const int score = device_score(pds[i]);
        if (score > best)
        {
            best = score;
            pick.pd = pds[i];
            best_pick = pick;
        }
    }
    if (best < 0)
    {
        reason = whys.empty() ? "no usable Vulkan device" : whys;
        return false;
    }
    out = best_pick;
    return true;
}

bool VkCore_Probe(std::string& reason, std::string& device)
{
    if (env_on("KFX_VK_DISABLE"))
    {
        reason = "turned off by KFX_VK_DISABLE";
        return false;
    }
    VkInstance inst = VK_NULL_HANDLE;
    if (!create_instance(false, inst, reason, nullptr))
        return false;
    VkPick pick;
    const bool ok = pick_device(inst, pick, reason);
    if (ok)
        device = pick.name;
    vkDestroyInstance(inst, nullptr);
    VkLoader_Reset();
    return ok;
}

/******************************************************************************/
/* barriers                                                                   */
/******************************************************************************/

static void layout_stage_access(VkImageLayout layout, VkPipelineStageFlags& stage, VkAccessFlags& access)
{
    switch (layout)
    {
    case VK_IMAGE_LAYOUT_UNDEFINED:
        stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT; access = 0; break;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; break;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT; break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        stage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        access = VK_ACCESS_SHADER_READ_BIT; break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        stage = VK_PIPELINE_STAGE_TRANSFER_BIT; access = VK_ACCESS_TRANSFER_READ_BIT; break;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        stage = VK_PIPELINE_STAGE_TRANSFER_BIT; access = VK_ACCESS_TRANSFER_WRITE_BIT; break;
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
        stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT; access = 0; break;
    default:
        stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT; access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT; break;
    }
}

void VkCore_ImageBarrier(VkCommandBuffer cmd, VkImageObj& img, VkImageLayout from, VkImageLayout to,
                         VkPipelineStageFlags src_stage, VkAccessFlags src_access,
                         VkPipelineStageFlags dst_stage, VkAccessFlags dst_access)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange.aspectMask = img.aspect;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = (uint32_t)img.layers;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

static void barrier_auto(VkCommandBuffer cmd, VkImageObj& img, VkImageLayout from, VkImageLayout to)
{
    VkPipelineStageFlags ss, ds;
    VkAccessFlags sa, da;
    layout_stage_access(from, ss, sa);
    layout_stage_access(to, ds, da);
    VkCore_ImageBarrier(cmd, img, from, to, ss, sa, ds, da);
}

/******************************************************************************/
/* init / shutdown                                                            */
/******************************************************************************/

bool VkCore::Init(SDL_Window* window, bool want_present, std::string& reason)
{
    bool debug_utils = false;
    if (!create_instance(env_on("KFX_VK_VALIDATION"), instance, reason, &debug_utils))
        return false;
    if (debug_utils && vkCreateDebugUtilsMessengerEXT != nullptr)
    {
        VkDebugUtilsMessengerCreateInfoEXT mci = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                        | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mci.pfnUserCallback = vk_debug_callback;
        vkCreateDebugUtilsMessengerEXT(instance, &mci, nullptr, &m_messenger);
    }
    VkPick pick;
    if (!pick_device(instance, pick, reason))
        return false;
    phys = pick.pd;
    queue_family = pick.qfi;
    depth_format = pick.depth;
    device_name = pick.name;
    vkGetPhysicalDeviceProperties(phys, &props);
    vkGetPhysicalDeviceMemoryProperties(phys, &memprops);

    if (env_on("KFX_VK_FAIL_LATE"))
    {
        reason = "late failure asked for by KFX_VK_FAIL_LATE";
        return false;
    }

    m_window = window;
    if (want_present && window != nullptr)
    {
        if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &m_surface))
        {
            reason = std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError();
            m_surface = VK_NULL_HANDLE;
            return false;
        }
        VkBool32 supported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(phys, queue_family, m_surface, &supported);
        if (!supported)
        {
            reason = "the GPU cannot present to this window";
            return false;
        }
#ifdef __ANDROID__
        m_native_window = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
#endif
    }

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkPhysicalDeviceDynamicRenderingFeaturesKHR dr = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR };
    dr.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    f2.pNext = &dr;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)pick.exts.size();
    dci.ppEnabledExtensionNames = pick.exts.data();
    VkResult r = vkCreateDevice(phys, &dci, nullptr, &device);
    if (r != VK_SUCCESS)
    {
        reason = "vkCreateDevice failed (" + std::to_string((int)r) + ")";
        device = VK_NULL_HANDLE;
        return false;
    }
    m_swapchain_ext = true;
    if (!VkLoader_LoadDevice(device, pick.core13, m_swapchain_ext))
    {
        reason = "the Vulkan device lacks functions";
        return false;
    }
    vkGetDeviceQueue(device, queue_family, 0, &queue);

    VkPipelineCacheCreateInfo pcci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    vkCreatePipelineCache(device, &pcci, nullptr, &pipeline_cache);

    for (Frame& f : m_frames)
    {
        VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        cpci.queueFamilyIndex = queue_family;
        cpci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        if (vkCreateCommandPool(device, &cpci, nullptr, &f.pool) != VK_SUCCESS)
        { reason = "vkCreateCommandPool failed"; return false; }
        VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cbai.commandPool = f.pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 2;
        VkCommandBuffer cbs[2];
        if (vkAllocateCommandBuffers(device, &cbai, cbs) != VK_SUCCESS)
        { reason = "vkAllocateCommandBuffers failed"; return false; }
        f.upload_cmd = cbs[0];
        f.main_cmd = cbs[1];
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        vkCreateFence(device, &fci, nullptr, &f.fence);
        VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        vkCreateSemaphore(device, &sci, nullptr, &f.acquired);
        if (!CreateBuffer(f.ring, kRingSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                          | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true))
        { reason = "ring buffer allocation failed"; return false; }
    }
    VkCommandPoolCreateInfo ipci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    ipci.queueFamilyIndex = queue_family;
    ipci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCreateCommandPool(device, &ipci, nullptr, &m_init_pool);
    VkCommandBufferAllocateInfo icbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    icbai.commandPool = m_init_pool;
    icbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    icbai.commandBufferCount = 1;
    vkAllocateCommandBuffers(device, &icbai, &m_init_cmd);

    SYNCLOG("Vulkan: device '%s', API %u.%u.%u, driver 0x%x, depth format %d", props.deviceName,
            VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
            VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion, (int)depth_format);
    return true;
}

void VkCore::Shutdown()
{
    if (device != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(device);
        if (m_init_open)
        {
            vkEndCommandBuffer(m_init_cmd);
            m_init_open = false;
        }
        for (VkBufferObj& b : m_init_temp) DestroyBuffer(b);
        m_init_temp.clear();
        for (auto& fn : m_pending_init_garbage) fn();
        m_pending_init_garbage.clear();
        DestroySwapchain();
        DestroyRotatePipeline();
        for (Frame& f : m_frames)
        {
            RunGarbage(f);
            for (VkDescriptorPool p : f.desc_pools) vkDestroyDescriptorPool(device, p, nullptr);
            f.desc_pools.clear();
            DestroyBuffer(f.ring);
            if (f.fence) vkDestroyFence(device, f.fence, nullptr);
            if (f.acquired) vkDestroySemaphore(device, f.acquired, nullptr);
            if (f.pool) vkDestroyCommandPool(device, f.pool, nullptr);
            f = Frame();
        }
        if (m_init_pool) vkDestroyCommandPool(device, m_init_pool, nullptr);
        m_init_pool = VK_NULL_HANDLE;
        for (auto& kv : m_samplers) vkDestroySampler(device, kv.second, nullptr);
        m_samplers.clear();
        if (pipeline_cache) vkDestroyPipelineCache(device, pipeline_cache, nullptr);
        pipeline_cache = VK_NULL_HANDLE;
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }
    if (instance != VK_NULL_HANDLE)
    {
        if (m_surface != VK_NULL_HANDLE)
            vkDestroySurfaceKHR(instance, m_surface, nullptr);
        m_surface = VK_NULL_HANDLE;
        if (m_messenger != VK_NULL_HANDLE && vkDestroyDebugUtilsMessengerEXT != nullptr)
            vkDestroyDebugUtilsMessengerEXT(instance, m_messenger, nullptr);
        m_messenger = VK_NULL_HANDLE;
        vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
    }
    VkLoader_Reset();
}

/******************************************************************************/
/* objects                                                                    */
/******************************************************************************/

uint32_t VkCore::FindMemoryType(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid)
{
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want
            && (memprops.memoryTypes[i].propertyFlags & avoid) == 0)
            return i;
    if (avoid != 0)
        return FindMemoryType(bits, want, 0);
    return UINT32_MAX;
}

static VkImageAspectFlags format_aspect(VkFormat f)
{
    switch (f)
    {
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
    case VK_FORMAT_D16_UNORM_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D16_UNORM:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    default:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

bool VkCore::CreateImage(VkImageObj& out, int32_t w, int32_t h, int32_t layers, VkFormat format,
                         VkImageUsageFlags usage, const char* name, bool force_array)
{
    out = VkImageObj();
    if (w <= 0 || h <= 0 || layers <= 0)
        return false;
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = { (uint32_t)w, (uint32_t)h, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = (uint32_t)layers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r = vkCreateImage(device, &ici, nullptr, &out.image);
    if (r != VK_SUCCESS)
    {
        ERRORLOG("Vulkan: vkCreateImage '%s' %dx%dx%d format %d failed (%d)", name, w, h, layers, (int)format, (int)r);
        out.image = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(device, out.image, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device, &mai, nullptr, &out.memory) != VK_SUCCESS)
    {
        ERRORLOG("Vulkan: no memory for image '%s' (%u bytes)", name, (unsigned)mr.size);
        vkDestroyImage(device, out.image, nullptr);
        out = VkImageObj();
        return false;
    }
    vkBindImageMemory(device, out.image, out.memory, 0);
    out.format = format;
    out.aspect = format_aspect(format);
    out.width = w;
    out.height = h;
    out.layers = layers;
    out.is_array = force_array || layers > 1;
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = out.image;
    vci.viewType = out.is_array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange.aspectMask = (out.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? (VkImageAspectFlags)VK_IMAGE_ASPECT_DEPTH_BIT : out.aspect;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = (uint32_t)layers;
    if (vkCreateImageView(device, &vci, nullptr, &out.view) != VK_SUCCESS)
    {
        ERRORLOG("Vulkan: vkCreateImageView '%s' failed", name);
        vkDestroyImage(device, out.image, nullptr);
        vkFreeMemory(device, out.memory, nullptr);
        out = VkImageObj();
        return false;
    }
    return true;
}

void VkCore::DestroyImage(VkImageObj& img)
{
    if (img.image == VK_NULL_HANDLE)
        return;
    VkImage image = img.image;
    VkImageView view = img.view;
    VkDeviceMemory mem = img.memory;
    VkDevice dev = device;
    Defer([dev, image, view, mem]() {
        if (view) vkDestroyImageView(dev, view, nullptr);
        vkDestroyImage(dev, image, nullptr);
        if (mem) vkFreeMemory(dev, mem, nullptr);
    });
    img = VkImageObj();
}

bool VkCore::CreateBuffer(VkBufferObj& out, VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible)
{
    out = VkBufferObj();
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &bci, nullptr, &out.buffer) != VK_SUCCESS)
        return false;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(device, out.buffer, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    if (host_visible)
    {
        // readbacks want cached memory; uploads want coherent
        const bool readback = (usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) != 0;
        mai.memoryTypeIndex = readback
            ? FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
            : UINT32_MAX;
        if (mai.memoryTypeIndex == UINT32_MAX)
            mai.memoryTypeIndex = FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    else
        mai.memoryTypeIndex = FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device, &mai, nullptr, &out.memory) != VK_SUCCESS)
    {
        vkDestroyBuffer(device, out.buffer, nullptr);
        out = VkBufferObj();
        return false;
    }
    vkBindBufferMemory(device, out.buffer, out.memory, 0);
    out.size = size;
    if (host_visible && vkMapMemory(device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped) != VK_SUCCESS)
    {
        DestroyBuffer(out);
        return false;
    }
    return true;
}

void VkCore::DestroyBuffer(VkBufferObj& buf)
{
    if (buf.buffer == VK_NULL_HANDLE)
        return;
    if (buf.mapped) vkUnmapMemory(device, buf.memory);
    vkDestroyBuffer(device, buf.buffer, nullptr);
    vkFreeMemory(device, buf.memory, nullptr);
    buf = VkBufferObj();
}

VkSampler VkCore::GetSampler(bool linear, bool repeat)
{
    const uint32_t key = (linear ? 1u : 0u) | (repeat ? 2u : 0u);
    auto it = m_samplers.find(key);
    if (it != m_samplers.end())
        return it->second;
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.minFilter = sci.magFilter;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    const VkSamplerAddressMode mode = repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = mode;
    sci.maxLod = 0.0f;
    VkSampler s = VK_NULL_HANDLE;
    vkCreateSampler(device, &sci, nullptr, &s);
    m_samplers[key] = s;
    return s;
}

VkShaderModule VkCore::CreateShaderModule(const uint32_t* code, size_t size)
{
    VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smci.codeSize = size;
    smci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &smci, nullptr, &m) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return m;
}

VkDescriptorSetLayout VkCore::CreateSetLayout(const std::vector<VkDescriptorSetLayoutBinding>& bindings)
{
    VkDescriptorSetLayoutCreateInfo ci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    ci.bindingCount = (uint32_t)bindings.size();
    ci.pBindings = bindings.data();
    VkDescriptorSetLayout l = VK_NULL_HANDLE;
    vkCreateDescriptorSetLayout(device, &ci, nullptr, &l);
    return l;
}

VkPipeline VkCore::CreatePipeline(const VkPipelineDesc& d)
{
    VkShaderModule vs = CreateShaderModule(d.vs_code, d.vs_size);
    VkShaderModule fs = CreateShaderModule(d.fs_code, d.fs_size);
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE)
    {
        ERRORLOG("Vulkan: shader module failed for pipeline '%s'", d.debug_name);
        if (vs) vkDestroyShaderModule(device, vs, nullptr);
        if (fs) vkDestroyShaderModule(device, fs, nullptr);
        return VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount = (uint32_t)d.bindings.size();
    vi.pVertexBindingDescriptions = d.bindings.data();
    vi.vertexAttributeDescriptionCount = (uint32_t)d.attributes.size();
    vi.pVertexAttributeDescriptions = d.attributes.data();
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = d.topology;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = d.depth_test ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = d.depth_write ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = d.depth_test ? d.depth_op : VK_COMPARE_OP_ALWAYS;
    ds.stencilTestEnable = d.stencil_test ? VK_TRUE : VK_FALSE;
    ds.front = d.stencil_front;
    ds.back = d.stencil_back;
    ds.maxDepthBounds = 1.0f;
    VkPipelineColorBlendAttachmentState cba = {};
    cba.blendEnable = d.blend ? VK_TRUE : VK_FALSE;
    cba.srcColorBlendFactor = d.src_color;
    cba.dstColorBlendFactor = d.dst_color;
    cba.colorBlendOp = d.color_op;
    cba.srcAlphaBlendFactor = d.src_alpha;
    cba.dstAlphaBlendFactor = d.dst_alpha;
    cba.alphaBlendOp = d.alpha_op;
    cba.colorWriteMask = d.write_mask;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 3;
    dy.pDynamicStates = dyn;

    VkPipelineRenderingCreateInfoKHR rci = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &d.color_format;
    rci.depthAttachmentFormat = d.has_depth ? depth_format : VK_FORMAT_UNDEFINED;
    rci.stencilAttachmentFormat = d.has_depth ? depth_format : VK_FORMAT_UNDEFINED;

    VkGraphicsPipelineCreateInfo gci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gci.pNext = &rci;
    gci.stageCount = 2;
    gci.pStages = stages;
    gci.pVertexInputState = &vi;
    gci.pInputAssemblyState = &ia;
    gci.pViewportState = &vp;
    gci.pRasterizationState = &rs;
    gci.pMultisampleState = &ms;
    gci.pDepthStencilState = &ds;
    gci.pColorBlendState = &cb;
    gci.pDynamicState = &dy;
    gci.layout = d.layout;
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(device, pipeline_cache, 1, &gci, nullptr, &p);
    vkDestroyShaderModule(device, vs, nullptr);
    vkDestroyShaderModule(device, fs, nullptr);
    if (r != VK_SUCCESS)
    {
        ERRORLOG("Vulkan: pipeline '%s' failed (%d)", d.debug_name, (int)r);
        return VK_NULL_HANDLE;
    }
    return p;
}

void VkCore::Defer(std::function<void()> fn)
{
    if (m_in_frame)
        m_frames[m_frame].garbage.push_back(std::move(fn));
    else
    {
        // between frames: the last submitted frame may still use it, so it waits for that slot's next turn
        const int last = (m_frame + kFramesInFlight - 1) % kFramesInFlight;
        if (m_frames[last].submitted)
            m_frames[last].garbage.push_back(std::move(fn));
        else
            m_pending_init_garbage.push_back(std::move(fn));
    }
}

void VkCore::RunGarbage(Frame& f)
{
    for (auto& fn : f.garbage) fn();
    f.garbage.clear();
    for (VkBufferObj& b : f.temp_buffers) DestroyBuffer(b);
    f.temp_buffers.clear();
}

/******************************************************************************/
/* frames                                                                     */
/******************************************************************************/

VkCommandBuffer VkCore::UploadCmd()
{
    if (m_in_frame)
    {
        m_frames[m_frame].upload_used = true;
        return m_frames[m_frame].upload_cmd;
    }
    if (!m_init_open)
    {
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(m_init_cmd, &bi);
        m_init_open = true;
    }
    return m_init_cmd;
}

void VkCore::FlushInit()
{
    if (!m_init_open)
        return;
    vkEndCommandBuffer(m_init_cmd);
    m_init_open = false;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &m_init_cmd;
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    for (VkBufferObj& b : m_init_temp) DestroyBuffer(b);
    m_init_temp.clear();
}

bool VkCore::BeginFrame()
{
    if (device_lost)
        return false;
    FlushInit();
    Frame& f = m_frames[m_frame];
    if (f.submitted)
    {
#ifdef __ANDROID__
        const uint64_t t0 = vk_now_ns();
#endif
        VkResult r = vkWaitForFences(device, 1, &f.fence, VK_TRUE, UINT64_MAX);
#ifdef __ANDROID__
        {
            const uint64_t d = vk_now_ns() - t0;
            wait_fence_ns += d;
            wait_total_ns += d;
        }
#endif
        if (r == VK_ERROR_DEVICE_LOST)
        {
            ERRORLOG("Vulkan: device lost (last pass '%s')", last_pass);
            device_lost = true;
            return false;
        }
        for (auto& fn : f.after_done) fn();
        f.after_done.clear();
        f.submitted = false;
    }
    vkResetFences(device, 1, &f.fence);
    RunGarbage(f);
    if (!m_pending_init_garbage.empty())
    {
        // made before any frame: nothing in flight can use it once this slot's fence was waited
        for (auto& fn : m_pending_init_garbage) fn();
        m_pending_init_garbage.clear();
    }
    vkResetCommandPool(device, f.pool, 0);
    for (VkDescriptorPool p : f.desc_pools)
        vkResetDescriptorPool(device, p, 0);
    f.desc_pool_cur = 0;
    f.ring_used = 0;
    f.upload_used = false;

    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.upload_cmd, &bi);
    vkBeginCommandBuffer(f.main_cmd, &bi);
    m_in_frame = true;
    m_swap_index = UINT32_MAX;
    m_bound_color = m_bound_depth = nullptr;
    m_scissor_on = false;
    bind_serial++;
    last_pass = "frame start";
    return true;
}

void VkCore::SubmitFrame()
{
    if (!m_in_frame)
        return;
    EndPass();
    m_suspended = false;
    Frame& f = m_frames[m_frame];
    vkEndCommandBuffer(f.upload_cmd);
    vkEndCommandBuffer(f.main_cmd);
    VkCommandBuffer cmds[2] = { f.upload_cmd, f.main_cmd };
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 2;
    si.pCommandBuffers = cmds;
#ifdef __ANDROID__
    // (pre-rotated, the image is first written as a colour attachment, not by a copy)
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
#else
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
#endif
    if (m_swap_index != UINT32_MAX)
    {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &f.acquired;
        si.pWaitDstStageMask = &wait_stage;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &m_swap_done[m_swap_index];
    }
    VkResult r = vkQueueSubmit(queue, 1, &si, f.fence);
    m_in_frame = false;
    if (r != VK_SUCCESS)
    {
        ERRORLOG("Vulkan: vkQueueSubmit failed (%d, last pass '%s')", (int)r, last_pass);
        if (r == VK_ERROR_DEVICE_LOST)
            device_lost = true;
        m_swap_index = UINT32_MAX;
        return;
    }
    f.submitted = true;
    if (!f.after_done.empty())
    {
        // readbacks: the picture is wanted now (the launcher's background, a screenshot), as glReadPixels gave it
        WaitFrameDone();
    }
}

void VkCore::WaitFrameDone()
{
    Frame& f = m_frames[m_frame];
    if (!f.submitted)
        return;
    VkResult r = vkWaitForFences(device, 1, &f.fence, VK_TRUE, UINT64_MAX);
    if (r == VK_ERROR_DEVICE_LOST)
    {
        ERRORLOG("Vulkan: device lost (last pass '%s')", last_pass);
        device_lost = true;
        return;
    }
    for (auto& fn : f.after_done) fn();
    f.after_done.clear();
}

void VkCore::Present()
{
    if (m_swap_index != UINT32_MAX && !device_lost)
    {
        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &m_swap_done[m_swap_index];
        pi.swapchainCount = 1;
        pi.pSwapchains = &m_swapchain;
        pi.pImageIndices = &m_swap_index;
#ifdef __ANDROID__
        const uint64_t t0 = vk_now_ns();
#endif
        VkResult r = vkQueuePresentKHR(queue, &pi);
#ifdef __ANDROID__
        wait_present_ns += vk_now_ns() - t0;
#endif
        if (r == VK_ERROR_OUT_OF_DATE_KHR)
            m_swap_dirty = true;
#ifndef __ANDROID__
        else if (r == VK_SUBOPTIMAL_KHR)
            m_swap_dirty = true;
#else
        else if (r == VK_SUBOPTIMAL_KHR)
            CheckTransformChanged();
#endif
        else if (r == VK_ERROR_DEVICE_LOST)
        {
            ERRORLOG("Vulkan: device lost at present");
            device_lost = true;
        }
        else if (r != VK_SUCCESS && r != VK_ERROR_SURFACE_LOST_KHR && r != VK_SUBOPTIMAL_KHR)
            WARNLOG("Vulkan: vkQueuePresentKHR returned %d", (int)r);
        if (r == VK_ERROR_SURFACE_LOST_KHR)
            m_surface_lost = true;
    }
    m_swap_index = UINT32_MAX;
    m_frame = (m_frame + 1) % kFramesInFlight;
}

VkRingSlice VkCore::RingAlloc(VkDeviceSize size, VkDeviceSize align)
{
    VkRingSlice s;
    Frame& f = m_frames[m_frame];
    if (align < 4) align = 4;
    const VkDeviceSize ubo_align = props.limits.minUniformBufferOffsetAlignment;
    if (ubo_align > align) align = ubo_align;
    VkDeviceSize off = (f.ring_used + align - 1) & ~(align - 1);
    if (m_in_frame && off + size <= f.ring.size)
    {
        f.ring_used = off + size;
        s.ptr = (uint8_t*)f.ring.mapped + off;
        s.buffer = f.ring.buffer;
        s.offset = off;
        return s;
    }
    // too big for the ring (or outside a frame): a buffer of its own, freed with this frame slot
    VkBufferObj tmp;
    if (!CreateBuffer(tmp, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                      | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true))
    {
        ERRORLOG("Vulkan: no memory for a %u byte staging buffer", (unsigned)size);
        return s;
    }
    s.ptr = tmp.mapped;
    s.buffer = tmp.buffer;
    s.offset = 0;
    if (m_in_frame)
        f.temp_buffers.push_back(tmp);
    else
        m_init_temp.push_back(tmp);
    return s;
}

VkDescriptorSet VkCore::AllocSet(VkDescriptorSetLayout layout)
{
    Frame& f = m_frames[m_frame];
    for (;;)
    {
        if (f.desc_pool_cur >= f.desc_pools.size())
        {
            VkDescriptorPoolSize sizes[] = {
                { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192 },
                { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2048 },
            };
            VkDescriptorPoolCreateInfo ci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            ci.maxSets = 2048;
            ci.poolSizeCount = 2;
            ci.pPoolSizes = sizes;
            VkDescriptorPool p = VK_NULL_HANDLE;
            if (vkCreateDescriptorPool(device, &ci, nullptr, &p) != VK_SUCCESS)
                return VK_NULL_HANDLE;
            f.desc_pools.push_back(p);
        }
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = f.desc_pools[f.desc_pool_cur];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkResult r = vkAllocateDescriptorSets(device, &ai, &set);
        if (r == VK_SUCCESS)
            return set;
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL)
            return VK_NULL_HANDLE;
        f.desc_pool_cur++;
    }
}

/******************************************************************************/
/* uploads                                                                    */
/******************************************************************************/

void VkCore::StagingCopy(VkCommandBuffer cmd, std::vector<VkBufferObj>* temps, VkImageObj& img, int32_t x, int32_t y,
                         int32_t layer, int32_t w, int32_t h, const void* data, uint32_t row_pitch_bytes,
                         uint32_t bpp)
{
    (void)temps;
    const VkDeviceSize row = (VkDeviceSize)w * bpp;
    const VkDeviceSize size = row * (VkDeviceSize)h;
    VkRingSlice s = RingAlloc(size, bpp < 4 ? 4 : bpp);
    if (s.ptr == nullptr)
        return;
    const uint8_t* src = (const uint8_t*)data;
    if (row_pitch_bytes == row)
        memcpy(s.ptr, src, (size_t)size);
    else
        for (int32_t r = 0; r < h; r++)
            memcpy((uint8_t*)s.ptr + r * row, src + (size_t)r * row_pitch_bytes, (size_t)row);
    VkBufferImageCopy c = {};
    c.bufferOffset = s.offset;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.baseArrayLayer = (uint32_t)layer;
    c.imageSubresource.layerCount = 1;
    c.imageOffset = { x, y, 0 };
    c.imageExtent = { (uint32_t)w, (uint32_t)h, 1 };
    vkCmdCopyBufferToImage(cmd, s.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}

void VkCore::InitSampledImage(VkImageObj& img)
{
    VkCommandBuffer cmd = UploadCmd();
    barrier_auto(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue zero = {};
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, (uint32_t)img.layers };
    vkCmdClearColorImage(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
    barrier_auto(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    img.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void VkCore::UploadImage(VkImageObj& img, int32_t x, int32_t y, int32_t layer, int32_t w, int32_t h,
                         const void* data, uint32_t row_pitch_bytes, uint32_t bpp)
{
    if (img.image == VK_NULL_HANDLE || w <= 0 || h <= 0 || data == nullptr)
        return;
    VkCommandBuffer cmd = UploadCmd();
    // a texture rests in SHADER_READ_ONLY_OPTIMAL between command buffers (InitSampledImage made sure)
    barrier_auto(cmd, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    StagingCopy(cmd, nullptr, img, x, y, layer, w, h, data, row_pitch_bytes, bpp);
    barrier_auto(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void VkCore::UploadImageInline(VkImageObj& img, int32_t x, int32_t y, int32_t layer, int32_t w, int32_t h,
                               const void* data, uint32_t row_pitch_bytes, uint32_t bpp)
{
    if (!m_in_frame)
    {
        UploadImage(img, x, y, layer, w, h, data, row_pitch_bytes, bpp);
        return;
    }
    if (img.image == VK_NULL_HANDLE || w <= 0 || h <= 0 || data == nullptr)
        return;
    SuspendPass();
    VkCommandBuffer cmd = Cmd();
    barrier_auto(cmd, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    StagingCopy(cmd, nullptr, img, x, y, layer, w, h, data, row_pitch_bytes, bpp);
    barrier_auto(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    ResumePass();
}

void VkCore::CopyImage(VkImageObj& src, VkImageObj& dst, int32_t w, int32_t h)
{
    if (!m_in_frame || src.image == VK_NULL_HANDLE || dst.image == VK_NULL_HANDLE || w <= 0 || h <= 0)
        return;
    EndPass();
    const VkImageLayout dst_rest = (dst.layout == VK_IMAGE_LAYOUT_UNDEFINED) ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : dst.layout;
    Transition(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy c = {};
    c.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    c.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    c.extent = { (uint32_t)std::min(w, std::min(src.width, dst.width)), (uint32_t)std::min(h, std::min(src.height, dst.height)), 1 };
    vkCmdCopyImage(Cmd(), src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    // a texture goes back to resting in SHADER_READ_ONLY_OPTIMAL; a render target to what it was
    Transition(dst, dst_rest == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : dst_rest);
}

void VkCore::Transition(VkImageObj& img, VkImageLayout to)
{
    if (img.image == VK_NULL_HANDLE || img.layout == to)
        return;
    if (InPass() && (&img == m_pass_color || &img == m_pass_depth))
        EndPass();
    else if (InPass())
        SuspendPass();
    barrier_auto(Cmd(), img, img.layout, to);
    img.layout = to;
    if (m_suspended)
        ResumePass();
}

/******************************************************************************/
/* passes                                                                     */
/******************************************************************************/

void VkCore::BeginPass(VkImageObj* color, VkImageObj* depth, const float* clear_rgba, bool clear_depth)
{
    EndPass();
    m_suspended = false;
    if (color == nullptr || color->image == VK_NULL_HANDLE)
        return;
    VkCommandBuffer cmd = Cmd();
    if (color->layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
    {
        barrier_auto(cmd, *color, color->layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        color->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    if (depth != nullptr && depth->image != VK_NULL_HANDLE && depth->layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
    {
        barrier_auto(cmd, *depth, depth->layout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        depth->layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    VkRenderingAttachmentInfoKHR ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR };
    ca.imageView = color->view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = clear_rgba ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (clear_rgba)
        memcpy(ca.clearValue.color.float32, clear_rgba, sizeof(float) * 4);
    VkRenderingAttachmentInfoKHR da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR };
    const bool has_depth = depth != nullptr && depth->image != VK_NULL_HANDLE;
    if (has_depth)
    {
        da.imageView = depth->view;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        da.loadOp = clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        da.clearValue.depthStencil.depth = 1.0f;
        da.clearValue.depthStencil.stencil = 0;
    }
    VkRenderingInfoKHR ri = { VK_STRUCTURE_TYPE_RENDERING_INFO_KHR };
    ri.renderArea.extent = { (uint32_t)color->width, (uint32_t)color->height };
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    if (has_depth)
    {
        ri.pDepthAttachment = &da;
        if (depth->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)
            ri.pStencilAttachment = &da;
    }
    kfx_vkCmdBeginRendering(cmd, &ri);
    m_pass_color = color;
    m_pass_depth = has_depth ? depth : nullptr;
    m_bound_color = m_pass_color;
    m_bound_depth = m_pass_depth;
    bind_serial++;
    ApplyDynamic();
}

bool VkCore::EnsurePass()
{
    if (m_pass_color != nullptr)
        return true;
    if (!m_in_frame || m_bound_color == nullptr || m_bound_color->image == VK_NULL_HANDLE)
        return false;
    BeginPass(m_bound_color, m_bound_depth, nullptr, false);
    return m_pass_color != nullptr;
}

void VkCore::ApplyDynamic()
{
    if (m_pass_color == nullptr)
        return;
    VkCommandBuffer cmd = Cmd();
    vkCmdSetViewport(cmd, 0, 1, &m_viewport);
    VkRect2D r;
    if (m_scissor_on)
    {
        int32_t x = m_scissor.offset.x, y = m_scissor.offset.y;
        int32_t w = (int32_t)m_scissor.extent.width, h = (int32_t)m_scissor.extent.height;
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > m_pass_color->width) w = m_pass_color->width - x;
        if (y + h > m_pass_color->height) h = m_pass_color->height - y;
        if (w < 0) w = 0;
        if (h < 0) h = 0;
        r = { { x, y }, { (uint32_t)w, (uint32_t)h } };
    }
    else
        r = { { 0, 0 }, { (uint32_t)m_pass_color->width, (uint32_t)m_pass_color->height } };
    vkCmdSetScissor(cmd, 0, 1, &r);
}

static VkClearRect clear_rect(const VkImageObj* color, bool scissor_on, const VkRect2D& sc)
{
    VkClearRect cr = {};
    cr.layerCount = 1;
    cr.rect = { { 0, 0 }, { (uint32_t)color->width, (uint32_t)color->height } };
    if (scissor_on)
    {
        int32_t x = sc.offset.x, y = sc.offset.y, w = (int32_t)sc.extent.width, hh = (int32_t)sc.extent.height;
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { hh += y; y = 0; }
        if (x + w > color->width) w = color->width - x;
        if (y + hh > color->height) hh = color->height - y;
        cr.rect = { { x, y }, { (uint32_t)(w > 0 ? w : 0), (uint32_t)(hh > 0 ? hh : 0) } };
    }
    return cr;
}

void VkCore::ClearColor(const float* rgba)
{
    if (!EnsurePass())
        return;
    VkClearAttachment a = {};
    a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    a.colorAttachment = 0;
    memcpy(a.clearValue.color.float32, rgba, sizeof(float) * 4);
    VkClearRect cr = clear_rect(m_pass_color, m_scissor_on, m_scissor);
    if (cr.rect.extent.width == 0 || cr.rect.extent.height == 0)
        return;
    vkCmdClearAttachments(Cmd(), 1, &a, 1, &cr);
}

void VkCore::ClearDepthStencil(float depth, uint32_t stencil)
{
    if (!EnsurePass() || m_pass_depth == nullptr)
        return;
    VkClearAttachment a = {};
    a.aspectMask = m_pass_depth->aspect;
    a.clearValue.depthStencil.depth = depth;
    a.clearValue.depthStencil.stencil = stencil;
    VkClearRect cr = clear_rect(m_pass_color, m_scissor_on, m_scissor);
    if (cr.rect.extent.width == 0 || cr.rect.extent.height == 0)
        return;
    vkCmdClearAttachments(Cmd(), 1, &a, 1, &cr);
}

bool VkCore::BindVertices(const void* data, size_t bytes, uint32_t binding)
{
    if (bytes == 0 || data == nullptr)
        return false;
    VkRingSlice s = RingAlloc(bytes, 16);
    if (s.ptr == nullptr)
        return false;
    memcpy(s.ptr, data, bytes);
    vkCmdBindVertexBuffers(Cmd(), binding, 1, &s.buffer, &s.offset);
    return true;
}

void VkCore::EndPass()
{
    if (m_pass_color == nullptr)
        return;
    kfx_vkCmdEndRendering(Cmd());
    m_pass_color = nullptr;
    m_pass_depth = nullptr;
}

void VkCore::SuspendPass()
{
    if (m_pass_color == nullptr)
        return;
    m_suspended_color = m_pass_color;
    m_suspended_depth = m_pass_depth;
    m_suspended = true;
    EndPass();
}

void VkCore::ResumePass()
{
    if (!m_suspended)
        return;
    m_suspended = false;
    BeginPass(m_suspended_color, m_suspended_depth, nullptr, false);
}

void VkCore::SetViewport(float x, float y, float w, float h)
{
    // OpenGL window coordinates, unchanged (see the file header)
    m_viewport = { x, y, w, h, 0.0f, 1.0f };
    ApplyDynamic();
}

void VkCore::SetScissorEnabled(bool on)
{
    m_scissor_on = on;
    ApplyDynamic();
}

void VkCore::SetScissor(int32_t x, int32_t y, int32_t w, int32_t h)
{
    m_scissor = { { x, y }, { (uint32_t)(w > 0 ? w : 0), (uint32_t)(h > 0 ? h : 0) } };
    ApplyDynamic();
}

/******************************************************************************/
/* swapchain                                                                  */
/******************************************************************************/

void VkCore::DestroySwapViews()
{
    for (VkImageView v : m_swap_views)
        if (v != VK_NULL_HANDLE)
            vkDestroyImageView(device, v, nullptr);
    m_swap_views.clear();
}

void VkCore::DestroyRotatePipeline()
{
    if (device == VK_NULL_HANDLE)
        return;
    if (m_rot_pipe != VK_NULL_HANDLE) vkDestroyPipeline(device, m_rot_pipe, nullptr);
    if (m_rot_layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, m_rot_layout, nullptr);
    if (m_rot_set_layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, m_rot_set_layout, nullptr);
    m_rot_pipe = VK_NULL_HANDLE;
    m_rot_layout = VK_NULL_HANDLE;
    m_rot_set_layout = VK_NULL_HANDLE;
    m_rot_format = VK_FORMAT_UNDEFINED;
}

void VkCore::DestroySwapchain()
{
    if (device == VK_NULL_HANDLE)
        return;
    DestroySwapViews();
    for (VkSemaphore s : m_swap_done)
        vkDestroySemaphore(device, s, nullptr);
    m_swap_done.clear();
    m_swap_images.clear();
    if (m_swapchain != VK_NULL_HANDLE)
        vkDestroySwapchainKHR(device, m_swapchain, nullptr);
    m_swapchain = VK_NULL_HANDLE;
}

bool VkCore::RecreateSurface(void* native_window)
{
    vkDeviceWaitIdle(device);
    DestroySwapchain();
    if (m_surface != VK_NULL_HANDLE)
        vkDestroySurfaceKHR(instance, m_surface, nullptr);
    m_surface = VK_NULL_HANDLE;
    if (m_window == nullptr || !SDL_Vulkan_CreateSurface(m_window, instance, nullptr, &m_surface))
    {
        WARNLOG("Vulkan: the window's surface could not be made again: %s", SDL_GetError());
        m_surface = VK_NULL_HANDLE;
        return false;
    }
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(phys, queue_family, m_surface, &supported);
    m_native_window = native_window;
    m_surface_lost = false;
    m_swap_dirty = true;
    SYNCLOG("Vulkan: the window's surface was made again (presentation %s)", supported ? "supported" : "NOT supported");
    return supported == VK_TRUE;
}

bool VkCore::CreateSwapchain(int32_t w, int32_t h, bool vsync)
{
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, m_surface, &caps) != VK_SUCCESS)
        return false;
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu)
    {
        extent.width = (uint32_t)std::max<int32_t>(1, w);
        extent.height = (uint32_t)std::max<int32_t>(1, h);
        extent.width = std::min(std::max(extent.width, caps.minImageExtent.width), caps.maxImageExtent.width);
        extent.height = std::min(std::max(extent.height, caps.minImageExtent.height), caps.maxImageExtent.height);
    }
#ifdef __ANDROID__
    // the extent in the window's orientation (Android may give it in the panel's)
    if (extent.width != 0xFFFFFFFFu && ((extent.width > extent.height) != (w > h)))
        std::swap(extent.width, extent.height);
#endif
    VkSurfaceTransformFlagBitsKHR transform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                                            ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    VkExtent2D image_extent = extent;
    bool draw_rotated = false;
#ifdef __ANDROID__
    // Pre-rotation (2026-11-24: a phone showed Vulkan at 36 FPS where OpenGL ES ran at 120): the images get the
    // panel's own orientation and the frame is drawn into them turned (DrawRotated), so the compositor shows them as
    // they are instead of turning every frame itself, which some phones do with the GPU, at a cost.
    if (allow_prerotate
        && (caps.currentTransform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR || caps.currentTransform == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR
         || caps.currentTransform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)
        && (caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
    {
        transform = caps.currentTransform;
        draw_rotated = true;
        if (transform != VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR)
            std::swap(image_extent.width, image_extent.height);
    }
#endif
    if (extent.width == 0 || extent.height == 0)
        return false; // minimised
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
    {
        ERRORLOG("Vulkan: the swapchain cannot be a copy destination");
        return false;
    }

    uint32_t fc = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, m_surface, &fc, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fc);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, m_surface, &fc, formats.data());
    // the same 8-bit SDR picture OpenGL shows: UNORM, sRGB colour space, no conversion
    VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR } : formats[0];
    for (const VkSurfaceFormatKHR& f : formats)
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM)
            && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        { chosen = f; break; }

    uint32_t pc = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, m_surface, &pc, nullptr);
    std::vector<VkPresentModeKHR> modes(pc);
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, m_surface, &pc, modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync)
    {
        if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end())
            mode = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_IMMEDIATE_KHR) != modes.end())
            mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    uint32_t count = std::max(caps.minImageCount + 1, 3u);
#ifdef __ANDROID__
    // paced by vsync: no spare image beyond what the screen needs, so a frame made early waits for the screen
    if (vsync_pacing && mode == VK_PRESENT_MODE_FIFO_KHR)
        count = std::max(caps.minImageCount, 2u);
#endif
    if (caps.maxImageCount > 0 && count > caps.maxImageCount)
        count = caps.maxImageCount;

    VkSwapchainCreateInfoKHR sci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    sci.surface = m_surface;
    sci.minImageCount = count;
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = image_extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | (draw_rotated ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0);
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = transform;
    sci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                       ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & -(int)caps.supportedCompositeAlpha);
    sci.presentMode = mode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = m_swapchain;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkResult r = vkCreateSwapchainKHR(device, &sci, nullptr, &sc);
    if (m_swapchain != VK_NULL_HANDLE)
    {
        DestroySwapViews();
        for (VkSemaphore s : m_swap_done) vkDestroySemaphore(device, s, nullptr);
        m_swap_done.clear();
        vkDestroySwapchainKHR(device, m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }
    if (r != VK_SUCCESS)
    {
        ERRORLOG("Vulkan: vkCreateSwapchainKHR %ux%u failed (%d)", extent.width, extent.height, (int)r);
        return false;
    }
    m_swapchain = sc;
    uint32_t ic = 0;
    vkGetSwapchainImagesKHR(device, m_swapchain, &ic, nullptr);
    m_swap_images.resize(ic);
    vkGetSwapchainImagesKHR(device, m_swapchain, &ic, m_swap_images.data());
    m_swap_done.resize(ic);
    for (uint32_t i = 0; i < ic; i++)
    {
        VkSemaphoreCreateInfo sei = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        vkCreateSemaphore(device, &sei, nullptr, &m_swap_done[i]);
    }
    if (draw_rotated)
    {
        m_swap_views.resize(ic, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < ic; i++)
        {
            VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            vci.image = m_swap_images[i];
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = chosen.format;
            vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            if (vkCreateImageView(device, &vci, nullptr, &m_swap_views[i]) != VK_SUCCESS)
                m_swap_views[i] = VK_NULL_HANDLE; // (that image is shown unturned: BlitToSwapchain's copy)
        }
    }
    m_swap_format = chosen.format;
    m_swap_extent = extent;
    m_swap_image_extent = image_extent;
    m_swap_transform = draw_rotated ? transform : VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    m_swap_seen_transform = caps.currentTransform;
    m_swap_vsync = vsync;
    m_swap_dirty = false;
    const int turns = !draw_rotated ? 0 : transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ? 1
                    : transform == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR ? 2 : 3;
    swap_rotation = turns;
    swapchain_serial++;
    SYNCLOG("Vulkan: swapchain %ux%u, %u images (least %u), format %d, present mode %d, surface transform %d, %s (images %ux%u)",
            extent.width, extent.height, ic, caps.minImageCount, (int)chosen.format, (int)mode, (int)caps.currentTransform,
            draw_rotated ? (turns == 1 ? "pre-rotated 90" : turns == 2 ? "pre-rotated 180" : "pre-rotated 270") : "not pre-rotated",
            image_extent.width, image_extent.height);
    return true;
}

bool VkCore::BlitToSwapchain(VkImageObj& src, int32_t px, int32_t py, int32_t pw, int32_t ph,
                             int32_t drawable_w, int32_t drawable_h, bool vsync)
{
#ifdef __ANDROID__
    if (m_window != nullptr && m_in_frame)
    {
        void* nw = SDL_GetPointerProperty(SDL_GetWindowProperties(m_window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
        if (nw == nullptr)
            return false; // in the background: nothing to show the frame on
        if (nw != m_native_window || m_surface_lost || m_surface == VK_NULL_HANDLE)
        {
            if (!RecreateSurface(nw))
                return false;
        }
    }
#endif
    if (m_surface == VK_NULL_HANDLE || m_surface_lost || !m_in_frame || drawable_w <= 0 || drawable_h <= 0 || pw <= 0 || ph <= 0)
        return false;
    if (m_swapchain == VK_NULL_HANDLE || m_swap_dirty || vsync != m_swap_vsync
        || m_swap_extent.width != (uint32_t)drawable_w || m_swap_extent.height != (uint32_t)drawable_h)
    {
        vkDeviceWaitIdle(device);
        if (!CreateSwapchain(drawable_w, drawable_h, vsync))
            return false;
    }
    Frame& f = m_frames[m_frame];
    uint32_t index = 0;
#ifdef __ANDROID__
    const uint64_t t0 = vk_now_ns();
#endif
    VkResult r = vkAcquireNextImageKHR(device, m_swapchain, 1000000000ull, f.acquired, VK_NULL_HANDLE, &index);
#ifdef __ANDROID__
    {
        const uint64_t d = vk_now_ns() - t0;
        wait_acquire_ns += d;
        wait_total_ns += d;
    }
#endif
    if (r == VK_ERROR_OUT_OF_DATE_KHR)
    {
        vkDeviceWaitIdle(device);
        if (!CreateSwapchain(drawable_w, drawable_h, vsync))
            return false;
        r = vkAcquireNextImageKHR(device, m_swapchain, 1000000000ull, f.acquired, VK_NULL_HANDLE, &index);
    }
    if (r == VK_SUBOPTIMAL_KHR)
    {
#ifndef __ANDROID__
        m_swap_dirty = true;
#else
        // (Android also says suboptimal for every frame not pre-rotated: only a turned phone makes a new swapchain)
        CheckTransformChanged();
#endif
    }
    else if (r != VK_SUCCESS)
    {
        if (r == VK_ERROR_SURFACE_LOST_KHR) m_surface_lost = true;
        if (r == VK_ERROR_DEVICE_LOST) device_lost = true;
        if (r != VK_TIMEOUT && r != VK_NOT_READY)
            WARNLOG("Vulkan: vkAcquireNextImageKHR returned %d", (int)r);
        return false;
    }
    m_swap_index = index;

    EndPass();
    VkCommandBuffer cmd = Cmd();
#ifdef __ANDROID__
    if (m_swap_transform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
        && DrawRotated(cmd, src, index, px, py, pw, ph, drawable_w, drawable_h))
        return true;
#endif
    last_pass = "present blit";
    VkImageObj dst;
    dst.image = m_swap_images[index];
    dst.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    dst.layers = 1;
    barrier_auto(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (src.layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
    {
        barrier_auto(cmd, src, src.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        src.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    VkClearColorValue black = {};
    black.float32[3] = 1.0f;
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cmd, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    // the present rect is in drawable pixels, top-left origin; the swapchain may differ in size (scale it)
    const float sx = (float)m_swap_extent.width / (float)drawable_w;
    const float sy = (float)m_swap_extent.height / (float)drawable_h;
    VkImageBlit b = {};
    b.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    b.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    // the screen image holds the picture bottom row first (OpenGL's order): read it upside down
    b.srcOffsets[0] = { 0, src.height, 0 };
    b.srcOffsets[1] = { src.width, 0, 1 };
    int32_t x0 = (int32_t)(px * sx), y0 = (int32_t)(py * sy);
    int32_t x1 = (int32_t)((px + pw) * sx), y1 = (int32_t)((py + ph) * sy);
    x0 = std::max<int32_t>(0, x0); y0 = std::max<int32_t>(0, y0);
    x1 = std::min<int32_t>((int32_t)m_swap_image_extent.width, x1); y1 = std::min<int32_t>((int32_t)m_swap_image_extent.height, y1);
    if (x1 > x0 && y1 > y0)
    {
        b.dstOffsets[0] = { x0, y0, 0 };
        b.dstOffsets[1] = { x1, y1, 1 };
        vkCmdBlitImage(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    }
    barrier_auto(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    return true;
}

/** Android pre-rotation: draws src (bottom row first) into the acquired image turned to the panel, black round the
 *  present rect. False (nothing recorded) if it cannot: the caller then copies it unturned. */
bool VkCore::DrawRotated(VkCommandBuffer cmd, VkImageObj& src, uint32_t index, int32_t px, int32_t py, int32_t pw, int32_t ph,
                         int32_t drawable_w, int32_t drawable_h)
{
    if (index >= m_swap_views.size() || m_swap_views[index] == VK_NULL_HANDLE || src.view == VK_NULL_HANDLE)
        return false;
    if (m_rot_format != m_swap_format)
    {
        m_rot_format = m_swap_format; // tried once per format
        if (m_rot_set_layout == VK_NULL_HANDLE)
        {
            VkDescriptorSetLayoutBinding b = {};
            b.binding = 0;
            b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b.descriptorCount = 1;
            b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            m_rot_set_layout = CreateSetLayout({ b });
        }
        if (m_rot_layout == VK_NULL_HANDLE && m_rot_set_layout != VK_NULL_HANDLE)
        {
            VkPushConstantRange pr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 5 * sizeof(float) };
            VkPipelineLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            lci.setLayoutCount = 1;
            lci.pSetLayouts = &m_rot_set_layout;
            lci.pushConstantRangeCount = 1;
            lci.pPushConstantRanges = &pr;
            if (vkCreatePipelineLayout(device, &lci, nullptr, &m_rot_layout) != VK_SUCCESS)
                m_rot_layout = VK_NULL_HANDLE;
        }
        if (m_rot_pipe != VK_NULL_HANDLE)
        {
            VkDevice dev = device;
            VkPipeline old = m_rot_pipe;
            Defer([dev, old]() { vkDestroyPipeline(dev, old, nullptr); });
            m_rot_pipe = VK_NULL_HANDLE;
        }
        if (m_rot_layout != VK_NULL_HANDLE)
        {
            VkPipelineDesc d;
            d.vs_code = kfx_vk_prerotate_vs; d.vs_size = sizeof(kfx_vk_prerotate_vs);
            d.fs_code = kfx_vk_prerotate_fs; d.fs_size = sizeof(kfx_vk_prerotate_fs);
            d.layout = m_rot_layout;
            d.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            d.color_format = m_swap_format;
            d.has_depth = false;
            d.debug_name = "present pre-rotated";
            m_rot_pipe = CreatePipeline(d);
        }
        if (m_rot_pipe == VK_NULL_HANDLE)
            ERRORLOG("Vulkan: no pre-rotation pipeline for format %d: frames are shown unturned", (int)m_swap_format);
    }
    if (m_rot_pipe == VK_NULL_HANDLE || drawable_w <= 0 || drawable_h <= 0)
        return false;
    VkDescriptorSet set = AllocSet(m_rot_set_layout);
    if (set == VK_NULL_HANDLE)
        return false;
    VkDescriptorImageInfo ii = { GetSampler(false, false), src.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wr.dstSet = set;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(device, 1, &wr, 0, nullptr);

    last_pass = "present pre-rotated";
    if (src.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        barrier_auto(cmd, src, src.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        src.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    VkImageObj dst;
    dst.image = m_swap_images[index];
    dst.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    dst.layers = 1;
    // (the acquire semaphore is waited at the colour output stage: the layout change waits for it there)
    VkCore_ImageBarrier(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfoKHR ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR };
    ca.imageView = m_swap_views[index];
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color.float32[3] = 1.0f;
    VkRenderingInfoKHR ri = { VK_STRUCTURE_TYPE_RENDERING_INFO_KHR };
    ri.renderArea.extent = m_swap_image_extent;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    kfx_vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_rot_pipe);
    const VkViewport vp = { 0.0f, 0.0f, (float)m_swap_image_extent.width, (float)m_swap_image_extent.height, 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    const VkRect2D sc = { { 0, 0 }, m_swap_image_extent };
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_rot_layout, 0, 1, &set, 0, nullptr);
    struct { float rect[4]; int32_t rot; } pc;
    // the present rect (drawable pixels, top-left origin) in the window's clip space
    pc.rect[0] = 2.0f * (float)px / (float)drawable_w - 1.0f;
    pc.rect[1] = 2.0f * (float)py / (float)drawable_h - 1.0f;
    pc.rect[2] = 2.0f * (float)(px + pw) / (float)drawable_w - 1.0f;
    pc.rect[3] = 2.0f * (float)(py + ph) / (float)drawable_h - 1.0f;
    pc.rot = m_swap_transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ? 1
           : m_swap_transform == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR ? 2 : 3;
    static_assert(sizeof(pc) == 5 * sizeof(float), "push constants as the shader has them");
    vkCmdPushConstants(cmd, m_rot_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 4, 1, 0, 0);
    kfx_vkCmdEndRendering(cmd);
    barrier_auto(cmd, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    bind_serial++;
    return true;
}

/** Android: a suboptimal answer means a new swapchain only when the phone was turned since the last one was made. */
void VkCore::CheckTransformChanged()
{
    VkSurfaceCapabilitiesKHR c;
    if (m_surface != VK_NULL_HANDLE && vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, m_surface, &c) == VK_SUCCESS
        && c.currentTransform != m_swap_seen_transform)
        m_swap_dirty = true;
}

/******************************************************************************/
/* readback                                                                   */
/******************************************************************************/

void VkCore::Readback(VkImageObj& img, std::function<void(const uint8_t* rgba, int w, int h)> fn)
{
    if (!m_in_frame || img.image == VK_NULL_HANDLE)
        return;
    EndPass();
    VkCommandBuffer cmd = Cmd();
    if (img.layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
    {
        barrier_auto(cmd, img, img.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        img.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    const VkDeviceSize size = (VkDeviceSize)img.width * img.height * 4;
    VkBufferObj buf;
    if (!CreateBuffer(buf, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true))
    {
        ERRORLOG("Vulkan: no memory for a %dx%d readback", img.width, img.height);
        return;
    }
    VkBufferImageCopy c = {};
    c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    c.imageExtent = { (uint32_t)img.width, (uint32_t)img.height, 1 };
    vkCmdCopyImageToBuffer(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf.buffer, 1, &c);
    VkBufferMemoryBarrier bb = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = buf.buffer;
    bb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &bb, 0, nullptr);
    Frame& f = m_frames[m_frame];
    const int w = img.width, h = img.height;
    VkDevice dev = device;
    VkDeviceMemory mem = buf.memory;
    void* mapped = buf.mapped;
    f.temp_buffers.push_back(buf);
    f.after_done.push_back([dev, mem, mapped, w, h, fn]() {
        VkMappedMemoryRange mr = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
        mr.memory = mem;
        mr.size = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(dev, 1, &mr);
        fn((const uint8_t*)mapped, w, h);
    });
}
