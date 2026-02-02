#version 330 core

layout (location = 0) out vec3 gPos;
layout (location = 1) out vec3 gNormal;
layout (location = 2) out vec4 gAlbedoSpec;

in VS_OUT {
	vec3 FragPos;
	vec3 Normal;
	vec2 TexCoords;
	mat3 TBN;
} fs_in;

uniform sampler2D texture_diffuse1;
uniform sampler2D texture_specular1;
uniform sampler2D texture_normal1;

uniform bool hasNormalMap;
uniform bool hasSpecularMap;


void main()
{
	gPos = fs_in.FragPos;
	if(hasNormalMap){
		gNormal = texture(texture_normal1,fs_in.TexCoords).rgb;
		gNormal = normalize(gNormal * 2.0 - 1.0);  
		gNormal = normalize(fs_in.TBN * gNormal);
	}
	else{
		gNormal = normalize(fs_in.Normal);
	}
	// and the diffuse per-fragment color
	gAlbedoSpec.rgb = texture(texture_diffuse1, fs_in.TexCoords).rgb;
	// store specular intensity in gAlbedoSpec's alpha component
	if(!hasSpecularMap)
		gAlbedoSpec.a = 1.0;
	else
	gAlbedoSpec.a = texture(texture_specular1, fs_in.TexCoords).r;
}