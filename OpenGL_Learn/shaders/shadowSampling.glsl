// Shared shadow filtering used by every lighting path.
//
// The production path uses a stable Vogel disk so neighboring fragments share
// a coherent low-discrepancy kernel. The original per-fragment randomized
// kernel remains available only for controlled A/B regression measurements.

const int SHADOW_MODE_HARD = 0;
const int SHADOW_MODE_PCF = 1;
const int SHADOW_MODE_PCSS = 2;
const int SHADOW_SAMPLING_LEGACY_RANDOM = 0;
const int SHADOW_SAMPLING_STABLE_VOGEL = 1;
const int SHADOW_OPT_EXACT_EARLY_OUT = 1;
const int SHADOW_OPT_ADAPTIVE_POINT_SAMPLES = 2;
const int SHADOW_OPT_ADAPTIVE_PCSS_FILTER = 4;
const int SHADOW_OPT_STAGED_PCSS_BLOCKER = 8;
const int SHADOW_OPT_HARDWARE_DEPTH_COMPARE = 16;
const int SHADOW_OPT_HARDWARE_LINEAR_PCF = 32;
const int SHADOW_OPT_HARDWARE_REDUCED_PCF = 64;
const int SHADOW_OPT_TEXEL_SCALED_BIAS = 128;
const int SHADOW_OPT_SPOT_RADIAL_BIAS_DIRECTION = 256;
const int SHADOW_OPT_SPOT_PCSS_LINEAR_DEPTH = 512;
const int SHADOW_OPT_SPOT_PCSS_REDUCED_FILTER = 1024;
const int SHADOW_OPT_PREPARED_POINT_INPUTS = 4096;
const int SHADOW_MAX_SAMPLES = 64;
const int SHADOW_STABLE_BLOCKER_SAMPLES = 8;
const int SHADOW_SPOT_PCSS_REDUCED_FILTER_SAMPLES = 8;

const float SHADOW_EPSILON = 0.00001;
const float SHADOW_TWO_PI = 6.283185307179586;
const float SHADOW_PCF_RADIUS_TEXELS = 2.0;
const float SHADOW_PCSS_SEARCH_RADIUS_TEXELS = 8.0;
const float SHADOW_PCSS_LIGHT_SIZE_TEXELS = 24.0;
const float SHADOW_PCSS_MAX_RADIUS_TEXELS = 16.0;
const vec2 SHADOW_VOGEL_ROTATION =
	vec2(-0.7373688781, 0.6754902943);
const vec2 SHADOW_BLOCKER_ROTATION =
	vec2(0.3623748901, 0.9320324238);

vec2 shadowSamplingDisk[SHADOW_MAX_SAMPLES];

bool ShadowOptimizationEnabled(int flag)
{
	return (shadowOptimizationFlags & flag) != 0;
}

int ShadowSampleCount()
{
	return clamp(shadowSampleNum, 1, SHADOW_MAX_SAMPLES);
}

int ShadowMediumSampleCount()
{
	int sampleCount = ShadowSampleCount();
	int minimumCount =
		clamp(shadowAdaptiveMinSamples, 1, sampleCount);
	int mediumCount = (sampleCount * 3 + 3) / 4;
	return clamp(mediumCount, minimumCount, sampleCount);
}

int ShadowSubsampleIndex(int sampleIndex, int selectedSampleCount)
{
	int sampleCount = ShadowSampleCount();
	if (selectedSampleCount <= 1 || sampleCount <= 1) return 0;
	return sampleIndex * (sampleCount - 1) /
		(selectedSampleCount - 1);
}

int ShadowRadiusSampleCount(float radiusTexels)
{
	int sampleCount = ShadowSampleCount();
	int minimumCount =
		clamp(shadowAdaptiveMinSamples, 1, sampleCount);
	if (radiusTexels <= 2.0) return minimumCount;
	if (radiusTexels <= 8.0) return ShadowMediumSampleCount();
	return sampleCount;
}

