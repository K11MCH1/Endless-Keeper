/******************************************************************************/
// Endless Keeper - Vulkan renderer
/******************************************************************************/
/** @file VkCore.h
 *     Vulkan device, frames in flight, uploads, render passes, swapchain and readback.
 * @par Threading
 *     Everything here runs on the render thread (RenderThreadManager), like the OpenGL context does, except
 *     VkCore_Probe(), which runs on the game thread before the window exists.
 * @par Frames
 *     Two frames in flight. Each frame records two command buffers: `upload` (copies into textures, run first)
 *     and `main` (the draws). Sampled textures rest in SHADER_READ_ONLY_OPTIMAL between command buffers, so an
 *     upload recorded in the middle of the frame's draws is still done before them.
 */
/******************************************************************************/
#ifndef RENDERER_VULKAN_VKCORE_H
#define RENDERER_VULKAN_VKCORE_H

#include "kfx/renderer/vulkan/VkLoader.h"
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

struct SDL_Window;

/** An image with its memory and a view. `layout` is the layout the main command buffer leaves it in. */
struct VkImageObj {
    VkImage        image  = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView    view   = VK_NULL_HANDLE;
    VkFormat       format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    int32_t        width  = 0;
    int32_t        height = 0;
    int32_t        layers = 1;
    VkImageLayout  layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkSampler      sampler = VK_NULL_HANDLE; // not owned
    bool           is_array = false;
};

struct VkBufferObj {
    VkBuffer       buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void*          mapped = nullptr;
    VkDeviceSize   size   = 0;
};

/** Where a ring allocation landed. */
struct VkRingSlice {
    void*        ptr    = nullptr;
    VkBuffer     buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
};

/** Fixed state of one graphics pipeline. */
struct VkPipelineDesc {
    const uint32_t* vs_code = nullptr; size_t vs_size = 0;
    const uint32_t* fs_code = nullptr; size_t fs_size = 0;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::vector<VkVertexInputBindingDescription>   bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    // blending (one colour attachment)
    bool          blend = false;
    VkBlendFactor src_color = VK_BLEND_FACTOR_SRC_ALPHA, dst_color = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    VkBlendFactor src_alpha = VK_BLEND_FACTOR_ONE,       dst_alpha = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    VkBlendOp     color_op = VK_BLEND_OP_ADD, alpha_op = VK_BLEND_OP_ADD;
    VkColorComponentFlags write_mask = 0xF;
    // depth / stencil
    bool          depth_test = false;
    bool          depth_write = false;
    VkCompareOp   depth_op = VK_COMPARE_OP_LESS;
    bool          stencil_test = false;
    VkStencilOpState stencil_front = {}, stencil_back = {};
    // attachments
    VkFormat      color_format = VK_FORMAT_R8G8B8A8_UNORM;
    bool          has_depth = true;   // the pass this pipeline draws in has the depth/stencil attachment
    const char*   debug_name = "";
};

class VkCore {
public:
    static constexpr int kFramesInFlight = 2;

    bool Init(SDL_Window* window, bool want_present, std::string& reason);
    void Shutdown();

    // ── device ──
    VkInstance       instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice         device = VK_NULL_HANDLE;
    VkQueue          queue = VK_NULL_HANDLE;
    uint32_t         queue_family = 0;
    VkPhysicalDeviceProperties       props = {};
    VkPhysicalDeviceMemoryProperties memprops = {};
    VkFormat         depth_format = VK_FORMAT_UNDEFINED;
    VkPipelineCache  pipeline_cache = VK_NULL_HANDLE;
    std::string      device_name;

    // ── objects ──
    bool CreateImage(VkImageObj& out, int32_t w, int32_t h, int32_t layers, VkFormat format,
                     VkImageUsageFlags usage, const char* name, bool force_array = false);
    /** Deferred until the GPU is done with the frames that may use it. */
    void DestroyImage(VkImageObj& img);
    bool CreateBuffer(VkBufferObj& out, VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible);
    void DestroyBuffer(VkBufferObj& buf);
    VkSampler GetSampler(bool linear, bool repeat);
    VkShaderModule CreateShaderModule(const uint32_t* code, size_t size);
    VkPipeline CreatePipeline(const VkPipelineDesc& desc);
    VkDescriptorSetLayout CreateSetLayout(const std::vector<VkDescriptorSetLayoutBinding>& bindings);
    /** Runs fn once the GPU no longer uses anything recorded so far. */
    void Defer(std::function<void()> fn);

