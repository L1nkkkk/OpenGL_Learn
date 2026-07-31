#version 330 core

layout (location = 0) out vec4 FragColor;
layout (location = 1) out vec4 BrightColor;
layout (location = 2) out vec4 NormalOut;

const float PI = 3.14159265359;
const int MAX_POINT_LIGHTS = 16;
const int MAX_DIR_LIGHTS = 16;
const int MAX_SPOT_LIGHTS = 16;
const int MAX_SHADOW_SAMPLES = 64;

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
	vec3 albedo;
	float metallic;
	float roughness;
	float ao;
	vec3 emissive;
	float opacity;
	float alphaCutoff;
	bool useAlphaCutoff;
	bool metallicRoughnessPacked;
	sampler2D texture_diffuse1;
	bool use_texture_diffuse;
	sampler2D texture_normal1;
	bool use_texture_normal;
	sampler2D texture_metallic1;
	bool use_texture_metallic;
	sampler2D texture_roughness1;
	bool use_texture_roughness;
	sampler2D texture_ao1;
	bool use_texture_ao;
	sampler2D texture_emissive1;
	bool use_texture_emissive;
	sampler2D texture_opacity1;
	bool use_texture_opacity;
};

in VS_OUT {
	vec3 FragPos;
	vec3 Normal;
	vec2 TexCoords;
	mat3 TBN;
} fs_in;

uniform Material material;
uniform vec3 viewPos;
uniform int NR_POINT_LIGHTS;
uniform int NR_DIR_LIGHTS;
uniform int NR_SPOT_LIGHTS;
uniform PointLight pointLights[MAX_POINT_LIGHTS];
uniform DirLight dirLights[MAX_DIR_LIGHTS];
uniform SpotLight spotLights[MAX_SPOT_LIGHTS];
uniform bool useIBL;
uniform samplerCube irradianceMap;
uniform samplerCube prefilterMap;
uniform sampler2D brdfLUT;

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

float DistributionGGX(vec3 normal, vec3 halfDir, float roughness)
{
	float alpha = roughness * roughness;
	float alpha2 = alpha * alpha;
	float nDotH = max(dot(normal, halfDir), 0.0);
	float denominator = nDotH * nDotH * (alpha2 - 1.0) + 1.0;
	return alpha2 / max(PI * denominator * denominator, 0.000001);
}

float GeometrySchlickGGX(float nDotV, float roughness)
{
	float r = roughness + 1.0;
	float k = (r * r) / 8.0;
	return nDotV / max(nDotV * (1.0 - k) + k, 0.000001);
}

float GeometrySmith(vec3 normal, vec3 viewDir, vec3 lightDir, float roughness)
{
	return GeometrySchlickGGX(max(dot(normal, viewDir), 0.0), roughness) *
		GeometrySchlickGGX(max(dot(normal, lightDir), 0.0), roughness);
}

