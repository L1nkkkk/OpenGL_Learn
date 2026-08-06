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

uniform sampler2D gPosition;
uniform sampler2D gDepth;
uniform sampler2D gNormal;
uniform sampler2D gAlbedoSpec;
uniform sampler2D gMaterial;
uniform sampler2D ssaoMap;
uniform bool useSSAO;
uniform bool reconstructPosition;
uniform mat4 inverseProjection;
uniform mat4 inverseView;
uniform mat4 viewMatrix;
uniform vec3 viewPos;

uniform usamplerBuffer gridMetadata;
uniform usamplerBuffer gridIndices;
uniform samplerBuffer gridLights;
uniform int gridMode;
uniform int gridTilesX;
uniform int gridTilesY;
uniform int gridSliceCount;
uniform float gridNearPlane;
uniform float gridFarPlane;

#include "positionReconstruction.glsl"

int PointLightDepthSlice(float viewDepth)
{
    float d = clamp(viewDepth, gridNearPlane, gridFarPlane);
    float normalized = log(d / gridNearPlane) /
        log(gridFarPlane / gridNearPlane);
    return clamp(int(floor(normalized * float(gridSliceCount))),
        0, gridSliceCount - 1);
}

void main()
{
    vec2 texCoords = gl_FragCoord.xy / vec2(screenWidth, screenHeight);
    vec3 fragPos;
    if (!LoadWorldPosition(texCoords, fragPos)) discard;

    ivec2 tile = clamp(ivec2(gl_FragCoord.xy) / 16,
        ivec2(0), ivec2(gridTilesX - 1, gridTilesY - 1));
    int slice = 0;
    if (gridSliceCount > 1) {
        float viewDepth = -(viewMatrix * vec4(fragPos, 1.0)).z;
        slice = PointLightDepthSlice(viewDepth);
    }
    int cell = (slice * gridTilesY + tile.y) * gridTilesX + tile.x;
    uvec2 range = texelFetch(gridMetadata, cell).rg;

    vec3 normal = normalize(texture(gNormal, texCoords).rgb);
    vec3 viewDir = normalize(viewPos - fragPos);
    vec3 albedo = texture(gAlbedoSpec, texCoords).rgb;
    vec4 material = texture(gMaterial, texCoords);
    float ao = useSSAO ? texture(ssaoMap, texCoords).r : 1.0;
    vec3 totalLit = vec3(0.0);
    vec3 totalBright = vec3(0.0);
    float contributingLights = 0.0;

    for (uint localIndex = 0u; localIndex < range.y; ++localIndex) {
        uint lightIndex = texelFetch(gridIndices,
            int(range.x + localIndex)).r;
        int base = int(lightIndex) * 4;
        vec4 positionRadius = texelFetch(gridLights, base + 0);
        vec4 ambientConstant = texelFetch(gridLights, base + 1);
        vec4 diffuseLinear = texelFetch(gridLights, base + 2);
        vec4 specularQuadratic = texelFetch(gridLights, base + 3);

        vec3 lightVector = positionRadius.xyz - fragPos;
        float distanceSquared = dot(lightVector, lightVector);
        if (distanceSquared > positionRadius.w) continue;
        float distanceToLight = sqrt(distanceSquared);
        vec3 lightDir = distanceToLight > 0.0
            ? lightVector / distanceToLight
            : vec3(0.0, 1.0, 0.0);
        float diff = max(dot(normal, lightDir), 0.0);
        vec3 halfwayDir = normalize(lightDir + viewDir);
        float spec = pow(max(dot(normal, halfwayDir), 0.0), material.a);
        float attenuation = 1.0 /
            (ambientConstant.w + diffuseLinear.w * distanceToLight +
                specularQuadratic.w * distanceSquared);

        vec3 ambient = ambientConstant.xyz * material.r * ao;
        vec3 diffuse = diffuseLinear.xyz * material.g * diff;
        vec3 specular = specularQuadratic.xyz * material.b * spec;
        vec3 contribution =
            (ambient + diffuse + specular) * attenuation * albedo;
        totalLit += contribution;
        float brightness = dot(contribution, vec3(0.2126, 0.7152, 0.0722));
        if (brightness > bloomThreshold) totalBright += contribution;
        contributingLights += 1.0;
    }

    FragColor = vec4(totalLit, contributingLights);
    BrightColor = vec4(totalBright, contributingLights);
}
