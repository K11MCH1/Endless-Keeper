/******************************************************************************/
// Endless Keeper - Vulkan renderer
/******************************************************************************/
/** @file VkResourceMapper.h
 *     The OpenGL resource mapper's handles, slots and deferred destroys, realised as Vulkan images.
 * A realised GLTexture / GLRenderTarget carries an index into this mapper's image table in its GLuint fields
 * (id, fbo, attachments): the shared code passes the structs around unchanged, the Vulkan passes look the
 * images up with Image().
 */
/******************************************************************************/
#ifndef RENDERER_VULKAN_VKRESOURCEMAPPER_H
#define RENDERER_VULKAN_VKRESOURCEMAPPER_H

#include "kfx/renderer/opengl/GLResourceMapper.h"
#include "kfx/renderer/vulkan/VkCore.h"
#include <deque>
#include <vector>

class VkResourceMapper : public GLResourceMapper {
public:
    void SetCore(VkCore* core) { m_core = core; }

    /** The image behind a realised texture id / attachment id (0 = none). Render thread only. */
    VkImageObj* Image(unsigned int id);
    VkImageObj* TextureImage(GpuResourceHandle tex);
    /** Colour attachment 0 / depth attachment of a render target handle. */
    VkImageObj* TargetColor(GpuResourceHandle rt, int attachment = 0);
    VkImageObj* TargetDepth(GpuResourceHandle rt);

    static VkFormat ToVkFormat(GpuTextureFormat f, VkFormat depth_format);
    static uint32_t BytesPerPixel(GpuTextureFormat f);

    VkCore* Core() const { return m_core; }

    /** The frame's screen target (OpenGL's SetScreenFramebuffer()). */
    void SetScreenTarget(GpuResourceHandle rt) { m_screen_rt = rt; }
    GpuResourceHandle GetScreenTarget() const { return m_screen_rt; }
    VkImageObj* ScreenColor() { return TargetColor(m_screen_rt); }
    VkImageObj* ScreenDepth() { return TargetDepth(m_screen_rt); }
    /** Makes the screen target the pass being drawn into (OpenGL: glBindFramebuffer(screen)). */
    void BindScreen();

protected:
    bool RealizeTexture(const GpuTextureDesc& desc, GLTexture& out) override;
    bool RealizeRenderTarget(const GpuRenderTargetDesc& desc, GLRenderTarget& out) override;
    bool RealizeProgram(const GpuProgramDesc& desc, GLProgram& out) override;
    bool RealizeGeometryBuffer(const GpuGeometryBufferDesc& desc, GLGeometryBuffer& out) override;
    void DestroyTexture(GLTexture& texture) override;
    void DestroyRenderTarget(GLRenderTarget& target) override;
    void DestroyProgram(GLProgram& program) override;
    void DestroyGeometryBuffer(GLGeometryBuffer& buffer) override;

private:
    VkCore* m_core = nullptr;
    GpuResourceHandle m_screen_rt = kInvalidGpuResource;
    std::deque<VkImageObj> m_images;    // index = id - 1; a deque: pointers stay valid as it grows
    std::vector<unsigned int> m_free_ids;
    unsigned int AddImage(const VkImageObj& img);
    void FreeImage(unsigned int id);
};

#endif // RENDERER_VULKAN_VKRESOURCEMAPPER_H
