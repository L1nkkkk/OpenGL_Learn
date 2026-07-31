#version 330 core
layout (location = 0) out vec4 FragColor;
layout (location = 1) out vec4 BrightColor;
layout (location = 2) out vec4 NormalOut;

const int MAX_POINT_LIGHTS = 16;
const int MAX_DIR_LIGHTS = 16;
const int MAX_SPOT_LIGHTS = 16;

struct DirLight {
	vec3 direction;
	vec3 ambient;
	vec3 diffuse;
	vec3 specular;
	sampler2D shadowMap;
	sampler2DShadow shadowCompareMap;
	bool useShadowMap;
	mat4 lightSpaceMatrix;
	vec4 shadowBiasParams;
	bool isActive;
};

struct PointLight {
	vec3 position;
	float constant;
	float linear;
	float quadratic;
	vec3 ambient;
	vec3 diffuse;
	vec3 specular;
	float far_plane;
	samplerCube shadowCubeMap;
	bool useShadowMap;
	bool isActive;
};

struct SpotLight {
	vec3 position;
	vec3 direction;
	float cutOff;
	float outerCutOff;
	float constant;
	float linear;
	float quadratic;
	vec3 ambient;
	vec3 diffuse;
	vec3 specular;
	sampler2D shadowMap;
	sampler2DShadow shadowCompareMap;
	bool useShadowMap;
	mat4 lightSpaceMatrix;
	vec4 shadowBiasParams;
	bool isActive;
};

struct Material {
	vec3 ambient;
	vec3 diffuse;
	vec3 specular;
	float shininess;
	float opacity;
	float alphaCutoff;
	bool useAlphaCutoff;
	sampler2D texture_diffuse1;
	bool use_texture_diffuse;
	sampler2D texture_normal1;
	bool use_texture_normal;
	sampler2D texture_specular1;
	bool use_texture_specular;
	sampler2D texture_opacity1;
	bool use_texture_opacity;
	bool hasBloom;
};

in VS_OUT {
	vec3 FragPos;
	vec3 Normal;
	vec2 TexCoords;
	mat3 TBN;
} fs_in;

uniform vec3 viewPos;
uniform Material material;
uniform int NR_POINT_LIGHTS;
uniform int NR_DIR_LIGHTS;
uniform int NR_SPOT_LIGHTS;
uniform PointLight pointLights[MAX_POINT_LIGHTS];
uniform DirLight dirLights[MAX_DIR_LIGHTS];
uniform SpotLight spotLights[MAX_SPOT_LIGHTS];

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
	int shadowSamplingPattern;
	int shadowOptimizationFlags;
	int shadowAdaptiveMinSamples;
	float shadowBias2DMinTexels;
	float shadowBias2DSlopeTexels;
	float shadowBiasCubeMinTexels;
	float shadowBiasCubeSlopeTexels;
};

layout(std140) uniform Matrices {
	mat4 view;
	mat4 projection;
};

#include "shadowSampling.glsl"

