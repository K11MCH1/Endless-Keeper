/******************************************************************************/
// Endless Keeper - Vulkan renderer
/******************************************************************************/
/** @file VkPasses.h
 *     Vulkan versions of the full-screen passes and atlases: image presents (FMV, splash, backgrounds, the
 *     landview frame and zoom), the parchment transition, zoom-box tiles, the palette index lookup and the
 *     tile atlas. Their game-thread halves are the OpenGL classes', inherited unchanged.
 */
/******************************************************************************/
#ifndef RENDERER_VULKAN_VKPASSES_H
#define RENDERER_VULKAN_VKPASSES_H

#include "kfx/renderer/opengl/GLImagePresentPass.h"
#include "kfx/renderer/opengl/GLMapFadePass.h"
#include "kfx/renderer/opengl/GLZoomBoxTilesPass.h"
#include "kfx/renderer/opengl/GLPaletteIndexLookup.h"
#include "kfx/renderer/opengl/GLTileAtlas.h"
#include "kfx/renderer/vulkan/VkResourceMapper.h"

class VkPaletteIndexLookup : public GLPaletteIndexLookup {
protected:
    void upload() override;
};

class VkTileAtlas : public GLTileAtlas {
public:
    bool Init() override;
    void Rebuild() override;
protected:
    void UploadFull(int variation) override;
    void UploadAnimatedStrip(int variation, int y_offset, int h_pixels) override;
};

class VkImagePresentPass : public GLImagePresentPass {
public:
    ~VkImagePresentPass();
    bool CompileShaders() override;
    void Shutdown() override;
    void Resolve(int screen_w, int screen_h) override;
    void ResolveOverlay(int screen_w, int screen_h) override;
protected:
    bool init_quad() override;
    void upload_overlay_texture() override;
    void upload_overlay_coverage_texture() override;
    void upload_opaque_texture(size_t idx) override;
    void upload_zoom_texture() override;
    void upload_embedded_palette(const std::vector<unsigned char>& embedded_palette) override;
    void draw_quad(GLuint program, GLuint image_tex, GLuint palette_tex,
                   int dst_x, int dst_y, int dst_w, int dst_h, int screen_w, int screen_h,
                   GLuint coverage_tex = 0) override;
private:
    VkResourceMapper* vk() const { return static_cast<VkResourceMapper*>(m_resource_mapper); }
    struct Impl;
    Impl* m_vk = nullptr;
};

class VkMapFadePass : public GLMapFadePass {
public:
    ~VkMapFadePass();
    bool CompileShaders() override;
    void Shutdown() override;
    void CaptureWorldFrame(int w, int h) override;
    void CaptureParchmentFrame(int w, int h) override;
    void ResolveComposite(int screen_w, int screen_h) override;
protected:
    bool init_quad() override;
private:
    VkResourceMapper* vk() const { return static_cast<VkResourceMapper*>(m_resource_mapper); }
    struct Impl;
    Impl* m_vk = nullptr;
};

class VkZoomBoxTilesPass : public GLZoomBoxTilesPass {
public:
    ~VkZoomBoxTilesPass();
    bool CompileShaders() override;
    void Shutdown() override;
    void Resolve(int screen_w, int screen_h) override;
private:
    VkResourceMapper* vk() const { return static_cast<VkResourceMapper*>(m_resource_mapper); }
    struct Impl;
    Impl* m_vk = nullptr;
};

#endif // RENDERER_VULKAN_VKPASSES_H
