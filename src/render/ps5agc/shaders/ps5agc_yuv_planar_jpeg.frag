#version 450

layout(location = 0) in vec4 color;
layout(location = 1) in vec2 texcoord;
layout(set = 0, binding = 0) uniform sampler2D y_image;
layout(set = 0, binding = 1) uniform sampler2D u_image;
layout(set = 0, binding = 2) uniform sampler2D v_image;
layout(location = 0) out vec4 output_color;

void main()
{
    float y = texture(y_image, texcoord).r;
    float u = texture(u_image, texcoord).r - 128.0 / 255.0;
    float v = texture(v_image, texcoord).r - 128.0 / 255.0;
    vec3 rgb = vec3(y + 1.402 * v,
                    y - 0.344136 * u - 0.714136 * v,
                    y + 1.772 * u);
    output_color = color * vec4(rgb, 1.0);
}
