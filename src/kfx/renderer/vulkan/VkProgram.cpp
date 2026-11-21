/******************************************************************************/
// Endless Keeper - Vulkan renderer
/******************************************************************************/
/** @file VkProgram.cpp
 *     The OpenGL renderer's shader programs on Vulkan.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "kfx/renderer/vulkan/VkProgram.h"
#include "bflib_basics.h"
#include "globals.h"
#include <cstring>
#include "post_inc.h"

const VkShaderProgramInfo* VkShaders_Find(const char* name)
{
    for (const VkShaderProgramInfo* p = g_vk_shader_programs; p->name != nullptr; p++)
        if (strcmp(p->name, name) == 0)
            return p;
    return nullptr;
}

/******************************************************************************/
/* dummies                                                                    */
/******************************************************************************/

static VkImageObj s_dummy[8];

VkImageObj* VkProgram_DummyImage(VkCore* core, uint32_t kind)
{
    kind &= 7;
    VkImageObj& d = s_dummy[kind];
    if (d.image != VK_NULL_HANDLE)
        return &d;
    const uint32_t base = kind & 3;
    const VkFormat fmt = (base == 2) ? VK_FORMAT_R16G16B16A16_UINT
                       : (base == 3) ? VK_FORMAT_R16G16B16A16_SINT : VK_FORMAT_R8G8B8A8_UNORM;
    if (!core->CreateImage(d, 1, 1, 1, fmt, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           "dummy", (kind & 4) != 0))
        return nullptr;
    d.sampler = core->GetSampler(false, false);
    core->InitSampledImage(d);
    return &d;
}

void VkProgram_FreeDummies(VkCore* core)
{
    for (VkImageObj& d : s_dummy)
        core->DestroyImage(d);
}

/******************************************************************************/
/* program                                                                    */
/******************************************************************************/

