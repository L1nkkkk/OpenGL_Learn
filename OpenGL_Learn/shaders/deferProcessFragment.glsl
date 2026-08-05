#version 330 core

layout (location = 0) out vec4 gPosition;
layout (location = 1) out vec3 gNormal;
layout (location = 2) out vec3 gAlbedoSpec;
layout (location = 3) out vec4 gMaterial;
layout (location = 4) out vec3 gEmissive;

in VS_OUT {
	vec3 FragPos;
	float ViewDepth;
	vec3 Normal;
	vec2 TexCoords;
	mat3 TBN;
} fs_in;

struct Material {
	vec3 ambient;
	vec3 diffuse;
	vec3 specular;
	float shininess;
	vec3 albedo;
	float metallic;
	float roughness;
	float ao;
	vec3 emissive;
	float opacity;
	float alphaCutoff;
	bool useAlphaCutoff;
	bool usePBR;
	bool metallicRoughnessPacked;
	bool occlusionMapStoresOcclusion;
};

uniform sampler2D texture_diffuse1;
uniform sampler2D texture_specular1;
uniform sampler2D texture_normal1;
uniform sampler2D texture_metallic1;
uniform sampler2D texture_roughness1;
uniform sampler2D texture_ao1;
uniform sampler2D texture_emissive1;
uniform sampler2D texture_opacity1;

uniform bool hasDiffuseMap;
uniform bool hasSpecularMap;
uniform bool hasNormalMap;
uniform bool hasMetallicMap;
uniform bool hasRoughnessMap;
uniform bool hasAoMap;
uniform bool hasEmissiveMap;
uniform bool hasOpacityMap;
uniform Material material;

layout(std140) uniform SystemProperties {
	bool useBloom;
	bool useShadowMap;
	bool useGamma;
	bool useHDR;
	float bloomThreshold;
	float gamma;
	float exposure;
	int bloomBlurIterations;
	int shadowSampleNum;
	int shadowSampleRings;
	int shadowType;
	int screenWidth;
	int screenHeight;
};

vec3 LinearToSrgb(vec3 value)
{
	vec3 low = value * 12.92;
	vec3 high = 1.055 * pow(max(value, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
	return mix(low, high, step(vec3(0.0031308), value));
}

vec4 SampleLegacyColor(sampler2D source, vec2 uv)
{
	vec4 value = texture(source, uv);
	if (!useGamma) value.rgb = LinearToSrgb(value.rgb);
	return value;
}

vec3 DecodeTangentSpaceNormal(vec3 encodedNormal)
{
	if (encodedNormal.b <= (1.0 / 255.0)) {
		vec2 xy = encodedNormal.rg * 2.0 - 1.0;
		xy.y = -xy.y;
		float z = sqrt(max(1.0 - dot(xy, xy), 0.0));
		return normalize(vec3(xy, z));
	}
	return normalize(encodedNormal * 2.0 - 1.0);
}

void main()
{
	gPosition = vec4(fs_in.FragPos, max(fs_in.ViewDepth, 0.000001));
	if (hasNormalMap) {
		gNormal = normalize(fs_in.TBN * DecodeTangentSpaceNormal(
			texture(texture_normal1, fs_in.TexCoords).rgb));
	} else {
		gNormal = normalize(fs_in.Normal);
	}

	vec4 baseSample = vec4(1.0);
	if (hasDiffuseMap) {
		baseSample = material.usePBR
			? texture(texture_diffuse1, fs_in.TexCoords)
			: SampleLegacyColor(texture_diffuse1, fs_in.TexCoords);
	}
	float alpha = baseSample.a * material.opacity;
	if (hasOpacityMap) {
		alpha *= texture(texture_opacity1, fs_in.TexCoords).r;
	}
	float cutoff = material.useAlphaCutoff ? material.alphaCutoff : 0.0;
	if (alpha < cutoff) discard;

	if (!material.usePBR) {
		gAlbedoSpec = hasDiffuseMap ? baseSample.rgb : material.diffuse;
		gMaterial = vec4(
			(material.ambient.r + material.ambient.g + material.ambient.b) / 3.0,
			(material.diffuse.r + material.diffuse.g + material.diffuse.b) / 3.0,
			(material.specular.r + material.specular.g + material.specular.b) / 3.0,
			max(material.shininess, 1.0));
		gEmissive = vec3(0.0);
		return;
	}

	vec3 albedo = material.albedo * baseSample.rgb;
	float metallic = clamp(material.metallic, 0.0, 1.0);
	float roughness = clamp(material.roughness, 0.04, 1.0);
	if (material.metallicRoughnessPacked && hasMetallicMap) {
		vec3 packedSample = texture(texture_metallic1, fs_in.TexCoords).rgb;
		metallic *= packedSample.b;
		roughness *= packedSample.g;
	} else {
		if (hasMetallicMap) metallic *= texture(texture_metallic1, fs_in.TexCoords).r;
		if (hasRoughnessMap) roughness *= texture(texture_roughness1, fs_in.TexCoords).r;
	}
	float ao = material.ao;
	if (hasAoMap) {
		float occlusionSample = texture(texture_ao1, fs_in.TexCoords).r;
		ao *= material.occlusionMapStoresOcclusion
			? 1.0 - occlusionSample
			: occlusionSample;
	}
	vec3 emissive = material.emissive;
	if (hasEmissiveMap) emissive *= texture(texture_emissive1, fs_in.TexCoords).rgb;

	gAlbedoSpec = max(albedo, vec3(0.0));
	gMaterial = vec4(
		clamp(metallic, 0.0, 1.0),
		clamp(roughness, 0.04, 1.0),
		clamp(ao, 0.0, 1.0),
		-1.0);
	gEmissive = max(emissive, vec3(0.0));
}