int ShadowPcssFilterSampleCount(
	float radiusTexels,
	bool spotPcssLinearDepth)
{
	int selectedSampleCount = ShadowSampleCount();
	if (!ShadowOptimizationEnabled(
			SHADOW_OPT_ADAPTIVE_PCSS_FILTER)) {
		selectedSampleCount = ShadowSampleCount();
	}
	else {
		selectedSampleCount = ShadowRadiusSampleCount(radiusTexels);
	}
	if (spotPcssLinearDepth &&
		ShadowOptimizationEnabled(
			SHADOW_OPT_SPOT_PCSS_REDUCED_FILTER)) {
		selectedSampleCount = min(
			selectedSampleCount,
			SHADOW_SPOT_PCSS_REDUCED_FILTER_SAMPLES);
	}
	return selectedSampleCount;
}

int ShadowPointContributionSampleCount(float lightContribution)
{
	if (!ShadowOptimizationEnabled(
			SHADOW_OPT_ADAPTIVE_POINT_SAMPLES)) {
		return ShadowSampleCount();
	}
	int minimumCount = clamp(
		shadowAdaptiveMinSamples,
		1,
		ShadowSampleCount());
	if (lightContribution < 0.04) return minimumCount;
	if (lightContribution < 0.12) return ShadowMediumSampleCount();
	return ShadowSampleCount();
}

int ShadowPointPcssFilterSampleCount(
	float radiusTexels,
	float lightContribution)
{
	int sampleCount = ShadowSampleCount();
	int selectedCount = sampleCount;
	if (ShadowOptimizationEnabled(
			SHADOW_OPT_ADAPTIVE_PCSS_FILTER)) {
		selectedCount = ShadowRadiusSampleCount(radiusTexels);
	}
	if (ShadowOptimizationEnabled(
			SHADOW_OPT_ADAPTIVE_POINT_SAMPLES)) {
		selectedCount = max(
			ShadowPointContributionSampleCount(lightContribution),
			ShadowRadiusSampleCount(radiusTexels));
	}
	return clamp(selectedCount, 1, sampleCount);
}

int ShadowHardwarePcfSampleCount()
{
	if (!ShadowOptimizationEnabled(
			SHADOW_OPT_HARDWARE_REDUCED_PCF)) {
		return ShadowSampleCount();
	}
	// Each GL_LINEAR shadow lookup performs a hardware 2x2 percentage-closer
	// filter, so four disk taps provide sixteen depth comparisons.
	return min(4, ShadowSampleCount());
}

float ShadowRandom(vec2 seed)
{
	const vec2 coefficients = vec2(12.9898, 78.233);
	return fract(sin(dot(seed, coefficients)) * 43758.5453);
}

vec2 RotateShadowDiskOffset(vec2 offset, vec2 rotation)
{
	return vec2(
		offset.x * rotation.x - offset.y * rotation.y,
		offset.x * rotation.y + offset.y * rotation.x);
}

void GenerateLegacyShadowPoissonDisk(vec2 randomSeed)
{
	int sampleCount = ShadowSampleCount();
	int ringCount = clamp(shadowSampleRings, 1, sampleCount);
	float inverseSampleCount = 1.0 / float(sampleCount);
	float angle = ShadowRandom(randomSeed) * SHADOW_TWO_PI;
	float angleStep = SHADOW_TWO_PI * float(ringCount) * inverseSampleCount;
	float radius = inverseSampleCount;
	vec2 direction = vec2(cos(angle), sin(angle));
	vec2 rotation = vec2(cos(angleStep), sin(angleStep));

	for (int i = 0; i < SHADOW_MAX_SAMPLES; ++i) {
		if (i >= sampleCount) break;
		shadowSamplingDisk[i] = direction * pow(radius, 0.75);
		direction = RotateShadowDiskOffset(direction, rotation);
		radius += inverseSampleCount;
	}
}

