/******************************************************************************/
// Endless Keeper - Vulkan renderer
/******************************************************************************/
/** @file VkWorldViewRenderer.h
 *     The 3D world for Vulkan. The OpenGL world renderer's game-thread half (engine buckets -> tile vertices,
 *     keeper-sprite instances, shadows, lens commands) is inherited unchanged; the draws are Vulkan.
 */
/******************************************************************************/
#ifndef RENDERER_VULKAN_VKWORLDVIEWRENDERER_H
#define RENDERER_VULKAN_VKWORLDVIEWRENDERER_H

#include "kfx/renderer/opengl/GLWorldViewRenderer.h"
#include "kfx/renderer/vulkan/VkResourceMapper.h"

class VkWorldViewRenderer : public GLWorldViewRenderer {
public:
    ~VkWorldViewRenderer() override;
    const char* GetName() const override { return "VK-World"; }

    void RefreshFadeTableAndSettings() override;
    bool BeginLensCapture(const float* clear_rgba) override;
    void ResolveLensComposite() override;
    void DrawCursorKeeperSprites() override;

protected:
    bool init_gl_resources() override;
    void free_gl_resources() override;
    bool compile_world_shaders() override;
    void push_shade_uniforms() override;
    void gpu_execute_passes(int vp_x, int vp_y_gl, int screen_w, int screen_h,
                            const std::vector<WorldVertex>& tile_verts) override;
    bool init_keeper_sprite_shader() override;
    bool init_keeper_sprite_instancing() override;
    void execute_preload_atlas() override;
    void ensure_clut_valid() override;
    int resolve_atlas_layer(int32_t sprite_id, const unsigned char* data, int src_w, int src_h,
                            bool* darkens = nullptr) override;
    float resolve_clut_v(const unsigned char* remap) override;
    void flush_keeper_sprite_instances() override;
    int render_keepersprite_gpu(const IRWorldKeeperSpriteCmd& cmd) override;
    bool init_lens_shaders() override;
    void upload_lens_textures_if_dirty() override;
    bool init_shadow_shader() override;
    void draw_shadows_gpu() override;

private:
    VkResourceMapper* vk() const { return static_cast<VkResourceMapper*>(m_resource_mapper); }
    struct Impl;
    Impl* m_vk = nullptr;
};

#endif // RENDERER_VULKAN_VKWORLDVIEWRENDERER_H
