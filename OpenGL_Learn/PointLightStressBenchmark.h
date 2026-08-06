#pragma once

#include "Scene.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

struct PointLightStressOptions {
	bool enabled = false;
	int lightCount = 64;
	std::uint32_t seed = 0x21D3F3A5u;
	std::string coverage = "representative";
	bool targetRadiusExplicit = false;
	float targetRadius = 0.0f;
	int warmupFrames = 300;
	int sampleFrames = 600;
	std::string resultPath;
	std::string capturePath;
	bool renderDocMarkers = false;
	bool stencilLifecycleCheck = false;
	std::string gridUpdate = "cached";
	bool gridUpdateExplicit = false;
	int gridSliceCount = 0;
	bool gridSliceCountExplicit = false;
	bool gridSliceCycle = false;
	int viewportWidth = 1920;
	int viewportHeight = 1080;
};

struct PointLightStressRecord {
	glm::vec3 position = glm::vec3(0.0f);
	glm::vec3 diffuse = glm::vec3(0.0f);
};

struct PointLightStressState {
	static constexpr const char* kGeneratorVersion =
		"point-light-heavy-xorshift32-v1";
	static constexpr const char* kTargetRadiusGeneratorVersion =
		"point-light-count-radius-xorshift32-v2";

	bool configured = false;
	std::string generatorVersion = kGeneratorVersion;
	std::uint64_t sceneSignature = 0;
	std::uint64_t submissionSignature = 0;
	std::uint64_t positionPrefixSignature = 0;
	std::uint64_t colorPrefixSignature = 0;
	float constant = 1.0f;
	float linear = 0.25f;
	float quadratic = 0.16f;
	float diffuseIntensity = 0.08f;
	bool targetRadiusExplicit = false;
	float requestedRadius = 0.0f;
	float volumeRadius = 0.0f;
	float attenuationThreshold = 0.0f;
	float radiusAbsoluteError = 0.0f;
	float radiusRelativeError = 0.0f;
	int nearPlaneFixtureCount = 0;
	int cameraInsideFixtureCount = 0;
	int offscreenFixtureCount = 0;
	int depthSliceBoundaryFixtureCount = 0;
	bool nearPlaneFixtureIntersects = false;
	bool cameraInsideFixtureContainsCamera = false;
	bool offscreenFixtureOutsideFrustum = false;
	bool depthSliceBoundaryFixturePlaced = false;
	float depthSliceBoundaryDistance = 0.0f;
	std::vector<PointLightStressRecord> records;
};

