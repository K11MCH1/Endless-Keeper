/******************************************************************************/
// Endless Keeper - Vulkan renderer
/******************************************************************************/
/** @file VkResourceMapper.cpp
 *     The OpenGL resource mapper's handles realised as Vulkan images.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "kfx/renderer/vulkan/VkResourceMapper.h"
#include "bflib_basics.h"
#include "globals.h"
#include "post_inc.h"

VkFormat VkResourceMapper::ToVkFormat(GpuTextureFormat f, VkFormat depth_format)
{
    switch (f)
    {
    case GpuTextureFormat::R8:              return VK_FORMAT_R8_UNORM;
    case GpuTextureFormat::RGBA8:           return VK_FORMAT_R8G8B8A8_UNORM;
    case GpuTextureFormat::RGBA16F:         return VK_FORMAT_R16G16B16A16_SFLOAT;
    case GpuTextureFormat::Depth24Stencil8: return depth_format;
    case GpuTextureFormat::R16UI:           return VK_FORMAT_R16_UINT;
    case GpuTextureFormat::RG16UI:          return VK_FORMAT_R16G16_UINT;
    case GpuTextureFormat::RG8:             return VK_FORMAT_R8G8_UNORM;
    }
    return VK_FORMAT_R8G8B8A8_UNORM;
}

uint32_t VkResourceMapper::BytesPerPixel(GpuTextureFormat f)
{
    switch (f)
    {
    case GpuTextureFormat::R8:      return 1;
    case GpuTextureFormat::RGBA8:   return 4;
    case GpuTextureFormat::RGBA16F: return 8;
    case GpuTextureFormat::R16UI:   return 2;
    case GpuTextureFormat::RG16UI:  return 4;
    case GpuTextureFormat::RG8:     return 2;
    default:                        return 4;
    }
}

static bool is_integer_format(GpuTextureFormat f)
{
    return f == GpuTextureFormat::R16UI || f == GpuTextureFormat::RG16UI;
}

unsigned int VkResourceMapper::AddImage(const VkImageObj& img)
{
    if (!m_free_ids.empty())
    {
        const unsigned int id = m_free_ids.back();
        m_free_ids.pop_back();
        m_images[id - 1] = img;
        return id;
    }
    m_images.push_back(img);
    return (unsigned int)m_images.size();
}

void VkResourceMapper::FreeImage(unsigned int id)
{
    if (id == 0 || id > m_images.size())
        return;
    if (m_core != nullptr)
        m_core->DestroyImage(m_images[id - 1]);
    m_images[id - 1] = VkImageObj();
    m_free_ids.push_back(id);
}

VkImageObj* VkResourceMapper::Image(unsigned int id)
{
    if (id == 0 || id > m_images.size() || m_images[id - 1].image == VK_NULL_HANDLE)
        return nullptr;
    return &m_images[id - 1];
}

VkImageObj* VkResourceMapper::TextureImage(GpuResourceHandle tex)
{
    if (tex == kInvalidGpuResource)
        return nullptr;
    const GLTexture* t = ResolveTexture(tex);
    return t ? Image(t->id) : nullptr;
}

VkImageObj* VkResourceMapper::TargetColor(GpuResourceHandle rt, int attachment)
{
    if (rt == kInvalidGpuResource)
        return nullptr;
    const GLRenderTarget* t = ResolveRenderTarget(rt);
    if (t == nullptr || attachment < 0 || attachment >= (int)t->color_attachments.size())
        return nullptr;
    return Image(t->color_attachments[attachment]);
}

VkImageObj* VkResourceMapper::TargetDepth(GpuResourceHandle rt)
{
    if (rt == kInvalidGpuResource)
        return nullptr;
    const GLRenderTarget* t = ResolveRenderTarget(rt);
    return t ? Image(t->depth_stencil_attachment) : nullptr;
}

void VkResourceMapper::BindScreen()
{
    if (m_core == nullptr)
        return;
    VkImageObj* color = ScreenColor();
    if (color == nullptr)
        return;
    if (m_core->InPass() && m_core->PassColor() == color)
        return;
    m_core->BeginPass(color, ScreenDepth(), nullptr, false);
}

bool VkResourceMapper::RealizeTexture(const GpuTextureDesc& desc, GLTexture& out)
{
    if (m_core == nullptr)
        return false;
    VkImageObj img;
    const VkFormat fmt = ToVkFormat(desc.format, m_core->depth_format);
    const bool array = desc.array_layers > 1;
    if (!m_core->CreateImage(img, desc.width, desc.height, desc.array_layers, fmt,
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                             desc.debug_name, array))
        return false;
    const bool linear = !is_integer_format(desc.format)
        && (desc.mag_filter == GpuTextureFilter::Linear || desc.min_filter == GpuTextureFilter::Linear);
    img.sampler = m_core->GetSampler(linear, desc.wrap == GpuTextureWrap::Repeat);
    m_core->InitSampledImage(img);
    const uint32_t bpp = BytesPerPixel(desc.format);
    const size_t layer_bytes = (size_t)desc.width * desc.height * bpp;
    if (!desc.initial_pixels.empty())
    {
        for (int32_t l = 0; l < desc.array_layers; l++)
        {
            if (desc.initial_pixels.size() < layer_bytes * (size_t)(l + 1))
                break;
            m_core->UploadImage(img, 0, 0, l, desc.width, desc.height, desc.initial_pixels.data() + layer_bytes * l,
                                (uint32_t)(desc.width * bpp), bpp);
        }
    }
    out.id = AddImage(img);
    out.target = array ? 0x8C1A /*GL_TEXTURE_2D_ARRAY*/ : 0x0DE1 /*GL_TEXTURE_2D*/;
    out.width = desc.width;
    out.height = desc.height;
    out.array_layers = desc.array_layers;
    return true;
}

