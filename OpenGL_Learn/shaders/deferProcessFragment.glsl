#version 330 core

layout (location = 0) out vec4 gPos;
layout (location = 1) out vec3 gNormal;
layout (location = 2) out vec3 gAlbedoSpec;
layout (location = 3) out vec4 gMaterial;

in VS_OUT {
	vec3 FragPos;
	float ViewDepth;
	vec3 Normal;
	vec2 TexCoords;
	mat3 TBN;
} fs_in;

struct Material{
	vec3 ambient;           // Ka�������ⷴ��ϵ��
    vec3 diffuse;           // Kd�����������ɫ
    vec3 specular;          // Ks�����淴��ϵ��
    float shininess;        // Ns���߹�ָ��
    float opacity;          // d��͸���ȣ�1=��͸����
	float alphaCutoff;
	bool useAlphaCutoff;
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
	// Store linear view-space depth in alpha; >0 means valid geometry pixel.
	gPos = vec4(fs_in.FragPos, max(fs_in.ViewDepth, 1e-6));
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
		// Fallback to material diffuse color when no texture is bound.
		gAlbedoSpec = material.diffuse;
	}
	else{
		vec4 diffuseSample = texture(texture_diffuse1, vec2(fs_in.TexCoords));
		// Material-driven cutout for cloth/card-like geometry.
		float cutoff = material.useAlphaCutoff ? material.alphaCutoff : 0.1;
		if (diffuseSample.a < cutoff) {
			discard;
		}
		gAlbedoSpec = diffuseSample.rgb;
	}

	// Store scalar factors for deferred lighting (use channel average instead of only .r).
	gMaterial.r = (material.ambient.r + material.ambient.g + material.ambient.b) / 3.0;
	gMaterial.g = (material.diffuse.r + material.diffuse.g + material.diffuse.b) / 3.0;
	gMaterial.b = (material.specular.r + material.specular.g + material.specular.b) / 3.0;
	gMaterial.a = material.shininess;
}