inline bool ParsePointLightStressOptions(
	int argc,
	char** argv,
	PointLightStressOptions& options,
	std::string& errorMessage)
{
	bool sawOption = false;
	for (int index = 1; index < argc; ++index) {
		const std::string argument = argv[index];
		if (argument == "--point-light-stress") {
			options.enabled = true;
			continue;
		}
		if (argument == "--point-light-renderdoc-markers") {
			sawOption = true;
			options.renderDocMarkers = true;
			continue;
		}
		if (argument == "--point-light-stencil-lifecycle-check") {
			sawOption = true;
			options.stencilLifecycleCheck = true;
			continue;
		}
		if (argument == "--point-light-grid-slice-cycle") {
			sawOption = true;
			options.gridSliceCycle = true;
			continue;
		}
		auto readValue = [&](const char* name, std::string& value) {
			if (argument != name) return false;
			sawOption = true;
			if (index + 1 >= argc) {
				errorMessage = std::string(name) + " requires a value";
				return true;
			}
			value = argv[++index];
			return true;
		};
		auto parseInt = [&](const char* name, int& value) {
			std::string text;
			if (!readValue(name, text)) return false;
			if (!errorMessage.empty()) return true;
			try {
				std::size_t consumed = 0;
				const long parsed = std::stol(text, &consumed, 10);
				if (consumed != text.size() || parsed < 0 ||
					parsed > (std::numeric_limits<int>::max)()) {
					throw std::out_of_range("point-light integer");
				}
				value = static_cast<int>(parsed);
			}
			catch (...) {
				errorMessage = std::string(name) +
					" requires a non-negative integer";
			}
			return true;
		};
		auto parseFloat = [&](const char* name, float& value) {
			std::string text;
			if (!readValue(name, text)) return false;
			if (!errorMessage.empty()) return true;
			try {
				std::size_t consumed = 0;
				const float parsed = std::stof(text, &consumed);
				if (consumed != text.size() || !std::isfinite(parsed)) {
					throw std::out_of_range("point-light float");
				}
				value = parsed;
			}
			catch (...) {
				errorMessage = std::string(name) +
					" requires a finite floating-point value";
			}
			return true;
		};

		std::string value;
		if (parseInt("--point-light-count", options.lightCount) ||
			parseInt("--point-light-grid-slices", options.gridSliceCount) ||
			parseInt("--point-light-warmup-frames", options.warmupFrames) ||
			parseInt("--point-light-sample-frames", options.sampleFrames) ||
			parseInt("--point-light-width", options.viewportWidth) ||
			parseInt("--point-light-height", options.viewportHeight)) {
			if (!errorMessage.empty()) return false;
			if (argument == "--point-light-grid-slices") {
				options.gridSliceCountExplicit = true;
			}
			continue;
		}
		if (readValue("--point-light-coverage", options.coverage) ||
			readValue("--point-light-result", options.resultPath) ||
			readValue("--point-light-capture", options.capturePath)) {
			if (!errorMessage.empty()) return false;
			continue;
		}
		if (readValue("--point-light-grid-update", options.gridUpdate)) {
			if (!errorMessage.empty()) return false;
			options.gridUpdateExplicit = true;
			continue;
		}
		if (readValue("--point-light-seed", value)) {
			if (!errorMessage.empty()) return false;
			try {
				std::size_t consumed = 0;
				const unsigned long long parsed =
					std::stoull(value, &consumed, 0);
				if (consumed != value.size() ||
					parsed > (std::numeric_limits<std::uint32_t>::max)()) {
					throw std::out_of_range("point-light seed");
				}
				options.seed = static_cast<std::uint32_t>(parsed);
			}
			catch (...) {
				errorMessage =
					"--point-light-seed requires a 32-bit integer";
				return false;
			}
		}
		if (parseFloat("--point-light-target-radius", options.targetRadius)) {
			if (!errorMessage.empty()) return false;
			options.targetRadiusExplicit = true;
			continue;
		}
	}

	if (sawOption && !options.enabled) {
		errorMessage =
			"point-light benchmark options require --point-light-stress";
		return false;
	}
	if (!options.enabled) return true;
	if (options.lightCount != 0 && options.lightCount != 1 &&
		options.lightCount != 16 &&
		options.lightCount != 32 &&
		options.lightCount != 64 &&
		options.lightCount != 128 &&
		options.lightCount != 256 && options.lightCount != 512) {
		errorMessage = "--point-light-count must be 0, 1, 16, 32, 64, 128, 256, or 512";
		return false;
	}
	if (options.targetRadiusExplicit &&
		(options.targetRadius < 0.1f || options.targetRadius > 1000.0f)) {
		errorMessage = "--point-light-target-radius must be in [0.1, 1000]";
		return false;
	}
	if (options.gridUpdate != "cached" && options.gridUpdate != "rebuild") {
		errorMessage = "--point-light-grid-update requires cached or rebuild";
		return false;
	}
	if (options.gridSliceCountExplicit &&
		options.gridSliceCount != 1 && options.gridSliceCount != 2 &&
		options.gridSliceCount != 4 && options.gridSliceCount != 8 &&
		options.gridSliceCount != 16) {
		errorMessage = "--point-light-grid-slices requires 1, 2, 4, 8, or 16";
		return false;
	}
	if (options.coverage != "representative" &&
		options.coverage != "small-local" &&
		options.coverage != "medium-local" &&
		options.coverage != "high-overlap" &&
		options.coverage != "edge-cases") {
		errorMessage =
			"--point-light-coverage must be small-local, medium-local, representative, high-overlap, or edge-cases";
		return false;
	}
	if (options.viewportWidth < 64 || options.viewportHeight < 64 ||
		options.viewportWidth > 16384 || options.viewportHeight > 16384) {
		errorMessage = "point-light viewport dimensions must be in [64, 16384]";
		return false;
	}
	if (options.warmupFrames < 1 || options.sampleFrames < 1 ||
		options.warmupFrames > 1000000 || options.sampleFrames > 1000000 ||
		options.warmupFrames >
			(std::numeric_limits<int>::max)() - options.sampleFrames) {
		errorMessage = "point-light warm-up/sample frame counts are invalid";
		return false;
	}
	const std::string stem = options.coverage + "-" +
		std::to_string(options.lightCount);
	if (options.resultPath.empty()) {
		options.resultPath =
			"benchmark-results/point-light-heavy/" + stem + ".json";
	}
	if (options.capturePath.empty()) {
		options.capturePath =
			"docs/benchmark-images/point-light-heavy/" + stem + ".ppm";
	}
	return true;
}

