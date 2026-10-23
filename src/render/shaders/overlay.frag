#version 450
// Host overlay: the colour times the font atlas' coverage (one solid cell for rectangles).
layout(set = 0, binding = 0) uniform sampler2D font;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 o_color;

void main()
{
    o_color = vec4(v_color.rgb, v_color.a * texture(font, v_uv).r);
}