vec3 LinearToSrgb(vec3 value)
{
	vec3 low = value * 12.92;
	vec3 high =
		1.055 * pow(max(value, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
	return mix(low, high, step(vec3(0.0031308), value));
}

vec4 SampleColorTexture(sampler2D source, vec2 uv)
{
	vec4 sampleValue = texture(source, uv);
	if (!useGamma) sampleValue.rgb = LinearToSrgb(sampleValue.rgb);
	return sampleValue;
}

float DirectionalShadow(DirLight light, vec3 normal, vec3 fragPos)
{
	if (!light.useShadowMap) return 0.0;
	float nDotL = max(
		dot(normalize(normal), normalize(-light.direction)),
		0.0);
	return SampleShadow2D(
		light.shadowMap,
		light.shadowCompareMap,
		light.lightSpaceMatrix * vec4(fragPos, 1.0),
		nDotL,
		light.shadowBiasParams);
}

float PointShadow(PointLight light, vec3 normal, vec3 fragPos)
{
	if (!light.useShadowMap) return 0.0;
	return SampleShadowCube(
		light.shadowCubeMap,
		fragPos - light.position,
		normal,
		light.far_plane);
}

float SpotShadow(SpotLight light, vec3 normal, vec3 fragPos)
{
	if (!light.useShadowMap) return 0.0;
	vec3 biasLightDirection = ShadowOptimizationEnabled(
		SHADOW_OPT_SPOT_RADIAL_BIAS_DIRECTION)
		? light.position - fragPos
		: -light.direction;
	float nDotL = max(
		dot(normalize(normal), normalize(biasLightDirection)),
		0.0);
	return SampleShadow2D(
		light.shadowMap,
		light.shadowCompareMap,
		light.lightSpaceMatrix * vec4(fragPos, 1.0),
		nDotL,
		light.shadowBiasParams);
}

vec3 CalcDirLight(
	DirLight light,
	vec3 normal,
	vec3 viewDir,
	float shadow)
{
	vec3 color =
		SampleColorTexture(material.texture_diffuse1, fs_in.TexCoords).rgb;
	vec3 lightDir = normalize(-light.direction);
	float diffuseTerm = max(dot(normal, lightDir), 0.0);
	vec3 halfwayDir = normalize(lightDir + viewDir);
	float specularTerm =
		pow(max(dot(normal, halfwayDir), 0.0), material.shininess);
	vec3 ambient = light.ambient * material.ambient;
	vec3 diffuse = light.diffuse * material.diffuse * diffuseTerm;
	vec3 specular = light.specular * material.specular * specularTerm;
	return (
		ambient +
		(1.0 - shadow) * (diffuse + specular)) *
		color;
}

vec3 CalcPointLight(
	PointLight light,
	vec3 normal,
	vec3 fragPos,
	vec3 viewDir,
	float shadow)
{
	vec3 color =
		SampleColorTexture(material.texture_diffuse1, fs_in.TexCoords).rgb;
	vec3 lightDir = normalize(light.position - fragPos);
	float diffuseTerm = max(dot(normal, lightDir), 0.0);
	vec3 halfwayDir = normalize(lightDir + viewDir);
	float specularTerm =
		pow(max(dot(normal, halfwayDir), 0.0), material.shininess);
	float distanceToLight = length(light.position - fragPos);
	float attenuation = 1.0 / (
		light.constant +
		light.linear * distanceToLight +
		light.quadratic * distanceToLight * distanceToLight);
	vec3 ambient = light.ambient * material.ambient * attenuation;
	vec3 diffuse =
		light.diffuse * material.diffuse * diffuseTerm * attenuation;
	vec3 specular =
		light.specular * material.specular * specularTerm * attenuation;
	return (
		ambient +
		(1.0 - shadow) * (diffuse + specular)) *
		color;
}

vec3 CalcSpotLight(
	SpotLight light,
	vec3 normal,
	vec3 fragPos,
	vec3 viewDir,
	float shadow)
{
	vec3 color =
		SampleColorTexture(material.texture_diffuse1, fs_in.TexCoords).rgb;
	vec3 lightDir = normalize(light.position - fragPos);
	float diffuseTerm = max(dot(normal, lightDir), 0.0);
	vec3 reflectDir = reflect(-lightDir, normal);
	float specularTerm =
		pow(max(dot(viewDir, reflectDir), 0.0), material.shininess);
	float distanceToLight = length(light.position - fragPos);
	float attenuation = 1.0 / (
		light.constant +
		light.linear * distanceToLight +
		light.quadratic * distanceToLight * distanceToLight);
	float theta = dot(lightDir, normalize(-light.direction));
	float epsilon = light.cutOff - light.outerCutOff;
	float intensity =
		clamp((theta - light.outerCutOff) / epsilon, 0.0, 1.0);
	attenuation *= intensity;
	vec3 ambient = light.ambient * material.ambient * attenuation;
	vec3 diffuse =
		light.diffuse * material.diffuse * diffuseTerm * attenuation;
	vec3 specular =
		light.specular * material.specular * specularTerm * attenuation;
	return (
		ambient +
		(1.0 - shadow) * (diffuse + specular)) *
		color;
}

void main()
{
	vec3 normal;
	if (material.use_texture_normal) {
		normal =
			texture(material.texture_normal1, fs_in.TexCoords).rgb *
			2.0 - 1.0;
		normal = normalize(fs_in.TBN * normalize(normal));
	}
	else {
		normal = normalize(fs_in.Normal);
	}
	vec3 viewDir = normalize(viewPos - fs_in.FragPos);

	float alpha = material.opacity;
	if (material.use_texture_diffuse) {
		alpha *= SampleColorTexture(
			material.texture_diffuse1,
			fs_in.TexCoords).a;
	}
	if (material.use_texture_opacity) {
		alpha *= texture(
			material.texture_opacity1,
			fs_in.TexCoords).r;
	}
	if (material.useAlphaCutoff && alpha < material.alphaCutoff) discard;

	vec3 viewSpaceNormal =
		normalize(mat3(transpose(inverse(view))) * normal);
	NormalOut = vec4(viewSpaceNormal * 0.5 + 0.5, 1.0);

	vec3 result = vec3(0.0);
	int dirLightCount = min(MAX_DIR_LIGHTS, NR_DIR_LIGHTS);
	for (int i = 0; i < dirLightCount; ++i) {
		if (!dirLights[i].isActive) continue;
		float shadow = DirectionalShadow(
			dirLights[i],
			normal,
			fs_in.FragPos);
		result += CalcDirLight(
			dirLights[i],
			normal,
			viewDir,
			shadow);
	}

	int pointLightCount = min(MAX_POINT_LIGHTS, NR_POINT_LIGHTS);
	for (int i = 0; i < pointLightCount; ++i) {
		if (!pointLights[i].isActive) continue;
		float shadow = PointShadow(
			pointLights[i],
			normal,
			fs_in.FragPos);
		result += CalcPointLight(
			pointLights[i],
			normal,
			fs_in.FragPos,
			viewDir,
			shadow);
	}

	int spotLightCount = min(MAX_SPOT_LIGHTS, NR_SPOT_LIGHTS);
	for (int i = 0; i < spotLightCount; ++i) {
		if (!spotLights[i].isActive) continue;
		float shadow = SpotShadow(
			spotLights[i],
			normal,
			fs_in.FragPos);
		result += CalcSpotLight(
			spotLights[i],
			normal,
			fs_in.FragPos,
			viewDir,
			shadow);
	}

	FragColor = vec4(result, alpha);
	float brightness = dot(
		FragColor.rgb,
		vec3(0.2126, 0.7152, 0.0722));
	BrightColor = brightness > bloomThreshold
		? vec4(FragColor.rgb, 1.0)
		: vec4(0.0, 0.0, 0.0, 1.0);
}