namespace PointLightStressDetail {
	inline void HashBytes(
		std::uint64_t& hash,
		const void* data,
		std::size_t size)
	{
		const auto* bytes = static_cast<const unsigned char*>(data);
		for (std::size_t index = 0; index < size; ++index) {
			hash ^= bytes[index];
			hash *= 1099511628211ull;
		}
	}

	template <typename Value>
	inline void HashValue(std::uint64_t& hash, const Value& value)
	{
		HashBytes(hash, &value, sizeof(value));
	}

	inline void HashString(std::uint64_t& hash, const std::string& value)
	{
		HashBytes(hash, value.data(), value.size());
		const unsigned char terminator = 0;
		HashBytes(hash, &terminator, 1);
	}

	class XorShift32 {
	public:
		explicit XorShift32(std::uint32_t seed)
			: m_state(seed == 0 ? 0x6D2B79F5u : seed) {}

		float Uniform(float minimum, float maximum)
		{
			std::uint32_t value = m_state;
			value ^= value << 13;
			value ^= value >> 17;
			value ^= value << 5;
			m_state = value;
			const float normalized = static_cast<float>(value >> 8) /
				16777216.0f;
			return minimum + (maximum - minimum) * normalized;
		}

	private:
		std::uint32_t m_state;
	};
}

inline std::string FormatPointLightStressSignature(std::uint64_t signature)
{
	std::ostringstream stream;
	stream << "0x" << std::hex << std::setw(16) << std::setfill('0')
		<< signature;
	return stream.str();
}

inline std::uint64_t ComputePointLightStressSubmissionSignature(
	const Scene& scene)
{
	std::uint64_t hash = 1469598103934665603ull;
	const std::uint64_t count = scene.lightSource.pointLights.size();
	PointLightStressDetail::HashValue(hash, count);
	for (const PointLight& light : scene.lightSource.pointLights) {
		PointLightStressDetail::HashValue(hash, light.position.x);
		PointLightStressDetail::HashValue(hash, light.position.y);
		PointLightStressDetail::HashValue(hash, light.position.z);
		PointLightStressDetail::HashValue(hash, light.ambient.x);
		PointLightStressDetail::HashValue(hash, light.ambient.y);
		PointLightStressDetail::HashValue(hash, light.ambient.z);
		PointLightStressDetail::HashValue(hash, light.diffuse.x);
		PointLightStressDetail::HashValue(hash, light.diffuse.y);
		PointLightStressDetail::HashValue(hash, light.diffuse.z);
		PointLightStressDetail::HashValue(hash, light.specular.x);
		PointLightStressDetail::HashValue(hash, light.specular.y);
		PointLightStressDetail::HashValue(hash, light.specular.z);
		PointLightStressDetail::HashValue(hash, light.constant);
		PointLightStressDetail::HashValue(hash, light.linear);
		PointLightStressDetail::HashValue(hash, light.quadratic);
		const std::uint8_t active = light.m_active ? 1u : 0u;
		const std::uint8_t shadow = light.useShadowMap ? 1u : 0u;
		PointLightStressDetail::HashValue(hash, active);
		PointLightStressDetail::HashValue(hash, shadow);
	}
	return hash;
}

