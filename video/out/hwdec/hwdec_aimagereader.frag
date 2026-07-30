#version 450

layout(set = 0, binding = 0) uniform sampler2D source_image;
layout(location = 0) out vec4 target_color;

layout(push_constant) uniform PushConstants {
    vec2 uv_offset;
    vec2 uv_scale;
    ivec2 output_size;
} params;

void main()
{
    ivec2 position = ivec2(gl_FragCoord.xy);
    // The imported AHardwareBuffer extent is authoritative for external-format
    // images and avoids relying on driver image-size queries.
    vec2 uv = params.uv_offset +
              (vec2(position) + vec2(0.5)) * params.uv_scale;
    target_color = texture(source_image, uv);
}
