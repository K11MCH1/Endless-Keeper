#version 450
layout(set = 0, binding = 0) uniform sampler2D u_screen;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main()
{
    o_color = vec4(texture(u_screen, v_uv).rgb, 1.0);
}