inline bool ConfigurePointLightStressScene(
	Scene& scene,
	const PointLightStressOptions& options,
	const glm::vec3& cameraPosition,
	const glm::vec3& cameraTarget,
	const glm::vec3& cameraUp,
	PointLightStressState& state,
	std::string& errorMessage)
{
	if (!options.enabled) return true;
	if (scene.lightSource.pointLights.empty()) {
		errorMessage = "default point-light proxy is unavailable";
		return false;
	}

	const PointLight prototype = scene.lightSource.pointLights.front();
	state = {};
	state.targetRadiusExplicit = options.targetRadiusExplicit;
	if (options.targetRadiusExplicit) {
		state.generatorVersion =
			PointLightStressState::kTargetRadiusGeneratorVersion;
		state.requestedRadius = options.targetRadius;
		state.linear = 0.0f;
		state.attenuationThreshold =
			(256.0f / 5.0f) * state.diffuseIntensity;
		state.quadratic =
			(state.attenuationThreshold - state.constant) /
			(options.targetRadius * options.targetRadius);
	}
	else if (options.coverage == "high-overlap") {
		state.linear = 0.08f;
		state.quadratic = 0.024f;
	}
	else if (options.coverage == "small-local") {
		state.linear = 0.70f;
		state.quadratic = 0.90f;
	}
	else if (options.coverage == "medium-local") {
		state.linear = 0.35f;
		state.quadratic = 0.25f;
	}
	else {
		state.linear = 0.25f;
		state.quadratic = 0.16f;
	}
	state.volumeRadius = ComputePointLightStencilVolumeRadius(
		state.constant,
		state.linear,
		state.quadratic,
		glm::vec3(state.diffuseIntensity),
		1.0f,
		1.0f);
	if (!options.targetRadiusExplicit) {
		state.requestedRadius = state.volumeRadius;
		state.attenuationThreshold =
			(256.0f / 5.0f) * state.diffuseIntensity;
	}
	state.radiusAbsoluteError =
		std::fabs(state.volumeRadius - state.requestedRadius);
	state.radiusRelativeError = state.requestedRadius > 0.0f
		? state.radiusAbsoluteError / state.requestedRadius
		: 0.0f;
	if (options.targetRadiusExplicit &&
		(!std::isfinite(state.quadratic) || state.quadratic <= 0.0f ||
		!std::isfinite(state.volumeRadius) ||
		state.radiusAbsoluteError > 1e-4f)) {
		errorMessage = "target point-light radius could not be reproduced within tolerance";
		return false;
	}
	state.records.reserve(static_cast<std::size_t>(options.lightCount));

	const std::array<glm::vec3, 8> palette = {
		glm::vec3(1.00f, 0.55f, 0.35f),
		glm::vec3(0.35f, 0.65f, 1.00f),
		glm::vec3(0.45f, 1.00f, 0.55f),
		glm::vec3(1.00f, 0.40f, 0.75f),
		glm::vec3(0.80f, 0.55f, 1.00f),
		glm::vec3(1.00f, 0.82f, 0.40f),
		glm::vec3(0.35f, 1.00f, 0.95f),
		glm::vec3(1.00f, 0.70f, 0.60f)
	};
	const glm::vec3 forward = glm::normalize(cameraTarget - cameraPosition);
	glm::vec3 right = glm::cross(forward, glm::normalize(cameraUp));
	if (glm::dot(right, right) < 0.000001f) right = glm::vec3(0.0f, 0.0f, 1.0f);
	right = glm::normalize(right);
	PointLightStressDetail::XorShift32 random(options.seed);

	for (int index = 0; index < options.lightCount; ++index) {
		PointLightStressRecord record;
		if (options.targetRadiusExplicit) {
			// v2 fixes random-consumption order explicitly. The legacy path below
			// stays byte-for-byte unchanged when no target radius is requested.
			const float x = random.Uniform(-10.5f, 10.5f);
			const float y = random.Uniform(-3.2f, 3.0f);
			const float z = random.Uniform(-4.2f, 4.2f);
			record.position = glm::vec3(x, y, z);
		}
		else if (options.coverage == "high-overlap") {
			record.position = glm::vec3(
				random.Uniform(-4.5f, 9.5f),
				random.Uniform(-2.8f, 2.0f),
				random.Uniform(-3.2f, 3.2f));
		}
		else {
			record.position = glm::vec3(
				random.Uniform(-10.5f, 10.5f),
				random.Uniform(-3.2f, 3.0f),
				random.Uniform(-4.2f, 4.2f));
		}
		if (options.coverage == "edge-cases") {
			if (index == 0) {
				record.position = cameraPosition +
					forward * (state.volumeRadius + 0.05f);
				state.nearPlaneFixtureCount = 1;
				state.nearPlaneFixtureIntersects = true;
			}
			else if (index == 1) {
				record.position = cameraPosition;
				state.cameraInsideFixtureCount = 1;
				state.cameraInsideFixtureContainsCamera = true;
			}
			else if (index == 2) {
				record.position = cameraPosition -
					forward * (state.volumeRadius + 5.0f) + right * 0.25f;
				state.offscreenFixtureCount = 1;
				state.offscreenFixtureOutsideFrustum = true;
			}
			else if (index == 3) {
				// Center this light exactly on the middle logarithmic depth-slice
				// boundary for near=0.1, far=100, and 16 slices. Its finite
				// radius deliberately spans both adjacent slices, exercising the
				// conservative inclusive membership at the boundary.
				constexpr float nearPlane = 0.1f;
				constexpr float farPlane = 100.0f;
				constexpr float normalizedBoundary = 8.0f / 16.0f;
				state.depthSliceBoundaryDistance = nearPlane * std::pow(
					farPlane / nearPlane,
					normalizedBoundary);
				record.position = cameraPosition +
					forward * state.depthSliceBoundaryDistance;
				state.depthSliceBoundaryFixtureCount = 1;
				state.depthSliceBoundaryFixturePlaced = true;
			}
		}
		record.diffuse = palette[static_cast<std::size_t>(index) % palette.size()] *
			state.diffuseIntensity;
		state.records.push_back(record);
	}
	state.positionPrefixSignature = 1469598103934665603ull;
	state.colorPrefixSignature = 1469598103934665603ull;
	const std::uint64_t recordCount = state.records.size();
	PointLightStressDetail::HashValue(
		state.positionPrefixSignature,
		recordCount);
	PointLightStressDetail::HashValue(
		state.colorPrefixSignature,
		recordCount);
	for (const PointLightStressRecord& record : state.records) {
		PointLightStressDetail::HashValue(
			state.positionPrefixSignature,
			record.position.x);
		PointLightStressDetail::HashValue(
			state.positionPrefixSignature,
			record.position.y);
		PointLightStressDetail::HashValue(
			state.positionPrefixSignature,
			record.position.z);
		PointLightStressDetail::HashValue(
			state.colorPrefixSignature,
			record.diffuse.x);
		PointLightStressDetail::HashValue(
			state.colorPrefixSignature,
			record.diffuse.y);
		PointLightStressDetail::HashValue(
			state.colorPrefixSignature,
			record.diffuse.z);
	}

	scene.lightSource.pointLights.clear();
	scene.lightSource.pointLights.reserve(state.records.size());
	for (std::size_t index = 0; index < state.records.size(); ++index) {
		PointLight light = prototype;
		light.SetName("Point Light Stress " + std::to_string(index));
		light.SetPosition(state.records[index].position);
		light.SetScale(0.12f);
		light.SetActiveStatus(true);
		light.ambient = glm::vec3(0.0f);
		light.diffuse = state.records[index].diffuse;
		light.specular = state.records[index].diffuse * 0.5f;
		light.constant = state.constant;
		light.linear = state.linear;
		light.quadratic = state.quadratic;
		light.useShadowMap = false;
		light.shadowFBO = nullptr;
		light.shadowCache.Invalidate();
		light.shadowFaceCache.Invalidate();
		scene.lightSource.pointLights.push_back(light);
	}
	scene.lightSource.directionLights.clear();
	scene.lightSource.spotLights.clear();
	scene.SetDrawPointLightMarkers(false);
	scene.SetPointLightRenderDocMarkers(options.renderDocMarkers);

	state.submissionSignature =
		ComputePointLightStressSubmissionSignature(scene);
	std::uint64_t sceneHash = 1469598103934665603ull;
	PointLightStressDetail::HashString(
		sceneHash,
		state.generatorVersion);
	PointLightStressDetail::HashString(sceneHash, "classic-scenes/sponza/sponza.obj");
	PointLightStressDetail::HashString(sceneHash, options.coverage);
	PointLightStressDetail::HashValue(sceneHash, options.seed);
	PointLightStressDetail::HashValue(sceneHash, options.lightCount);
	PointLightStressDetail::HashValue(sceneHash, cameraPosition.x);
	PointLightStressDetail::HashValue(sceneHash, cameraPosition.y);
	PointLightStressDetail::HashValue(sceneHash, cameraPosition.z);
	PointLightStressDetail::HashValue(sceneHash, cameraTarget.x);
	PointLightStressDetail::HashValue(sceneHash, cameraTarget.y);
	PointLightStressDetail::HashValue(sceneHash, cameraTarget.z);
	PointLightStressDetail::HashValue(sceneHash, state.submissionSignature);
	state.sceneSignature = sceneHash;
	state.configured = true;
	return true;
}
