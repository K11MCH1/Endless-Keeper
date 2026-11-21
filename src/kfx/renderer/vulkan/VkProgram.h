/******************************************************************************/
// Endless Keeper - Vulkan renderer
/******************************************************************************/
/** @file VkProgram.h
 *     One of the OpenGL renderer's shader programs on Vulkan, used the way the OpenGL code uses its programs:
 *     uniforms set by name (glUniform*), textures bound by sampler name (glBindTexture on a unit), then a draw.
 *     Each Bind() copies the uniforms into the frame's ring, writes a descriptor set and binds the pipeline made
 *     (once, then cached) for the fixed-function state the draw asks for.
 */
/******************************************************************************/
#ifndef RENDERER_VULKAN_VKPROGRAM_H
#define RENDERER_VULKAN_VKPROGRAM_H

#include "kfx/renderer/vulkan/VkCore.h"
#include "kfx/renderer/vulkan/VkShaders.h"
#include <unordered_map>
#include <vector>

/** The fixed-function state of a draw (OpenGL's glEnable/glBlendFunc/glDepthFunc/... at the time of the draw). */
struct VkPipeState {
    uint8_t blend       = 0;
    uint8_t src_color   = VK_BLEND_FACTOR_SRC_ALPHA;
    uint8_t dst_color   = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    uint8_t src_alpha   = VK_BLEND_FACTOR_SRC_ALPHA;
    uint8_t dst_alpha   = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    uint8_t color_op    = VK_BLEND_OP_ADD;
    uint8_t alpha_op    = VK_BLEND_OP_ADD;
    uint8_t write_mask  = 0xF;
    uint8_t depth_test  = 0;
    uint8_t depth_write = 0;
    uint8_t depth_op    = VK_COMPARE_OP_LESS;
    uint8_t stencil     = 0;
    uint8_t st_fail     = VK_STENCIL_OP_KEEP;
    uint8_t st_pass     = VK_STENCIL_OP_KEEP;
    uint8_t st_zfail    = VK_STENCIL_OP_KEEP;
    uint8_t st_compare  = VK_COMPARE_OP_ALWAYS;
    uint8_t st_read     = 0xFF;
    uint8_t st_write    = 0xFF;
    uint8_t topology    = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    uint8_t has_depth   = 1;   // the pass has the depth/stencil attachment
    uint8_t rgba16f     = 0;   // the colour attachment is RGBA16F (else RGBA8)
    uint8_t layout_id   = 0;   // which of the program's vertex layouts

    /** glBlendFunc(src, dst) for colour and alpha alike. */
    VkPipeState& Blend(VkBlendFactor s, VkBlendFactor d, VkBlendOp op = VK_BLEND_OP_ADD)
    { blend = 1; src_color = src_alpha = (uint8_t)s; dst_color = dst_alpha = (uint8_t)d; color_op = alpha_op = (uint8_t)op; return *this; }
    VkPipeState& Depth(bool test, bool write, VkCompareOp op = VK_COMPARE_OP_LESS)
    { depth_test = test; depth_write = write; depth_op = (uint8_t)op; return *this; }
};

/** A vertex buffer layout, by the shader's attribute names (OpenGL's glVertexAttribPointer calls). */
struct VkVertexAttr {
    const char* name;
    VkFormat    format;
    uint32_t    offset;
    uint32_t    binding;   // 0 = per vertex, 1 = per instance
};
struct VkVertexLayout {
    uint32_t stride = 0;            // binding 0
    uint32_t instance_stride = 0;   // binding 1 (0 = none)
    std::vector<VkVertexAttr> attrs;
};

class VkProgram {
public:
    bool Create(VkCore* core, const char* program_name);
    void Destroy();
    bool Valid() const { return m_info != nullptr && m_layout != VK_NULL_HANDLE; }
    const char* Name() const { return m_info ? m_info->name : "?"; }

    /** Adds a vertex layout; returns its id for VkPipeState::layout_id. */
    uint8_t AddLayout(const VkVertexLayout& layout);

    int  Uniform(const char* name) const;   // -1 if the program has no such (used) uniform
    int  Sampler(const char* name) const;
    void Set1f(int u, float x);
    void Set2f(int u, float x, float y);
    void Set3f(int u, float x, float y, float z);
    void Set4f(int u, float x, float y, float z, float w);
    void Set1i(int u, int v);
    void SetMat3(int u, const float* m9);
    void SetTexture(int s, VkImageObj* img, VkSampler sampler = VK_NULL_HANDLE);
    // by name, for code that sets a uniform once
    void Set1f(const char* n, float x) { Set1f(Uniform(n), x); }
    void Set2f(const char* n, float x, float y) { Set2f(Uniform(n), x, y); }
    void Set1i(const char* n, int v) { Set1i(Uniform(n), v); }
    void SetTexture(const char* n, VkImageObj* img, VkSampler sampler = VK_NULL_HANDLE) { SetTexture(Sampler(n), img, sampler); }

    /** Binds the pipeline for `state` (in the current pass) and the uniforms + textures set so far. */
    bool Bind(const VkPipeState& state);

private:
    VkCore* m_core = nullptr;
    const VkShaderProgramInfo* m_info = nullptr;
    VkDescriptorSetLayout m_set_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    std::vector<uint8_t> m_ubo;
    struct Tex { VkImageObj* img = nullptr; VkSampler sampler = VK_NULL_HANDLE; };
    std::vector<Tex> m_tex;     // by sampler index
    std::vector<VkVertexLayout> m_layouts;
    std::unordered_map<uint64_t, VkPipeline> m_pipelines;
    int m_nuniforms = 0, m_nsamplers = 0;
    // last bind (skipped when repeated unchanged)
    uint64_t m_last_serial = 0;
    VkPipeline m_last_pipe = VK_NULL_HANDLE;
    std::vector<uint8_t> m_last_ubo;
    std::vector<Tex> m_last_tex;
};

/** 1x1 textures bound where a program's sampler was given none (OpenGL samples zero from an unbound unit). */
VkImageObj* VkProgram_DummyImage(VkCore* core, uint32_t kind);
void VkProgram_FreeDummies(VkCore* core);

#endif // RENDERER_VULKAN_VKPROGRAM_H
