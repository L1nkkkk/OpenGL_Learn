#version 330 core
layout (location = 0) out vec4 FragColor;
layout (location = 1) out vec4 BrightColor;

uniform vec3 Color;

void main()
{
	FragColor = vec4(Color, 1.0);
	BrightColor = vec4(0,0,0,1);
}