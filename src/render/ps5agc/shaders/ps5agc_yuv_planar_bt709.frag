#version 450

layout(location = 0) in vec4 color;
layout(location = 1) in vec2 texcoord;
layout(set = 0, binding = 0) uniform sampler2D y_image;
layout(set = 0, binding = 1) uniform sampler2D u_image;
layout(set = 0, binding = 2) uniform sampler2D v_image;
layout(location = 0) out vec4 output_color;

void main()
{
    float y = 1.16438356 * (texture(y_image, texcoord).r - 16.0 / 255.0);
    float u = texture(u_image, texcoord).r - 128.0 / 255.0;
    float v = texture(v_image, texcoord).r - 128.0 / 255.0;
    vec3 rgb = vec3(y + 1.79274107 * v,
                    y - 0.21324861 * u - 0.53290933 * v,
                    y + 2.11240179 * u);
    output_color = color * vec4(rgb, 1.0);
}
