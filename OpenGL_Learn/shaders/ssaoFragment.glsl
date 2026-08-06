#version 330 core
layout (location = 0) out float FragColor;

in vec2 TexCoords;

uniform sampler2D gPosition;
uniform sampler2D gDepth;
uniform sampler2D gNormal;
uniform sampler2D texNoise;
uniform bool reconstructPosition;
uniform mat4 inverseProjection;

uniform vec3 ssaoKernel[64];

uniform int screenWidth;
uniform int screenHeight;
uniform float radius;
uniform float bias;
uniform int kernelSize;

layout(std140) uniform Matrices {
	mat4 view;
	mat4 projection;
};

bool LoadViewPosition(vec2 uv, out vec3 positionVS, out float viewDepth)
{
	if (!reconstructPosition) {
		vec4 positionDepth = texture(gPosition, uv);
		viewDepth = positionDepth.a;
		if (viewDepth <= 0.0) return false;
		positionVS = (view * vec4(positionDepth.xyz, 1.0)).xyz;
		return true;
	}

	// gPosition uses GL_LINEAR in the control. Reconstruct the four depth
	// texels independently and then apply the same bilinear interpolation;
	// reconstructing one already-filtered nonlinear depth value is not
	// equivalent, especially across silhouettes. Clear-depth texels map to the
	// control attachment's cleared vec4(0), preserving its edge behavior.
	ivec2 size = textureSize(gDepth, 0);
	vec2 texelPosition = uv * vec2(size) - vec2(0.5);
	ivec2 base = ivec2(floor(texelPosition));
	vec2 blend = fract(texelPosition);
	vec3 reconstructed[4];
	for (int index = 0; index < 4; ++index) {
		ivec2 offset = ivec2(index & 1, index >> 1);
		ivec2 coordinate = clamp(base + offset, ivec2(0), size - ivec2(1));
		float deviceDepth = texelFetch(gDepth, coordinate, 0).r;
		if (deviceDepth >= 1.0) {
			reconstructed[index] = vec3(0.0);
			continue;
		}
		vec2 texelUv = (vec2(coordinate) + vec2(0.5)) / vec2(size);
		vec4 clip = vec4(
			texelUv * 2.0 - 1.0,
			deviceDepth * 2.0 - 1.0,
			1.0);
		vec4 value = inverseProjection * clip;
		reconstructed[index] = value.xyz / value.w;
	}
	positionVS = mix(
		mix(reconstructed[0], reconstructed[1], blend.x),
		mix(reconstructed[2], reconstructed[3], blend.x),
		blend.y);
	viewDepth = -positionVS.z;
	return viewDepth > 0.0;
}

void main()
{
	vec3 fragPosVS;
	float viewDepth = 0.0;
	if (!LoadViewPosition(TexCoords, fragPosVS, viewDepth)) {
		FragColor = 1.0;
		return;
	}

	vec3 normalWS = normalize(texture(gNormal, TexCoords).rgb);
	vec3 normalVS = normalize(mat3(view) * normalWS);

	vec2 noiseScale = vec2(float(screenWidth), float(screenHeight)) / vec2(4.0);
	vec3 randomVec = normalize(texture(texNoise, TexCoords * noiseScale).xyz);
	vec3 tangent = normalize(randomVec - normalVS * dot(randomVec, normalVS));
	vec3 bitangent = cross(normalVS, tangent);
	mat3 TBN = mat3(tangent, bitangent, normalVS);

	float occlusion = 0.0;
	int k = clamp(kernelSize, 1, 64);
	for (int i = 0; i < k; ++i) {
		vec3 sampleVS = TBN * ssaoKernel[i];
		sampleVS = fragPosVS + sampleVS * radius;
		vec4 offset = projection * vec4(sampleVS, 1.0);
		offset.xyz /= offset.w;
		offset.xy = offset.xy * 0.5 + 0.5;
		if (offset.x < 0.0 || offset.x > 1.0 || offset.y < 0.0 || offset.y > 1.0)
			continue;

		vec3 sampleVSBuf;
		float sampleViewDepth = 0.0;
		if (!LoadViewPosition(
			offset.xy,
			sampleVSBuf,
			sampleViewDepth))
			continue;

		float rangeCheck = smoothstep(0.0, 1.0, radius / max(abs(fragPosVS.z - sampleVSBuf.z), 1e-4));
		occlusion += (sampleVSBuf.z >= sampleVS.z + bias ? 1.0 : 0.0) * rangeCheck;
	}

	float vis = 1.0 - (occlusion / float(k));
	FragColor = clamp(vis, 0.0, 1.0);
}
