#version 330 core

out vec4 FragColor;
in vec3 LocalPos;

uniform samplerCube environmentMap;

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

void main()
{
	vec3 normal = normalize(LocalPos);
	vec3 up = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	vec3 tangent = normalize(cross(up, normal));
	vec3 bitangent = cross(normal, tangent);
	vec3 irradiance = vec3(0.0);

	for (uint i = 0u; i < SAMPLE_COUNT; ++i) {
		vec2 xi = Hammersley(i, SAMPLE_COUNT);
		float phi = 2.0 * PI * xi.x;
		float cosTheta = sqrt(1.0 - xi.y);
		float sinTheta = sqrt(xi.y);
		vec3 sampleTangent = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
		vec3 sampleDirection =
			tangent * sampleTangent.x +
			bitangent * sampleTangent.y +
			normal * sampleTangent.z;
		irradiance += texture(environmentMap, sampleDirection).rgb;
	}

	FragColor = vec4(PI * irradiance / float(SAMPLE_COUNT), 1.0);
}
