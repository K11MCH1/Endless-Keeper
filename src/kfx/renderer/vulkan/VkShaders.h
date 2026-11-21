/******************************************************************************/
// Endless Keeper - Vulkan renderer
/******************************************************************************/
/** @file VkShaders.h
 *     The OpenGL renderer's shader programs compiled to SPIR-V (VkShaders_gen.cpp, made by
 *     tools/vulkan/gen_shaders.py from GLShaders.h), with their uniform offsets, samplers and inputs.
 */
/******************************************************************************/
#ifndef RENDERER_VULKAN_VKSHADERS_H
#define RENDERER_VULKAN_VKSHADERS_H

#include <cstddef>
#include <cstdint>

struct VkShaderUniform { const char* name; uint32_t offset; uint32_t size; };
/** kind: 0 float 2D, 2 uint 2D, 3 int 2D; +4 for a 2D array. */
struct VkShaderSampler { const char* name; uint32_t binding; uint32_t kind; };
struct VkShaderInput   { const char* name; uint32_t location; };

struct VkShaderProgramInfo {
    const char*            name;
    const uint32_t*        vs;
    size_t                 vs_size;
    const uint32_t*        fs;
    size_t                 fs_size;
    uint32_t               ubo_size;     // 0 = no uniforms
    int32_t                ubo_binding;
    const VkShaderUniform* uniforms;     // null-name terminated
    const VkShaderSampler* samplers;
    const VkShaderInput*   inputs;
};

extern const VkShaderProgramInfo g_vk_shader_programs[];

/** The program by its generator name ("ui_sprite", "world", ...), or null. */
const VkShaderProgramInfo* VkShaders_Find(const char* name);

#endif // RENDERER_VULKAN_VKSHADERS_H