bool VkProgram::Create(VkCore* core, const char* program_name)
{
    Destroy();
    m_core = core;
    m_info = VkShaders_Find(program_name);
    if (m_info == nullptr)
    {
        ERRORLOG("Vulkan: no shader program '%s'", program_name);
        return false;
    }
    m_nuniforms = 0;
    while (m_info->uniforms[m_nuniforms].name != nullptr) m_nuniforms++;
    m_nsamplers = 0;
    while (m_info->samplers[m_nsamplers].name != nullptr) m_nsamplers++;
    m_ubo.assign(m_info->ubo_size, 0);
    m_tex.assign(m_nsamplers, Tex());

    std::vector<VkDescriptorSetLayoutBinding> b;
    if (m_info->ubo_size > 0)
    {
        VkDescriptorSetLayoutBinding u = {};
        u.binding = (uint32_t)m_info->ubo_binding;
        u.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        u.descriptorCount = 1;
        u.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        b.push_back(u);
    }
    for (int i = 0; i < m_nsamplers; i++)
    {
        VkDescriptorSetLayoutBinding s = {};
        s.binding = m_info->samplers[i].binding;
        s.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        s.descriptorCount = 1;
        s.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        b.push_back(s);
    }
    m_set_layout = core->CreateSetLayout(b);
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &m_set_layout;
    if (m_set_layout == VK_NULL_HANDLE || vkCreatePipelineLayout(core->device, &plci, nullptr, &m_layout) != VK_SUCCESS)
    {
        ERRORLOG("Vulkan: pipeline layout for '%s' failed", program_name);
        m_layout = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void VkProgram::Destroy()
{
    if (m_core != nullptr && m_core->device != VK_NULL_HANDLE)
    {
        for (auto& kv : m_pipelines)
            vkDestroyPipeline(m_core->device, kv.second, nullptr);
        if (m_layout) vkDestroyPipelineLayout(m_core->device, m_layout, nullptr);
        if (m_set_layout) vkDestroyDescriptorSetLayout(m_core->device, m_set_layout, nullptr);
    }
    m_pipelines.clear();
    m_layout = VK_NULL_HANDLE;
    m_set_layout = VK_NULL_HANDLE;
    m_info = nullptr;
}

uint8_t VkProgram::AddLayout(const VkVertexLayout& layout)
{
    m_layouts.push_back(layout);
    return (uint8_t)(m_layouts.size() - 1);
}

int VkProgram::Uniform(const char* name) const
{
    if (m_info == nullptr) return -1;
    for (int i = 0; i < m_nuniforms; i++)
        if (strcmp(m_info->uniforms[i].name, name) == 0)
            return i;
    return -1;
}

int VkProgram::Sampler(const char* name) const
{
    if (m_info == nullptr) return -1;
    for (int i = 0; i < m_nsamplers; i++)
        if (strcmp(m_info->samplers[i].name, name) == 0)
            return i;
    return -1;
}

void VkProgram::Set1f(int u, float x) { float v[4] = { x, 0, 0, 0 }; if (u >= 0) memcpy(&m_ubo[m_info->uniforms[u].offset], v, 4); }
void VkProgram::Set2f(int u, float x, float y) { float v[2] = { x, y }; if (u >= 0) memcpy(&m_ubo[m_info->uniforms[u].offset], v, 8); }
void VkProgram::Set3f(int u, float x, float y, float z) { float v[3] = { x, y, z }; if (u >= 0) memcpy(&m_ubo[m_info->uniforms[u].offset], v, 12); }
void VkProgram::Set4f(int u, float x, float y, float z, float w) { float v[4] = { x, y, z, w }; if (u >= 0) memcpy(&m_ubo[m_info->uniforms[u].offset], v, 16); }
void VkProgram::Set1i(int u, int v) { int32_t i = v; if (u >= 0) memcpy(&m_ubo[m_info->uniforms[u].offset], &i, 4); }

void VkProgram::SetMat3(int u, const float* m9)
{
    if (u < 0) return;
    // std140: each column of a mat3 is a vec4
    uint8_t* dst = &m_ubo[m_info->uniforms[u].offset];
    for (int c = 0; c < 3; c++)
        memcpy(dst + c * 16, m9 + c * 3, 12);
}

void VkProgram::SetTexture(int s, VkImageObj* img, VkSampler sampler)
{
    if (s < 0 || s >= m_nsamplers) return;
    m_tex[s].img = img;
    m_tex[s].sampler = sampler;
}

static uint64_t fnv64(const void* data, size_t n, uint64_t h = 1469598103934665603ull)
{
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

bool VkProgram::Bind(const VkPipeState& st)
{
    if (!Valid() || !m_core->InFrame() || !m_core->EnsurePass())
        return false;
    // textures sampled here must be in SHADER_READ_ONLY_OPTIMAL (render targets drawn into earlier this frame)
    for (int i = 0; i < m_nsamplers; i++)
    {
        VkImageObj* img = m_tex[i].img;
        if (img != nullptr && img->image != VK_NULL_HANDLE && img->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        {
            if (img == m_core->PassColor() || img == m_core->PassDepth())
            {
                ERRORLOG("Vulkan: '%s' samples the image it draws into", m_info->name);
                return false;
            }
            m_core->Transition(*img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    }
    if (!m_core->EnsurePass())
        return false;
    VkPipeState key = st;
    key.has_depth = (m_core->PassDepth() != nullptr) ? 1 : 0;
    key.rgba16f = (m_core->PassColor()->format == VK_FORMAT_R16G16B16A16_SFLOAT) ? 1 : 0;
    if (!key.has_depth) { key.depth_test = key.depth_write = 0; key.stencil = 0; }
    const uint64_t h = fnv64(&key, sizeof(key));
    VkPipeline pipe = VK_NULL_HANDLE;
    auto it = m_pipelines.find(h);
    if (it != m_pipelines.end())
        pipe = it->second;
    else
    {
        VkPipelineDesc d;
        d.vs_code = m_info->vs; d.vs_size = m_info->vs_size;
        d.fs_code = m_info->fs; d.fs_size = m_info->fs_size;
        d.layout = m_layout;
        if (key.layout_id < m_layouts.size())
        {
            const VkVertexLayout& vl = m_layouts[key.layout_id];
            if (vl.stride > 0)
                d.bindings.push_back({ 0, vl.stride, VK_VERTEX_INPUT_RATE_VERTEX });
            if (vl.instance_stride > 0)
                d.bindings.push_back({ 1, vl.instance_stride, VK_VERTEX_INPUT_RATE_INSTANCE });
            for (const VkVertexAttr& a : vl.attrs)
            {
                int loc = -1;
                for (const VkShaderInput* in = m_info->inputs; in->name != nullptr; in++)
                    if (strcmp(in->name, a.name) == 0) { loc = (int)in->location; break; }
                if (loc < 0)
                {
                    WARNLOG("Vulkan: program '%s' has no vertex input '%s'", m_info->name, a.name);
                    continue;
                }
                d.attributes.push_back({ (uint32_t)loc, a.binding, a.format, a.offset });
            }
        }
        d.topology = (VkPrimitiveTopology)key.topology;
        d.blend = key.blend != 0;
        d.src_color = (VkBlendFactor)key.src_color; d.dst_color = (VkBlendFactor)key.dst_color;
        d.src_alpha = (VkBlendFactor)key.src_alpha; d.dst_alpha = (VkBlendFactor)key.dst_alpha;
        d.color_op = (VkBlendOp)key.color_op; d.alpha_op = (VkBlendOp)key.alpha_op;
        d.write_mask = key.write_mask;
        d.depth_test = key.depth_test != 0;
        d.depth_write = key.depth_write != 0;
        d.depth_op = (VkCompareOp)key.depth_op;
        d.stencil_test = key.stencil != 0;
        VkStencilOpState so = {};
        so.failOp = (VkStencilOp)key.st_fail;
        so.passOp = (VkStencilOp)key.st_pass;
        so.depthFailOp = (VkStencilOp)key.st_zfail;
        so.compareOp = (VkCompareOp)key.st_compare;
        so.compareMask = key.st_read;
        so.writeMask = key.st_write;
        d.stencil_front = d.stencil_back = so;
        d.color_format = key.rgba16f ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
        d.has_depth = key.has_depth != 0;
        d.debug_name = m_info->name;
        pipe = m_core->CreatePipeline(d);
        m_pipelines[h] = pipe;
    }
    if (pipe == VK_NULL_HANDLE)
        return false;

    // the same pipeline, uniforms and textures as this program's last bind, with nothing bound since: keep it
    // (text draws one quad per glyph)
    if (m_last_serial == m_core->bind_serial && m_last_pipe == pipe && m_last_ubo == m_ubo)
    {
        bool same = true;
        for (int i = 0; i < m_nsamplers && same; i++)
            same = (m_last_tex[i].img == m_tex[i].img && m_last_tex[i].sampler == m_tex[i].sampler);
        if (same)
            return true;
    }

    VkDescriptorSet set = m_core->AllocSet(m_set_layout);
    if (set == VK_NULL_HANDLE)
        return false;
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorImageInfo> images(m_nsamplers);
    VkDescriptorBufferInfo bi = {};
    writes.reserve(m_nsamplers + 1);
    if (m_info->ubo_size > 0)
    {
        VkRingSlice s = m_core->RingAlloc(m_info->ubo_size, 16);
        if (s.ptr == nullptr)
            return false;
        memcpy(s.ptr, m_ubo.data(), m_info->ubo_size);
        bi.buffer = s.buffer;
        bi.offset = s.offset;
        bi.range = m_info->ubo_size;
        VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = set;
        w.dstBinding = (uint32_t)m_info->ubo_binding;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &bi;
        writes.push_back(w);
    }
    for (int i = 0; i < m_nsamplers; i++)
    {
        VkImageObj* img = m_tex[i].img;
        if (img == nullptr || img->image == VK_NULL_HANDLE)
            img = VkProgram_DummyImage(m_core, m_info->samplers[i].kind);
        if (img == nullptr)
            return false;
        images[i].imageView = img->view;
        images[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        images[i].sampler = m_tex[i].sampler ? m_tex[i].sampler : (img->sampler ? img->sampler : m_core->GetSampler(false, false));
        VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = set;
        w.dstBinding = m_info->samplers[i].binding;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &images[i];
        writes.push_back(w);
    }
    vkUpdateDescriptorSets(m_core->device, (uint32_t)writes.size(), writes.data(), 0, nullptr);
    VkCommandBuffer cmd = m_core->Cmd();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &set, 0, nullptr);
    m_core->bind_serial++;
    m_last_serial = m_core->bind_serial;
    m_last_pipe = pipe;
    m_last_ubo = m_ubo;
    m_last_tex = m_tex;
    return true;
}
