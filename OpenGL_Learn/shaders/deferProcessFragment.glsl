#version 330 core

layout (location = 0) out vec3 gPos;
layout (location = 1) out vec3 gNormal;
layout (location = 2) out vec3 gAlbedoSpec;
layout (location = 3) out vec4 gMaterial;

in VS_OUT {
	vec3 FragPos;
	vec3 Normal;
	vec2 TexCoords;
	mat3 TBN;
} fs_in;

struct Material{
	vec3 ambient;           // Ka：环境光反射系数
    vec3 diffuse;           // Kd：漫反射基础色
    vec3 specular;          // Ks：镜面反射系数
    float shininess;        // Ns：高光指数
    float opacity;          // d：透明度（1=不透明）
};

uniform sampler2D texture_diffuse1;
uniform sampler2D texture_specular1;
uniform sampler2D texture_normal1;

uniform bool hasNormalMap;
uniform bool hasSpecularMap;
uniform bool hasDiffuseMap;

uniform Material material;

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
	if(!hasDiffuseMap){
		gAlbedoSpec = vec3(0,0,0);
	}
	else{
		gAlbedoSpec = texture(texture_diffuse1, vec2(fs_in.TexCoords)).rgb;
	}

	// store material properties
	gMaterial.r = material.ambient.r;
	gMaterial.g = material.diffuse.r;
	gMaterial.b = material.specular.r;
	gMaterial.a = material.shininess;
}