void GenerateStableShadowVogelDisk()
{
	int sampleCount = ShadowSampleCount();
	shadowSamplingDisk[0] = vec2(0.0);
	if (sampleCount <= 1) return;

	float inverseDistributedCount = 1.0 / float(sampleCount - 1);
	vec2 direction = vec2(1.0, 0.0);
	for (int i = 1; i < SHADOW_MAX_SAMPLES; ++i) {
		if (i >= sampleCount) break;
		float radius = sqrt(
			(float(i) - 0.5) * inverseDistributedCount);
		shadowSamplingDisk[i] = direction * radius;
		direction = RotateShadowDiskOffset(
			direction,
			SHADOW_VOGEL_ROTATION);
	}
}

void GenerateShadowSamplingDisk(vec2 legacyRandomSeed)
{
	if (shadowSamplingPattern == SHADOW_SAMPLING_LEGACY_RANDOM) {
		GenerateLegacyShadowPoissonDisk(legacyRandomSeed);
		return;
	}
	GenerateStableShadowVogelDisk();
}

vec2 ShadowBlockerDiskOffset(int sampleIndex)
{
	vec2 offset = shadowSamplingDisk[sampleIndex];
	if (shadowSamplingPattern == SHADOW_SAMPLING_STABLE_VOGEL) {
		return RotateShadowDiskOffset(
			offset,
			SHADOW_BLOCKER_ROTATION);
	}
	return offset;
}

int ShadowBlockerSampleCount()
{
	int sampleCount = ShadowSampleCount();
	if (shadowSamplingPattern == SHADOW_SAMPLING_STABLE_VOGEL) {
		return min(sampleCount, SHADOW_STABLE_BLOCKER_SAMPLES);
	}
	return sampleCount;
}

int ShadowBlockerSampleIndex(int blockerIndex, int blockerSampleCount)
{
	int sampleCount = ShadowSampleCount();
	if (blockerSampleCount <= 1 || sampleCount <= 1) return 0;
	return blockerIndex * (sampleCount - 1) /
		(blockerSampleCount - 1);
}

bool ProjectShadowCoordinate(vec4 lightSpacePosition, out vec3 projected)
{
	if (lightSpacePosition.w <= SHADOW_EPSILON) return false;
	projected = lightSpacePosition.xyz / lightSpacePosition.w;
	projected = projected * 0.5 + 0.5;
	return projected.z > 0.0 && projected.z < 1.0 &&
		projected.x > 0.0 && projected.x < 1.0 &&
		projected.y > 0.0 && projected.y < 1.0;
}

float ShadowBiasTexelCount(
	float nDotL,
	float minimumTexels,
	float slopeTexels)
{
	return max(
		minimumTexels,
		slopeTexels * (1.0 - clamp(nDotL, 0.0, 1.0)));
}

float ShadowBias2D(
	float nDotL,
	vec3 projected,
	vec4 biasParams)
{
	nDotL = clamp(nDotL, 0.0, 1.0);
	if (!ShadowOptimizationEnabled(
			SHADOW_OPT_TEXEL_SCALED_BIAS)) {
		return max(0.005 * (1.0 - nDotL), 0.0005);
	}

	float biasTexels = ShadowBiasTexelCount(
		nDotL,
		shadowBias2DMinTexels,
		shadowBias2DSlopeTexels);
	float normalizedDepthPerTexel = max(
		biasParams.x,
		SHADOW_EPSILON);
	if (biasParams.w > 0.5) {
		// For a perspective projection depth = A - B / distance.
		// Moving the receiver toward the light by the fractional distance
		// a therefore changes depth by (A - depth) * a / (1 - a).
		// This is algebraically equivalent to linearizing and projecting
		// again, but avoids two reciprocal-heavy conversions per fragment.
		float distanceFraction = clamp(
			max(biasParams.z, SHADOW_EPSILON) * biasTexels,
			0.0,
			1.0 - SHADOW_EPSILON);
		float depthBias =
			max(biasParams.x - projected.z, 0.0) *
			distanceFraction /
			max(1.0 - distanceFraction, SHADOW_EPSILON);
		return max(
			min(depthBias, projected.z),
			SHADOW_EPSILON);
	}

	return max(
		normalizedDepthPerTexel * biasTexels,
		SHADOW_EPSILON);
}

