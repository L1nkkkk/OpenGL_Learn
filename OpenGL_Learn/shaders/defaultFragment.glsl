#version 330 core

layout (location = 0) out vec4 FragColor;
layout (location = 1) out vec4 BrightColor;

in vec2 TexCoords;

struct Material{
	vec3 color;
};

uniform Material material;


void main()
{
	FragColor = vec4(material.color,1.0);
	BrightColor = vec4(0,0,0,1);
}
