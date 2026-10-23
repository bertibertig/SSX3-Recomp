#version 450
// Host overlay (src/ui/overlay.c): window-pixel positions, font atlas UVs, RGBA colour.
layout(push_constant) uniform Push { vec2 extent; } pc;
layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

void main()
{
    gl_Position = vec4(in_pos / pc.extent * 2.0 - 1.0, 0.0, 1.0);
    v_uv = in_uv;
    v_color = in_color;
}