float FilterShadow2D(
	sampler2D shadowMap,
	vec3 projected,
	float bias,
	vec2 texelSize,
	float radiusTexels,
	int selectedSampleCount)
{
	float shadow = 0.0;
	int sampleCount =
		clamp(selectedSampleCount, 1, ShadowSampleCount());
	for (int i = 0; i < SHADOW_MAX_SAMPLES; ++i) {
		if (i >= sampleCount) break;
		int sampleIndex =
			ShadowSubsampleIndex(i, sampleCount);
		vec2 sampleUv =
			projected.xy + shadowSamplingDisk[sampleIndex] *
			texelSize * radiusTexels;
		float closestDepth = 1.0;
		if (sampleUv.x > 0.0 && sampleUv.x < 1.0 &&
			sampleUv.y > 0.0 && sampleUv.y < 1.0) {
			closestDepth = texture(shadowMap, sampleUv).r;
		}
		shadow += projected.z - bias > closestDepth ? 1.0 : 0.0;
	}
	return shadow / float(sampleCount);
}

float CompareShadow2D(
	sampler2DShadow shadowMap,
	vec2 sampleUv,
	float referenceDepth)
{
	if (sampleUv.x <= 0.0 || sampleUv.x >= 1.0 ||
		sampleUv.y <= 0.0 || sampleUv.y >= 1.0) {
		return 0.0;
	}
	float visibility = texture(
		shadowMap,
		vec3(sampleUv, referenceDepth));
	return 1.0 - visibility;
}

float FilterShadowCompare2D(
	sampler2DShadow shadowMap,
	vec3 projected,
	float bias,
	vec2 texelSize,
	float radiusTexels,
	int selectedSampleCount)
{
	float shadow = 0.0;
	int sampleCount =
		clamp(selectedSampleCount, 1, ShadowSampleCount());
	float referenceDepth = projected.z - bias;
	for (int i = 0; i < SHADOW_MAX_SAMPLES; ++i) {
		if (i >= sampleCount) break;
		int sampleIndex =
			ShadowSubsampleIndex(i, sampleCount);
		vec2 sampleUv =
			projected.xy + shadowSamplingDisk[sampleIndex] *
			texelSize * radiusTexels;
		shadow += CompareShadow2D(
			shadowMap,
			sampleUv,
			referenceDepth);
	}
	return shadow / float(sampleCount);
}