bool VkResourceMapper::RealizeRenderTarget(const GpuRenderTargetDesc& desc, GLRenderTarget& out)
{
    if (m_core == nullptr)
        return false;
    out = GLRenderTarget();
    for (const GpuRenderTargetAttachmentDesc& a : desc.attachments)
    {
        VkImageObj img;
        const VkFormat fmt = ToVkFormat(a.format, m_core->depth_format);
        const VkImageUsageFlags usage = a.is_depth_stencil
            ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
            : (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
               | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        if (!m_core->CreateImage(img, desc.width, desc.height, 1, fmt, usage, desc.debug_name))
        {
            DestroyRenderTarget(out);
            return false;
        }
        img.sampler = m_core->GetSampler(false, false);
        const unsigned int id = AddImage(img);
        if (a.is_depth_stencil)
            out.depth_stencil_attachment = id;
        else
            out.color_attachments.push_back(id);
    }
    out.fbo = out.color_attachments.empty() ? out.depth_stencil_attachment : out.color_attachments[0];
    out.width = desc.width;
    out.height = desc.height;
    return true;
}

bool VkResourceMapper::RealizeProgram(const GpuProgramDesc& desc, GLProgram& out)
{
    // The Vulkan passes build their own pipelines from SPIR-V; a GLSL program asked for by shared code is unused.
    SYNCDBG(7, "Vulkan: GLSL program '%s' not realised (Vulkan pipelines are made by the passes)", desc.debug_name);
    out.id = 0;
    return true;
}

bool VkResourceMapper::RealizeGeometryBuffer(const GpuGeometryBufferDesc& desc, GLGeometryBuffer& out)
{
    (void)desc;
    out = GLGeometryBuffer();
    return true;
}

void VkResourceMapper::DestroyTexture(GLTexture& texture)
{
    FreeImage(texture.id);
    texture.id = 0;
}

void VkResourceMapper::DestroyRenderTarget(GLRenderTarget& target)
{
    for (unsigned int id : target.color_attachments)
        FreeImage(id);
    target.color_attachments.clear();
    FreeImage(target.depth_stencil_attachment);
    target.depth_stencil_attachment = 0;
    target.fbo = 0;
}

void VkResourceMapper::DestroyProgram(GLProgram& program)
{
    program.id = 0;
}

void VkResourceMapper::DestroyGeometryBuffer(GLGeometryBuffer& buffer)
{
    buffer = GLGeometryBuffer();
}
