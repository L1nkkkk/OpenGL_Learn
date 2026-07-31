#version 330 core

layout (location = 0) out float FragColor;

in vec2 TexCoords;

uniform sampler2D halfAO;
uniform sampler2D gPosition;
uniform sampler2D gNormal;

// Standard deviation for the relative linear-depth difference:
// abs(sampleDepth - centerDepth) / centerDepth.
uniform float depthSigma;
uniform float normalPower;

vec3 SafeNormalize(vec3 value)
{
	return value * inversesqrt(max(dot(value, value), 1e-12));
}

void main()
{
	ivec2 fullSize = textureSize(gPosition, 0);
	ivec2 halfSize = textureSize(halfAO, 0);
	ivec2 fullCoord = clamp(
		ivec2(gl_FragCoord.xy),
		ivec2(0),
		fullSize - ivec2(1));

	vec4 centerPosition = texelFetch(gPosition, fullCoord, 0);
	if (centerPosition.a <= 0.0) {
		FragColor = 1.0;
		return;
	}

	vec3 centerNormalRaw = texelFetch(gNormal, fullCoord, 0).xyz;
	vec3 centerNormal = SafeNormalize(centerNormalRaw);

	// Convert the full-resolution pixel center to the half-resolution texel
	// coordinate system. The four surrounding texels are the exact bilinear
	// footprint; depth and normal weights only suppress incompatible samples.
	vec2 halfPosition = TexCoords * vec2(halfSize) - vec2(0.5);
	ivec2 baseCoord = ivec2(floor(halfPosition));
	vec2 fraction = fract(halfPosition);

	float weightedAO = 0.0;
	float weightSum = 0.0;
	float sigma = max(depthSigma, 1e-5);
	float exponent = max(normalPower, 1e-4);

	for (int y = 0; y < 2; ++y) {
		for (int x = 0; x < 2; ++x) {
			ivec2 halfCoord = clamp(
				baseCoord + ivec2(x, y),
				ivec2(0),
				halfSize - ivec2(1));
			vec2 guideUV =
				(vec2(halfCoord) + vec2(0.5)) / vec2(halfSize);

			vec4 samplePosition = texture(gPosition, guideUV);
			vec3 sampleNormalRaw = texture(gNormal, guideUV).xyz;
			if (samplePosition.a <= 0.0 ||
				dot(sampleNormalRaw, sampleNormalRaw) <= 1e-12) {
				continue;
			}

			float xWeight = x == 0 ? 1.0 - fraction.x : fraction.x;
			float yWeight = y == 0 ? 1.0 - fraction.y : fraction.y;
			float spatialWeight = xWeight * yWeight;

			float relativeDepthDelta =
				abs(samplePosition.a - centerPosition.a) /
				max(abs(centerPosition.a), 1e-4);
			float normalizedDepthDelta = relativeDepthDelta / sigma;
			float depthWeight = exp(
				-0.5 * normalizedDepthDelta * normalizedDepthDelta);

			vec3 sampleNormal = SafeNormalize(sampleNormalRaw);
			float normalWeight = pow(
				clamp(dot(centerNormal, sampleNormal), 0.0, 1.0),
				exponent);

			float weight = spatialWeight * depthWeight * normalWeight;
			weightedAO += texelFetch(halfAO, halfCoord, 0).r * weight;
			weightSum += weight;
		}
	}

	ivec2 nearestCoord = clamp(
		ivec2(floor(halfPosition + vec2(0.5))),
		ivec2(0),
		halfSize - ivec2(1));
	float nearestAO = texelFetch(halfAO, nearestCoord, 0).r;
	FragColor = clamp(
		weightSum > 1e-6 ? weightedAO / weightSum : nearestAO,
		0.0,
		1.0);
}
