#version 330 core

layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aTexCoords;

layout (location = 3) in mat4 instanceModel;

uniform mat4 model;


layout(std140) uniform Matrices {
    mat4 view;
    mat4 projection;
};

out VS_OUT{
    vec3 Normal;
    vec2 TexCoords;
} vs_out;

void main()
{
    vs_out.Normal = mat3(transpose(inverse(instanceModel))) * aNormal;
    vs_out.TexCoords = aTexCoords;
    gl_Position = projection * view * instanceModel * vec4(aPos, 1.0);
}