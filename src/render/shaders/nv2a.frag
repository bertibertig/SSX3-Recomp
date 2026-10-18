#version 450
// Milestone 2: diffuse x texture stage 0, alpha test. Register combiners
// (the NV2A's pixel pipeline) replace this in milestone 3.

layout(location = 0) in vec4 v_diffuse;
layout(location = 1) in vec2 v_uv;

layout(set = 0, binding = 0) uniform sampler2D tex0;

layout(push_constant) uniform PC {
    vec2  surface_size;
    vec2  tex_size;
    float z_scale;
    uint  flags;
} pc;

layout(location = 0) out vec4 o_color;

// NV2A alpha test: GL compare functions 0x200 (NEVER) .. 0x207 (ALWAYS),
// passed as 0..7, reference 0..255.
bool alpha_pass(float a)
{
    uint func = (pc.flags >> 8) & 7u;
    float ref = float((pc.flags >> 16) & 255u) / 255.0;
    // Compare on the 8-bit grid the hardware uses, not on floats.
    float av = floor(a * 255.0 + 0.5) / 255.0;
    switch (func) {
    case 0u: return false;
    case 1u: return av <  ref;
    case 2u: return av == ref;
    case 3u: return av <= ref;
    case 4u: return av >  ref;
    case 5u: return av != ref;
    case 6u: return av >= ref;
    default: return true;
    }
}

void main()
{
    vec4 c = v_diffuse;
    if ((pc.flags & 1u) != 0u)
        c *= texture(tex0, v_uv);
    if ((pc.flags & 0x80000000u) != 0u && !alpha_pass(c.a))
        discard;
    o_color = c;
}
