#version 330 core
layout (location = 0) out float FragColor;

in vec2 TexCoords;

uniform sampler2D gPosition;
uniform sampler2D gNormal;
uniform sampler2D texNoise;

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

void main()
{
	vec4 gP = texture(gPosition, TexCoords);
	float viewDepth = gP.a;
	if (viewDepth <= 0.0) {
		FragColor = 1.0;
		return;
	}

	vec3 fragWorld = gP.xyz;
	vec3 fragPosVS = (view * vec4(fragWorld, 1.0)).xyz;
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

		vec4 sampleG = texture(gPosition, offset.xy);
		if (sampleG.a <= 0.0)
			continue;

		vec3 sampleVSBuf = (view * vec4(sampleG.xyz, 1.0)).xyz;

		float rangeCheck = smoothstep(0.0, 1.0, radius / max(abs(fragPosVS.z - sampleVSBuf.z), 1e-4));
		occlusion += (sampleVSBuf.z >= sampleVS.z + bias ? 1.0 : 0.0) * rangeCheck;
	}

	float vis = 1.0 - (occlusion / float(k));
	FragColor = clamp(vis, 0.0, 1.0);
}
