#version 450
// Vertices as the executor hands them over (nv2a_backend.h): binding 0 is
// Nv2aVertex -- position in surface pixels, z as the title produced it, 1/w,
// diffuse 0xAARRGGBB (bound as B8G8R8A8_UNORM, so .rgba arrives in order),
// texel UVs; binding 1 is Nv2aVertexExt -- four texture-coordinate sets,
// specular, fog factor. Batches without the extension bind one zeroed
// element with stride 0.

layout(location = 0) in vec4 in_pos;      // x, y, z, rhw
layout(location = 1) in vec4 in_diffuse;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_tex0;
layout(location = 4) in vec4 in_tex1;
layout(location = 5) in vec4 in_tex2;
layout(location = 6) in vec4 in_tex3;
layout(location = 7) in vec4 in_specular;
layout(location = 8) in float in_fog;

layout(push_constant) uniform PC {
    vec2  surface_size;   // real (anti-aliased) pixels
    vec2  tex_size;       // texels of stage 0; (1,1) when untextured
    float z_scale;        // 1 / depth-buffer maximum (Z24: 1/16777215)
    uint  flags;          // see nv2a.frag
} pc;

layout(location = 0) out vec4 v_diffuse;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out vec4 v_tex0;
layout(location = 3) out vec4 v_tex1;
layout(location = 4) out vec4 v_tex2;
layout(location = 5) out vec4 v_tex3;
layout(location = 6) out vec4 v_specular;
layout(location = 7) out float v_fog;

void main()
{
    // Surface pixels have y down, as Vulkan's framebuffer does.
    vec2 ndc = in_pos.xy / pc.surface_size * 2.0 - 1.0;
    // w = 1/rhw restores the clip-space w the title projected with, so
    // colour and UVs interpolate perspective-correctly; pre-transformed
    // vertices carry rhw = 1.
    float w = in_pos.w > 0.0 ? 1.0 / in_pos.w : 1.0;
    gl_Position = vec4(ndc * w, in_pos.z * pc.z_scale * w, w);
    v_diffuse = in_diffuse;
    v_uv = in_uv / pc.tex_size;
    v_tex0 = in_tex0;
    v_tex1 = in_tex1;
    v_tex2 = in_tex2;
    v_tex3 = in_tex3;
    v_specular = in_specular;
    v_fog = in_fog;
}
