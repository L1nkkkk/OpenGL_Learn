#version 330 core
layout (location = 0) out vec4 FragColor;
layout (location = 1) out vec4 BrightColor;

in VS_OUT {
	vec3 Normal;
	vec2 TexCoords;
} fs_in;

uniform sampler2D texture_diffuse1;

void main()
{
	FragColor = texture(texture_diffuse1, fs_in.TexCoords);	
	BrightColor = vec4(0,0,0,1);
}