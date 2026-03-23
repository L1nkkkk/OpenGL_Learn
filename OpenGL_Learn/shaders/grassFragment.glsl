#version 330 core
layout (location = 0) out vec4 FragColor;
layout (location = 1) out vec4 BrightColor;
layout (location = 2) out vec4 NormalOut;

struct Material {
	sampler2D texture_diffuse1;
};

in vec2 TexCoords;
in vec3 NormalVS;

uniform Material material;

void main()
{   
	vec4 color = texture(material.texture_diffuse1, TexCoords);
	if(color.a < 0.99) FragColor = color;
	else FragColor = vec4(color.rgb,1.0);
	BrightColor = vec4(0,0,0,1);
	NormalOut = vec4(normalize(NormalVS) * 0.5 + 0.5, 1.0);
}