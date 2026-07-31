#version 330 core

layout (location = 0) in vec3 aPos;
layout (location = 2) in vec2 aTexCoords;

uniform mat4 model;
uniform mat4 shadowMatrix;

out vec4 FragPos;
out vec2 TexCoords;

void main()
{
    FragPos = model * vec4(aPos, 1.0);
    TexCoords = aTexCoords;
    gl_Position = shadowMatrix * FragPos;
}