    // ── frame ──
    /** Waits for this frame slot's previous use, resets its pools, opens its command buffers. */
    bool BeginFrame();
    /** Ends + submits the command buffers (and presents if a swapchain image was acquired). */
    void SubmitFrame();
    /** Presents the acquired image (render thread's present step). */
    void Present();
    bool InFrame() const { return m_in_frame; }
    VkCommandBuffer Cmd() const { return m_frames[m_frame].main_cmd; }
    /** Blocks until everything submitted is done (readback, shutdown). */
    void WaitFrameDone();

    VkRingSlice RingAlloc(VkDeviceSize size, VkDeviceSize align);
    VkDescriptorSet AllocSet(VkDescriptorSetLayout layout);

    /** Copies pixels into a sampled image (texture), done before this frame's draws. */
    void UploadImage(VkImageObj& img, int32_t x, int32_t y, int32_t layer, int32_t w, int32_t h,
                     const void* data, uint32_t row_pitch_bytes, uint32_t bytes_per_pixel);
    /** Same, but recorded in the draws' order (ends the current pass). */
    void UploadImageInline(VkImageObj& img, int32_t x, int32_t y, int32_t layer, int32_t w, int32_t h,
                           const void* data, uint32_t row_pitch_bytes, uint32_t bytes_per_pixel);
    /** A freshly made sampled image: cleared to zero and put in SHADER_READ_ONLY_OPTIMAL. */
    void InitSampledImage(VkImageObj& img);

    /** glBlitFramebuffer / glCopyTexSubImage at 1:1: copies src's w x h into dst in draw order. */
    void CopyImage(VkImageObj& src, VkImageObj& dst, int32_t w, int32_t h);
    /** Main command buffer layout change (ends the current pass first). */
    void Transition(VkImageObj& img, VkImageLayout to);

    // ── passes (dynamic rendering) ──
    void BeginPass(VkImageObj* color, VkImageObj* depth, const float* clear_rgba, bool clear_depth);
    void EndPass();
    bool InPass() const { return m_pass_color != nullptr; }
    VkImageObj* PassColor() const { return m_pass_color; }
    VkImageObj* PassDepth() const { return m_pass_depth; }
    /** Ends the pass if open and remembers it; ResumePass() reopens it with LOAD. */
    void SuspendPass();
    void ResumePass();
    /** OpenGL's framebuffer binding persists after a pass ends: reopens the last target (LOAD) if none is open. */
    bool EnsurePass();
    /** OpenGL-like dynamic state that survives passes: glViewport, glEnable(GL_SCISSOR_TEST), glScissor. */
    void SetViewport(float x, float y, float w, float h);
    void SetScissorEnabled(bool on);
    void SetScissor(int32_t x, int32_t y, int32_t w, int32_t h);
    /** glClear of the bound target's colour / depth+stencil, within the scissor when it is on (as OpenGL). */
    void ClearColor(const float* rgba);
    void ClearDepthStencil(float depth = 1.0f, uint32_t stencil = 0);
    /** Copies vertices into the ring and binds them (binding 0 or 1). */
    bool BindVertices(const void* data, size_t bytes, uint32_t binding = 0);
    /** Bumped whenever a pipeline/descriptor binding may have changed (programs skip identical re-binds). */
    uint64_t bind_serial = 1;

    // ── present ──
    bool HasSurface() const { return m_surface != VK_NULL_HANDLE; }
    /** Acquires a swapchain image and blits src into the rect (black around it). False if nothing to present to. */
    bool BlitToSwapchain(VkImageObj& src, int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                         int32_t drawable_w, int32_t drawable_h, bool vsync);

