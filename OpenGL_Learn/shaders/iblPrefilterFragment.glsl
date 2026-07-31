#version 330 core

out vec4 FragColor;
in vec3 LocalPos;

uniform samplerCube environmentMap;
uniform float roughness;

const float PI = 3.14159265359;
const uint SAMPLE_COUNT = 256u;

float RadicalInverseVdC(uint bits)
{
	bits = (bits << 16u) | (bits >> 16u);
	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
	return float(bits) * 2.3283064365386963e-10;
}

vec2 Hammersley(uint index, uint count)
{
	return vec2(float(index) / float(count), RadicalInverseVdC(index));
}

vec3 ImportanceSampleGGX(vec2 xi, vec3 normal, float surfaceRoughness)
{
	float alpha = surfaceRoughness * surfaceRoughness;
	float phi = 2.0 * PI * xi.x;
	float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y));
	float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
	vec3 halfTangent = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
	vec3 up = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	vec3 tangent = normalize(cross(up, normal));
	vec3 bitangent = cross(normal, tangent);
	return normalize(tangent * halfTangent.x + bitangent * halfTangent.y + normal * halfTangent.z);
}

void main()
{
	vec3 normal = normalize(LocalPos);
	vec3 viewDir = normal;
	vec3 prefiltered = vec3(0.0);
	float totalWeight = 0.0;
	for (uint i = 0u; i < SAMPLE_COUNT; ++i) {
		vec3 halfDir = ImportanceSampleGGX(Hammersley(i, SAMPLE_COUNT), normal, roughness);
		vec3 lightDir = normalize(2.0 * dot(viewDir, halfDir) * halfDir - viewDir);
		float nDotL = max(dot(normal, lightDir), 0.0);
		if (nDotL > 0.0) {
			prefiltered += texture(environmentMap, lightDir).rgb * nDotL;
			totalWeight += nDotL;
		}
	}
	FragColor = vec4(prefiltered / max(totalWeight, 0.0001), 1.0);
}
