/******************************************************************************/
// Endless Keeper - Vulkan renderer
/******************************************************************************/
/** @file VkUIRenderer.h
 *     The UI, text and sprite atlas for Vulkan: the OpenGL classes' game-thread halves (IR capture, quad
 *     building, layout, atlas packing) are inherited unchanged; only the draws and uploads are Vulkan.
 */
/******************************************************************************/
#ifndef RENDERER_VULKAN_VKUIRENDERER_H
#define RENDERER_VULKAN_VKUIRENDERER_H

#include "kfx/renderer/opengl/GLUIRenderer.h"
#include "kfx/renderer/opengl/GLTextRenderer.h"
#include "kfx/renderer/opengl/GLSpriteAtlas.h"
#include "kfx/renderer/vulkan/VkResourceMapper.h"

class VkSpriteAtlas : public GLSpriteAtlas {
protected:
    void flush_dirty(unsigned int tex_id) override;
};

class VkUIRenderer : public GLUIRenderer {
public:
    bool Init() override;
    void Shutdown() override;
    void DrawWorldSpriteLayerRT() override;
    void DrawWorldOverlayFlatLayerRT() override;
    const char* GetName() const override { return "VK-UI"; }

    /** Render thread: sets the scissor used by the following quad draws (text clip). -1 w = none. */
    void SetClip(int x, int y, int w, int h);

protected:
    void draw_textured_quad(GpuResourceHandle shader_handle, float x, float y, float w, float h,
                            float u0, float v0, float u1, float v1,
                            float r, float g, float b, float a,
                            float remap_row = -1.0f) override;
    void draw_solid_quad(float x, float y, float w, float h, float r, float g, float b, float a) override;
    void ensure_clut_valid() override;
    int resolve_clut_row(const unsigned char* cmap) override;
    void FlushQuadLayer(std::vector<UIQuad>& quads, bool depth_test) override;
    void FlushQuadRun(const std::vector<UIQuad>& run, PassType pass, int remap_row) override;
    void DrawGameUIQuadsInterleaved(std::vector<UIQuad>& quads, const TextCommandBuffers& text,
                                    GLTextRenderer* text_renderer) override;
    void FlushPendingSlabUpload() override;
    bool UploadMinimap(int slot, int size) override;

private:
    VkResourceMapper* vk() const { return static_cast<VkResourceMapper*>(m_resource_mapper); }
    struct Impl;
    Impl* m_vk = nullptr;
    bool m_depth_test = false;
};

class VkTextRenderer : public GLTextRenderer {
public:
    void DrawGlyphs(const IRTextDrawCmd& cmd, const TextCommandBuffers& text) override;
    const char* GetName() const override { return "VK-TEXT"; }
};

#endif // RENDERER_VULKAN_VKUIRENDERER_H
