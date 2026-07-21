#version 330 core

layout (location = 0) out vec4 FragColor;
layout (location = 1) out vec4 BrightColor;
in vec2 TexCoords;

const float PI = 3.14159265359;
const int MAX_POINT_LIGHTS = 16;
const int MAX_DIR_LIGHTS = 16;
const int MAX_SPOT_LIGHTS = 16;

struct DirLight {
	vec3 direction;
	vec3 ambient;
	vec3 diffuse;
	vec3 specular;
	mat4 lightSpaceMatrix;
	sampler2D shadowMap;
	bool useShadowMap;
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
	bool isActive;
};

uniform sampler2D gPosition;
uniform sampler2D gNormal;
uniform sampler2D gAlbedoSpec;
uniform sampler2D gMaterial;
uniform sampler2D gEmissive;
uniform sampler2D ssaoMap;
uniform bool useSSAO;
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
};

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

vec3 EvaluatePbrDirect(
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

vec3 EvaluatePbrIBL(
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
	vec3 prefiltered = textureLod(
		prefilterMap,
		reflect(-viewDir, normal),
		roughness * 4.0).rgb;
	vec2 brdf = texture(brdfLUT, vec2(nDotV, roughness)).rg;
	return (diffuseWeight * diffuse + prefiltered * (fresnel * brdf.x + brdf.y)) * ao;
}

float DirectionalShadow(DirLight light, vec3 normal, vec3 fragPos)
{
	if (!light.useShadowMap) return 0.0;
	vec4 lightSpace = light.lightSpaceMatrix * vec4(fragPos, 1.0);
	vec3 projected = lightSpace.xyz / lightSpace.w;
	projected = projected * 0.5 + 0.5;
	if (projected.z >= 1.0 || projected.z <= 0.0) return 0.0;
	float bias = max(0.005 * (1.0 - dot(normal, normalize(-light.direction))), 0.0005);
	if (shadowType == 0) {
		return projected.z - bias > texture(light.shadowMap, projected.xy).r ? 1.0 : 0.0;
	}
	vec2 texel = 1.0 / vec2(textureSize(light.shadowMap, 0));
	int radius = shadowType == 2 ? 2 : 1;
	float shadow = 0.0;
	float samples = 0.0;
	for (int x = -2; x <= 2; ++x) {
		for (int y = -2; y <= 2; ++y) {
			if (abs(x) > radius || abs(y) > radius) continue;
			shadow += projected.z - bias >
				texture(light.shadowMap, projected.xy + vec2(x, y) * texel).r ? 1.0 : 0.0;
			samples += 1.0;
		}
	}
	return shadow / max(samples, 1.0);
}

float PointShadow(PointLight light, vec3 normal, vec3 fragPos)
{
	if (!light.useShadowMap) return 0.0;
	vec3 fragToLight = fragPos - light.position;
	float currentDepth = length(fragToLight);
	float bias = max(0.02 * (1.0 - dot(normal, normalize(-fragToLight))), 0.005);
	float closest = texture(light.shadowCubeMap, fragToLight).r * light.far_plane;
	return currentDepth - bias > closest ? 1.0 : 0.0;
}

vec3 LegacyDirectional(
	DirLight light,
	vec3 normal,
	vec3 viewDir,
	vec3 albedo,
	vec4 material,
	float ao,
	float shadow)
{
	vec3 lightDir = normalize(-light.direction);
	float diffuseTerm = max(dot(normal, lightDir), 0.0);
	vec3 halfDir = normalize(lightDir + viewDir);
	float specularTerm = pow(max(dot(normal, halfDir), 0.0), material.a);
	vec3 ambient = light.ambient * material.r * ao;
	vec3 diffuse = light.diffuse * material.g * diffuseTerm;
	vec3 specular = light.specular * material.b * specularTerm;
	return (ambient + (1.0 - shadow) * (diffuse + specular)) * albedo;
}

vec3 LegacyPoint(
	PointLight light,
	vec3 normal,
	vec3 fragPos,
	vec3 viewDir,
	vec3 albedo,
	vec4 material,
	float ao,
	float shadow)
{
	vec3 lightVector = light.position - fragPos;
	float distanceToLight = length(lightVector);
	vec3 lightDir = lightVector / max(distanceToLight, 0.0001);
	float diffuseTerm = max(dot(normal, lightDir), 0.0);
	vec3 halfDir = normalize(lightDir + viewDir);
	float specularTerm = pow(max(dot(normal, halfDir), 0.0), material.a);
	float attenuation = 1.0 / max(
		light.constant + light.linear * distanceToLight +
		light.quadratic * distanceToLight * distanceToLight,
		0.0001);
	vec3 ambient = light.ambient * material.r * ao;
	vec3 diffuse = light.diffuse * material.g * diffuseTerm;
	vec3 specular = light.specular * material.b * specularTerm;
	return attenuation * (ambient + (1.0 - shadow) * (diffuse + specular)) * albedo;
}

vec3 LegacySpot(
	SpotLight light,
	vec3 normal,
	vec3 fragPos,
	vec3 viewDir,
	vec3 albedo,
	vec4 material,
	float ao)
{
	vec3 lightVector = light.position - fragPos;
	float distanceToLight = length(lightVector);
	vec3 lightDir = lightVector / max(distanceToLight, 0.0001);
	float theta = dot(lightDir, normalize(-light.direction));
	float cone = clamp(
		(theta - light.outerCutOff) / max(light.cutOff - light.outerCutOff, 0.0001),
		0.0,
		1.0);
	float attenuation = cone / max(
		light.constant + light.linear * distanceToLight +
		light.quadratic * distanceToLight * distanceToLight,
		0.0001);
	float diffuseTerm = max(dot(normal, lightDir), 0.0);
	vec3 reflected = reflect(-lightDir, normal);
	float specularTerm = pow(max(dot(viewDir, reflected), 0.0), material.a);
	return attenuation * (
		light.ambient * material.r * ao +
		light.diffuse * material.g * diffuseTerm +
		light.specular * material.b * specularTerm) * albedo;
}

void main()
{
	vec4 positionDepth = texture(gPosition, TexCoords);
	if (positionDepth.a <= 0.0) {
		FragColor = vec4(0.0, 0.0, 0.0, 1.0);
		BrightColor = vec4(0.0, 0.0, 0.0, 1.0);
		return;
	}
	vec3 fragPos = positionDepth.rgb;
	vec3 normal = normalize(texture(gNormal, TexCoords).rgb);
	vec3 albedo = texture(gAlbedoSpec, TexCoords).rgb;
	vec4 material = texture(gMaterial, TexCoords);
	vec3 emissive = texture(gEmissive, TexCoords).rgb;
	vec3 viewDir = normalize(viewPos - fragPos);
	float ssao = useSSAO ? texture(ssaoMap, TexCoords).r : 1.0;
	bool isPbr = material.a < 0.0;
	float metallic = clamp(material.r, 0.0, 1.0);
	float roughness = clamp(material.g, 0.04, 1.0);
	float ao = isPbr ? clamp(material.b * ssao, 0.0, 1.0) : ssao;
	vec3 color = isPbr
		? EvaluatePbrIBL(normal, viewDir, albedo, metallic, roughness, ao) + emissive
		: vec3(0.0);

	int dirCount = min(NR_DIR_LIGHTS, MAX_DIR_LIGHTS);
	for (int i = 0; i < dirCount; ++i) {
		if (!dirLights[i].isActive) continue;
		float shadow = DirectionalShadow(dirLights[i], normal, fragPos);
		if (isPbr) {
			color += (1.0 - shadow) * EvaluatePbrDirect(
				normal, viewDir, normalize(-dirLights[i].direction), dirLights[i].diffuse,
				albedo, metallic, roughness);
		} else {
			color += LegacyDirectional(dirLights[i], normal, viewDir, albedo, material, ao, shadow);
		}
	}

	int pointCount = min(NR_POINT_LIGHTS, MAX_POINT_LIGHTS);
	for (int i = 0; i < pointCount; ++i) {
		if (!pointLights[i].isActive) continue;
		float shadow = PointShadow(pointLights[i], normal, fragPos);
		if (isPbr) {
			vec3 lightVector = pointLights[i].position - fragPos;
			float distanceToLight = length(lightVector);
			float attenuation = 1.0 / max(
				pointLights[i].constant + pointLights[i].linear * distanceToLight +
				pointLights[i].quadratic * distanceToLight * distanceToLight,
				0.0001);
			color += (1.0 - shadow) * EvaluatePbrDirect(
				normal, viewDir, lightVector / max(distanceToLight, 0.0001),
				pointLights[i].diffuse * attenuation, albedo, metallic, roughness);
		} else {
			color += LegacyPoint(pointLights[i], normal, fragPos, viewDir, albedo, material, ao, shadow);
		}
	}

	int spotCount = min(NR_SPOT_LIGHTS, MAX_SPOT_LIGHTS);
	for (int i = 0; i < spotCount; ++i) {
		if (!spotLights[i].isActive) continue;
		if (isPbr) {
			vec3 lightVector = spotLights[i].position - fragPos;
			float distanceToLight = length(lightVector);
			vec3 lightDir = lightVector / max(distanceToLight, 0.0001);
			float theta = dot(lightDir, normalize(-spotLights[i].direction));
			float cone = clamp(
				(theta - spotLights[i].outerCutOff) /
				max(spotLights[i].cutOff - spotLights[i].outerCutOff, 0.0001),
				0.0,
				1.0);
			float attenuation = cone / max(
				spotLights[i].constant + spotLights[i].linear * distanceToLight +
				spotLights[i].quadratic * distanceToLight * distanceToLight,
				0.0001);
			color += EvaluatePbrDirect(
				normal, viewDir, lightDir, spotLights[i].diffuse * attenuation,
				albedo, metallic, roughness);
		} else {
			color += LegacySpot(spotLights[i], normal, fragPos, viewDir, albedo, material, ao);
		}
	}

	FragColor = vec4(color, 1.0);
	float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
	BrightColor = brightness > bloomThreshold
		? vec4(color, 1.0)
		: vec4(0.0, 0.0, 0.0, 1.0);
}
