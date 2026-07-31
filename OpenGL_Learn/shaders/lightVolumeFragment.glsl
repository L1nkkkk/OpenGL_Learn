#version 330 core
layout (location = 0) out vec4 FragColor;
layout (location = 1) out vec4 BrightColor;

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

vec2 screenSize = vec2(screenWidth, screenHeight);
vec2 TexCoords;

struct PointLight{
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
};

uniform PointLight pointLight;

uniform vec3 viewPos;

uniform sampler2D gPosition;
uniform sampler2D gNormal;
uniform sampler2D gAlbedoSpec;
uniform sampler2D gMaterial;
uniform sampler2D ssaoMap;
uniform bool useSSAO;

void main()
{
	TexCoords = gl_FragCoord.xy / screenSize;

	float shadow = 0.0;
	
	vec3 color = texture(gAlbedoSpec,TexCoords).rgb;
	float ao = useSSAO ? texture(ssaoMap, TexCoords).r : 1.0;
	vec3 fragPos = texture(gPosition, TexCoords).rgb;
	vec3 normal = normalize(texture(gNormal, TexCoords).rgb);
	vec3 lightDir = normalize(pointLight.position - fragPos);
	vec3 viewDir = normalize(viewPos - fragPos);
	vec4 material = texture(gMaterial, TexCoords);
	// diffuse shading
	float diff = max(dot(normal, lightDir), 0.0);
	// specular shading
	vec3 halfwayDir = normalize(lightDir + viewDir);
	float spec = pow(max(dot(normal, halfwayDir), 0.0), material.a);
	// attenuation
	float distance = length(pointLight.position - fragPos);
	float attenuation = 1.0 / (pointLight.constant + pointLight.linear * distance + pointLight.quadratic * (distance * distance));
	// combine results
	vec3 ambient = pointLight.ambient * material.r * ao;
	vec3 diffuse = pointLight.diffuse * material.g *diff;
	vec3 specular = pointLight.specular * material.b* spec;
	ambient *= attenuation;
	diffuse *= attenuation;
	specular *= attenuation;

	if (pointLight.useShadowMap) {
		shadow = SampleShadowCube(
			pointLight.shadowCubeMap,
			fragPos - pointLight.position,
			normal,
			pointLight.far_plane);
	}

	vec3 lit = (ambient + (1.0 - shadow) * (diffuse + specular)) * color;
	FragColor = vec4(lit, 1.0);
	float brightness = dot(FragColor.rgb, vec3(0.2126, 0.7152, 0.0722));
	if (brightness > bloomThreshold)
		BrightColor = vec4(FragColor.rgb, 1.0);
	else
		BrightColor = vec4(0.0, 0.0, 0.0, 1.0);
}
