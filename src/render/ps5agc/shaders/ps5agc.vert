#version 450

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec2 in_texcoord;

out gl_PerVertex {
    vec4 gl_Position;
};

layout(location = 0) out vec4 color;
layout(location = 1) out vec2 texcoord;

void main()
{
    gl_Position = vec4(in_position, 0.0, 1.0);
    color = in_color;
    texcoord = in_texcoord;
}