vec3 FresnelSchlick(float cosTheta, vec3 f0)
{
	return f0 + (1.0 - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 FresnelSchlickRoughness(float cosTheta, vec3 f0, float roughness)
{
	return f0 + (max(vec3(1.0 - roughness), f0) - f0) *
		pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 EvaluateDirect(
	vec3 normal,
	vec3 viewDir,
	vec3 lightDir,
	vec3 radiance,
	vec3 albedo,
	float metallic,
	float roughness)
{
	vec3 halfDir = normalize(viewDir + lightDir);
	vec3 f0 = mix(vec3(0.04), albedo, metallic);
	vec3 fresnel = FresnelSchlick(max(dot(halfDir, viewDir), 0.0), f0);
	float distribution = DistributionGGX(normal, halfDir, roughness);
	float geometry = GeometrySmith(normal, viewDir, lightDir, roughness);
	vec3 specular = distribution * geometry * fresnel /
		max(4.0 * max(dot(normal, viewDir), 0.0) * max(dot(normal, lightDir), 0.0), 0.001);
	vec3 diffuseWeight = (vec3(1.0) - fresnel) * (1.0 - metallic);
	return (diffuseWeight * albedo / PI + specular) * radiance *
		max(dot(normal, lightDir), 0.0);
}

float DirectionalShadow(DirLight light, vec3 fragPos, float nDotL)
{
	if (!light.useShadowMap) return 0.0;
	return SampleShadow2D(
		light.shadowMap,
		light.shadowCompareMap,
		light.lightSpaceMatrix * vec4(fragPos, 1.0),
		nDotL,
		light.shadowBiasParams);
}

float SpotShadow(SpotLight light, vec3 fragPos, float biasNDotL)
{
	if (!light.useShadowMap) return 0.0;
	return SampleShadow2D(
		light.shadowMap,
		light.shadowCompareMap,
		light.lightSpaceMatrix * vec4(fragPos, 1.0),
		biasNDotL,
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

float PointShadowPrepared(
	PointLight light,
	vec3 lightToSurfaceDirection,
	float currentDepth,
	float nDotL,
	float lightContribution)
{
	if (!light.useShadowMap) return 0.0;
	return SampleShadowCubePrepared(
		light.shadowCubeMap,
		lightToSurfaceDirection,
		currentDepth,
		nDotL,
		light.far_plane,
		lightContribution);
}

vec3 EvaluateIBL(
	vec3 normal,
	vec3 viewDir,
	vec3 albedo,
	float metallic,
	float roughness,
	float ao)
{
	if (!useIBL) return vec3(0.03) * albedo * ao;
	vec3 f0 = mix(vec3(0.04), albedo, metallic);
	float nDotV = max(dot(normal, viewDir), 0.0);
	vec3 fresnel = FresnelSchlickRoughness(nDotV, f0, roughness);
	vec3 diffuseWeight = (vec3(1.0) - fresnel) * (1.0 - metallic);
	vec3 diffuse = texture(irradianceMap, normal).rgb * albedo;
	vec3 reflection = reflect(-viewDir, normal);
	vec3 prefiltered = textureLod(prefilterMap, reflection, roughness * 4.0).rgb;
	vec2 brdf = texture(brdfLUT, vec2(nDotV, roughness)).rg;
	vec3 specular = prefiltered * (fresnel * brdf.x + brdf.y);
	return (diffuseWeight * diffuse + specular) * ao;
}

void main()
{
	vec4 baseSample = vec4(1.0);
	if (material.use_texture_diffuse) {
		baseSample = texture(material.texture_diffuse1, fs_in.TexCoords);
	}
	float alpha = baseSample.a * material.opacity;
	if (material.use_texture_opacity) {
		alpha *= texture(material.texture_opacity1, fs_in.TexCoords).r;
	}
	if (material.useAlphaCutoff && alpha < material.alphaCutoff) discard;

	vec3 albedo = max(material.albedo * baseSample.rgb, vec3(0.0));
	float metallic = clamp(material.metallic, 0.0, 1.0);
	float roughness = clamp(material.roughness, 0.04, 1.0);
	if (material.metallicRoughnessPacked && material.use_texture_metallic) {
		vec3 packedSample = texture(material.texture_metallic1, fs_in.TexCoords).rgb;
		metallic *= packedSample.b;
		roughness *= packedSample.g;
	} else {
		if (material.use_texture_metallic)
			metallic *= texture(material.texture_metallic1, fs_in.TexCoords).r;
		if (material.use_texture_roughness)
			roughness *= texture(material.texture_roughness1, fs_in.TexCoords).r;
	}
	metallic = clamp(metallic, 0.0, 1.0);
	roughness = clamp(roughness, 0.04, 1.0);
	float ao = material.ao;
	if (material.use_texture_ao) ao *= texture(material.texture_ao1, fs_in.TexCoords).r;
	ao = clamp(ao, 0.0, 1.0);
	vec3 emissive = material.emissive;
	if (material.use_texture_emissive)
		emissive *= texture(material.texture_emissive1, fs_in.TexCoords).rgb;

	vec3 normal = normalize(fs_in.Normal);
	if (material.use_texture_normal) {
		normal = texture(material.texture_normal1, fs_in.TexCoords).rgb * 2.0 - 1.0;
		normal = normalize(fs_in.TBN * normal);
	}
	vec3 viewDir = normalize(viewPos - fs_in.FragPos);
	vec3 color = EvaluateIBL(normal, viewDir, albedo, metallic, roughness, ao) + emissive;
	bool exactEarlyOut =
		ShadowOptimizationEnabled(SHADOW_OPT_EXACT_EARLY_OUT);

	int dirCount = min(NR_DIR_LIGHTS, MAX_DIR_LIGHTS);
	for (int i = 0; i < dirCount; ++i) {
		if (!dirLights[i].isActive) continue;
		vec3 lightDir = normalize(-dirLights[i].direction);
		float nDotL = max(dot(normal, lightDir), 0.0);
		if (exactEarlyOut && nDotL <= 0.0) {
			continue;
		}
		float shadow = DirectionalShadow(
			dirLights[i],
			fs_in.FragPos,
			nDotL);
		color += (1.0 - shadow) * EvaluateDirect(
			normal, viewDir, lightDir, dirLights[i].diffuse,
			albedo, metallic, roughness);
	}

	int pointCount = min(NR_POINT_LIGHTS, MAX_POINT_LIGHTS);
	for (int i = 0; i < pointCount; ++i) {
		if (!pointLights[i].isActive) continue;
		vec3 lightVector = pointLights[i].position - fs_in.FragPos;
		float distanceToLight = length(lightVector);
		vec3 lightDir = distanceToLight > SHADOW_EPSILON
			? lightVector / distanceToLight
			: vec3(0.0);
		float nDotL = max(dot(normal, lightDir), 0.0);
		if (exactEarlyOut && nDotL <= 0.0) {
			continue;
		}
		float attenuation = 1.0 / max(
			pointLights[i].constant +
			pointLights[i].linear * distanceToLight +
			pointLights[i].quadratic * distanceToLight * distanceToLight,
			0.0001);
		float lightStrength = max(
			max(pointLights[i].diffuse.r, pointLights[i].diffuse.g),
			pointLights[i].diffuse.b);
		float lightContribution = lightStrength * attenuation;
		bool usePreparedPointShadow =
			ShadowOptimizationEnabled(
				SHADOW_OPT_PREPARED_POINT_INPUTS) ||
			ShadowOptimizationEnabled(
				SHADOW_OPT_ADAPTIVE_POINT_SAMPLES);
		float shadow = usePreparedPointShadow
			? PointShadowPrepared(
				pointLights[i],
				-lightDir,
				distanceToLight,
				nDotL,
				lightContribution)
			: PointShadow(pointLights[i], normal, fs_in.FragPos);
		color += (1.0 - shadow) * EvaluateDirect(
			normal, viewDir, lightDir, pointLights[i].diffuse * attenuation,
			albedo, metallic, roughness);
	}

	int spotCount = min(NR_SPOT_LIGHTS, MAX_SPOT_LIGHTS);
	for (int i = 0; i < spotCount; ++i) {
		if (!spotLights[i].isActive) continue;
		vec3 lightVector = spotLights[i].position - fs_in.FragPos;
		float distanceToLight = length(lightVector);
		vec3 lightDir = lightVector / max(distanceToLight, 0.0001);
		vec3 spotAxis = normalize(-spotLights[i].direction);
		float theta = dot(lightDir, spotAxis);
		float radialNDotL = max(dot(normal, lightDir), 0.0);
		float cone = clamp(
			(theta - spotLights[i].outerCutOff) /
			max(spotLights[i].cutOff - spotLights[i].outerCutOff, 0.0001),
			0.0,
			1.0);
		float attenuation = cone / max(
			spotLights[i].constant +
			spotLights[i].linear * distanceToLight +
			spotLights[i].quadratic * distanceToLight * distanceToLight,
			0.0001);
		if (exactEarlyOut &&
			(radialNDotL <= 0.0 || cone <= 0.0)) {
			continue;
		}
		float biasNDotL = ShadowOptimizationEnabled(
			SHADOW_OPT_SPOT_RADIAL_BIAS_DIRECTION)
			? radialNDotL
			: max(dot(normal, spotAxis), 0.0);
		float shadow = SpotShadow(
			spotLights[i],
			fs_in.FragPos,
			biasNDotL);
		color += (1.0 - shadow) * EvaluateDirect(
			normal, viewDir, lightDir, spotLights[i].diffuse * attenuation,
			albedo, metallic, roughness);
	}

	FragColor = vec4(color, alpha);
	NormalOut = vec4(normalize(mat3(transpose(inverse(view))) * normal) * 0.5 + 0.5, 1.0);
	float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
	BrightColor = brightness > bloomThreshold ? vec4(color, 1.0) : vec4(0.0, 0.0, 0.0, 1.0);
}