float FindAverageBlocker2D(
	sampler2D shadowMap,
	vec3 projected,
	float bias,
	vec2 texelSize,
	bool linearizePerspectiveDepth,
	vec2 perspectiveDepthParams)
{
	float blockerDepthSum = 0.0;
	int blockerCount = 0;
	int blockerSampleCount = ShadowBlockerSampleCount();
	for (int i = 0; i < SHADOW_MAX_SAMPLES; ++i) {
		if (i >= blockerSampleCount) break;
		int sampleIndex =
			ShadowBlockerSampleIndex(i, blockerSampleCount);
		vec2 sampleUv = projected.xy +
			ShadowBlockerDiskOffset(sampleIndex) * texelSize *
			SHADOW_PCSS_SEARCH_RADIUS_TEXELS;
		if (sampleUv.x <= 0.0 || sampleUv.x >= 1.0 ||
			sampleUv.y <= 0.0 || sampleUv.y >= 1.0) {
			continue;
		}
		float sampleDepth = texture(shadowMap, sampleUv).r;
		// The occlusion decision stays in the stored projected-depth domain.
		// Only accepted Spot blockers are converted before distance averaging.
		// 1 / (A - depth) is distance / B. The common B scale cancels in
		// the receiver/blocker ratio and avoids one multiply per blocker.
		if (sampleDepth < projected.z - bias) {
			// For a valid [0, 1] perspective depth, A - depth is bounded by
			// A - 1. Use that projection-derived floor instead of the generic
			// shadow epsilon: with a very small near plane, A - 1 can legally
			// be below 1e-5 and clamping to that value distorts the distance.
			float perspectiveDepthDenominator = max(
				perspectiveDepthParams.x - sampleDepth,
				max(perspectiveDepthParams.x - 1.0, 1e-8));
			blockerDepthSum += linearizePerspectiveDepth
				? 1.0 / perspectiveDepthDenominator
				: sampleDepth;
			++blockerCount;
		}
	}
	return blockerCount > 0
		? blockerDepthSum / float(blockerCount)
		: -1.0;
}

float SampleShadow2D(
	sampler2D shadowMap,
	sampler2DShadow shadowCompareMap,
	vec4 lightSpacePosition,
	float nDotL,
	vec4 shadowBiasParams)
{
	vec3 projected;
	if (!ProjectShadowCoordinate(lightSpacePosition, projected)) return 0.0;

	float bias = ShadowBias2D(
		nDotL,
		projected,
		shadowBiasParams);
	bool hardwareCompare = ShadowOptimizationEnabled(
		SHADOW_OPT_HARDWARE_DEPTH_COMPARE) &&
		shadowType == SHADOW_MODE_PCF;
	if (shadowType == SHADOW_MODE_HARD) {
		if (hardwareCompare) {
			return CompareShadow2D(
				shadowCompareMap,
				projected.xy,
				projected.z - bias);
		}
		float closestDepth = texture(shadowMap, projected.xy).r;
		return projected.z - bias > closestDepth ? 1.0 : 0.0;
	}

	vec2 shadowMapSize = hardwareCompare &&
		shadowType == SHADOW_MODE_PCF
		? vec2(textureSize(shadowCompareMap, 0))
		: vec2(textureSize(shadowMap, 0));
	shadowMapSize = max(shadowMapSize, vec2(1.0));
	vec2 texelSize = 1.0 / shadowMapSize;
	GenerateShadowSamplingDisk(
		projected.xy * shadowMapSize +
		projected.z * vec2(37.0, 17.0));

	if (shadowType == SHADOW_MODE_PCF) {
		if (hardwareCompare) {
			return FilterShadowCompare2D(
				shadowCompareMap,
				projected,
				bias,
				texelSize,
				SHADOW_PCF_RADIUS_TEXELS,
				ShadowHardwarePcfSampleCount());
		}
		return FilterShadow2D(
			shadowMap,
			projected,
			bias,
			texelSize,
			SHADOW_PCF_RADIUS_TEXELS,
			ShadowSampleCount());
	}

	bool spotPcssLinearDepth =
		shadowBiasParams.w > 0.5 &&
		ShadowOptimizationEnabled(
			SHADOW_OPT_SPOT_PCSS_LINEAR_DEPTH);
	float averageBlockerDepth = FindAverageBlocker2D(
		shadowMap,
		projected,
		bias,
		texelSize,
		spotPcssLinearDepth,
		shadowBiasParams.xy);
	if (averageBlockerDepth < 0.0) return 0.0;

	float receiverDepth = spotPcssLinearDepth
		// Perspective clip-space w is linear light-view distance. Params.y
		// is 1 / B, so receiver and blocker use the same scaled domain.
		? max(
			lightSpacePosition.w * shadowBiasParams.y,
			SHADOW_EPSILON)
		: projected.z;
	float penumbraRatio = max(
		(receiverDepth - averageBlockerDepth) /
			max(averageBlockerDepth, SHADOW_EPSILON),
		0.0);
	float filterRadiusTexels = clamp(
		SHADOW_PCSS_LIGHT_SIZE_TEXELS * penumbraRatio,
		1.0,
		SHADOW_PCSS_MAX_RADIUS_TEXELS);
	return FilterShadow2D(
		shadowMap,
		projected,
		bias,
		texelSize,
		filterRadiusTexels,
		ShadowPcssFilterSampleCount(
			filterRadiusTexels,
			spotPcssLinearDepth));
}

