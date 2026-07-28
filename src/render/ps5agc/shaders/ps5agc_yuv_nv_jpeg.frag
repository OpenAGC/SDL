#version 450

layout(location = 0) in vec4 color;
layout(location = 1) in vec2 texcoord;
layout(set = 0, binding = 0) uniform sampler2D y_image;
layout(set = 0, binding = 1) uniform sampler2D uv_image;
layout(location = 0) out vec4 output_color;

void main()
{
    float y = texture(y_image, texcoord).r;
    vec2 uv = texture(uv_image, texcoord).rg - vec2(128.0 / 255.0);
    vec3 rgb = vec3(y + 1.402 * uv.y,
                    y - 0.344136 * uv.x - 0.714136 * uv.y,
                    y + 1.772 * uv.x);
    output_color = color * vec4(rgb, 1.0);
}
