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

#include "pointLightLighting.glsl"

void main()
{
	vec2 texCoords = gl_FragCoord.xy / vec2(screenWidth, screenHeight);
	vec3 fragPos;
	if (!LoadWorldPosition(texCoords, fragPos)) discard;
	vec3 delta = fragPos - pointLight.position;
	if (dot(delta, delta) > pointLightRadiusSquared) discard;
	vec3 color = texture(gAlbedoSpec, texCoords).rgb;
	float ao = useSSAO ? texture(ssaoMap, texCoords).r : 1.0;
	EvaluatePointLight(texCoords, fragPos, color, ao);
}