void BuildShadowCubeBasis(
	vec3 direction,
	out vec3 tangent,
	out vec3 bitangent)
{
	vec3 helper = abs(direction.y) < 0.999
		? vec3(0.0, 1.0, 0.0)
		: vec3(1.0, 0.0, 0.0);
	tangent = normalize(cross(helper, direction));
	bitangent = cross(direction, tangent);
}

vec3 OffsetShadowCubeDirection(
	vec3 direction,
	vec3 tangent,
	vec3 bitangent,
	vec2 diskOffset,
	float radiusTexels,
	float shadowMapSize)
{
	float angularRadius =
		2.0 * radiusTexels / max(shadowMapSize, 1.0);
	return normalize(
		direction +
		(tangent * diskOffset.x + bitangent * diskOffset.y) *
			angularRadius);
}

float FilterShadowCube(
	samplerCube shadowMap,
	vec3 direction,
	vec3 tangent,
	vec3 bitangent,
	float receiverDepth,
	float bias,
	float shadowMapSize,
	float radiusTexels,
	int selectedSampleCount)
{
	float shadow = 0.0;
	int sampleCount =
		clamp(selectedSampleCount, 1, ShadowSampleCount());
	for (int i = 0; i < SHADOW_MAX_SAMPLES; ++i) {
		if (i >= sampleCount) break;
		int sampleIndex =
			ShadowSubsampleIndex(i, sampleCount);
		vec3 sampleDirection = OffsetShadowCubeDirection(
			direction,
			tangent,
			bitangent,
			shadowSamplingDisk[sampleIndex],
			radiusTexels,
			shadowMapSize);
		float closestDepth = texture(shadowMap, sampleDirection).r;
		shadow += receiverDepth - bias > closestDepth ? 1.0 : 0.0;
	}
	return shadow / float(sampleCount);
}

float FindAverageBlockerCube(
	samplerCube shadowMap,
	vec3 direction,
	vec3 tangent,
	vec3 bitangent,
	float receiverDepth,
	float bias,
	float shadowMapSize)
{
	float blockerDepthSum = 0.0;
	int blockerCount = 0;
	int blockerSampleCount = ShadowBlockerSampleCount();
	for (int i = 0; i < SHADOW_MAX_SAMPLES; ++i) {
		if (i >= blockerSampleCount) break;
		int sampleIndex =
			ShadowBlockerSampleIndex(i, blockerSampleCount);
		vec3 sampleDirection = OffsetShadowCubeDirection(
			direction,
			tangent,
			bitangent,
			ShadowBlockerDiskOffset(sampleIndex),
			SHADOW_PCSS_SEARCH_RADIUS_TEXELS,
			shadowMapSize);
		float sampleDepth = texture(shadowMap, sampleDirection).r;
		if (sampleDepth < receiverDepth - bias) {
			blockerDepthSum += sampleDepth;
			++blockerCount;
		}
	}
	return blockerCount > 0
		? blockerDepthSum / float(blockerCount)
		: -1.0;
}

