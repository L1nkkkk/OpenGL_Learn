#version 330 core
layout (location = 0) out vec4 FragColor;
layout (location = 1) out vec4 BrightColor;
in vec2 TexCoords;

uniform sampler2D gPosition;
uniform sampler2D gNormal;
uniform sampler2D gAlbedoSpec;
uniform sampler2D gMaterial;
uniform sampler2D ssaoMap;
uniform bool useSSAO;

vec3 albedoSpec;
vec4 material;
float gAO = 1.0;

struct DirLight {
	vec3 direction;
	vec3 ambient;
	vec3 diffuse;
	vec3 specular;
	mat4 lightSpaceMatrix;
	sampler2D shadowMap;
	sampler2DShadow shadowCompareMap;
	bool useShadowMap;
	vec4 shadowBiasParams;
};

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

#include "shadowSampling.glsl"

const int MAX_DIR_LIGHTS = 16;

uniform vec3 viewPos;
uniform int NR_DIR_LIGHTS;
uniform DirLight dirLights[MAX_DIR_LIGHTS];

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

vec3 CalcDirLight(
	DirLight light,
	vec3 normal,
	vec3 viewDir,
	float shadow)
{
	vec3 lightDir = normalize(-light.direction);
	float diffuseTerm = max(dot(normal, lightDir), 0.0);
	vec3 halfwayDir = normalize(lightDir + viewDir);
	float specularTerm =
		pow(max(dot(normal, halfwayDir), 0.0), material.a);
	vec3 ambient = light.ambient * material.r * gAO;
	vec3 diffuse = light.diffuse * material.g * diffuseTerm;
	vec3 specular = light.specular * material.b * specularTerm;
	return (
		ambient +
		(1.0 - shadow) * (diffuse + specular)) *
		albedoSpec;
}

void main()
{
	albedoSpec = texture(gAlbedoSpec, TexCoords).rgb;
	material = texture(gMaterial, TexCoords);
	gAO = useSSAO ? texture(ssaoMap, TexCoords).r : 1.0;
	vec3 normal = normalize(texture(gNormal, TexCoords).rgb);
	vec3 fragPos = texture(gPosition, TexCoords).rgb;
	vec3 viewDir = normalize(viewPos - fragPos);
	vec3 result = vec3(0.0);

	int dirLightCount = min(MAX_DIR_LIGHTS, NR_DIR_LIGHTS);
	for (int i = 0; i < dirLightCount; ++i) {
		float shadow = DirectionalShadow(
			dirLights[i],
			normal,
			fragPos);
		result += CalcDirLight(
			dirLights[i],
			normal,
			viewDir,
			shadow);
	}

	FragColor = vec4(result, 1.0);
	float brightness = dot(
		FragColor.rgb,
		vec3(0.2126, 0.7152, 0.0722));
	BrightColor = brightness > bloomThreshold
		? vec4(FragColor.rgb, 1.0)
		: vec4(0.0, 0.0, 0.0, 1.0);
}
