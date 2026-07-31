#version 330 core

layout (location = 0) out vec2 FragColor;
in vec2 TexCoords;

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

vec3 ImportanceSampleGGX(vec2 xi, float roughness)
{
	float alpha = roughness * roughness;
	float phi = 2.0 * PI * xi.x;
	float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y));
	float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
	return vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

float GeometrySchlickGGX(float nDotV, float roughness)
{
	float k = (roughness * roughness) * 0.5;
	return nDotV / (nDotV * (1.0 - k) + k);
}

float GeometrySmith(float nDotV, float nDotL, float roughness)
{
	return GeometrySchlickGGX(nDotV, roughness) * GeometrySchlickGGX(nDotL, roughness);
}

vec2 IntegrateBRDF(float nDotV, float roughness)
{
	vec3 viewDir = vec3(sqrt(max(0.0, 1.0 - nDotV * nDotV)), 0.0, nDotV);
	float scale = 0.0;
	float bias = 0.0;
	for (uint i = 0u; i < SAMPLE_COUNT; ++i) {
		vec3 halfDir = ImportanceSampleGGX(Hammersley(i, SAMPLE_COUNT), roughness);
		vec3 lightDir = normalize(2.0 * dot(viewDir, halfDir) * halfDir - viewDir);
		float nDotL = max(lightDir.z, 0.0);
		float nDotH = max(halfDir.z, 0.0);
		float vDotH = max(dot(viewDir, halfDir), 0.0);
		if (nDotL > 0.0) {
			float geometry = GeometrySmith(nDotV, nDotL, roughness);
			float visibility = (geometry * vDotH) / max(nDotH * nDotV, 0.0001);
			float fresnel = pow(1.0 - vDotH, 5.0);
			scale += (1.0 - fresnel) * visibility;
			bias += fresnel * visibility;
		}
	}
	return vec2(scale, bias) / float(SAMPLE_COUNT);
}

void main()
{
	FragColor = IntegrateBRDF(clamp(TexCoords.x, 0.001, 0.999), clamp(TexCoords.y, 0.04, 1.0));
}