float SampleShadowCubePrepared(
	samplerCube shadowMap,
	vec3 direction,
	float currentDepth,
	float nDotL,
	float farPlane,
	float lightContribution)
{
	if (farPlane <= SHADOW_EPSILON ||
		currentDepth <= SHADOW_EPSILON ||
		currentDepth >= farPlane) {
		return 0.0;
	}

	float receiverDepth = currentDepth / farPlane;
	float shadowMapSize =
		max(float(textureSize(shadowMap, 0).x), 1.0);
	float bias = max(0.02 * (1.0 - nDotL), 0.005) / farPlane;
	if (ShadowOptimizationEnabled(
		SHADOW_OPT_TEXEL_SCALED_BIAS)) {
		vec3 absoluteDirection = abs(direction);
		float faceDepthScale = max(
			absoluteDirection.x,
			max(absoluteDirection.y, absoluteDirection.z));
		float normalizedDepthPerTexel =
			2.0 * receiverDepth * faceDepthScale /
			shadowMapSize;
		bias = max(
			normalizedDepthPerTexel *
			ShadowBiasTexelCount(
				nDotL,
				shadowBiasCubeMinTexels,
				shadowBiasCubeSlopeTexels),
			SHADOW_EPSILON);
	}
	if (shadowType == SHADOW_MODE_HARD) {
		return receiverDepth - bias > texture(shadowMap, direction).r
			? 1.0
			: 0.0;
	}

	GenerateShadowSamplingDisk(
		direction.xy * 257.0 +
		vec2(direction.z * 113.0, direction.z * 53.0));
	vec3 tangent;
	vec3 bitangent;
	BuildShadowCubeBasis(direction, tangent, bitangent);

	if (shadowType == SHADOW_MODE_PCF) {
		return FilterShadowCube(
			shadowMap,
			direction,
			tangent,
			bitangent,
			receiverDepth,
			bias,
			shadowMapSize,
			SHADOW_PCF_RADIUS_TEXELS,
			ShadowPointContributionSampleCount(lightContribution));
	}

	float averageBlockerDepth = FindAverageBlockerCube(
		shadowMap,
		direction,
		tangent,
		bitangent,
		receiverDepth,
		bias,
		shadowMapSize);
	if (averageBlockerDepth < 0.0) return 0.0;

	float penumbraRatio = max(
		(receiverDepth - averageBlockerDepth) /
			max(averageBlockerDepth, SHADOW_EPSILON),
		0.0);
	float filterRadiusTexels = clamp(
		SHADOW_PCSS_LIGHT_SIZE_TEXELS * penumbraRatio,
		1.0,
		SHADOW_PCSS_MAX_RADIUS_TEXELS);
	return FilterShadowCube(
		shadowMap,
		direction,
		tangent,
		bitangent,
		receiverDepth,
		bias,
		shadowMapSize,
		filterRadiusTexels,
		ShadowPointPcssFilterSampleCount(
			filterRadiusTexels,
			lightContribution));
}

float SampleShadowCubePrepared(
	samplerCube shadowMap,
	vec3 direction,
	float currentDepth,
	float nDotL,
	float farPlane)
{
	return SampleShadowCubePrepared(
		shadowMap,
		direction,
		currentDepth,
		nDotL,
		farPlane,
		1.0);
}

float SampleShadowCubePrepared(
	samplerCube shadowMap,
	vec3 direction,
	float currentDepth,
	vec3 normal,
	float farPlane)
{
	float nDotL = max(
		dot(normalize(normal), -direction),
		0.0);
	return SampleShadowCubePrepared(
		shadowMap,
		direction,
		currentDepth,
		nDotL,
		farPlane);
}

float SampleShadowCube(
	samplerCube shadowMap,
	vec3 lightToSurface,
	vec3 normal,
	float farPlane)
{
	float currentDepth = length(lightToSurface);
	if (farPlane <= SHADOW_EPSILON ||
		currentDepth <= SHADOW_EPSILON ||
		currentDepth >= farPlane) {
		return 0.0;
	}
	return SampleShadowCubePrepared(
		shadowMap,
		lightToSurface / currentDepth,
		currentDepth,
		normal,
		farPlane);
}
