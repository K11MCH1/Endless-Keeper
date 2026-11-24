#version 450
// Endless Keeper: the frame drawn onto an Android swapchain image that is pre-rotated to the panel.
layout(push_constant) uniform PC {
    vec4 rect;   // the picture's rect in window-oriented clip space (x0, y0, x1, y1; y down)
    int  rot;    // quarter turns the panel needs (1 = ROTATE_90, 2 = ROTATE_180, 3 = ROTATE_270)
} pc;
layout(location = 0) out vec2 v_uv;
void main()
{
    // a strip of 4: (0,0) (1,0) (0,1) (1,1), top-left first in the window's orientation
    vec2 t = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));
    vec2 p = mix(pc.rect.xy, pc.rect.zw, t);
    if (pc.rot == 1)      p = vec2(-p.y, p.x);
    else if (pc.rot == 2) p = -p;
    else if (pc.rot == 3) p = vec2(p.y, -p.x);
    gl_Position = vec4(p, 0.0, 1.0);
    // the screen image is stored bottom row first (OpenGL's order)
    v_uv = vec2(t.x, 1.0 - t.y);
}