    // ── readback ──
    /** Copies the image into host memory after this frame's draws; fn runs once it is there. */
    void Readback(VkImageObj& img, std::function<void(const uint8_t* rgba, int w, int h)> fn);

    /** Name of the pass being recorded, for device-lost logs. */
    const char* last_pass = "";
    bool device_lost = false;

private:
    struct Frame {
        VkCommandPool   pool = VK_NULL_HANDLE;
        VkCommandBuffer upload_cmd = VK_NULL_HANDLE;
        VkCommandBuffer main_cmd = VK_NULL_HANDLE;
        VkFence         fence = VK_NULL_HANDLE;
        VkSemaphore     acquired = VK_NULL_HANDLE;
        bool            submitted = false;
        bool            upload_used = false;
        std::vector<VkDescriptorPool> desc_pools;
        size_t          desc_pool_cur = 0;
        VkBufferObj     ring;
        VkDeviceSize    ring_used = 0;
        std::vector<VkBufferObj> temp_buffers;
        std::vector<std::function<void()>> garbage;
        std::vector<std::function<void()>> after_done;
    };
    Frame m_frames[kFramesInFlight];
    int  m_frame = 0;
    bool m_in_frame = false;
    std::vector<std::function<void()>> m_pending_init_garbage;

    // init-time uploads (before the first frame)
    VkCommandPool   m_init_pool = VK_NULL_HANDLE;
    VkCommandBuffer m_init_cmd = VK_NULL_HANDLE;
    bool            m_init_open = false;
    std::vector<VkBufferObj> m_init_temp;
    VkCommandBuffer UploadCmd();
    void FlushInit();

    VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;
    SDL_Window*  m_window = nullptr;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    bool m_swapchain_ext = false;

    // swapchain
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat       m_swap_format = VK_FORMAT_UNDEFINED;
    VkExtent2D     m_swap_extent = {0, 0};
    bool           m_swap_vsync = true;
    std::vector<VkImage>     m_swap_images;
    std::vector<VkSemaphore> m_swap_done;   // one per image: rendering done -> present
    uint32_t       m_swap_index = UINT32_MAX;
    bool           m_swap_dirty = false;
    bool CreateSwapchain(int32_t w, int32_t h, bool vsync);
    void DestroySwapchain();
    // Android: the window's ANativeWindow goes away in the background and comes back new
    void* m_native_window = nullptr;
    bool  m_surface_lost = false;
    bool  RecreateSurface(void* native_window);

    // pass state
    VkImageObj* m_pass_color = nullptr;
    VkImageObj* m_pass_depth = nullptr;
    VkImageObj* m_suspended_color = nullptr;
    VkImageObj* m_suspended_depth = nullptr;
    bool m_suspended = false;
    VkImageObj* m_bound_color = nullptr;   // the "bound framebuffer" (kept after EndPass)
    VkImageObj* m_bound_depth = nullptr;
    VkViewport  m_viewport = { 0, 0, 1, 1, 0, 1 };
    bool        m_scissor_on = false;
    VkRect2D    m_scissor = { { 0, 0 }, { 1, 1 } };
    void ApplyDynamic();

    std::unordered_map<uint32_t, VkSampler> m_samplers;

    uint32_t FindMemoryType(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid = 0);
    void StagingCopy(VkCommandBuffer cmd, std::vector<VkBufferObj>* temps, VkImageObj& img, int32_t x, int32_t y,
                     int32_t layer, int32_t w, int32_t h, const void* data, uint32_t row_pitch_bytes,
                     uint32_t bytes_per_pixel);
    void RunGarbage(Frame& f);
};

/** Before the window exists: is there a Vulkan GPU the renderer can use? Fills `reason` when not, `device` when so. */
bool VkCore_Probe(std::string& reason, std::string& device);

/** Barrier helper for one image (all mips/layers). */
void VkCore_ImageBarrier(VkCommandBuffer cmd, VkImageObj& img, VkImageLayout from, VkImageLayout to,
                         VkPipelineStageFlags src_stage, VkAccessFlags src_access,
                         VkPipelineStageFlags dst_stage, VkAccessFlags dst_access);

#endif // RENDERER_VULKAN_VKCORE_H
