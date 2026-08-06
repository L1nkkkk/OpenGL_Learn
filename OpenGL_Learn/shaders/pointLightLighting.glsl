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
uniform sampler2D gDepth;
uniform sampler2D gNormal;
uniform sampler2D gAlbedoSpec;
uniform sampler2D gMaterial;
uniform sampler2D ssaoMap;
uniform bool useSSAO;
uniform bool reconstructPosition;
uniform mat4 inverseProjection;
uniform mat4 inverseView;
uniform bool useAnalyticRadiusPredicate;
uniform float pointLightRadiusSquared;

#include "positionReconstruction.glsl"

void WriteZeroPointLightOutputs()
{
    FragColor = vec4(0.0, 0.0, 0.0, 1.0);
    BrightColor = vec4(0.0, 0.0, 0.0, 1.0);
}

void EvaluatePointLight(
    vec2 texCoords,
    vec3 fragPos,
    vec3 color,
    float ao)
{
    float shadow = 0.0;
    vec3 normal = normalize(texture(gNormal, texCoords).rgb);
    vec3 lightDir = normalize(pointLight.position - fragPos);
    vec3 viewDir = normalize(viewPos - fragPos);
    vec4 material = texture(gMaterial, texCoords);

    float diff = max(dot(normal, lightDir), 0.0);
    vec3 halfwayDir = normalize(lightDir + viewDir);
    float spec = pow(max(dot(normal, halfwayDir), 0.0), material.a);
    float distance = length(pointLight.position - fragPos);
    float attenuation = 1.0 /
        (pointLight.constant + pointLight.linear * distance +
            pointLight.quadratic * (distance * distance));

    vec3 ambient = pointLight.ambient * material.r * ao;
    vec3 diffuse = pointLight.diffuse * material.g * diff;
    vec3 specular = pointLight.specular * material.b * spec;
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
