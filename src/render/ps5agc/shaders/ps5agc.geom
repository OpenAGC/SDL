#version 450

layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;

in gl_PerVertex {
    vec4 gl_Position;
} gl_in[];

out gl_PerVertex {
    vec4 gl_Position;
};

layout(location = 0) in vec4 in_color[];
layout(location = 1) in vec2 in_texcoord[];
layout(location = 0) out vec4 color;
layout(location = 1) out vec2 texcoord;

void main()
{
    for (int i = 0; i < 3; ++i) {
        gl_Position = gl_in[i].gl_Position;
        color = in_color[i];
        texcoord = in_texcoord[i];
        EmitVertex();
    }
    EndPrimitive();
}
