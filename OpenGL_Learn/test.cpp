#pragma once
//
//#define USE_GEOMETRY_SHADER
#define USE_SCENE_SHADER
//#define USE_PLANET_SHADER
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4005)
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif
#include "third_party/renderdoc/renderdoc_app.h"
#endif
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include "Learn.h"
#include "Model.h"
#include <iostream>
#include "callbacks.h"
#include "Camera.h"
#include "Scene.h"
#include "mygui.h"
#include "ShaderManager.h"
#include "Global.h"
#include "ModelsLoader.h"
#include "Timer.h"
#include "ForwardRenderPass.h"
#include "DeferRenderPass.h"
#include "PostprocessRenderPass.h"
#include "Profiler.h"
#include "PerformanceBenchmark.h"
#include "GLStateCache.h"
#include "ImageBasedLighting.h"
#include "SceneStateIO.h"
#include "EditorSceneManager.h"
#include "BenchmarkMotionTimeline.h"
#include "SubmissionStressScene.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <vector>
#ifdef _MSC_VER
#pragma warning(pop)
#endif


bool firstMouse = true;
bool lastFrameMkeyState = false;


auto& properties = SystemProperties::GetInstance();
auto& xmlMaterialManager = XmlMaterialManager::GetInstance();

Camera camera(5.0f, glm::vec3(0.0f, 1.0f, -3.0f), properties.SCREEN_WIDTH / 2.0f, properties.SCREEN_HEIGHT / 2.0f);
glm::mat4 view, projection;


glm::vec3 coral(1.0f, 0.5f, 0.31f);
glm::vec3 lightColor(1.0f);
glm::vec3 toyColor(1.0f, 0.5f, 0.31f);
glm::vec3 result = lightColor * toyColor;

Timer& timer = Timer::GetInstance();

struct FrameCaptureStats {
	bool valid = false;
	double meanLuminance = 0.0;
	double nonBlackRatio = 0.0;
	std::vector<unsigned char> pixels;
};

struct FloatCaptureStats {
	bool valid = false;
	int width = 0;
	int height = 0;
	int channels = 0;
	std::uint64_t finiteValueCount = 0;
	std::uint64_t nonFiniteValueCount = 0;
	double minimum = 0.0;
	double maximum = 0.0;
	double mean = 0.0;
};

enum class FloatCaptureSource {
	Red,
	Alpha,
	RGB,
};

struct FrameTimingStats {
	std::size_t sampleCount = 0;
	double meanMilliseconds = 0.0;
	double medianMilliseconds = 0.0;
	double p95Milliseconds = 0.0;
	double p99Milliseconds = 0.0;
};

struct BenchmarkTimelineFrameTelemetry {
	int measurementFrame = 0;
	BenchmarkMotionSample motion;
	double wallMilliseconds = 0.0;
	double shadowUpdateCpuMilliseconds = 0.0;
	double cacheCheckCpuMilliseconds = 0.0;
	double casterStateSyncCpuMilliseconds = 0.0;
	double pointShadowFaceDemandCpuMilliseconds = 0.0;
	double pointShadowFaceSignatureCpuMilliseconds = 0.0;
	std::uint64_t updateCount = 0;
	std::uint64_t cacheHitCount = 0;
	std::uint64_t lightCacheHitCount = 0;
	std::uint64_t updatedLightCount = 0;
	std::uint64_t directionalLightUpdateCount = 0;
	std::uint64_t pointLightUpdateCount = 0;
	std::uint64_t pointShadowSubmissionPassCount = 0;
	std::uint64_t pointShadowRequiredFaceCount = 0;
	std::uint64_t pointShadowRenderedFaceCount = 0;
	std::uint64_t pointShadowFaceCacheHitCount = 0;
	std::uint64_t pointShadowDeferredFaceCount = 0;
	std::uint8_t pointShadowRequiredFaceMask = 0;
	std::uint8_t pointShadowUpdateFaceMask = 0;
	std::uint64_t spotLightUpdateCount = 0;
	std::uint64_t casterBoundsRebuildCount = 0;
	std::uint64_t sceneTopologyRevision = 0;
	std::uint64_t sceneTopologyInvalidationCount = 0;
	std::uint64_t sceneTopologyModelCount = 0;
};

struct PointShadowFaceEvidence {
	bool valid = false;
	std::uint64_t bitwiseHash = 0;
	std::uint64_t nonFarSampleCount = 0;
	float minDepth = 0.0f;
	float maxDepth = 0.0f;
};

struct PointShadowCubeEvidence {
	bool valid = false;
	int lightIndex = -1;
	int width = 0;
	int height = 0;
	std::uint64_t sampleCountPerFace = 0;
	std::array<PointShadowFaceEvidence, 6> faces;
};

struct SsaoTemporalCaptureRoi {
	std::string name;
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
};

struct ClassicSceneTestOptions {
	bool enabled = false;
	bool untextured = false;
	bool shadowExperiment = false;
	bool ssaoExperiment = false;
	bool deterministicCameraTimeline = false;
	bool ssaoTemporalCaptureReferenceGuides = false;
	bool captureFinalFrame = true;
	bool gpuSynchronized = false;
	std::string modelPath;
	std::string sceneName;
	std::string capturePath;
	std::string ssaoCapturePath;
	std::string ssaoFloatCapturePath;
	std::string ssaoRawFloatCapturePath;
	std::string ssaoDepthCapturePath;
	std::string ssaoNormalCapturePath;
	std::string ssaoTemporalCaptureDirectory;
	std::string resultPath;
	std::string renderDocCaptureTemplate;
	std::string shadowMode = "off";
	std::string shadowSampling = "stable";
	std::string shadowLights = "directional";
	std::string shadowWorkload = "static-hit";
	std::string shadowVariant = "default";
	std::string renderPath = "pbr-forward";
	std::string ssaoMode = "legacy-full";
	glm::vec3 cameraPosition = glm::vec3(27.0f, 18.0f, 32.0f);
	glm::vec3 cameraTarget = glm::vec3(0.0f);
	glm::vec3 cameraUp = glm::vec3(0.0f, 1.0f, 0.0f);
	glm::vec3 directionalLightDirection =
		glm::vec3(-0.45f, -1.0f, -0.25f);
	glm::vec3 pointLightPosition = glm::vec3(0.0f, 1.5f, -3.0f);
	glm::vec3 spotLightPosition = glm::vec3(0.0f);
	glm::vec3 spotLightDirection = glm::vec3(0.0f, -1.0f, 0.0f);
	bool hasSpotLightPosition = false;
	bool hasSpotLightDirection = false;
	float spotShadowNearPlane = 0.0f;
	float spotShadowFarPlane = 0.0f;
	bool hasSpotShadowNearPlane = false;
	bool hasSpotShadowFarPlane = false;
	float normalizedRadius = 15.0f;
	float worldScale = 1.0f;
	float fov = 45.0f;
	int shadowResolution = 0;
	int width = 1440;
	int height = 900;
	int ssaoSamples = 0;
	int warmupFrames = 15;
	int captureFrame = 60;
	int renderDocCaptureFrame = 0;
	int timelineFixedFramesPerSecond = 60;
	int timelineCycleFrames = 600;
	float cameraTimelinePositionRadiusRatio = 0.05f;
	float cameraTimelineTargetRadiusRatio = 0.01f;
	int ssaoTemporalCaptureStartFrame = 0;
	int ssaoTemporalCaptureFrameCount = 0;
	int ssaoTemporalCaptureStride = 1;
	std::vector<SsaoTemporalCaptureRoi> ssaoTemporalCaptureRois;
};

bool ParseFloatArgument(const std::string& text, float& value)
{
	try {
		std::size_t consumed = 0;
		value = std::stof(text, &consumed);
		return consumed == text.size() && std::isfinite(value);
	}
	catch (...) {
		return false;
	}
}

bool ParseIntArgument(const std::string& text, int& value)
{
	try {
		std::size_t consumed = 0;
		value = std::stoi(text, &consumed);
		return consumed == text.size();
	}
	catch (...) {
		return false;
	}
}

bool ParseClassicSceneTestOptions(
	int argc,
	char** argv,
	ClassicSceneTestOptions& options,
	std::string& errorMessage)
{
	for (int i = 1; i < argc; ++i) {
		const std::string argument = argv[i];
		auto readString = [&](const char* optionName, std::string& value) {
			if (argument != optionName) {
				return false;
			}
			if (i + 1 >= argc) {
				errorMessage = std::string(optionName) + " requires a value";
				return true;
			}
			value = argv[++i];
			return true;
		};
		auto readFloat = [&](const char* optionName, float& value) {
			std::string text;
			if (!readString(optionName, text)) {
				return false;
			}
			if (errorMessage.empty() && !ParseFloatArgument(text, value)) {
				errorMessage = std::string(optionName) + " requires a finite number";
			}
			return true;
		};
		auto readInt = [&](const char* optionName, int& value) {
			std::string text;
			if (!readString(optionName, text)) {
				return false;
			}
			if (errorMessage.empty() && !ParseIntArgument(text, value)) {
				errorMessage = std::string(optionName) + " requires an integer";
			}
			return true;
		};
		auto readVec3 = [&](const char* optionName, glm::vec3& value) {
			if (argument != optionName) {
				return false;
			}
			if (i + 3 >= argc) {
				errorMessage = std::string(optionName) + " requires three numbers";
				return true;
			}
			glm::vec3 parsed;
			if (!ParseFloatArgument(argv[++i], parsed.x) ||
				!ParseFloatArgument(argv[++i], parsed.y) ||
				!ParseFloatArgument(argv[++i], parsed.z)) {
				errorMessage = std::string(optionName) + " requires three finite numbers";
			}
			else {
				value = parsed;
			}
			return true;
		};
		auto readTemporalRoi = [&]() {
			constexpr const char* optionName =
				"--classic-scene-ssao-temporal-capture-roi";
			if (argument != optionName) {
				return false;
			}
			if (i + 5 >= argc) {
				errorMessage =
					std::string(optionName) +
					" requires name, x, y, width, and height";
				return true;
			}
			SsaoTemporalCaptureRoi roi;
			roi.name = argv[++i];
			if (!ParseIntArgument(argv[++i], roi.x) ||
				!ParseIntArgument(argv[++i], roi.y) ||
				!ParseIntArgument(argv[++i], roi.width) ||
				!ParseIntArgument(argv[++i], roi.height)) {
				errorMessage =
					std::string(optionName) +
					" requires integer x, y, width, and height";
			}
			else {
				options.ssaoTemporalCaptureRois.push_back(roi);
			}
			return true;
		};

		if (readString("--classic-scene-test", options.modelPath)) {
			options.enabled = true;
		}
		else if (readString("--classic-scene-name", options.sceneName)) {
		}
		else if (readString("--classic-scene-capture", options.capturePath)) {
		}
		else if (readString(
			"--classic-scene-ssao-capture",
			options.ssaoCapturePath)) {
		}
		else if (readString(
			"--classic-scene-ssao-float-capture",
			options.ssaoFloatCapturePath)) {
		}
		else if (readString(
			"--classic-scene-ssao-raw-float-capture",
			options.ssaoRawFloatCapturePath)) {
		}
		else if (readString(
			"--classic-scene-ssao-depth-capture",
			options.ssaoDepthCapturePath)) {
		}
		else if (readString(
			"--classic-scene-ssao-normal-capture",
			options.ssaoNormalCapturePath)) {
		}
		else if (readString(
			"--classic-scene-ssao-temporal-capture-directory",
			options.ssaoTemporalCaptureDirectory)) {
		}
		else if (readString("--classic-scene-result", options.resultPath)) {
		}
		else if (readString(
			"--classic-scene-renderdoc-capture-template",
			options.renderDocCaptureTemplate)) {
		}
		else if (readString("--classic-scene-shadow-mode", options.shadowMode)) {
			options.shadowExperiment = true;
			options.gpuSynchronized = true;
		}
		else if (readString(
			"--classic-scene-shadow-sampling",
			options.shadowSampling)) {
			options.shadowExperiment = true;
			options.gpuSynchronized = true;
		}
		else if (readString("--classic-scene-shadow-lights", options.shadowLights)) {
			options.shadowExperiment = true;
			options.gpuSynchronized = true;
		}
		else if (readString(
			"--classic-scene-shadow-workload",
			options.shadowWorkload)) {
			options.shadowExperiment = true;
			options.gpuSynchronized = true;
		}
		else if (readString(
			"--classic-scene-shadow-variant",
			options.shadowVariant)) {
			options.shadowExperiment = true;
			options.gpuSynchronized = true;
		}
		else if (readString("--classic-scene-render-path", options.renderPath)) {
		}
		else if (readVec3("--classic-scene-camera", options.cameraPosition)) {
		}
		else if (readVec3("--classic-scene-target", options.cameraTarget)) {
		}
		else if (readVec3("--classic-scene-up", options.cameraUp)) {
		}
		else if (readVec3(
			"--classic-scene-directional-light",
			options.directionalLightDirection)) {
			options.shadowExperiment = true;
			options.gpuSynchronized = true;
		}
		else if (readVec3(
			"--classic-scene-point-light",
			options.pointLightPosition)) {
			options.shadowExperiment = true;
			options.gpuSynchronized = true;
		}
		else if (readVec3(
			"--classic-scene-spot-light",
			options.spotLightPosition)) {
			options.hasSpotLightPosition = true;
			options.shadowExperiment = true;
			options.gpuSynchronized = true;
		}
		else if (readVec3(
			"--classic-scene-spot-direction",
			options.spotLightDirection)) {
			options.hasSpotLightDirection = true;
			options.shadowExperiment = true;
			options.gpuSynchronized = true;
		}
		else if (readFloat(
			"--classic-scene-spot-near-plane",
			options.spotShadowNearPlane)) {
			options.hasSpotShadowNearPlane = true;
			options.shadowExperiment = true;
			options.gpuSynchronized = true;
		}
		else if (readFloat(
			"--classic-scene-spot-far-plane",
			options.spotShadowFarPlane)) {
			options.hasSpotShadowFarPlane = true;
			options.shadowExperiment = true;
			options.gpuSynchronized = true;
		}
		else if (readFloat("--classic-scene-radius", options.normalizedRadius)) {
		}
		else if (readFloat(
			"--classic-scene-world-scale",
			options.worldScale)) {
		}
		else if (readFloat("--classic-scene-fov", options.fov)) {
		}
		else if (readInt(
			"--classic-scene-shadow-resolution",
			options.shadowResolution)) {
			options.shadowExperiment = true;
			options.gpuSynchronized = true;
		}
		else if (readInt("--classic-scene-width", options.width)) {
		}
		else if (readInt("--classic-scene-height", options.height)) {
		}
		else if (readInt(
			"--classic-scene-ssao-samples",
			options.ssaoSamples)) {
			options.ssaoExperiment = true;
		}
		else if (readString(
			"--classic-scene-ssao-mode",
			options.ssaoMode)) {
			options.ssaoExperiment = true;
		}
		else if (readInt(
			"--classic-scene-warmup-frames",
			options.warmupFrames)) {
		}
		else if (readInt("--classic-scene-capture-frame", options.captureFrame)) {
		}
		else if (readInt(
			"--classic-scene-renderdoc-capture-frame",
			options.renderDocCaptureFrame)) {
		}
		else if (readInt(
			"--classic-scene-timeline-fps",
			options.timelineFixedFramesPerSecond)) {
		}
		else if (readInt(
			"--classic-scene-timeline-cycle-frames",
			options.timelineCycleFrames)) {
		}
		else if (readFloat(
			"--classic-scene-camera-timeline-position-radius-ratio",
			options.cameraTimelinePositionRadiusRatio)) {
		}
		else if (readFloat(
			"--classic-scene-camera-timeline-target-radius-ratio",
			options.cameraTimelineTargetRadiusRatio)) {
		}
		else if (readInt(
			"--classic-scene-ssao-temporal-capture-start",
			options.ssaoTemporalCaptureStartFrame)) {
		}
		else if (readInt(
			"--classic-scene-ssao-temporal-capture-count",
			options.ssaoTemporalCaptureFrameCount)) {
		}
		else if (readInt(
			"--classic-scene-ssao-temporal-capture-stride",
			options.ssaoTemporalCaptureStride)) {
		}
		else if (readTemporalRoi()) {
		}
		else if (argument ==
			"--classic-scene-deterministic-camera-timeline") {
			options.deterministicCameraTimeline = true;
		}
		else if (argument ==
			"--classic-scene-ssao-temporal-capture-reference-guides") {
			options.ssaoTemporalCaptureReferenceGuides = true;
		}
		else if (argument == "--classic-scene-untextured") {
			options.untextured = true;
		}
		else if (argument == "--classic-scene-no-capture") {
			options.captureFinalFrame = false;
		}

		if (!errorMessage.empty()) {
			return false;
		}
	}

	if (!options.enabled) {
		return true;
	}
	if (options.modelPath.empty()) {
		errorMessage = "--classic-scene-test requires a model path";
		return false;
	}
	if (options.sceneName.empty()) {
		options.sceneName = std::filesystem::path(options.modelPath).stem().string();
	}
	if (options.captureFinalFrame && options.capturePath.empty()) {
		options.capturePath =
			"benchmark-results/classic-scenes/" + options.sceneName + ".ppm";
	}
	if (options.resultPath.empty()) {
		options.resultPath =
			"benchmark-results/classic-scenes/" + options.sceneName + ".json";
	}
	if (options.normalizedRadius <= 0.0f) {
		errorMessage = "--classic-scene-radius must be greater than zero";
		return false;
	}
	if (options.worldScale < 0.05f || options.worldScale > 2.0f) {
		errorMessage =
			"--classic-scene-world-scale must be between 0.05 and 2.0";
		return false;
	}
	if (options.shadowResolution != 0 &&
		(options.shadowResolution < 128 ||
			options.shadowResolution > 4096)) {
		errorMessage =
			"--classic-scene-shadow-resolution must be 0 or between 128 and 4096";
		return false;
	}
	if (options.width < 64 || options.width > 16384) {
		errorMessage =
			"--classic-scene-width must be between 64 and 16384";
		return false;
	}
	if (options.height < 64 || options.height > 16384) {
		errorMessage =
			"--classic-scene-height must be between 64 and 16384";
		return false;
	}
	if (options.ssaoExperiment &&
		options.ssaoSamples != 0 &&
		options.ssaoSamples != 8 &&
		options.ssaoSamples != 16 &&
		options.ssaoSamples != 32 &&
		options.ssaoSamples != 64) {
		errorMessage =
			"--classic-scene-ssao-samples must be 0, 8, 16, 32, or 64";
		return false;
	}
	if (options.ssaoMode != "legacy-full" &&
		options.ssaoMode != "half-raw" &&
		options.ssaoMode != "half-bilateral") {
		errorMessage =
			"--classic-scene-ssao-mode must be legacy-full, half-raw, "
			"or half-bilateral";
		return false;
	}
	if (options.ssaoSamples == 0 &&
		options.ssaoMode != "legacy-full") {
		errorMessage =
			"half-resolution SSAO modes require a positive sample count";
		return false;
	}
	if (options.fov < 1.0f || options.fov > 120.0f) {
		errorMessage = "--classic-scene-fov must be between 1 and 120 degrees";
		return false;
	}
	if (options.shadowMode != "off" &&
		options.shadowMode != "hard" &&
		options.shadowMode != "pcf" &&
		options.shadowMode != "pcss") {
		errorMessage =
			"--classic-scene-shadow-mode must be off, hard, pcf, or pcss";
		return false;
	}
	if (options.shadowSampling != "legacy" &&
		options.shadowSampling != "stable") {
		errorMessage =
			"--classic-scene-shadow-sampling must be legacy or stable";
		return false;
	}
	if (options.shadowLights != "directional" &&
		options.shadowLights != "point" &&
		options.shadowLights != "spot" &&
		options.shadowLights != "all") {
		errorMessage =
			"--classic-scene-shadow-lights must be directional, point, spot, or all";
		return false;
	}
	if (options.shadowWorkload != "static-hit" &&
		options.shadowWorkload != "force-update" &&
		options.shadowWorkload != "move-directional" &&
		options.shadowWorkload != "move-point" &&
		options.shadowWorkload != "move-spot" &&
		options.shadowWorkload != "move-caster" &&
		options.shadowWorkload != "move-local-caster" &&
		options.shadowWorkload != "change-caster-material" &&
		options.shadowWorkload != "reload-shadow-2d" &&
		options.shadowWorkload != "reload-shadow-point" &&
		options.shadowWorkload != "resize-point-shadow" &&
		options.shadowWorkload != "replace-point-shadow-target" &&
		options.shadowWorkload != "toggle-caster" &&
		options.shadowWorkload != "timeline-point" &&
		options.shadowWorkload != "timeline-point-camera" &&
		options.shadowWorkload != "timeline-caster" &&
		options.shadowWorkload != "timeline-camera" &&
		options.shadowWorkload != "timeline-mixed" &&
		options.shadowWorkload != "timeline-cache-3way" &&
		options.shadowWorkload != "deferred-face-required" &&
		options.shadowWorkload != "replace-model-aba") {
		errorMessage =
			"--classic-scene-shadow-workload must be static-hit, force-update, "
			"move-directional, move-point, move-spot, move-caster, "
			"move-local-caster, "
			"change-caster-material, reload-shadow-2d, "
			"reload-shadow-point, resize-point-shadow, "
			"replace-point-shadow-target, toggle-caster, timeline-point, "
			"timeline-point-camera, timeline-caster, timeline-camera, "
			"timeline-mixed, timeline-cache-3way, "
			"deferred-face-required, or replace-model-aba";
		return false;
	}
	if (options.shadowVariant.empty()) {
		errorMessage = "--classic-scene-shadow-variant must not be empty";
		return false;
	}
	if (options.renderPath != "pbr-forward" &&
		options.renderPath != "phong-forward" &&
		options.renderPath != "pbr-deferred" &&
		options.renderPath != "phong-deferred" &&
		options.renderPath != "phong-deferred-volume") {
		errorMessage =
			"--classic-scene-render-path must be pbr-forward, phong-forward, "
			"pbr-deferred, phong-deferred, or phong-deferred-volume";
		return false;
	}
	if (options.ssaoExperiment &&
		options.renderPath.find("deferred") == std::string::npos) {
		errorMessage =
			"--classic-scene-ssao-samples requires a deferred render path";
		return false;
	}
	if (options.ssaoExperiment && options.shadowExperiment) {
		errorMessage =
			"SSAO baseline options cannot be combined with shadow experiments";
		return false;
	}
	const bool hasSsaoCapture =
		!options.ssaoCapturePath.empty() ||
		!options.ssaoFloatCapturePath.empty() ||
		!options.ssaoRawFloatCapturePath.empty() ||
		!options.ssaoDepthCapturePath.empty() ||
		!options.ssaoNormalCapturePath.empty();
	if (hasSsaoCapture &&
		(!options.ssaoExperiment || options.ssaoSamples == 0)) {
		errorMessage =
			"SSAO capture options require 8, 16, 32, or 64 SSAO samples";
		return false;
	}
	const bool hasSsaoTemporalCapture =
		!options.ssaoTemporalCaptureDirectory.empty();
	if (hasSsaoTemporalCapture &&
		(!options.ssaoExperiment ||
			options.ssaoSamples == 0 ||
			!options.deterministicCameraTimeline)) {
		errorMessage =
			"SSAO temporal capture requires SSAO samples and "
			"--classic-scene-deterministic-camera-timeline";
		return false;
	}
	if (!hasSsaoTemporalCapture &&
		(options.ssaoTemporalCaptureFrameCount != 0 ||
			!options.ssaoTemporalCaptureRois.empty() ||
			options.ssaoTemporalCaptureReferenceGuides)) {
		errorMessage =
			"SSAO temporal capture options require "
			"--classic-scene-ssao-temporal-capture-directory";
		return false;
	}
	if (hasSsaoTemporalCapture) {
		if (options.ssaoTemporalCaptureStartFrame < 0 ||
			options.ssaoTemporalCaptureFrameCount < 1 ||
			options.ssaoTemporalCaptureFrameCount > 10000 ||
			options.ssaoTemporalCaptureStride < 1) {
			errorMessage =
				"SSAO temporal capture requires start >= 0, count in "
				"[1,10000], and stride >= 1";
			return false;
		}
		if (options.ssaoTemporalCaptureRois.empty()) {
			errorMessage =
				"SSAO temporal capture requires at least one ROI";
			return false;
		}
		for (const SsaoTemporalCaptureRoi& roi :
			options.ssaoTemporalCaptureRois) {
			const bool validName =
				!roi.name.empty() &&
				std::all_of(
					roi.name.begin(),
					roi.name.end(),
					[](unsigned char character) {
						return std::isalnum(character) ||
							character == '-' || character == '_';
					});
			if (!validName ||
				roi.x < 0 || roi.y < 0 ||
				roi.width < 1 || roi.height < 1 ||
				roi.x + roi.width > options.width ||
				roi.y + roi.height > options.height) {
				errorMessage =
					"SSAO temporal ROI names must be filesystem-safe and "
					"top-left coordinates must fit the full-resolution target";
				return false;
			}
		}
	}
	if (options.warmupFrames < 1) {
		errorMessage = "--classic-scene-warmup-frames must be at least 1";
		return false;
	}
	if (options.captureFrame <= options.warmupFrames) {
		errorMessage =
			"--classic-scene-capture-frame must be greater than the warm-up frame count";
		return false;
	}
	if (hasSsaoTemporalCapture) {
		const std::int64_t lastCaptureFrame =
			static_cast<std::int64_t>(
				options.ssaoTemporalCaptureStartFrame) +
			static_cast<std::int64_t>(
				options.ssaoTemporalCaptureFrameCount - 1) *
			options.ssaoTemporalCaptureStride;
		const std::int64_t measuredFrameCount =
			options.captureFrame - options.warmupFrames;
		if (lastCaptureFrame >= measuredFrameCount) {
			errorMessage =
				"SSAO temporal capture frames must fit inside the measured "
				"frame range";
			return false;
		}
	}
	if (options.renderDocCaptureFrame < 0) {
		errorMessage =
			"--classic-scene-renderdoc-capture-frame must not be negative";
		return false;
	}
	if (options.renderDocCaptureFrame == 0 &&
		!options.renderDocCaptureTemplate.empty()) {
		errorMessage =
			"--classic-scene-renderdoc-capture-template requires a positive "
			"RenderDoc capture frame";
		return false;
	}
	if (options.renderDocCaptureFrame > 0 &&
		options.renderDocCaptureTemplate.empty()) {
		errorMessage =
			"--classic-scene-renderdoc-capture-frame requires "
			"--classic-scene-renderdoc-capture-template";
		return false;
	}
	if (options.renderDocCaptureFrame == 1) {
		errorMessage =
			"--classic-scene-renderdoc-capture-frame must be at least 2 "
			"to avoid startup resource creation";
		return false;
	}
	if (options.renderDocCaptureFrame > options.captureFrame) {
		errorMessage =
			"--classic-scene-renderdoc-capture-frame must not exceed "
			"--classic-scene-capture-frame";
		return false;
	}
	if (options.timelineFixedFramesPerSecond < 1 ||
		options.timelineFixedFramesPerSecond > 1000) {
		errorMessage =
			"--classic-scene-timeline-fps must be between 1 and 1000";
		return false;
	}
	if (options.timelineCycleFrames < 4 ||
		options.timelineCycleFrames > 36000) {
		errorMessage =
			"--classic-scene-timeline-cycle-frames must be between 4 and 36000";
		return false;
	}
	if (options.cameraTimelinePositionRadiusRatio < 0.0f ||
		options.cameraTimelinePositionRadiusRatio > 0.25f) {
		errorMessage =
			"--classic-scene-camera-timeline-position-radius-ratio "
			"must be between 0 and 0.25";
		return false;
	}
	if (options.cameraTimelineTargetRadiusRatio < 0.0f ||
		options.cameraTimelineTargetRadiusRatio > 0.25f) {
		errorMessage =
			"--classic-scene-camera-timeline-target-radius-ratio "
			"must be between 0 and 0.25";
		return false;
	}
	if (options.shadowWorkload != "static-hit" &&
		options.shadowMode == "off") {
		errorMessage =
			"dynamic shadow workloads require hard, pcf, or pcss shadow mode";
		return false;
	}
	if (options.shadowWorkload == "move-directional" &&
		options.shadowLights != "directional" &&
		options.shadowLights != "all") {
		errorMessage =
			"move-directional requires directional or all shadow lights";
		return false;
	}
	if ((options.shadowWorkload == "move-point" ||
			options.shadowWorkload == "move-local-caster" ||
			options.shadowWorkload == "deferred-face-required" ||
			options.shadowWorkload == "replace-model-aba" ||
			options.shadowWorkload == "timeline-point" ||
			options.shadowWorkload == "timeline-point-camera") &&
		options.shadowLights != "point" &&
		options.shadowLights != "all") {
		errorMessage =
			"point-shadow workloads require point or all shadow lights";
		return false;
	}
	if ((options.shadowWorkload == "timeline-mixed" ||
			options.shadowWorkload == "timeline-cache-3way") &&
		options.shadowLights != "all") {
		errorMessage =
			"mixed timeline workloads require all shadow lights";
		return false;
	}
	if ((options.shadowWorkload == "reload-shadow-point" ||
			options.shadowWorkload == "resize-point-shadow" ||
			options.shadowWorkload == "replace-point-shadow-target") &&
		options.shadowLights != "point" &&
		options.shadowLights != "all") {
		errorMessage =
			"point-shadow cache validation requires point or all shadow lights";
		return false;
	}
	if (options.shadowWorkload == "move-spot" &&
		options.shadowLights != "spot" &&
		options.shadowLights != "all") {
		errorMessage = "move-spot requires spot or all shadow lights";
		return false;
	}
	if (glm::length(options.cameraTarget - options.cameraPosition) < 0.001f) {
		errorMessage = "classic scene camera and target must differ";
		return false;
	}
	if (glm::length(options.cameraUp) < 0.001f) {
		errorMessage = "classic scene camera up vector must be non-zero";
		return false;
	}
	if (glm::length(options.directionalLightDirection) < 0.001f) {
		errorMessage =
			"classic scene directional light vector must be non-zero";
		return false;
	}
	if (options.hasSpotLightDirection &&
		glm::length(options.spotLightDirection) < 0.001f) {
		errorMessage =
			"classic scene spot light direction must be non-zero";
		return false;
	}
	if (options.hasSpotShadowNearPlane !=
		options.hasSpotShadowFarPlane) {
		errorMessage =
			"--classic-scene-spot-near-plane and "
			"--classic-scene-spot-far-plane must be provided together";
		return false;
	}
	if (options.hasSpotShadowNearPlane &&
		(options.spotShadowNearPlane < 0.001f ||
			options.spotShadowFarPlane <=
				options.spotShadowNearPlane + 0.001f)) {
		errorMessage =
			"classic scene spot shadow planes require near >= 0.001 "
			"and far > near + 0.001";
		return false;
	}
	if (options.hasSpotShadowNearPlane &&
		(options.spotShadowNearPlane * options.worldScale < 0.001f ||
			(options.spotShadowFarPlane -
				options.spotShadowNearPlane) *
				options.worldScale <= 0.001f)) {
		errorMessage =
			"scaled classic scene spot shadow planes must preserve "
			"near >= 0.001 and far > near + 0.001";
		return false;
	}
	return true;
}

std::string EscapeJsonString(const std::string& value)
{
	std::ostringstream output;
	for (const char character : value) {
		switch (character) {
		case '\\': output << "\\\\"; break;
		case '"': output << "\\\""; break;
		case '\n': output << "\\n"; break;
		case '\r': output << "\\r"; break;
		case '\t': output << "\\t"; break;
		default: output << character; break;
		}
	}
	return output.str();
}

FrameTimingStats CalculateFrameTimingStats(
	const std::vector<double>& frameMilliseconds)
{
	FrameTimingStats stats;
	stats.sampleCount = frameMilliseconds.size();
	if (frameMilliseconds.empty()) {
		return stats;
	}

	std::vector<double> sorted = frameMilliseconds;
	std::sort(sorted.begin(), sorted.end());
	double sum = 0.0;
	for (double value : sorted) {
		sum += value;
	}
	stats.meanMilliseconds = sum / static_cast<double>(sorted.size());

	auto percentile = [&](double quantile) {
		const double position =
			quantile * static_cast<double>(sorted.size() - 1);
		const std::size_t lower = static_cast<std::size_t>(std::floor(position));
		const std::size_t upper = static_cast<std::size_t>(std::ceil(position));
		const double weight = position - static_cast<double>(lower);
		return sorted[lower] * (1.0 - weight) + sorted[upper] * weight;
	};
	stats.medianMilliseconds = percentile(0.5);
	stats.p95Milliseconds = percentile(0.95);
	stats.p99Milliseconds = percentile(0.99);
	return stats;
}

void WriteJsonDoubleArray(
	std::ostream& output,
	const std::vector<double>& values)
{
	output << "[";
	for (std::size_t index = 0; index < values.size(); ++index) {
		if (index > 0) {
			output << ", ";
		}
		output << values[index];
	}
	output << "]";
}

void WriteJsonDistribution(
	std::ostream& output,
	const std::vector<double>& values)
{
	const FrameTimingStats stats = CalculateFrameTimingStats(values);
	output << "{\n"
		<< "        \"count\": " << stats.sampleCount;
	if (stats.sampleCount == 0) {
		output << ",\n"
			<< "        \"mean\": null,\n"
			<< "        \"median\": null,\n"
			<< "        \"p95\": null,\n"
			<< "        \"p99\": null\n";
	}
	else {
		output << ",\n"
			<< "        \"mean\": " << stats.meanMilliseconds << ",\n"
			<< "        \"median\": " << stats.medianMilliseconds << ",\n"
			<< "        \"p95\": " << stats.p95Milliseconds << ",\n"
			<< "        \"p99\": " << stats.p99Milliseconds << "\n";
	}
	output << "      }";
}

std::vector<std::string> GetSortedZoneNames(
	const std::unordered_map<std::string, std::vector<double>>& zones)
{
	std::vector<std::string> names;
	names.reserve(zones.size());
	for (const auto& [name, values] : zones) {
		(void)values;
		names.push_back(name);
	}
	std::sort(names.begin(), names.end());
	return names;
}

void WriteJsonZoneDistributions(
	std::ostream& output,
	const std::unordered_map<std::string, std::vector<double>>& zones)
{
	output << "{";
	const std::vector<std::string> names = GetSortedZoneNames(zones);
	for (std::size_t index = 0; index < names.size(); ++index) {
		const std::string& name = names[index];
		output << (index == 0 ? "\n" : ",\n")
			<< "      \"" << EscapeJsonString(name) << "\": ";
		WriteJsonDistribution(output, zones.at(name));
	}
	if (!names.empty()) {
		output << "\n    ";
	}
	output << "}";
}

void WriteJsonZoneSamples(
	std::ostream& output,
	const std::unordered_map<std::string, std::vector<double>>& zones)
{
	output << "{";
	const std::vector<std::string> names = GetSortedZoneNames(zones);
	for (std::size_t index = 0; index < names.size(); ++index) {
		const std::string& name = names[index];
		output << (index == 0 ? "\n" : ",\n")
			<< "      \"" << EscapeJsonString(name) << "\": ";
		WriteJsonDoubleArray(output, zones.at(name));
	}
	if (!names.empty()) {
		output << "\n    ";
	}
	output << "}";
}

std::uint64_t CounterDelta(
	std::uint64_t before,
	std::uint64_t after)
{
	return after >= before ? after - before : 0;
}

double CounterDelta(
	double before,
	double after)
{
	return after >= before ? after - before : 0.0;
}

void WriteJsonVec3(std::ostream& output, const glm::vec3& value)
{
	output << "["
		<< value.x << ", "
		<< value.y << ", "
		<< value.z << "]";
}

void WriteJsonBenchmarkMotionTimeline(
	std::ostream& output,
	const BenchmarkMotionTimeline& timeline,
	const std::vector<BenchmarkTimelineFrameTelemetry>& telemetry)
{
	const BenchmarkMotionProfile profile = timeline.GetProfile();
	const BenchmarkMotionTimelineConfig& config = timeline.GetConfig();
	const BenchmarkMotionBaseState& baseState = timeline.GetBaseState();
	const std::uint32_t trackMask = timeline.GetTrackMask();
	output << "{\n"
		<< "    \"schemaVersion\": 1,\n"
		<< "    \"enabled\": "
		<< (profile != BenchmarkMotionProfile::None ? "true" : "false")
		<< ",\n"
		<< "    \"profile\": \""
		<< BenchmarkMotionTimeline::ProfileName(profile) << "\",\n"
		<< "    \"fixedFramesPerSecond\": "
		<< config.fixedFramesPerSecond << ",\n"
		<< "    \"cycleFrames\": " << config.cycleFrames << ",\n"
		<< "    \"sceneRadius\": " << config.sceneRadius << ",\n"
		<< "    \"trackMask\": " << trackMask << ",\n"
		<< "    \"tracks\": [";
	bool wroteTrack = false;
	auto writeTrack = [&](BenchmarkMotionTrack track, const char* name) {
		if (!BenchmarkMotionTimeline::HasTrack(trackMask, track)) {
			return;
		}
		output << (wroteTrack ? ", " : "") << "\"" << name << "\"";
		wroteTrack = true;
	};
	writeTrack(BenchmarkMotionTrack::Point, "point");
	writeTrack(BenchmarkMotionTrack::Caster, "caster");
	writeTrack(BenchmarkMotionTrack::Camera, "camera");
	output << "],\n"
		<< "    \"amplitudeRatios\": {\n"
		<< "      \"pointHorizontal\": "
		<< config.pointHorizontalRadiusRatio << ",\n"
		<< "      \"pointVertical\": "
		<< config.pointVerticalRadiusRatio << ",\n"
		<< "      \"casterHorizontal\": "
		<< config.casterHorizontalRadiusRatio << ",\n"
		<< "      \"casterVertical\": "
		<< config.casterVerticalRadiusRatio << ",\n"
		<< "      \"cameraPosition\": "
		<< config.cameraPositionRadiusRatio << ",\n"
		<< "      \"cameraTarget\": "
		<< config.cameraTargetRadiusRatio << "\n"
		<< "    },\n"
		<< "    \"baseState\": {\n"
		<< "      \"pointPosition\": ";
	WriteJsonVec3(output, baseState.pointPosition);
	output << ",\n"
		<< "      \"casterPosition\": ";
	WriteJsonVec3(output, baseState.casterPosition);
	output << ",\n"
		<< "      \"cameraPosition\": ";
	WriteJsonVec3(output, baseState.cameraPosition);
	output << ",\n"
		<< "      \"cameraTarget\": ";
	WriteJsonVec3(output, baseState.cameraTarget);
	output << ",\n"
		<< "      \"cameraUp\": ";
	WriteJsonVec3(output, baseState.cameraUp);
	output << "\n"
		<< "    },\n"
		<< "    \"samples\": [";
	for (std::size_t index = 0; index < telemetry.size(); ++index) {
		const BenchmarkTimelineFrameTelemetry& frame = telemetry[index];
		output << (index == 0 ? "\n" : ",\n")
			<< "      {\n"
			<< "        \"measurementFrame\": "
			<< frame.measurementFrame << ",\n"
			<< "        \"timelineFrame\": "
			<< frame.motion.frameIndex << ",\n"
			<< "        \"cycleFrame\": "
			<< frame.motion.cycleFrame << ",\n"
			<< "        \"fixedTimeSeconds\": "
			<< frame.motion.fixedTimeSeconds << ",\n"
			<< "        \"normalizedPhase\": "
			<< frame.motion.normalizedPhase << ",\n"
			<< "        \"wallMilliseconds\": "
			<< frame.wallMilliseconds << ",\n"
			<< "        \"pointPosition\": ";
		WriteJsonVec3(output, frame.motion.pointPosition);
		output << ",\n"
			<< "        \"casterPosition\": ";
		WriteJsonVec3(output, frame.motion.casterPosition);
		output << ",\n"
			<< "        \"cameraPosition\": ";
		WriteJsonVec3(output, frame.motion.cameraPosition);
		output << ",\n"
			<< "        \"cameraTarget\": ";
		WriteJsonVec3(output, frame.motion.cameraTarget);
		output << ",\n"
			<< "        \"shadow\": {\n"
			<< "          \"updateCount\": "
			<< frame.updateCount << ",\n"
			<< "          \"cacheHitCount\": "
			<< frame.cacheHitCount << ",\n"
			<< "          \"lightCacheHitCount\": "
			<< frame.lightCacheHitCount << ",\n"
			<< "          \"updatedLightCount\": "
			<< frame.updatedLightCount << ",\n"
			<< "          \"directionalLightUpdateCount\": "
			<< frame.directionalLightUpdateCount << ",\n"
			<< "          \"pointLightUpdateCount\": "
			<< frame.pointLightUpdateCount << ",\n"
			<< "          \"pointShadowSubmissionPassCount\": "
			<< frame.pointShadowSubmissionPassCount << ",\n"
			<< "          \"pointShadowRequiredFaceCount\": "
			<< frame.pointShadowRequiredFaceCount << ",\n"
			<< "          \"pointShadowRenderedFaceCount\": "
			<< frame.pointShadowRenderedFaceCount << ",\n"
			<< "          \"pointShadowFaceCacheHitCount\": "
			<< frame.pointShadowFaceCacheHitCount << ",\n"
			<< "          \"pointShadowDeferredFaceCount\": "
			<< frame.pointShadowDeferredFaceCount << ",\n"
			<< "          \"pointShadowRequiredFaceMask\": "
			<< static_cast<unsigned int>(
				frame.pointShadowRequiredFaceMask) << ",\n"
			<< "          \"pointShadowUpdateFaceMask\": "
			<< static_cast<unsigned int>(
				frame.pointShadowUpdateFaceMask) << ",\n"
			<< "          \"spotLightUpdateCount\": "
			<< frame.spotLightUpdateCount << ",\n"
			<< "          \"casterBoundsRebuildCount\": "
			<< frame.casterBoundsRebuildCount << ",\n"
			<< "          \"sceneTopologyRevision\": "
			<< frame.sceneTopologyRevision << ",\n"
			<< "          \"sceneTopologyInvalidationCount\": "
			<< frame.sceneTopologyInvalidationCount << ",\n"
			<< "          \"sceneTopologyModelCount\": "
			<< frame.sceneTopologyModelCount << ",\n"
			<< "          \"cacheCheckCpuMilliseconds\": "
			<< frame.cacheCheckCpuMilliseconds << ",\n"
			<< "          \"casterStateSyncCpuMilliseconds\": "
			<< frame.casterStateSyncCpuMilliseconds << ",\n"
			<< "          \"pointShadowFaceDemandCpuMilliseconds\": "
			<< frame.pointShadowFaceDemandCpuMilliseconds << ",\n"
			<< "          \"pointShadowFaceSignatureCpuMilliseconds\": "
			<< frame.pointShadowFaceSignatureCpuMilliseconds << ",\n"
			<< "          \"updateCpuMilliseconds\": "
			<< frame.shadowUpdateCpuMilliseconds << "\n"
			<< "        }\n"
			<< "      }";
	}
	if (!telemetry.empty()) {
		output << "\n    ";
	}
	output << "]\n"
		<< "  }";
}

std::uint64_t HashFloatBits(const std::vector<float>& samples)
{
	static_assert(
		sizeof(float) == sizeof(std::uint32_t),
		"Point shadow evidence requires 32-bit floats");
	constexpr std::uint64_t fnvOffsetBasis = 14695981039346656037ull;
	constexpr std::uint64_t fnvPrime = 1099511628211ull;
	std::uint64_t hash = fnvOffsetBasis;
	for (float sample : samples) {
		std::uint32_t bits = 0;
		std::memcpy(&bits, &sample, sizeof(bits));
		for (unsigned int shift = 0; shift < 32; shift += 8) {
			hash ^= static_cast<std::uint8_t>(bits >> shift);
			hash *= fnvPrime;
		}
	}
	return hash;
}

std::string FormatBitwiseHash(std::uint64_t hash)
{
	std::ostringstream output;
	output << "0x"
		<< std::hex
		<< std::setfill('0')
		<< std::setw(16)
		<< hash;
	return output.str();
}

PointShadowCubeEvidence CapturePointShadowCubeEvidence(Scene& scene)
{
	PointShadowCubeEvidence evidence;
	PointLight* selectedLight = nullptr;
	for (std::size_t index = 0;
		index < scene.lightSource.pointLights.size();
		++index) {
		PointLight& light = scene.lightSource.pointLights[index];
		FBO* target = light.shadowFBO;
		const bool isCubeTarget =
			target &&
			target->attr.isShadowMap &&
			target->attr.shadowType == FBOAttributes::ShadowBox &&
			target->attr.textureAttrs.size() == 1 &&
			target->attr.textureAttrs.front().target ==
				GL_TEXTURE_CUBE_MAP;
		if (light.GetActiveStatus() &&
			light.useShadowMap &&
			isCubeTarget &&
			light.shadowCache.IsSampleable(target)) {
			selectedLight = &light;
			evidence.lightIndex = static_cast<int>(index);
			break;
		}
	}
	if (!selectedLight || !selectedLight->shadowFBO) {
		return evidence;
	}

	const FBO* target = selectedLight->shadowFBO;
	evidence.width = target->width;
	evidence.height = target->height;
	if (evidence.width <= 0 ||
		evidence.height <= 0 ||
		target->textureIDs.size() != 1 ||
		target->textureIDs.front() == 0) {
		return evidence;
	}

	const std::uint64_t sampleCount =
		static_cast<std::uint64_t>(evidence.width) *
		static_cast<std::uint64_t>(evidence.height);
	if (sampleCount == 0 ||
		sampleCount >
			static_cast<std::uint64_t>(
				(std::numeric_limits<std::size_t>::max)())) {
		return evidence;
	}
	evidence.sampleCountPerFace = sampleCount;
	std::vector<float> depthSamples(static_cast<std::size_t>(sampleCount));

	// glGetTexImage reads through the current texture binding and pixel-pack
	// state. Preserve every state item touched here so the evidence collection
	// remains observational and stays outside the measured frame interval.
	GLint previousActiveTexture = GL_TEXTURE0;
	GLint previousCubeTexture = 0;
	GLint previousPixelPackBuffer = 0;
	GLint previousPackAlignment = 4;
	GLint previousPackRowLength = 0;
	GLint previousPackSkipPixels = 0;
	GLint previousPackSkipRows = 0;
	GLint previousPackImageHeight = 0;
	GLint previousPackSkipImages = 0;
	GLint previousPackSwapBytes = GL_FALSE;
	GLint previousPackLsbFirst = GL_FALSE;
	glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
	glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP, &previousCubeTexture);
	glGetIntegerv(
		GL_PIXEL_PACK_BUFFER_BINDING,
		&previousPixelPackBuffer);
	glGetIntegerv(GL_PACK_ALIGNMENT, &previousPackAlignment);
	glGetIntegerv(GL_PACK_ROW_LENGTH, &previousPackRowLength);
	glGetIntegerv(GL_PACK_SKIP_PIXELS, &previousPackSkipPixels);
	glGetIntegerv(GL_PACK_SKIP_ROWS, &previousPackSkipRows);
	glGetIntegerv(GL_PACK_IMAGE_HEIGHT, &previousPackImageHeight);
	glGetIntegerv(GL_PACK_SKIP_IMAGES, &previousPackSkipImages);
	glGetIntegerv(GL_PACK_SWAP_BYTES, &previousPackSwapBytes);
	glGetIntegerv(GL_PACK_LSB_FIRST, &previousPackLsbFirst);

	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
	glPixelStorei(GL_PACK_SKIP_ROWS, 0);
	glPixelStorei(GL_PACK_IMAGE_HEIGHT, 0);
	glPixelStorei(GL_PACK_SKIP_IMAGES, 0);
	glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
	glPixelStorei(GL_PACK_LSB_FIRST, GL_FALSE);
	glBindTexture(GL_TEXTURE_CUBE_MAP, target->textureIDs.front());

	bool allFacesValid = true;
	for (std::size_t faceIndex = 0;
		faceIndex < evidence.faces.size();
		++faceIndex) {
		glGetTexImage(
			GL_TEXTURE_CUBE_MAP_POSITIVE_X +
				static_cast<GLenum>(faceIndex),
			0,
			GL_DEPTH_COMPONENT,
			GL_FLOAT,
			depthSamples.data());
		const GLenum readError = glGetError();

		PointShadowFaceEvidence& face = evidence.faces[faceIndex];
		face.bitwiseHash = HashFloatBits(depthSamples);
		float minDepth = (std::numeric_limits<float>::max)();
		float maxDepth = (std::numeric_limits<float>::lowest)();
		bool allFinite = true;
		for (float depth : depthSamples) {
			if (!std::isfinite(depth)) {
				allFinite = false;
				continue;
			}
			minDepth = (std::min)(minDepth, depth);
			maxDepth = (std::max)(maxDepth, depth);
			if (depth < 1.0f) {
				++face.nonFarSampleCount;
			}
		}
		face.valid = readError == GL_NO_ERROR && allFinite;
		if (face.valid) {
			face.minDepth = minDepth;
			face.maxDepth = maxDepth;
		}
		else {
			allFacesValid = false;
		}
	}

	glBindTexture(
		GL_TEXTURE_CUBE_MAP,
		static_cast<GLuint>(previousCubeTexture));
	glPixelStorei(GL_PACK_LSB_FIRST, previousPackLsbFirst);
	glPixelStorei(GL_PACK_SWAP_BYTES, previousPackSwapBytes);
	glPixelStorei(GL_PACK_SKIP_IMAGES, previousPackSkipImages);
	glPixelStorei(GL_PACK_IMAGE_HEIGHT, previousPackImageHeight);
	glPixelStorei(GL_PACK_SKIP_ROWS, previousPackSkipRows);
	glPixelStorei(GL_PACK_SKIP_PIXELS, previousPackSkipPixels);
	glPixelStorei(GL_PACK_ROW_LENGTH, previousPackRowLength);
	glPixelStorei(GL_PACK_ALIGNMENT, previousPackAlignment);
	glBindBuffer(
		GL_PIXEL_PACK_BUFFER,
		static_cast<GLuint>(previousPixelPackBuffer));
	glActiveTexture(static_cast<GLenum>(previousActiveTexture));

	evidence.valid = allFacesValid;
	return evidence;
}

void WriteJsonPointShadowCubeEvidence(
	std::ostream& output,
	const PointShadowCubeEvidence& evidence)
{
	static const std::array<const char*, 6> faceNames = {
		"+X", "-X", "+Y", "-Y", "+Z", "-Z"
	};
	output << "{\n"
		<< "      \"valid\": "
		<< (evidence.valid ? "true" : "false") << ",\n"
		<< "      \"lightIndex\": " << evidence.lightIndex << ",\n"
		<< "      \"resolution\": ["
		<< evidence.width << ", " << evidence.height << "],\n"
		<< "      \"sampleCountPerFace\": "
		<< evidence.sampleCountPerFace << ",\n"
		<< "      \"faces\": [";
	for (std::size_t index = 0; index < evidence.faces.size(); ++index) {
		const PointShadowFaceEvidence& face = evidence.faces[index];
		output << (index == 0 ? "\n" : ",\n")
			<< "        {\n"
			<< "          \"index\": " << index << ",\n"
			<< "          \"name\": \"" << faceNames[index] << "\",\n"
			<< "          \"valid\": "
			<< (face.valid ? "true" : "false") << ",\n"
			<< "          \"bitwiseHash\": \""
			<< FormatBitwiseHash(face.bitwiseHash) << "\",\n"
			<< "          \"nonFarSampleCount\": "
			<< face.nonFarSampleCount << ",\n"
			<< "          \"minDepth\": " << face.minDepth << ",\n"
			<< "          \"maxDepth\": " << face.maxDepth << "\n"
			<< "        }";
	}
	if (!evidence.faces.empty()) {
		output << "\n      ";
	}
	output << "]\n"
		<< "    }";
}

void WriteJsonFloatCapture(
	std::ostream& output,
	const std::string& path,
	const FloatCaptureStats& capture)
{
	output << "{\n"
		<< "        \"path\": \"" << EscapeJsonString(path) << "\",\n"
		<< "        \"requested\": "
		<< (!path.empty() ? "true" : "false") << ",\n"
		<< "        \"valid\": "
		<< (capture.valid ? "true" : "false") << ",\n"
		<< "        \"width\": " << capture.width << ",\n"
		<< "        \"height\": " << capture.height << ",\n"
		<< "        \"channels\": " << capture.channels << ",\n"
		<< "        \"finiteValueCount\": "
		<< capture.finiteValueCount << ",\n"
		<< "        \"nonFiniteValueCount\": "
		<< capture.nonFiniteValueCount << ",\n"
		<< "        \"minimum\": " << capture.minimum << ",\n"
		<< "        \"maximum\": " << capture.maximum << ",\n"
		<< "        \"mean\": " << capture.mean << "\n"
		<< "      }";
}

bool WriteClassicSceneResult(
	const ClassicSceneTestOptions& options,
	Scene& scene,
	bool success,
	double loadMilliseconds,
	const FrameTimingStats& frameTiming,
	std::size_t meshCount,
	std::uint64_t vertexCount,
	std::uint64_t triangleCount,
	const glm::vec3& sourceCenter,
	float sourceRadius,
	float appliedScale,
	const FrameCaptureStats& capture,
	const FrameCaptureStats& ssaoCapture,
	const FloatCaptureStats& ssaoFloatCapture,
	const FloatCaptureStats& ssaoRawFloatCapture,
	const FloatCaptureStats& ssaoDepthCapture,
	const FloatCaptureStats& ssaoNormalCapture,
	const FBO* ssaoFBO,
	const FBO* ssaoGenerationFBO,
	const PointShadowCubeEvidence& pointShadowCubeEvidence,
	const MemoryStats& memory,
	const Scene::ShadowSystemStats& shadowStats,
	const Scene::ShadowSystemStats& measurementStartShadowStats,
	const BenchmarkMotionTimeline& motionTimeline,
	const std::vector<BenchmarkTimelineFrameTelemetry>& timelineTelemetry,
	const ProfilerBenchmarkSamples& profilerSamples,
	float actualSpotShadowNearPlane,
	float actualSpotShadowFarPlane)
{
	const std::filesystem::path path(options.resultPath);
	std::error_code directoryError;
	if (path.has_parent_path()) {
		std::filesystem::create_directories(path.parent_path(), directoryError);
	}
	if (directoryError) {
		return false;
	}

	std::ofstream output(path, std::ios::trunc);
	if (!output.is_open()) {
		return false;
	}
	auto memoryBytes = [&](MemoryResourceType type) {
		return memory.categories[static_cast<std::size_t>(type)].currentBytes;
	};
	std::vector<double> drawCallSamples;
	drawCallSamples.reserve(profilerSamples.renderStats.size());
	for (const RenderStats& sample : profilerSamples.renderStats) {
		drawCallSamples.push_back(
			static_cast<double>(sample.drawCalls));
	}
	int activePointLights = 0;
	int activeDirectionLights = 0;
	int activeSpotLights = 0;
	int shadowCastingLights = 0;
	for (auto& light : scene.lightSource.pointLights) {
		if (!light.GetActiveStatus()) {
			continue;
		}
		++activePointLights;
		if (light.useShadowMap) {
			++shadowCastingLights;
		}
	}
	for (auto& light : scene.lightSource.directionLights) {
		if (!light.GetActiveStatus()) {
			continue;
		}
		++activeDirectionLights;
		if (light.useShadowMap) {
			++shadowCastingLights;
		}
	}
	for (auto& light : scene.lightSource.spotLights) {
		if (!light.GetActiveStatus()) {
			continue;
		}
		++activeSpotLights;
		if (light.useShadowMap) {
			++shadowCastingLights;
		}
	}
	const bool ssaoOutputAvailable =
		ssaoFBO &&
		ssaoFBO->IsComplete() &&
		!ssaoFBO->textureIDs.empty();
	const bool ssaoOutputFullResolution =
		ssaoOutputAvailable &&
		ssaoFBO->width == properties.SCREEN_WIDTH &&
		ssaoFBO->height == properties.SCREEN_HEIGHT;
	const bool ssaoOutputR16F =
		ssaoOutputAvailable &&
		!ssaoFBO->attr.textureAttrs.empty() &&
		ssaoFBO->attr.textureAttrs.front().internalFormat == GL_R16F;
	const GLint ssaoOutputInternalFormat =
		ssaoOutputAvailable &&
		!ssaoFBO->attr.textureAttrs.empty()
			? ssaoFBO->attr.textureAttrs.front().internalFormat
			: 0;
	const bool ssaoGenerationAvailable =
		ssaoGenerationFBO &&
		ssaoGenerationFBO->IsComplete() &&
		!ssaoGenerationFBO->textureIDs.empty();
	const bool ssaoGenerationFullResolution =
		ssaoGenerationAvailable &&
		ssaoGenerationFBO->width == properties.SCREEN_WIDTH &&
		ssaoGenerationFBO->height == properties.SCREEN_HEIGHT;
	const bool ssaoGenerationR16F =
		ssaoGenerationAvailable &&
		!ssaoGenerationFBO->attr.textureAttrs.empty() &&
		ssaoGenerationFBO->attr.textureAttrs.front().internalFormat == GL_R16F;
	const GLint ssaoGenerationInternalFormat =
		ssaoGenerationAvailable &&
		!ssaoGenerationFBO->attr.textureAttrs.empty()
			? ssaoGenerationFBO->attr.textureAttrs.front().internalFormat
			: 0;
	const bool ssaoBilateral =
		options.ssaoMode == "half-bilateral";
	const FloatCaptureStats& generationFloatCapture =
		!options.ssaoRawFloatCapturePath.empty()
			? ssaoRawFloatCapture
			: ssaoFloatCapture;
	const std::string& generationFloatCapturePath =
		!options.ssaoRawFloatCapturePath.empty()
			? options.ssaoRawFloatCapturePath
			: options.ssaoFloatCapturePath;
	double shadowUpdateGpuMilliseconds = 0.0;
	for (const ProfilerZoneStats& zone :
		PerformanceProfiler::GetInstance().GetGpuZoneStats()) {
		if (zone.name == "Shadow Map Update" && zone.sampleCount > 0) {
			shadowUpdateGpuMilliseconds = zone.averageMs;
			break;
		}
	}
	output << std::fixed << std::setprecision(6);
	const char* glVendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
	const char* glRenderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
	const char* glVersion = reinterpret_cast<const char*>(glGetString(GL_VERSION));
#ifdef NDEBUG
	const char* buildConfiguration = "Release";
#else
	const char* buildConfiguration = "Debug";
#endif
#ifdef _WIN64
	const char* architecture = "x64";
#else
	const char* architecture = "Win32";
#endif
	const std::uint64_t measuredCacheCheckCount = CounterDelta(
		measurementStartShadowStats.cacheCheckCount,
		shadowStats.cacheCheckCount);
	const double measuredCacheCheckCpuMilliseconds = CounterDelta(
		measurementStartShadowStats.totalCacheCheckCpuMilliseconds,
		shadowStats.totalCacheCheckCpuMilliseconds);
	const std::uint64_t measuredCasterStateSyncCount = CounterDelta(
		measurementStartShadowStats.casterStateSyncCount,
		shadowStats.casterStateSyncCount);
	const double measuredCasterStateSyncCpuMilliseconds = CounterDelta(
		measurementStartShadowStats.totalCasterStateSyncCpuMilliseconds,
		shadowStats.totalCasterStateSyncCpuMilliseconds);
	const std::uint64_t measuredLightCacheHitCount = CounterDelta(
		measurementStartShadowStats.lightCacheHitCount,
		shadowStats.lightCacheHitCount);
	const std::uint64_t measuredDirectionalLightUpdateCount = CounterDelta(
		measurementStartShadowStats.directionalLightUpdateCount,
		shadowStats.directionalLightUpdateCount);
	const std::uint64_t measuredPointLightUpdateCount = CounterDelta(
		measurementStartShadowStats.pointLightUpdateCount,
		shadowStats.pointLightUpdateCount);
	const std::uint64_t measuredPointShadowLayeredUpdateCount = CounterDelta(
		measurementStartShadowStats.pointShadowLayeredUpdateCount,
		shadowStats.pointShadowLayeredUpdateCount);
	const std::uint64_t measuredPointShadowSixFaceUpdateCount = CounterDelta(
		measurementStartShadowStats.pointShadowSixFaceUpdateCount,
		shadowStats.pointShadowSixFaceUpdateCount);
	const std::uint64_t measuredPointShadowSubmissionPassCount = CounterDelta(
		measurementStartShadowStats.pointShadowSubmissionPassCount,
		shadowStats.pointShadowSubmissionPassCount);
	const std::uint64_t measuredPointShadowFaceCullingPassCount = CounterDelta(
		measurementStartShadowStats.pointShadowFaceCullingPassCount,
		shadowStats.pointShadowFaceCullingPassCount);
	const std::uint64_t measuredPointShadowRequiredFaceCount = CounterDelta(
		measurementStartShadowStats.pointShadowRequiredFaceCount,
		shadowStats.pointShadowRequiredFaceCount);
	const std::uint64_t measuredPointShadowRenderedFaceCount = CounterDelta(
		measurementStartShadowStats.pointShadowRenderedFaceCount,
		shadowStats.pointShadowRenderedFaceCount);
	const std::uint64_t measuredPointShadowFaceCacheHitCount = CounterDelta(
		measurementStartShadowStats.pointShadowFaceCacheHitCount,
		shadowStats.pointShadowFaceCacheHitCount);
	const std::uint64_t measuredPointShadowDeferredFaceCount = CounterDelta(
		measurementStartShadowStats.pointShadowDeferredFaceCount,
		shadowStats.pointShadowDeferredFaceCount);
	const std::uint64_t measuredPointShadowPartialUpdateCount = CounterDelta(
		measurementStartShadowStats.pointShadowPartialUpdateCount,
		shadowStats.pointShadowPartialUpdateCount);
	const std::uint64_t measuredPointShadowFullUpdateCount = CounterDelta(
		measurementStartShadowStats.pointShadowFullUpdateCount,
		shadowStats.pointShadowFullUpdateCount);
	const std::uint64_t measuredPointShadowZeroRequiredCount = CounterDelta(
		measurementStartShadowStats.pointShadowZeroRequiredCount,
		shadowStats.pointShadowZeroRequiredCount);
	const std::uint64_t measuredPointShadowFaceDemandCheckCount = CounterDelta(
		measurementStartShadowStats.pointShadowFaceDemandCheckCount,
		shadowStats.pointShadowFaceDemandCheckCount);
	const std::uint64_t measuredPointShadowFaceSignatureBuildCount =
		CounterDelta(
			measurementStartShadowStats.pointShadowFaceSignatureBuildCount,
			shadowStats.pointShadowFaceSignatureBuildCount);
	const double measuredPointShadowFaceDemandCpuMilliseconds = CounterDelta(
		measurementStartShadowStats
			.totalPointShadowFaceDemandCpuMilliseconds,
		shadowStats.totalPointShadowFaceDemandCpuMilliseconds);
	const double measuredPointShadowFaceSignatureCpuMilliseconds =
		CounterDelta(
			measurementStartShadowStats
				.totalPointShadowFaceSignatureCpuMilliseconds,
			shadowStats.totalPointShadowFaceSignatureCpuMilliseconds);
	const std::uint64_t measuredSpotLightUpdateCount = CounterDelta(
		measurementStartShadowStats.spotLightUpdateCount,
		shadowStats.spotLightUpdateCount);
	const std::uint64_t measuredEmptyShadowClearCount = CounterDelta(
		measurementStartShadowStats.emptyShadowClearCount,
		shadowStats.emptyShadowClearCount);
	const std::uint64_t measuredShadowResourceFailureCount = CounterDelta(
		measurementStartShadowStats.shadowResourceFailureCount,
		shadowStats.shadowResourceFailureCount);
	const std::uint64_t measuredConservativeShadowFallbackCount = CounterDelta(
		measurementStartShadowStats.conservativeShadowFallbackCount,
		shadowStats.conservativeShadowFallbackCount);
	const std::uint64_t measuredSpotFitCount = CounterDelta(
		measurementStartShadowStats.spotFitCount,
		shadowStats.spotFitCount);
	const std::uint64_t measuredSpotProjectionAwareFitCount = CounterDelta(
		measurementStartShadowStats.spotProjectionAwareFitCount,
		shadowStats.spotProjectionAwareFitCount);
	const std::uint64_t measuredSpotFitFallbackCount = CounterDelta(
		measurementStartShadowStats.spotFitFallbackCount,
		shadowStats.spotFitFallbackCount);
	const std::uint64_t measuredSpotFitCandidateCount = CounterDelta(
		measurementStartShadowStats.totalSpotFitCandidateCount,
		shadowStats.totalSpotFitCandidateCount);
	const std::uint64_t measuredSpotFitAcceptedCount = CounterDelta(
		measurementStartShadowStats.totalSpotFitAcceptedCount,
		shadowStats.totalSpotFitAcceptedCount);
	const std::uint64_t measuredSpotFitRejectedCount = CounterDelta(
		measurementStartShadowStats.totalSpotFitRejectedCount,
		shadowStats.totalSpotFitRejectedCount);
	const double measuredSpotFitCpuMilliseconds = CounterDelta(
		measurementStartShadowStats.totalSpotFitCpuMilliseconds,
		shadowStats.totalSpotFitCpuMilliseconds);
	const std::uint64_t measuredUpdatedLightCount =
		measuredDirectionalLightUpdateCount +
		measuredPointLightUpdateCount +
		measuredSpotLightUpdateCount;
	const std::uint64_t measuredCasterCandidateCount = CounterDelta(
		measurementStartShadowStats.totalCasterCandidateCount,
		shadowStats.totalCasterCandidateCount);
	const std::uint64_t measuredCasterCulledCount = CounterDelta(
		measurementStartShadowStats.totalCasterCulledCount,
		shadowStats.totalCasterCulledCount);
	const std::uint64_t measuredCasterCullingLightCount = CounterDelta(
		measurementStartShadowStats.totalCasterCullingLightCount,
		shadowStats.totalCasterCullingLightCount);
	const std::uint64_t measuredCasterDrawCount = CounterDelta(
		measurementStartShadowStats.totalCasterDrawCount,
		shadowStats.totalCasterDrawCount);
	const std::uint64_t measuredCasterTriangleCount = CounterDelta(
		measurementStartShadowStats.totalCasterTriangleCount,
		shadowStats.totalCasterTriangleCount);
	const std::uint64_t measuredDirectionalFitCount = CounterDelta(
		measurementStartShadowStats.directionalFitCount,
		shadowStats.directionalFitCount);
	const std::uint64_t measuredDirectionalLightAabbFitCount = CounterDelta(
		measurementStartShadowStats.directionalLightAabbFitCount,
		shadowStats.directionalLightAabbFitCount);
	const std::uint64_t measuredDirectionalResolutionChangeCount = CounterDelta(
		measurementStartShadowStats.directionalResolutionChangeCount,
		shadowStats.directionalResolutionChangeCount);
	const double measuredDirectionalFitCpuMilliseconds = CounterDelta(
		measurementStartShadowStats.totalDirectionalFitCpuMilliseconds,
		shadowStats.totalDirectionalFitCpuMilliseconds);
	output << "{\n"
		<< "  \"schemaVersion\": 21,\n"
		<< "  \"success\": " << (success ? "true" : "false") << ",\n"
		<< "  \"scene\": \"" << EscapeJsonString(options.sceneName) << "\",\n"
		<< "  \"modelPath\": \"" << EscapeJsonString(options.modelPath) << "\",\n"
		<< "  \"materialMode\": \""
		<< (options.untextured ? "override" : "source") << "\",\n"
		<< "  \"renderPath\": \""
		<< EscapeJsonString(options.renderPath) << "\",\n"
		<< "  \"capturePath\": \"" << EscapeJsonString(options.capturePath) << "\",\n"
		<< "  \"captureRequired\": "
		<< (options.captureFinalFrame ? "true" : "false") << ",\n"
		<< "  \"glVendor\": \""
		<< EscapeJsonString(glVendor ? glVendor : "") << "\",\n"
		<< "  \"glRenderer\": \""
		<< EscapeJsonString(glRenderer ? glRenderer : "") << "\",\n"
		<< "  \"glVersion\": \""
		<< EscapeJsonString(glVersion ? glVersion : "") << "\",\n"
		<< "  \"buildConfiguration\": \""
		<< buildConfiguration << "\",\n"
		<< "  \"architecture\": \"" << architecture << "\",\n"
		<< "  \"resolution\": ["
		<< properties.SCREEN_WIDTH << ", " << properties.SCREEN_HEIGHT << "],\n"
		<< "  \"camera\": {\n"
		<< "    \"position\": ["
		<< options.cameraPosition.x << ", "
		<< options.cameraPosition.y << ", "
		<< options.cameraPosition.z << "],\n"
		<< "    \"target\": ["
		<< options.cameraTarget.x << ", "
		<< options.cameraTarget.y << ", "
		<< options.cameraTarget.z << "],\n"
		<< "    \"up\": ["
		<< options.cameraUp.x << ", "
		<< options.cameraUp.y << ", "
		<< options.cameraUp.z << "],\n"
		<< "    \"fovDegrees\": " << options.fov << "\n"
		<< "  },\n"
		<< "  \"frameMeasurement\": \""
		<< (options.gpuSynchronized ? "gpu-synchronized-wall" : "cpu-submission-wall")
		<< "\",\n"
		<< "  \"settings\": {\n"
		<< "    \"requestedSwapInterval\": 0,\n"
		<< "    \"inputFrozen\": true,\n"
		<< "    \"deferredRendering\": "
		<< (properties.DEFER_RENDERING ? "true" : "false") << ",\n"
		<< "    \"bloom\": "
		<< (properties.BLOOM ? "true" : "false") << ",\n"
		<< "    \"gammaCorrection\": "
		<< (properties.GAMMA_CORRECTION ? "true" : "false") << ",\n"
		<< "    \"autoReloadShaders\": "
		<< (properties.AUTO_RELOAD_SHADERS ? "true" : "false") << ",\n"
		<< "    \"autoReloadMaterials\": "
		<< (properties.AUTO_RELOAD_MATERIALS ? "true" : "false") << ",\n"
		<< "    \"activePointLights\": " << activePointLights << ",\n"
		<< "    \"activeDirectionLights\": "
		<< activeDirectionLights << ",\n"
		<< "    \"activeSpotLights\": " << activeSpotLights << ",\n"
		<< "    \"shadowCastingLights\": "
		<< shadowCastingLights << "\n"
		<< "  },\n"
		<< "  \"ssao\": {\n"
		<< "    \"experiment\": "
		<< (options.ssaoExperiment ? "true" : "false") << ",\n"
		<< "    \"enabled\": "
		<< (properties.SSAO ? "true" : "false") << ",\n"
		<< "    \"mode\": \""
		<< EscapeJsonString(options.ssaoMode) << "\",\n"
		<< "    \"requestedSamples\": "
		<< options.ssaoSamples << ",\n"
		<< "    \"kernelSize\": "
		<< properties.SSAO_KERNEL_SIZE << ",\n"
		<< "    \"radius\": " << properties.SSAO_RADIUS << ",\n"
		<< "    \"bias\": " << properties.SSAO_BIAS << ",\n"
		<< "    \"kernelGeneration\": {\n"
		<< "      \"seed\": 1337,\n"
		<< "      \"capacity\": 64,\n"
		<< "      \"radialScaleDenominator\": 64,\n"
		<< "      \"selection\": \"prefix\",\n"
		<< "      \"sampleCountAndRadialDistributionCoupled\": "
		<< (options.ssaoSamples > 0 && options.ssaoSamples < 64
			? "true"
			: "false") << "\n"
		<< "    },\n"
		<< "    \"outputAvailable\": "
		<< (ssaoOutputAvailable ? "true" : "false") << ",\n"
		<< "    \"outputWidth\": "
		<< (ssaoOutputAvailable ? ssaoFBO->width : 0) << ",\n"
		<< "    \"outputHeight\": "
		<< (ssaoOutputAvailable ? ssaoFBO->height : 0) << ",\n"
		<< "    \"outputInternalFormat\": "
		<< ssaoOutputInternalFormat << ",\n"
		<< "    \"outputInternalFormatName\": \""
		<< (ssaoOutputR16F ? "GL_R16F" : "none-or-unexpected") << "\",\n"
		<< "    \"fullResolution\": "
		<< (ssaoOutputFullResolution ? "true" : "false") << ",\n"
		<< "    \"capturePath\": \""
		<< EscapeJsonString(options.ssaoCapturePath) << "\",\n"
		<< "    \"captureValid\": "
		<< (ssaoCapture.valid ? "true" : "false") << ",\n"
		<< "    \"generate\": {\n"
		<< "      \"available\": "
		<< (ssaoGenerationAvailable ? "true" : "false") << ",\n"
		<< "      \"width\": "
		<< (ssaoGenerationAvailable ? ssaoGenerationFBO->width : 0)
		<< ",\n"
		<< "      \"height\": "
		<< (ssaoGenerationAvailable ? ssaoGenerationFBO->height : 0)
		<< ",\n"
		<< "      \"internalFormat\": "
		<< ssaoGenerationInternalFormat << ",\n"
		<< "      \"internalFormatName\": \""
		<< (ssaoGenerationR16F ? "GL_R16F" : "none-or-unexpected")
		<< "\",\n"
		<< "      \"fullResolution\": "
		<< (ssaoGenerationFullResolution ? "true" : "false") << ",\n"
		<< "      \"resolutionPolicy\": \""
		<< (ssaoGenerationFullResolution ? "full" : "ceil-half")
		<< "\",\n"
		<< "      \"floatCapture\": ";
	WriteJsonFloatCapture(
		output,
		generationFloatCapturePath,
		generationFloatCapture);
	output << "\n"
		<< "    },\n"
		<< "    \"output\": {\n"
		<< "      \"available\": "
		<< (ssaoOutputAvailable ? "true" : "false") << ",\n"
		<< "      \"width\": "
		<< (ssaoOutputAvailable ? ssaoFBO->width : 0) << ",\n"
		<< "      \"height\": "
		<< (ssaoOutputAvailable ? ssaoFBO->height : 0) << ",\n"
		<< "      \"internalFormat\": "
		<< ssaoOutputInternalFormat << ",\n"
		<< "      \"internalFormatName\": \""
		<< (ssaoOutputR16F ? "GL_R16F" : "none-or-unexpected")
		<< "\",\n"
		<< "      \"fullResolution\": "
		<< (ssaoOutputFullResolution ? "true" : "false") << ",\n"
		<< "      \"sampling\": \""
		<< (ssaoBilateral
			? "full-resolution-depth-normal-aware-bilateral"
			: (options.ssaoMode == "half-raw"
				? "direct-gl-linear"
				: "full-resolution-direct")) << "\",\n"
		<< "      \"ldrCapturePath\": \""
		<< EscapeJsonString(options.ssaoCapturePath) << "\",\n"
		<< "      \"ldrCaptureValid\": "
		<< (ssaoCapture.valid ? "true" : "false") << ",\n"
		<< "      \"floatCapture\": ";
	WriteJsonFloatCapture(
		output,
		options.ssaoFloatCapturePath,
		ssaoFloatCapture);
	output << "\n"
		<< "    },\n"
		<< "    \"upsample\": {\n"
		<< "      \"enabled\": "
		<< (ssaoBilateral ? "true" : "false") << ",\n"
		<< "      \"algorithm\": \""
		<< (ssaoBilateral
			? "depth-normal-aware-bilateral-2x2"
			: "none") << "\",\n"
		<< "      \"depthSigma\": "
		<< properties.SSAO_BILATERAL_DEPTH_SIGMA << ",\n"
		<< "      \"normalPower\": "
		<< properties.SSAO_BILATERAL_NORMAL_POWER << ",\n"
		<< "      \"neighborhood\": \"2x2-bilinear-footprint\",\n"
		<< "      \"inputs\": [\"halfAO\", \"fullPositionDepth\", "
		<< "\"fullNormal\"]\n"
		<< "    },\n"
		<< "    \"guidance\": {\n"
		<< "      \"depthCapture\": ";
	WriteJsonFloatCapture(
		output,
		options.ssaoDepthCapturePath,
		ssaoDepthCapture);
	output << ",\n"
		<< "      \"normalCapture\": ";
	WriteJsonFloatCapture(
		output,
		options.ssaoNormalCapturePath,
		ssaoNormalCapture);
	output << "\n"
		<< "    }\n"
		<< "  },\n"
		<< "  \"warmupFrames\": " << options.warmupFrames << ",\n"
		<< "  \"measuredFrames\": "
		<< (options.captureFrame - options.warmupFrames) << ",\n"
		<< "  \"shadow\": {\n"
		<< "    \"experiment\": " << (options.shadowExperiment ? "true" : "false") << ",\n"
		<< "    \"mode\": \"" << EscapeJsonString(options.shadowMode) << "\",\n"
		<< "    \"sampling\": \""
		<< EscapeJsonString(options.shadowSampling) << "\",\n"
		<< "    \"lights\": \"" << EscapeJsonString(options.shadowLights) << "\",\n"
		<< "    \"workload\": \""
		<< EscapeJsonString(options.shadowWorkload) << "\",\n"
		<< "    \"variant\": \""
		<< EscapeJsonString(options.shadowVariant) << "\",\n"
		<< "    \"worldScale\": " << options.worldScale << ",\n"
		<< "    \"requestedResolution\": "
		<< options.shadowResolution << ",\n"
		<< "    \"legacyCacheSignatureEnabled\": "
		<< (properties.SHADOW_CACHE_USE_LEGACY_SIGNATURE
			? "true"
			: "false") << ",\n"
		<< "    \"cacheDisabled\": "
		<< (properties.SHADOW_CACHE_DISABLED ? "true" : "false")
		<< ",\n"
		<< "    \"perLightCacheEnabled\": "
		<< (properties.SHADOW_PER_LIGHT_CACHE ? "true" : "false")
		<< ",\n"
		<< "    \"spatialCasterCacheEnabled\": "
		<< (properties.SHADOW_SPATIAL_CASTER_CACHE
			? "true"
			: "false") << ",\n"
		<< "    \"directionalLight\": ["
		<< options.directionalLightDirection.x << ", "
		<< options.directionalLightDirection.y << ", "
		<< options.directionalLightDirection.z << "],\n"
		<< "    \"pointLightPosition\": ["
		<< options.pointLightPosition.x << ", "
		<< options.pointLightPosition.y << ", "
		<< options.pointLightPosition.z << "],\n"
		<< "    \"spotLightPosition\": ["
		<< (options.hasSpotLightPosition
			? options.spotLightPosition.x
			: options.cameraPosition.x) << ", "
		<< (options.hasSpotLightPosition
			? options.spotLightPosition.y
			: options.cameraPosition.y) << ", "
		<< (options.hasSpotLightPosition
			? options.spotLightPosition.z
			: options.cameraPosition.z) << "],\n"
		<< "    \"spotLightDirection\": ["
		<< (options.hasSpotLightDirection
			? glm::normalize(options.spotLightDirection).x
			: glm::normalize(
				options.cameraTarget -
				options.cameraPosition).x) << ", "
		<< (options.hasSpotLightDirection
			? glm::normalize(options.spotLightDirection).y
			: glm::normalize(
				options.cameraTarget -
				options.cameraPosition).y) << ", "
		<< (options.hasSpotLightDirection
			? glm::normalize(options.spotLightDirection).z
			: glm::normalize(
				options.cameraTarget -
				options.cameraPosition).z) << "],\n"
		<< "    \"spotShadowNearPlane\": "
		<< actualSpotShadowNearPlane << ",\n"
		<< "    \"spotShadowFarPlane\": "
		<< actualSpotShadowFarPlane << ",\n"
		<< "    \"casterCullingEnabled\": "
		<< (properties.SHADOW_CASTER_CULLING ? "true" : "false")
		<< ",\n"
		<< "    \"directionalLightAabbFitEnabled\": "
		<< (properties.DIRECTIONAL_SHADOW_LIGHT_AABB_FIT
			? "true"
			: "false") << ",\n"
		<< "    \"directionalDensityResolutionEnabled\": "
		<< (properties.DIRECTIONAL_SHADOW_DENSITY_RESOLUTION
			? "true"
			: "false") << ",\n"
		<< "    \"optimizationFlags\": "
		<< properties.GetShadowOptimizationFlags() << ",\n"
		<< "    \"exactEarlyOutEnabled\": "
		<< (properties.SHADOW_EXACT_EARLY_OUT ? "true" : "false")
		<< ",\n"
		<< "    \"preparedPointInputsEnabled\": "
		<< (properties.SHADOW_PREPARED_POINT_INPUTS ? "true" : "false")
		<< ",\n"
		<< "    \"adaptivePointSamplesEnabled\": "
		<< (properties.SHADOW_ADAPTIVE_POINT_SAMPLES
			? "true"
			: "false") << ",\n"
		<< "    \"adaptivePcssFilterEnabled\": "
		<< (properties.SHADOW_ADAPTIVE_PCSS_FILTER
			? "true"
			: "false") << ",\n"
		<< "    \"stagedPcssBlockerEnabled\": "
		<< (properties.SHADOW_STAGED_PCSS_BLOCKER
			? "true"
			: "false") << ",\n"
		<< "    \"adaptiveMinSamples\": "
		<< properties.SHADOW_ADAPTIVE_MIN_SAMPLES << ",\n"
		<< "    \"hardwareDepthCompareEnabled\": "
		<< (properties.SHADOW_HARDWARE_DEPTH_COMPARE
			? "true"
			: "false") << ",\n"
		<< "    \"hardwareLinearPcfEnabled\": "
		<< (properties.SHADOW_HARDWARE_LINEAR_PCF
			? "true"
			: "false") << ",\n"
		<< "    \"hardwareReducedPcfEnabled\": "
		<< (properties.SHADOW_HARDWARE_REDUCED_PCF
			? "true"
			: "false") << ",\n"
		<< "    \"texelScaledBiasEnabled\": "
		<< (properties.SHADOW_TEXEL_SCALED_BIAS
			? "true"
			: "false") << ",\n"
		<< "    \"spotRadialBiasDirectionEnabled\": "
		<< (properties.SHADOW_SPOT_RADIAL_BIAS_DIRECTION
			? "true"
			: "false") << ",\n"
		<< "    \"spotPcssLinearDepthEnabled\": "
		<< (properties.SHADOW_SPOT_PCSS_LINEAR_DEPTH
			? "true"
			: "false") << ",\n"
		<< "    \"spotPcssReducedFilterEnabled\": "
		<< (properties.SHADOW_SPOT_PCSS_REDUCED_FILTER
			? "true"
			: "false") << ",\n"
		<< "    \"spotCasterDepthFitEnabled\": "
		<< (properties.SHADOW_SPOT_CASTER_DEPTH_FIT
			? "true"
			: "false") << ",\n"
		<< "    \"bias2DMinTexels\": "
		<< properties.SHADOW_BIAS_2D_MIN_TEXELS << ",\n"
		<< "    \"bias2DSlopeTexels\": "
		<< properties.SHADOW_BIAS_2D_SLOPE_TEXELS << ",\n"
		<< "    \"biasCubeMinTexels\": "
		<< properties.SHADOW_BIAS_CUBE_MIN_TEXELS << ",\n"
		<< "    \"biasCubeSlopeTexels\": "
		<< properties.SHADOW_BIAS_CUBE_SLOPE_TEXELS << ",\n"
		<< "    \"pointShadowRenderPolicy\": \""
		<< (properties.POINT_SHADOW_ADAPTIVE_RENDERING
			? "adaptive"
			: (properties.POINT_SHADOW_SIX_FACE_RENDERING
				? "six-face"
				: "layered")) << "\",\n"
		<< "    \"pointShadowFaceCullingEnabled\": "
		<< (properties.POINT_SHADOW_FACE_CULLING
			? "true"
			: "false") << ",\n"
		<< "    \"pointShadowPerFaceCacheEnabled\": "
		<< (properties.POINT_SHADOW_PER_FACE_CACHE
			? "true"
			: "false") << ",\n"
		<< "    \"pointShadowForceAllFacesRequired\": "
		<< (properties.POINT_SHADOW_FORCE_ALL_FACES_REQUIRED
			? "true"
			: "false") << ",\n"
		<< "    \"updateCount\": " << shadowStats.updateCount << ",\n"
		<< "    \"cacheHitCount\": " << shadowStats.cacheHitCount << ",\n"
		<< "    \"measuredUpdateCount\": "
		<< CounterDelta(
			measurementStartShadowStats.updateCount,
			shadowStats.updateCount) << ",\n"
		<< "    \"measuredCacheHitCount\": "
		<< CounterDelta(
			measurementStartShadowStats.cacheHitCount,
			shadowStats.cacheHitCount) << ",\n"
		<< "    \"cacheCheckCount\": "
		<< shadowStats.cacheCheckCount << ",\n"
		<< "    \"measuredCacheCheckCount\": "
		<< measuredCacheCheckCount << ",\n"
		<< "    \"cacheMissCount\": "
		<< shadowStats.cacheMissCount << ",\n"
		<< "    \"measuredCacheMissCount\": "
		<< CounterDelta(
			measurementStartShadowStats.cacheMissCount,
			shadowStats.cacheMissCount) << ",\n"
		<< "    \"legacySignatureCheckCount\": "
		<< shadowStats.legacySignatureCheckCount << ",\n"
		<< "    \"measuredLegacySignatureCheckCount\": "
		<< CounterDelta(
			measurementStartShadowStats.legacySignatureCheckCount,
			shadowStats.legacySignatureCheckCount) << ",\n"
		<< "    \"revisionCheckCount\": "
		<< shadowStats.revisionCheckCount << ",\n"
		<< "    \"measuredRevisionCheckCount\": "
		<< CounterDelta(
			measurementStartShadowStats.revisionCheckCount,
			shadowStats.revisionCheckCount) << ",\n"
		<< "    \"casterStateSyncCount\": "
		<< shadowStats.casterStateSyncCount << ",\n"
		<< "    \"measuredCasterStateSyncCount\": "
		<< measuredCasterStateSyncCount << ",\n"
		<< "    \"casterBoundsRebuildCount\": "
		<< shadowStats.casterBoundsRebuildCount << ",\n"
		<< "    \"measuredCasterBoundsRebuildCount\": "
		<< CounterDelta(
			measurementStartShadowStats.casterBoundsRebuildCount,
			shadowStats.casterBoundsRebuildCount) << ",\n"
		<< "    \"casterRevision\": "
		<< shadowStats.casterRevision << ",\n"
		<< "    \"sceneTopologyRevision\": "
		<< shadowStats.sceneTopologyRevision << ",\n"
		<< "    \"measurementStartSceneTopologyRevision\": "
		<< measurementStartShadowStats.sceneTopologyRevision << ",\n"
		<< "    \"sceneTopologyInvalidationCount\": "
		<< shadowStats.sceneTopologyInvalidationCount << ",\n"
		<< "    \"measuredSceneTopologyInvalidationCount\": "
		<< CounterDelta(
			measurementStartShadowStats.sceneTopologyInvalidationCount,
			shadowStats.sceneTopologyInvalidationCount) << ",\n"
		<< "    \"sceneTopologyModelCount\": "
		<< shadowStats.sceneTopologyModelCount << ",\n"
		<< "    \"measurementStartSceneTopologyModelCount\": "
		<< measurementStartShadowStats.sceneTopologyModelCount << ",\n"
		<< "    \"lastCacheCheckUsedLegacySignature\": "
		<< (shadowStats.lastCacheCheckUsedLegacySignature
			? "true"
			: "false") << ",\n"
		<< "    \"lastCacheCheckCpuMilliseconds\": "
		<< shadowStats.lastCacheCheckCpuMilliseconds << ",\n"
		<< "    \"totalCacheCheckCpuMilliseconds\": "
		<< shadowStats.totalCacheCheckCpuMilliseconds << ",\n"
		<< "    \"measuredCacheCheckCpuMilliseconds\": "
		<< measuredCacheCheckCpuMilliseconds << ",\n"
		<< "    \"measuredAverageCacheCheckCpuMilliseconds\": "
		<< (measuredCacheCheckCount > 0
			? measuredCacheCheckCpuMilliseconds /
				static_cast<double>(measuredCacheCheckCount)
			: 0.0) << ",\n"
		<< "    \"lastCasterStateSyncCpuMilliseconds\": "
		<< shadowStats.lastCasterStateSyncCpuMilliseconds << ",\n"
		<< "    \"totalCasterStateSyncCpuMilliseconds\": "
		<< shadowStats.totalCasterStateSyncCpuMilliseconds << ",\n"
		<< "    \"measuredCasterStateSyncCpuMilliseconds\": "
		<< measuredCasterStateSyncCpuMilliseconds << ",\n"
		<< "    \"measuredAverageCasterStateSyncCpuMilliseconds\": "
		<< (measuredCasterStateSyncCount > 0
			? measuredCasterStateSyncCpuMilliseconds /
				static_cast<double>(measuredCasterStateSyncCount)
			: 0.0) << ",\n"
		<< "    \"lightCacheHitCount\": "
		<< shadowStats.lightCacheHitCount << ",\n"
		<< "    \"measuredLightCacheHitCount\": "
		<< measuredLightCacheHitCount << ",\n"
		<< "    \"directionalLightUpdateCount\": "
		<< shadowStats.directionalLightUpdateCount << ",\n"
		<< "    \"measuredDirectionalLightUpdateCount\": "
		<< measuredDirectionalLightUpdateCount << ",\n"
		<< "    \"directionalFitCount\": "
		<< shadowStats.directionalFitCount << ",\n"
		<< "    \"measuredDirectionalFitCount\": "
		<< measuredDirectionalFitCount << ",\n"
		<< "    \"directionalLightAabbFitCount\": "
		<< shadowStats.directionalLightAabbFitCount << ",\n"
		<< "    \"measuredDirectionalLightAabbFitCount\": "
		<< measuredDirectionalLightAabbFitCount << ",\n"
		<< "    \"directionalResolutionChangeCount\": "
		<< shadowStats.directionalResolutionChangeCount << ",\n"
		<< "    \"measuredDirectionalResolutionChangeCount\": "
		<< measuredDirectionalResolutionChangeCount << ",\n"
		<< "    \"totalDirectionalFitCpuMilliseconds\": "
		<< shadowStats.totalDirectionalFitCpuMilliseconds << ",\n"
		<< "    \"measuredDirectionalFitCpuMilliseconds\": "
		<< measuredDirectionalFitCpuMilliseconds << ",\n"
		<< "    \"measuredAverageDirectionalFitCpuMilliseconds\": "
		<< (measuredDirectionalFitCount > 0
			? measuredDirectionalFitCpuMilliseconds /
				static_cast<double>(measuredDirectionalFitCount)
			: 0.0) << ",\n"
		<< "    \"lastDirectionalFitRawWidth\": "
		<< shadowStats.lastDirectionalFitRawWidth << ",\n"
		<< "    \"lastDirectionalFitRawHeight\": "
		<< shadowStats.lastDirectionalFitRawHeight << ",\n"
		<< "    \"lastDirectionalFitRawDepth\": "
		<< shadowStats.lastDirectionalFitRawDepth << ",\n"
		<< "    \"lastDirectionalFitWidth\": "
		<< shadowStats.lastDirectionalFitWidth << ",\n"
		<< "    \"lastDirectionalFitHeight\": "
		<< shadowStats.lastDirectionalFitHeight << ",\n"
		<< "    \"lastDirectionalFitDepth\": "
		<< shadowStats.lastDirectionalFitDepth << ",\n"
		<< "    \"lastDirectionalFitTexelSizeX\": "
		<< shadowStats.lastDirectionalFitTexelSizeX << ",\n"
		<< "    \"lastDirectionalFitTexelSizeY\": "
		<< shadowStats.lastDirectionalFitTexelSizeY << ",\n"
		<< "    \"lastDirectionalFitUtilization\": "
		<< shadowStats.lastDirectionalFitUtilization << ",\n"
		<< "    \"lastDirectionalFitReferenceTexelSize\": "
		<< shadowStats.lastDirectionalFitReferenceTexelSize << ",\n"
		<< "    \"lastDirectionalFitResolution\": "
		<< shadowStats.lastDirectionalFitResolution << ",\n"
		<< "    \"pointLightUpdateCount\": "
		<< shadowStats.pointLightUpdateCount << ",\n"
		<< "    \"measuredPointLightUpdateCount\": "
		<< measuredPointLightUpdateCount << ",\n"
		<< "    \"pointShadowLayeredUpdateCount\": "
		<< shadowStats.pointShadowLayeredUpdateCount << ",\n"
		<< "    \"measuredPointShadowLayeredUpdateCount\": "
		<< measuredPointShadowLayeredUpdateCount << ",\n"
		<< "    \"pointShadowSixFaceUpdateCount\": "
		<< shadowStats.pointShadowSixFaceUpdateCount << ",\n"
		<< "    \"measuredPointShadowSixFaceUpdateCount\": "
		<< measuredPointShadowSixFaceUpdateCount << ",\n"
		<< "    \"pointShadowSubmissionPassCount\": "
		<< shadowStats.pointShadowSubmissionPassCount << ",\n"
		<< "    \"measuredPointShadowSubmissionPassCount\": "
		<< measuredPointShadowSubmissionPassCount << ",\n"
		<< "    \"pointShadowFaceCullingPassCount\": "
		<< shadowStats.pointShadowFaceCullingPassCount << ",\n"
		<< "    \"measuredPointShadowFaceCullingPassCount\": "
		<< measuredPointShadowFaceCullingPassCount << ",\n"
		<< "    \"pointShadowRequiredFaceCount\": "
		<< shadowStats.pointShadowRequiredFaceCount << ",\n"
		<< "    \"measuredPointShadowRequiredFaceCount\": "
		<< measuredPointShadowRequiredFaceCount << ",\n"
		<< "    \"pointShadowRenderedFaceCount\": "
		<< shadowStats.pointShadowRenderedFaceCount << ",\n"
		<< "    \"measuredPointShadowRenderedFaceCount\": "
		<< measuredPointShadowRenderedFaceCount << ",\n"
		<< "    \"pointShadowFaceCacheHitCount\": "
		<< shadowStats.pointShadowFaceCacheHitCount << ",\n"
		<< "    \"measuredPointShadowFaceCacheHitCount\": "
		<< measuredPointShadowFaceCacheHitCount << ",\n"
		<< "    \"pointShadowDeferredFaceCount\": "
		<< shadowStats.pointShadowDeferredFaceCount << ",\n"
		<< "    \"measuredPointShadowDeferredFaceCount\": "
		<< measuredPointShadowDeferredFaceCount << ",\n"
		<< "    \"measuredPointShadowPartialUpdateCount\": "
		<< measuredPointShadowPartialUpdateCount << ",\n"
		<< "    \"measuredPointShadowFullUpdateCount\": "
		<< measuredPointShadowFullUpdateCount << ",\n"
		<< "    \"measuredPointShadowZeroRequiredCount\": "
		<< measuredPointShadowZeroRequiredCount << ",\n"
		<< "    \"measuredPointShadowFaceDemandCheckCount\": "
		<< measuredPointShadowFaceDemandCheckCount << ",\n"
		<< "    \"measuredPointShadowFaceSignatureBuildCount\": "
		<< measuredPointShadowFaceSignatureBuildCount << ",\n"
		<< "    \"measuredPointShadowFaceDemandCpuMilliseconds\": "
		<< measuredPointShadowFaceDemandCpuMilliseconds << ",\n"
		<< "    \"measuredAveragePointShadowFaceDemandCpuMilliseconds\": "
		<< (measuredPointShadowFaceDemandCheckCount > 0
			? measuredPointShadowFaceDemandCpuMilliseconds /
				static_cast<double>(
					measuredPointShadowFaceDemandCheckCount)
			: 0.0) << ",\n"
		<< "    \"measuredPointShadowFaceSignatureCpuMilliseconds\": "
		<< measuredPointShadowFaceSignatureCpuMilliseconds << ",\n"
		<< "    \"measuredAveragePointShadowFaceSignatureCpuMilliseconds\": "
		<< (measuredPointShadowFaceSignatureBuildCount > 0
			? measuredPointShadowFaceSignatureCpuMilliseconds /
				static_cast<double>(
					measuredPointShadowFaceSignatureBuildCount)
			: 0.0) << ",\n"
		<< "    \"lastPointShadowRequiredFaceMask\": "
		<< static_cast<unsigned int>(
			shadowStats.lastPointShadowRequiredFaceMask) << ",\n"
		<< "    \"lastPointShadowUpdateFaceMask\": "
		<< static_cast<unsigned int>(
			shadowStats.lastPointShadowUpdateFaceMask) << ",\n"
		<< "    \"spotLightUpdateCount\": "
		<< shadowStats.spotLightUpdateCount << ",\n"
		<< "    \"measuredSpotLightUpdateCount\": "
		<< measuredSpotLightUpdateCount << ",\n"
		<< "    \"emptyShadowClearCount\": "
		<< shadowStats.emptyShadowClearCount << ",\n"
		<< "    \"measuredEmptyShadowClearCount\": "
		<< measuredEmptyShadowClearCount << ",\n"
		<< "    \"shadowResourceFailureCount\": "
		<< shadowStats.shadowResourceFailureCount << ",\n"
		<< "    \"measuredShadowResourceFailureCount\": "
		<< measuredShadowResourceFailureCount << ",\n"
		<< "    \"conservativeShadowFallbackCount\": "
		<< shadowStats.conservativeShadowFallbackCount << ",\n"
		<< "    \"measuredConservativeShadowFallbackCount\": "
		<< measuredConservativeShadowFallbackCount << ",\n"
		<< "    \"spotFitCount\": "
		<< shadowStats.spotFitCount << ",\n"
		<< "    \"measuredSpotFitCount\": "
		<< measuredSpotFitCount << ",\n"
		<< "    \"spotProjectionAwareFitCount\": "
		<< shadowStats.spotProjectionAwareFitCount << ",\n"
		<< "    \"measuredSpotProjectionAwareFitCount\": "
		<< measuredSpotProjectionAwareFitCount << ",\n"
		<< "    \"spotFitFallbackCount\": "
		<< shadowStats.spotFitFallbackCount << ",\n"
		<< "    \"measuredSpotFitFallbackCount\": "
		<< measuredSpotFitFallbackCount << ",\n"
		<< "    \"measuredSpotFitCandidateCount\": "
		<< measuredSpotFitCandidateCount << ",\n"
		<< "    \"measuredSpotFitAcceptedCount\": "
		<< measuredSpotFitAcceptedCount << ",\n"
		<< "    \"measuredSpotFitRejectedCount\": "
		<< measuredSpotFitRejectedCount << ",\n"
		<< "    \"totalSpotFitCpuMilliseconds\": "
		<< shadowStats.totalSpotFitCpuMilliseconds << ",\n"
		<< "    \"measuredSpotFitCpuMilliseconds\": "
		<< measuredSpotFitCpuMilliseconds << ",\n"
		<< "    \"measuredAverageSpotFitCpuMilliseconds\": "
		<< (measuredSpotFitCount > 0
			? measuredSpotFitCpuMilliseconds /
				static_cast<double>(measuredSpotFitCount)
			: 0.0) << ",\n"
		<< "    \"lastSpotFitCandidateCount\": "
		<< shadowStats.lastSpotFitCandidateCount << ",\n"
		<< "    \"lastSpotFitAcceptedCount\": "
		<< shadowStats.lastSpotFitAcceptedCount << ",\n"
		<< "    \"lastSpotFitRejectedCount\": "
		<< shadowStats.lastSpotFitRejectedCount << ",\n"
		<< "    \"lastSpotFitLegacyNear\": "
		<< shadowStats.lastSpotFitLegacyNear << ",\n"
		<< "    \"lastSpotFitLegacyFar\": "
		<< shadowStats.lastSpotFitLegacyFar << ",\n"
		<< "    \"lastSpotFitRawNear\": "
		<< shadowStats.lastSpotFitRawNear << ",\n"
		<< "    \"lastSpotFitRawFar\": "
		<< shadowStats.lastSpotFitRawFar << ",\n"
		<< "    \"lastSpotFitNear\": "
		<< shadowStats.lastSpotFitNear << ",\n"
		<< "    \"lastSpotFitFar\": "
		<< shadowStats.lastSpotFitFar << ",\n"
		<< "    \"lastSpotFitDepthSpanReduction\": "
		<< shadowStats.lastSpotFitDepthSpanReduction << ",\n"
		<< "    \"lastSpotFitDepthUtilization\": "
		<< shadowStats.lastSpotFitDepthUtilization << ",\n"
		<< "    \"lastSpotFitProjectionDepthScale\": "
		<< shadowStats.lastSpotFitProjectionDepthScale << ",\n"
		<< "    \"lastSpotFitPrecisionGain\": "
		<< shadowStats.lastSpotFitPrecisionGain << ",\n"
		<< "    \"lastSpotFitMinimumProjectedCoverageMargin\": "
		<< shadowStats.lastSpotFitMinimumProjectedCoverageMargin << ",\n"
		<< "    \"lastSpotFitLightIndex\": "
		<< shadowStats.lastSpotFitLightIndex << ",\n"
		<< "    \"lastSpotFitRawNearClipped\": "
		<< (shadowStats.lastSpotFitRawNearClipped
			? "true"
			: "false") << ",\n"
		<< "    \"lastSpotFitProjectionAware\": "
		<< (shadowStats.lastSpotFitProjectionAware
			? "true"
			: "false") << ",\n"
		<< "    \"measuredUpdatedLightCount\": "
		<< measuredUpdatedLightCount << ",\n"
		<< "    \"updatedLightCount\": " << shadowStats.updatedLightCount << ",\n"
		<< "    \"casterCandidateCount\": "
		<< shadowStats.casterCandidateCount << ",\n"
		<< "    \"casterCulledCount\": "
		<< shadowStats.casterCulledCount << ",\n"
		<< "    \"casterCullingLightCount\": "
		<< shadowStats.casterCullingLightCount << ",\n"
		<< "    \"casterDrawCount\": " << shadowStats.casterDrawCount << ",\n"
		<< "    \"casterTriangleCount\": " << shadowStats.casterTriangleCount << ",\n"
		<< "    \"measuredCasterCandidateCount\": "
		<< measuredCasterCandidateCount << ",\n"
		<< "    \"measuredCasterCulledCount\": "
		<< measuredCasterCulledCount << ",\n"
		<< "    \"measuredCasterCullingLightCount\": "
		<< measuredCasterCullingLightCount << ",\n"
		<< "    \"measuredCasterDrawCount\": "
		<< measuredCasterDrawCount << ",\n"
		<< "    \"measuredCasterTriangleCount\": "
		<< measuredCasterTriangleCount << ",\n"
		<< "    \"lastUpdateCpuMilliseconds\": "
		<< shadowStats.lastUpdateCpuMilliseconds << ",\n"
		<< "    \"updateGpuMilliseconds\": "
		<< shadowUpdateGpuMilliseconds << ",\n"
		<< "    \"pointShadowCubeEvidence\": ";
	WriteJsonPointShadowCubeEvidence(output, pointShadowCubeEvidence);
	output << "\n"
		<< "  },\n"
		<< "  \"motionTimeline\": ";
	WriteJsonBenchmarkMotionTimeline(
		output,
		motionTimeline,
		timelineTelemetry);
	output << ",\n"
		<< "  \"loadMilliseconds\": " << loadMilliseconds << ",\n"
		<< "  \"frameTimeMilliseconds\": {\n"
		<< "    \"sampleCount\": " << frameTiming.sampleCount << ",\n"
		<< "    \"mean\": " << frameTiming.meanMilliseconds << ",\n"
		<< "    \"median\": " << frameTiming.medianMilliseconds << ",\n"
		<< "    \"p95\": " << frameTiming.p95Milliseconds << ",\n"
		<< "    \"p99\": " << frameTiming.p99Milliseconds << "\n"
		<< "  },\n"
		<< "  \"averageFrameMilliseconds\": " << frameTiming.meanMilliseconds << ",\n"
		<< "  \"averageFps\": "
		<< (frameTiming.meanMilliseconds > 0.0
			? 1000.0 / frameTiming.meanMilliseconds
			: 0.0)
		<< ",\n"
		<< "  \"meshCount\": " << meshCount << ",\n"
		<< "  \"vertexCount\": " << vertexCount << ",\n"
		<< "  \"triangleCount\": " << triangleCount << ",\n"
		<< "  \"sourceCenter\": ["
		<< sourceCenter.x << ", " << sourceCenter.y << ", " << sourceCenter.z << "],\n"
		<< "  \"sourceRadius\": " << sourceRadius << ",\n"
		<< "  \"appliedScale\": " << appliedScale << ",\n"
		<< "  \"meanLuminance\": " << capture.meanLuminance << ",\n"
		<< "  \"nonBlackRatio\": " << capture.nonBlackRatio << ",\n"
		<< "  \"memoryBytes\": {\n"
		<< "    \"processWorkingSet\": " << memory.processWorkingSetBytes << ",\n"
		<< "    \"processPrivate\": " << memory.processPrivateBytes << ",\n"
		<< "    \"texture\": " << memoryBytes(MemoryResourceType::Texture) << ",\n"
		<< "    \"meshCpu\": " << memoryBytes(MemoryResourceType::MeshCpu) << ",\n"
		<< "    \"meshGpu\": " << memoryBytes(MemoryResourceType::MeshGpu) << ",\n"
		<< "    \"renderTarget\": " << memoryBytes(MemoryResourceType::RenderTarget) << "\n"
		<< "  },\n"
		<< "  \"profiler\": {\n"
		<< "    \"gpuTimingSupported\": "
		<< (PerformanceProfiler::GetInstance().IsGpuTimingSupported()
			? "true"
			: "false") << ",\n"
		<< "    \"summary\": {\n"
		<< "      \"wallFrame\": ";
	WriteJsonDistribution(output, profilerSamples.wallFrameMs);
	output << ",\n      \"cpuFrame\": ";
	WriteJsonDistribution(output, profilerSamples.cpuFrameMs);
	output << ",\n      \"gpuFrame\": ";
	WriteJsonDistribution(output, profilerSamples.gpuFrameMs);
	output << ",\n      \"drawCalls\": ";
	WriteJsonDistribution(output, drawCallSamples);
	output << ",\n      \"cpuZones\": ";
	WriteJsonZoneDistributions(output, profilerSamples.cpuZoneMs);
	output << ",\n      \"gpuZones\": ";
	WriteJsonZoneDistributions(output, profilerSamples.gpuZoneMs);
	output << "\n    },\n"
		<< "    \"samples\": {\n"
		<< "      \"wallFrame\": ";
	WriteJsonDoubleArray(output, profilerSamples.wallFrameMs);
	output << ",\n      \"cpuFrame\": ";
	WriteJsonDoubleArray(output, profilerSamples.cpuFrameMs);
	output << ",\n      \"gpuFrame\": ";
	WriteJsonDoubleArray(output, profilerSamples.gpuFrameMs);
	output << ",\n      \"drawCalls\": ";
	WriteJsonDoubleArray(output, drawCallSamples);
	output << ",\n      \"cpuZones\": ";
	WriteJsonZoneSamples(output, profilerSamples.cpuZoneMs);
	output << ",\n      \"gpuZones\": ";
	WriteJsonZoneSamples(output, profilerSamples.gpuZoneMs);
	output << "\n    }\n"
		<< "  }\n"
		<< "}\n";
	return output.good();
}

FrameCaptureStats CaptureFramebufferPpm(
	const FBO* fbo,
	const std::string& outputPath,
	bool replicateRedChannel = false)
{
	FrameCaptureStats stats;
	if (!fbo || fbo->framebufferID == 0 || fbo->width <= 0 || fbo->height <= 0) {
		return stats;
	}

	const int width = fbo->width;
	const int height = fbo->height;
	std::vector<unsigned char> pixels(
		static_cast<size_t>(width) * static_cast<size_t>(height) * 3u);
	std::vector<unsigned char> redPixels;
	if (replicateRedChannel) {
		redPixels.resize(
			static_cast<size_t>(width) *
			static_cast<size_t>(height));
	}
	GLenum pendingError = GL_NO_ERROR;
	for (GLenum error = glGetError(); error != GL_NO_ERROR; error = glGetError()) {
		pendingError = error;
	}
	if (pendingError != GL_NO_ERROR) {
		std::cerr << "[ClassicScene] cleared pre-capture OpenGL error 0x"
			<< std::hex << pendingError << std::dec << std::endl;
	}
	GLState::BindFramebuffer(GL_READ_FRAMEBUFFER, fbo->framebufferID);
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(
		0,
		0,
		width,
		height,
		replicateRedChannel ? GL_RED : GL_RGB,
		GL_UNSIGNED_BYTE,
		replicateRedChannel ? redPixels.data() : pixels.data());
	const GLenum readError = glGetError();
	GLState::BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	if (readError != GL_NO_ERROR) {
		std::cerr << "PBR capture glReadPixels failed with error 0x"
			<< std::hex << readError << std::dec << std::endl;
		return stats;
	}
	if (replicateRedChannel) {
		for (std::size_t index = 0; index < redPixels.size(); ++index) {
			pixels[index * 3u] = redPixels[index];
			pixels[index * 3u + 1u] = redPixels[index];
			pixels[index * 3u + 2u] = redPixels[index];
		}
	}

	std::error_code directoryError;
	const std::filesystem::path path(outputPath);
	if (path.has_parent_path()) {
		std::filesystem::create_directories(path.parent_path(), directoryError);
	}
	if (directoryError) {
		return stats;
	}
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	if (!output.is_open()) {
		return stats;
	}
	output << "P6\n" << width << ' ' << height << "\n255\n";
	const size_t rowBytes = static_cast<size_t>(width) * 3u;
	for (int row = height - 1; row >= 0; --row) {
		output.write(
			reinterpret_cast<const char*>(pixels.data() + static_cast<size_t>(row) * rowBytes),
			static_cast<std::streamsize>(rowBytes));
	}
	if (!output.good()) {
		return stats;
	}

	double luminanceSum = 0.0;
	std::uint64_t nonBlackPixels = 0;
	const std::uint64_t pixelCount = static_cast<std::uint64_t>(width) * height;
	for (size_t i = 0; i < pixels.size(); i += 3) {
		const double red = pixels[i] / 255.0;
		const double green = pixels[i + 1] / 255.0;
		const double blue = pixels[i + 2] / 255.0;
		const double luminance = 0.2126 * red + 0.7152 * green + 0.0722 * blue;
		luminanceSum += luminance;
		if (luminance > 0.01) {
			++nonBlackPixels;
		}
	}
	stats.meanLuminance = pixelCount != 0 ? luminanceSum / pixelCount : 0.0;
	stats.nonBlackRatio = pixelCount != 0
		? static_cast<double>(nonBlackPixels) / pixelCount
		: 0.0;
	stats.valid = stats.meanLuminance > 0.005 && stats.nonBlackRatio > 0.01;
	stats.pixels = std::move(pixels);
	return stats;
}

FloatCaptureStats CaptureFramebufferPfm(
	const FBO* fbo,
	int attachmentIndex,
	const std::string& outputPath,
	FloatCaptureSource source)
{
	FloatCaptureStats stats;
	if (!fbo ||
		fbo->framebufferID == 0 ||
		!fbo->IsComplete() ||
		fbo->width <= 0 ||
		fbo->height <= 0 ||
		attachmentIndex < 0 ||
		attachmentIndex >= static_cast<int>(fbo->textureIDs.size()) ||
		outputPath.empty()) {
		return stats;
	}

	const int width = fbo->width;
	const int height = fbo->height;
	const int outputChannels =
		source == FloatCaptureSource::RGB ? 3 : 1;
	const int readChannels =
		source == FloatCaptureSource::Alpha ? 4 : outputChannels;
	const GLenum readFormat =
		source == FloatCaptureSource::Red
			? GL_RED
			: (source == FloatCaptureSource::RGB ? GL_RGB : GL_RGBA);
	const std::size_t pixelCount =
		static_cast<std::size_t>(width) *
		static_cast<std::size_t>(height);
	std::vector<float> readPixels(
		pixelCount * static_cast<std::size_t>(readChannels));
	std::vector<float> outputPixels(
		pixelCount * static_cast<std::size_t>(outputChannels));

	for (GLenum error = glGetError();
		error != GL_NO_ERROR;
		error = glGetError()) {
	}
	GLState::BindFramebuffer(GL_READ_FRAMEBUFFER, fbo->framebufferID);
	glReadBuffer(GL_COLOR_ATTACHMENT0 + attachmentIndex);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(
		0,
		0,
		width,
		height,
		readFormat,
		GL_FLOAT,
		readPixels.data());
	const GLenum readError = glGetError();
	GLState::BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	if (readError != GL_NO_ERROR) {
		std::cerr << "[ClassicScene] PFM glReadPixels failed with error 0x"
			<< std::hex << readError << std::dec << std::endl;
		return stats;
	}

	if (source == FloatCaptureSource::Alpha) {
		for (std::size_t index = 0; index < pixelCount; ++index) {
			outputPixels[index] = readPixels[index * 4u + 3u];
		}
	}
	else {
		outputPixels = std::move(readPixels);
	}

	std::error_code directoryError;
	const std::filesystem::path path(outputPath);
	if (path.has_parent_path()) {
		std::filesystem::create_directories(path.parent_path(), directoryError);
	}
	if (directoryError) {
		return stats;
	}
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	if (!output.is_open()) {
		return stats;
	}
	output << (outputChannels == 3 ? "PF\n" : "Pf\n")
		<< width << ' ' << height << "\n-1.0\n";
	// OpenGL and PFM both store the bottom scanline first. The negative
	// scale records little-endian IEEE-754 data on the Windows benchmark host.
	output.write(
		reinterpret_cast<const char*>(outputPixels.data()),
		static_cast<std::streamsize>(
			outputPixels.size() * sizeof(float)));
	if (!output.good()) {
		return stats;
	}

	double sum = 0.0;
	double minimum = (std::numeric_limits<double>::max)();
	double maximum = (std::numeric_limits<double>::lowest)();
	std::uint64_t finiteCount = 0;
	std::uint64_t nonFiniteCount = 0;
	for (float value : outputPixels) {
		if (!std::isfinite(value)) {
			++nonFiniteCount;
			continue;
		}
		const double converted = static_cast<double>(value);
		minimum = (std::min)(minimum, converted);
		maximum = (std::max)(maximum, converted);
		sum += converted;
		++finiteCount;
	}
	stats.valid =
		finiteCount == outputPixels.size() &&
		nonFiniteCount == 0 &&
		output.good();
	stats.width = width;
	stats.height = height;
	stats.channels = outputChannels;
	stats.finiteValueCount = finiteCount;
	stats.nonFiniteValueCount = nonFiniteCount;
	stats.minimum = finiteCount > 0 ? minimum : 0.0;
	stats.maximum = finiteCount > 0 ? maximum : 0.0;
	stats.mean = finiteCount > 0
		? sum / static_cast<double>(finiteCount)
		: 0.0;
	return stats;
}

FrameCaptureStats CaptureFramebufferPpmRegion(
	const FBO* fbo,
	const std::string& outputPath,
	int x,
	int y,
	int width,
	int height)
{
	FrameCaptureStats stats;
	if (!fbo ||
		fbo->framebufferID == 0 ||
		!fbo->IsComplete() ||
		outputPath.empty() ||
		x < 0 || y < 0 || width <= 0 || height <= 0 ||
		x + width > fbo->width || y + height > fbo->height) {
		return stats;
	}

	std::vector<unsigned char> pixels(
		static_cast<std::size_t>(width) *
		static_cast<std::size_t>(height) * 3u);
	GLState::BindFramebuffer(GL_READ_FRAMEBUFFER, fbo->framebufferID);
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(
		x,
		fbo->height - y - height,
		width,
		height,
		GL_RGB,
		GL_UNSIGNED_BYTE,
		pixels.data());
	const GLenum readError = glGetError();
	GLState::BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	if (readError != GL_NO_ERROR) {
		std::cerr
			<< "[ClassicScene] ROI PPM glReadPixels failed with error 0x"
			<< std::hex << readError << std::dec << std::endl;
		return stats;
	}

	std::error_code directoryError;
	const std::filesystem::path path(outputPath);
	if (path.has_parent_path()) {
		std::filesystem::create_directories(
			path.parent_path(),
			directoryError);
	}
	if (directoryError) {
		return stats;
	}
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	if (!output.is_open()) {
		return stats;
	}
	output << "P6\n" << width << ' ' << height << "\n255\n";
	const std::size_t rowBytes = static_cast<std::size_t>(width) * 3u;
	for (int row = height - 1; row >= 0; --row) {
		output.write(
			reinterpret_cast<const char*>(
				pixels.data() + static_cast<std::size_t>(row) * rowBytes),
			static_cast<std::streamsize>(rowBytes));
	}
	if (!output.good()) {
		return stats;
	}

	double luminanceSum = 0.0;
	std::uint64_t nonBlackPixels = 0;
	for (std::size_t index = 0; index < pixels.size(); index += 3u) {
		const double luminance =
			0.2126 * pixels[index] / 255.0 +
			0.7152 * pixels[index + 1u] / 255.0 +
			0.0722 * pixels[index + 2u] / 255.0;
		luminanceSum += luminance;
		if (luminance > 0.01) {
			++nonBlackPixels;
		}
	}
	const std::uint64_t pixelCount =
		static_cast<std::uint64_t>(width) *
		static_cast<std::uint64_t>(height);
	stats.meanLuminance =
		pixelCount > 0 ? luminanceSum / pixelCount : 0.0;
	stats.nonBlackRatio =
		pixelCount > 0
			? static_cast<double>(nonBlackPixels) / pixelCount
			: 0.0;
	// A dark ROI is still a valid lossless capture. Content suitability is
	// evaluated later by the quality analyzer rather than by a brightness gate.
	stats.valid = true;
	return stats;
}

FloatCaptureStats CaptureFramebufferPfmRegion(
	const FBO* fbo,
	int attachmentIndex,
	const std::string& outputPath,
	FloatCaptureSource source,
	int x,
	int y,
	int width,
	int height)
{
	FloatCaptureStats stats;
	if (!fbo ||
		fbo->framebufferID == 0 ||
		!fbo->IsComplete() ||
		attachmentIndex < 0 ||
		attachmentIndex >= static_cast<int>(fbo->textureIDs.size()) ||
		outputPath.empty() ||
		x < 0 || y < 0 || width <= 0 || height <= 0 ||
		x + width > fbo->width || y + height > fbo->height) {
		return stats;
	}

	const int outputChannels =
		source == FloatCaptureSource::RGB ? 3 : 1;
	const int readChannels =
		source == FloatCaptureSource::Alpha ? 4 : outputChannels;
	const GLenum readFormat =
		source == FloatCaptureSource::Red
			? GL_RED
			: (source == FloatCaptureSource::RGB ? GL_RGB : GL_RGBA);
	const std::size_t pixelCount =
		static_cast<std::size_t>(width) *
		static_cast<std::size_t>(height);
	std::vector<float> readPixels(
		pixelCount * static_cast<std::size_t>(readChannels));
	std::vector<float> outputPixels(
		pixelCount * static_cast<std::size_t>(outputChannels));

	GLState::BindFramebuffer(GL_READ_FRAMEBUFFER, fbo->framebufferID);
	glReadBuffer(GL_COLOR_ATTACHMENT0 + attachmentIndex);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(
		x,
		fbo->height - y - height,
		width,
		height,
		readFormat,
		GL_FLOAT,
		readPixels.data());
	const GLenum readError = glGetError();
	GLState::BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	if (readError != GL_NO_ERROR) {
		std::cerr
			<< "[ClassicScene] ROI PFM glReadPixels failed with error 0x"
			<< std::hex << readError << std::dec << std::endl;
		return stats;
	}

	if (source == FloatCaptureSource::Alpha) {
		for (std::size_t index = 0; index < pixelCount; ++index) {
			outputPixels[index] = readPixels[index * 4u + 3u];
		}
	}
	else {
		outputPixels = std::move(readPixels);
	}

	std::error_code directoryError;
	const std::filesystem::path path(outputPath);
	if (path.has_parent_path()) {
		std::filesystem::create_directories(
			path.parent_path(),
			directoryError);
	}
	if (directoryError) {
		return stats;
	}
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	if (!output.is_open()) {
		return stats;
	}
	output << (outputChannels == 3 ? "PF\n" : "Pf\n")
		<< width << ' ' << height << "\n-1.0\n";
	output.write(
		reinterpret_cast<const char*>(outputPixels.data()),
		static_cast<std::streamsize>(
			outputPixels.size() * sizeof(float)));
	if (!output.good()) {
		return stats;
	}

	double sum = 0.0;
	double minimum = (std::numeric_limits<double>::max)();
	double maximum = (std::numeric_limits<double>::lowest)();
	std::uint64_t finiteCount = 0;
	std::uint64_t nonFiniteCount = 0;
	for (float value : outputPixels) {
		if (!std::isfinite(value)) {
			++nonFiniteCount;
			continue;
		}
		const double converted = static_cast<double>(value);
		minimum = (std::min)(minimum, converted);
		maximum = (std::max)(maximum, converted);
		sum += converted;
		++finiteCount;
	}
	stats.valid =
		finiteCount == outputPixels.size() &&
		nonFiniteCount == 0;
	stats.width = width;
	stats.height = height;
	stats.channels = outputChannels;
	stats.finiteValueCount = finiteCount;
	stats.nonFiniteValueCount = nonFiniteCount;
	stats.minimum = finiteCount > 0 ? minimum : 0.0;
	stats.maximum = finiteCount > 0 ? maximum : 0.0;
	stats.mean =
		finiteCount > 0
			? sum / static_cast<double>(finiteCount)
			: 0.0;
	return stats;
}


void ProcessInput(GLFWwindow* window) {
	if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
		glfwSetWindowShouldClose(window, true);
	}
	const bool keyboardCaptured =
		ImGui::GetCurrentContext() != nullptr &&
		ImGui::GetIO().WantCaptureKeyboard;
	const bool currentMkeyState =
		!keyboardCaptured &&
		glfwGetKey(window, GLFW_KEY_M) == GLFW_PRESS;
	if (!keyboardCaptured &&
		glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) {
		camera.UpdatePositionByDelta(timer.GetDeltaTime() *camera.cameraSpeed * camera.cameraFront);
	}
	if (!keyboardCaptured &&
		glfwGetKey(window,GLFW_KEY_S) == GLFW_PRESS){
		camera.UpdatePositionByDelta(-timer.GetDeltaTime() * camera.cameraSpeed * camera.cameraFront);
	}
	if (!keyboardCaptured &&
		glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {
		camera.UpdatePositionByDelta(timer.GetDeltaTime() * camera.cameraSpeed * glm::cross(camera.cameraFront, camera.up));
	}
	if (!keyboardCaptured &&
		glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) {
		camera.UpdatePositionByDelta(-timer.GetDeltaTime() * camera.cameraSpeed * glm::cross(camera.cameraFront, camera.up));
	}

	if (currentMkeyState && !lastFrameMkeyState) {
		if (firstMouse) {
			glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
			firstMouse = false;
		}
		else {
			glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
			firstMouse = true;
		}
	}
	lastFrameMkeyState = currentMkeyState;
}

void framebuffer_size_callback(GLFWwindow* window, int width, int height) {
	if (width <= 0 || height <= 0) {
		return;
	}

	glViewport(0, 0, width, height);
	properties.SCREEN_HEIGHT = height;
	properties.SCREEN_WIDTH = width;

	auto& fBuffersMgr = FramebuffersManager::GetInstance();
	fBuffersMgr.Resize();
}

void mouse_callback(GLFWwindow* window, double xpos, double ypos) {
	if(!firstMouse)
		camera.CameraMouseCallback(xpos, ypos);
	else {
		camera.SetLastPos(xpos, ypos);
	}
}

void scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
	camera.CameraSrollCallback(xoffset, yoffset);
}

void SetGui() {
	MyGui& mygui = MyGui::GetInstance();
	mygui.NewFrame();
	ImGui::SetNextWindowSize(ImVec2(400, 300));
}

void SetUniformBuffer() {
	view = camera.GetViewMatrix();
	const float aspectRatio = static_cast<float>(properties.SCREEN_WIDTH) /
		static_cast<float>((std::max)(1, properties.SCREEN_HEIGHT));
	projection = camera.GetProjectionMatrix(aspectRatio);
	ShaderManager& ShaderMgr = ShaderManager::GetInstance();
	ShaderMgr.SetUBOData(ShaderManager::Matrices, 0, sizeof(glm::mat4), &view);
	ShaderMgr.SetUBOData(ShaderManager::Matrices, sizeof(glm::mat4), sizeof(glm::mat4), &projection);
	ShaderMgr.UpdateSystemUBO();
}

int main(int argc, char** argv) {
	const auto applicationStart = PerformanceBenchmarkSession::Clock::now();
	bool resourceSmokeTest = false;
	bool pbrSmokeTest = false;
	bool pbrSmokeFailed = false;
	bool benchmarkPhongMaterialScene = false;
	bool benchmarkPbrMaterialScene = false;
	bool benchmarkUnsharedImportedMaterials = false;
	ClassicSceneTestOptions classicSceneOptions;
	std::string classicSceneOptionError;
	if (!ParseClassicSceneTestOptions(
		argc,
		argv,
		classicSceneOptions,
		classicSceneOptionError)) {
		std::cerr << "Classic scene option error: "
			<< classicSceneOptionError << std::endl;
		return 4;
	}
	PerformanceBenchmarkOptions benchmarkOptions;
	std::string benchmarkOptionError;
	if (!ParsePerformanceBenchmarkOptions(argc, argv, benchmarkOptions, benchmarkOptionError)) {
		std::cerr << "Performance benchmark option error: " << benchmarkOptionError << std::endl;
		return 4;
	}
	SubmissionStressOptions submissionStressOptions;
	std::string submissionStressOptionError;
	if (!ParseSubmissionStressOptions(
		argc,
		argv,
		submissionStressOptions,
		submissionStressOptionError)) {
		std::cerr << "Submission stress option error: "
			<< submissionStressOptionError << std::endl;
		return 4;
	}
	for (int i = 1; i < argc; ++i) {
		if (std::string(argv[i]) == "--resource-smoke-test") {
			resourceSmokeTest = true;
		}
		else if (std::string(argv[i]) == "--pbr-smoke-test") {
			pbrSmokeTest = true;
		}
		else if (std::string(argv[i]) == "--benchmark-phong-material-scene") {
			benchmarkPhongMaterialScene = true;
		}
		else if (std::string(argv[i]) == "--benchmark-pbr-material-scene") {
			benchmarkPbrMaterialScene = true;
		}
		else if (std::string(argv[i]) == "--benchmark-unshared-imported-materials") {
			benchmarkUnsharedImportedMaterials = true;
		}
	}
	if ((resourceSmokeTest && benchmarkOptions.enabled) ||
		(pbrSmokeTest && (resourceSmokeTest || benchmarkOptions.enabled)) ||
		(benchmarkPhongMaterialScene && benchmarkPbrMaterialScene) ||
		((benchmarkPhongMaterialScene || benchmarkPbrMaterialScene) && !benchmarkOptions.enabled) ||
		(benchmarkUnsharedImportedMaterials &&
			(!benchmarkOptions.enabled || !benchmarkPbrMaterialScene)) ||
		(submissionStressOptions.enabled &&
			(resourceSmokeTest ||
				pbrSmokeTest ||
				benchmarkPhongMaterialScene ||
				benchmarkPbrMaterialScene ||
				classicSceneOptions.enabled)) ||
		(classicSceneOptions.enabled &&
			(resourceSmokeTest || pbrSmokeTest || benchmarkOptions.enabled ||
				benchmarkPhongMaterialScene || benchmarkPbrMaterialScene))) {
		std::cerr
			<< "automated smoke and fixed-scene modes are mutually exclusive"
			<< std::endl;
		return 4;
	}
	const bool automatedValidation =
		benchmarkOptions.enabled ||
		resourceSmokeTest ||
		pbrSmokeTest ||
		classicSceneOptions.enabled;
	if (submissionStressOptions.enabled) {
		properties.SCREEN_WIDTH = submissionStressOptions.width;
		properties.SCREEN_HEIGHT = submissionStressOptions.height;
	}
	else if (classicSceneOptions.enabled) {
		properties.SCREEN_WIDTH = classicSceneOptions.width;
		properties.SCREEN_HEIGHT = classicSceneOptions.height;
	}
#ifdef _WIN32
	RENDERDOC_API_1_6_0* renderDocApi = nullptr;
	if (classicSceneOptions.renderDocCaptureFrame > 0) {
		std::error_code capturePathError;
		const std::filesystem::path captureTemplatePath =
			std::filesystem::absolute(
				std::filesystem::path(
					classicSceneOptions.renderDocCaptureTemplate),
				capturePathError);
		if (capturePathError) {
			std::cerr
				<< "[RenderDocCapture] failed to resolve capture template: "
				<< capturePathError.message()
				<< std::endl;
			return 8;
		}
		std::filesystem::create_directories(
			captureTemplatePath.parent_path(),
			capturePathError);
		if (capturePathError) {
			std::cerr
				<< "[RenderDocCapture] failed to create capture directory: "
				<< capturePathError.message()
				<< std::endl;
			return 8;
		}
		classicSceneOptions.renderDocCaptureTemplate =
			captureTemplatePath.string();

		const HMODULE renderDocModule =
			GetModuleHandleW(L"renderdoc.dll");
		const pRENDERDOC_GetAPI getRenderDocApi =
			renderDocModule
				? reinterpret_cast<pRENDERDOC_GetAPI>(
					GetProcAddress(
						renderDocModule,
						"RENDERDOC_GetAPI"))
				: nullptr;
		void* apiPointer = nullptr;
		if (!getRenderDocApi ||
			getRenderDocApi(
				eRENDERDOC_API_Version_1_6_0,
				&apiPointer) != 1 ||
			!apiPointer) {
			std::cerr
				<< "[RenderDocCapture] RenderDoc 1.6 API is unavailable; "
				<< "launch this executable through renderdoccmd capture"
				<< std::endl;
			return 8;
		}
		renderDocApi =
			static_cast<RENDERDOC_API_1_6_0*>(apiPointer);
		renderDocApi->SetCaptureFilePathTemplate(
			classicSceneOptions.renderDocCaptureTemplate.c_str());
		int apiMajor = 0;
		int apiMinor = 0;
		int apiPatch = 0;
		renderDocApi->GetAPIVersion(
			&apiMajor,
			&apiMinor,
			&apiPatch);
		std::cout
			<< "[RenderDocCapture] api="
			<< apiMajor << "." << apiMinor << "." << apiPatch
			<< " frame="
			<< classicSceneOptions.renderDocCaptureFrame
			<< " template="
			<< renderDocApi->GetCaptureFilePathTemplate()
			<< std::endl;
	}
#else
	if (classicSceneOptions.renderDocCaptureFrame > 0) {
		std::cerr
			<< "[RenderDocCapture] in-application capture is only supported "
			<< "by this diagnostic entry on Windows"
			<< std::endl;
		return 8;
	}
#endif
	glfwInit();
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
	glfwWindowHint(GLFW_SAMPLES, 4);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	if (classicSceneOptions.enabled) {
		glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
	}
	

	GLFWwindow* window = glfwCreateWindow(properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT, "Learn OpenGL", NULL, NULL);
	if (!window) {
		std::cout << "Fail to create a window" << std::endl;
		glfwTerminate();		
		return -1;
	}
	glfwMakeContextCurrent(window);
	if (benchmarkOptions.enabled || classicSceneOptions.enabled) {
		// Make the benchmark request explicit. GPU timestamp zones remain the
		// authoritative metric if a driver-level frame limiter is still active.
		glfwSwapInterval(0);
	}
	else {
		glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
	}
	//register function after initializing window and before renderering
	glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
	if (!benchmarkOptions.enabled && !classicSceneOptions.enabled) {
		glfwSetCursorPosCallback(window, mouse_callback);
		glfwSetScrollCallback(window, scroll_callback);
	}
	if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
		std::cout << "Fail to initialize GLAD" << std::endl;
		return -1;
	}
	glfwGetFramebufferSize(window, &properties.SCREEN_WIDTH, &properties.SCREEN_HEIGHT);
	glViewport(0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT);
	PerformanceProfiler::GetInstance().Initialize();
	GLState::Initialize();

	InitVAOs();

	MyGui& mygui = MyGui::GetInstance();
	mygui.Init(window);
	if (automatedValidation) {
		// Automated runs must not modify the user's editor layout or depend on
		// ImGui's periodic ini writes while checks are being collected.
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard;
	}

	ShaderManager& shaderManager = ShaderManager::GetInstance();
	shaderManager.Init();
	if (automatedValidation) {
		bool invalidShaderFound = false;
		for (const std::string& shaderName : shaderManager.GetNames()) {
			auto shader = shaderManager.GetShaderByName(shaderName);
			if (!shader || shader->ID == 0) {
				std::cerr << "Automated validation: invalid shader '"
					<< shaderName << "'" << std::endl;
				invalidShaderFound = true;
			}
		}
		const auto shadowCubeShader =
			shaderManager.GetShader(ShaderManager::ShadowCube);
		if (!shadowCubeShader ||
			!shadowCubeShader->IsGeometryShader()) {
			std::cerr
				<< "Automated validation: shadowCube must include "
				<< "its geometry stage"
				<< std::endl;
			invalidShaderFound = true;
		}
		const auto shadowCubeFaceShader =
			shaderManager.GetShader(ShaderManager::ShadowCubeFace);
		if (!shadowCubeFaceShader ||
			shadowCubeFaceShader->ID == 0 ||
			shadowCubeFaceShader->IsGeometryShader()) {
			std::cerr
				<< "Automated validation: shadowCubeFace must be a "
				<< "linked vertex/fragment program"
				<< std::endl;
			invalidShaderFound = true;
		}
		if (invalidShaderFound) {
			return 5;
		}
	}
	Shader& debugShader = *(shaderManager.GetShader(ShaderManager::DebugScene));

#ifdef USE_GEOMETRY_SHADER
	GeometryShader geometryShader("shaders/geometryVertex.vs", "shaders/geometryGeometry.gs", "shaders/geometryFragment.fs");
	float points[] = {
	-0.5f,  0.5f, 1.0f, 0.0f, 0.0f, // ????
	 0.5f,  0.5f, 0.0f, 1.0f, 0.0f, // ????
	 0.5f, -0.5f, 0.0f, 0.0f, 1.0f, // ????
	-0.5f, -0.5f, 1.0f, 1.0f, 0.0f  // ????
	};
	unsigned int VAO, VBO;
	glGenVertexArrays(1, &VAO);
	glGenBuffers(1, &VBO);
	GLState::BindVertexArray(VAO);
	glBindBuffer(GL_ARRAY_BUFFER, VBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(points), points, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(2 * sizeof(float)));
	glEnableVertexAttribArray(1);
#elif defined(USE_PLANET_SHADER)
	Planet planet;
	planet.Init();
#endif
	xmlMaterialManager.LoadFromFile("materials.xml");
	Scene scene(&camera, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT);
	const std::string sceneStatePath = "saved/last_scene.json";
	EditorSceneManager editorSceneManager;
	if (!automatedValidation) {
		editorSceneManager.Initialize(
			"classic-scenes.manifest.json",
			sceneStatePath);
	}
	SubmissionStressSceneState submissionStressState;
	bool classicSceneFailed = false;
	bool classicSceneCaptured = false;
	double classicSceneLoadMilliseconds = 0.0;
	FrameTimingStats classicSceneFrameTiming;
	std::size_t classicSceneMeshCount = 0;
	std::uint64_t classicSceneVertexCount = 0;
	std::uint64_t classicSceneTriangleCount = 0;
	glm::vec3 classicSceneSourceCenter(0.0f);
	float classicSceneSourceRadius = 0.0f;
	float classicSceneAppliedScale = 1.0f;
	std::shared_ptr<Material> classicSceneOverrideMaterial;
	std::shared_ptr<Model> classicSceneModel;
	std::shared_ptr<Material> classicSceneMotionCasterMaterial;
	std::shared_ptr<Model> classicSceneMotionCaster;
	std::shared_ptr<Model> classicSceneDeferredReceiver;
	std::shared_ptr<Model> classicSceneReplacementCaster;
	const bool useBuiltInMaterialScene =
		pbrSmokeTest || benchmarkPhongMaterialScene || benchmarkPbrMaterialScene;
	if (submissionStressOptions.enabled) {
		std::string buildError;
		if (!BuildSubmissionStressScene(
			scene,
			camera,
			submissionStressOptions,
			submissionStressState,
			buildError)) {
			std::cerr << "[SubmissionStress] " << buildError << std::endl;
			return 6;
		}
		std::cout << "[SubmissionStress] "
			<< DescribeSubmissionStressScene(
				submissionStressOptions,
				submissionStressState)
			<< " resolution="
			<< submissionStressOptions.width << 'x'
			<< submissionStressOptions.height
			<< " cameraDistance=" << std::fixed << std::setprecision(2)
			<< submissionStressState.cameraDistance
			<< std::endl;
	}
	else if (classicSceneOptions.enabled) {
		const float worldScale = classicSceneOptions.worldScale;
		classicSceneOptions.cameraPosition *= worldScale;
		classicSceneOptions.cameraTarget *= worldScale;
		classicSceneOptions.normalizedRadius *= worldScale;
		classicSceneOptions.pointLightPosition *= worldScale;
		if (classicSceneOptions.hasSpotLightPosition) {
			classicSceneOptions.spotLightPosition *= worldScale;
		}
		if (classicSceneOptions.hasSpotShadowNearPlane) {
			classicSceneOptions.spotShadowNearPlane *= worldScale;
			classicSceneOptions.spotShadowFarPlane *= worldScale;
		}
		LoadDefaultLights(scene);
		for (auto& light : scene.lightSource.pointLights) {
			light.SetPosition(classicSceneOptions.pointLightPosition);
			light.SetScale(0.2f * worldScale);
			light.linear /= worldScale;
			light.quadratic /= worldScale * worldScale;
		}
		if (classicSceneOptions.shadowExperiment) {
			const bool enableShadows = classicSceneOptions.shadowMode != "off";
			const bool useDirectional =
				classicSceneOptions.shadowLights == "directional" ||
				classicSceneOptions.shadowLights == "all";
			const bool usePoint =
				classicSceneOptions.shadowLights == "point" ||
				classicSceneOptions.shadowLights == "all";
			const bool useSpot =
				classicSceneOptions.shadowLights == "spot" ||
				classicSceneOptions.shadowLights == "all";
			if (classicSceneOptions.shadowMode == "pcf") {
				properties.SHADOW_TYPE = ShadowProperty::PCF;
			}
			else if (classicSceneOptions.shadowMode == "pcss") {
				properties.SHADOW_TYPE = ShadowProperty::PCSS;
			}
			else {
				properties.SHADOW_TYPE = ShadowProperty::Default;
			}
			properties.SHADOW_PCF_SAMPLE_NUM = 16;
			properties.SHADOW_PCF_RING_NUM = 8;
			properties.SHADOW_SAMPLING_PATTERN =
				classicSceneOptions.shadowSampling == "legacy"
					? ShadowProperty::LegacyRandom
					: ShadowProperty::StableVogel;

			for (auto& light : scene.lightSource.directionLights) {
				light.m_active = useDirectional;
				light.direction = glm::normalize(
					classicSceneOptions.directionalLightDirection);
				light.ambient = glm::vec3(0.02f);
				light.diffuse = glm::vec3(2.0f);
				light.specular = glm::vec3(1.0f);
				light.autoFitShadow = true;
				light.shadowResolution =
					classicSceneOptions.shadowResolution > 0
						? classicSceneOptions.shadowResolution
						: 2048;
				light.useShadowMap = enableShadows && useDirectional;
			}
			for (auto& light : scene.lightSource.pointLights) {
				light.m_active = usePoint;
				light.ambient = glm::vec3(0.0f);
				light.diffuse = glm::vec3(4.0f);
				light.specular = glm::vec3(1.0f);
				light.autoFitShadow = true;
				light.shadowResolution =
					classicSceneOptions.shadowResolution > 0
						? classicSceneOptions.shadowResolution
						: 1024;
				light.useShadowMap = enableShadows && usePoint;
			}
			if (useSpot) {
				const glm::vec3 spotPosition =
					classicSceneOptions.hasSpotLightPosition
						? classicSceneOptions.spotLightPosition
						: classicSceneOptions.cameraPosition;
				const glm::vec3 spotDirection =
					classicSceneOptions.hasSpotLightDirection
						? glm::normalize(
							classicSceneOptions.spotLightDirection)
						: glm::normalize(
							classicSceneOptions.cameraTarget -
							classicSceneOptions.cameraPosition);
				SpotLight spotLight(
					spotPosition,
					spotDirection,
					glm::vec3(0.0f),
					glm::vec3(3.0f),
					glm::vec3(1.0f),
					25.0f,
					35.0f);
				spotLight.autoFitShadow = true;
				if (classicSceneOptions.hasSpotShadowNearPlane) {
					spotLight.autoFitShadow = false;
					spotLight.near_plane =
						classicSceneOptions.spotShadowNearPlane;
					spotLight.far_plane =
						classicSceneOptions.spotShadowFarPlane;
				}
				spotLight.shadowResolution =
					classicSceneOptions.shadowResolution > 0
						? classicSceneOptions.shadowResolution
						: 1024;
				spotLight.linear /= worldScale;
				spotLight.quadratic /= worldScale * worldScale;
				spotLight.useShadowMap = enableShadows && useSpot;
				scene.lightSource.AddSpotLight(spotLight);
			}
		}
		const auto loadStart = PerformanceBenchmarkSession::Clock::now();
		if (classicSceneOptions.untextured) {
			classicSceneOverrideMaterial = std::make_shared<Material>("pbr");
			classicSceneOverrideMaterial->AddProperty(
				"albedo",
				MaterialProperty::CreateColor(glm::vec3(0.65f, 0.68f, 0.72f)));
			classicSceneOverrideMaterial->AddProperty(
				"metallic",
				MaterialProperty::CreateFloat(0.0f, 0.0f, 1.0f, 0.01f));
			classicSceneOverrideMaterial->AddProperty(
				"roughness",
				MaterialProperty::CreateFloat(0.65f, 0.04f, 1.0f, 0.01f));
			classicSceneOverrideMaterial->AddProperty(
				"ao",
				MaterialProperty::CreateFloat(1.0f, 0.0f, 1.0f, 0.01f));
			classicSceneOverrideMaterial->AddProperty(
				"emissive",
				MaterialProperty::CreateColor(glm::vec3(0.0f)));
			classicSceneOverrideMaterial->AddProperty(
				"opacity",
				MaterialProperty::CreateFloat(1.0f));
			classicSceneOverrideMaterial->AddProperty(
				"useAlphaCutoff",
				MaterialProperty::CreateBool(false));
			classicSceneOverrideMaterial->AddProperty(
				"alphaCutoff",
				MaterialProperty::CreateFloat(0.0f, 0.0f, 1.0f, 0.01f));
			classicSceneOverrideMaterial->AddProperty(
				"useBloom",
				MaterialProperty::CreateBool(false));
			classicSceneModel = std::make_shared<Model>(
				classicSceneOptions.modelPath,
				classicSceneOverrideMaterial.get());
		}
		else {
			const bool usePhong =
				classicSceneOptions.renderPath.find("phong-") == 0;
			classicSceneModel = std::make_shared<Model>(
				classicSceneOptions.modelPath,
				shaderManager.GetShader(
					usePhong ? ShaderManager::Phong : ShaderManager::Pbr));
		}
		classicSceneLoadMilliseconds = std::chrono::duration<double, std::milli>(
			PerformanceBenchmarkSession::Clock::now() - loadStart).count();
		classicSceneModel->SetName(classicSceneOptions.sceneName);
		classicSceneSourceCenter = classicSceneModel->GetLoacalCenter();
		classicSceneSourceRadius = classicSceneModel->GetLocalBoundingRadius();
		classicSceneMeshCount = classicSceneModel->GetMeshes().size();
		for (const Mesh& mesh : classicSceneModel->GetMeshes()) {
			classicSceneVertexCount += mesh.GetVertexCount();
			classicSceneTriangleCount += mesh.UsesIndices()
				? mesh.GetIndexCount() / 3u
				: mesh.GetVertexCount() / 3u;
		}
		if (classicSceneMeshCount == 0 || classicSceneSourceRadius <= 0.0001f) {
			classicSceneFailed = true;
			std::cerr << "[ClassicScene] failed to load usable geometry from "
				<< classicSceneOptions.modelPath << std::endl;
		}
		else {
			classicSceneAppliedScale =
				classicSceneOptions.normalizedRadius / classicSceneSourceRadius;
			classicSceneModel->SetScale(classicSceneAppliedScale);
			classicSceneModel->SetPosition(
				-classicSceneSourceCenter * classicSceneAppliedScale);
		}
		scene.modelSource.AddModel(classicSceneModel);
		const bool needsLocalMotionCaster =
			classicSceneOptions.shadowWorkload ==
				"timeline-cache-3way" ||
			classicSceneOptions.shadowWorkload ==
				"move-local-caster" ||
			classicSceneOptions.shadowWorkload ==
				"deferred-face-required" ||
			classicSceneOptions.shadowWorkload ==
				"replace-model-aba";
		if (needsLocalMotionCaster) {
			classicSceneMotionCasterMaterial =
				std::make_shared<Material>("pbr");
			classicSceneMotionCasterMaterial->AddProperty(
				"albedo",
				MaterialProperty::CreateColor(
					glm::vec3(0.86f, 0.22f, 0.12f)));
			classicSceneMotionCasterMaterial->AddProperty(
				"metallic",
				MaterialProperty::CreateFloat(
					0.0f, 0.0f, 1.0f, 0.01f));
			classicSceneMotionCasterMaterial->AddProperty(
				"roughness",
				MaterialProperty::CreateFloat(
					0.45f, 0.04f, 1.0f, 0.01f));
			classicSceneMotionCasterMaterial->AddProperty(
				"ao",
				MaterialProperty::CreateFloat(
					1.0f, 0.0f, 1.0f, 0.01f));
			classicSceneMotionCasterMaterial->AddProperty(
				"opacity",
				MaterialProperty::CreateFloat(1.0f));
			classicSceneMotionCasterMaterial->AddProperty(
				"useAlphaCutoff",
				MaterialProperty::CreateBool(false));
			classicSceneMotionCaster =
				std::make_shared<Model>(
					"models/sphere/sphere.obj",
					classicSceneMotionCasterMaterial.get());
			classicSceneMotionCaster->SetName(
				"Shadow Cache Motion Caster");
			const float casterSourceRadius =
				(std::max)(
					0.0001f,
					classicSceneMotionCaster
						->GetLocalBoundingRadius());
			const float casterRadius =
				classicSceneOptions.normalizedRadius * 0.02f;
			classicSceneMotionCaster->SetScale(
				casterRadius / casterSourceRadius);
			classicSceneMotionCaster->SetPosition(
				classicSceneOptions.pointLightPosition +
				classicSceneOptions.normalizedRadius *
					glm::vec3(0.12f, 0.02f, 0.0f));
			if (classicSceneOptions.shadowWorkload ==
				"deferred-face-required") {
				classicSceneMotionCaster->SetName(
					"Deferred -X Shadow Caster");
				classicSceneMotionCaster->SetPosition(
					classicSceneOptions.pointLightPosition +
						classicSceneOptions.normalizedRadius *
							glm::vec3(-0.12f, 0.0f, 0.0f));
			}
			scene.modelSource.AddModel(classicSceneMotionCaster);
			if (classicSceneOptions.shadowWorkload ==
				"deferred-face-required") {
				classicSceneModel->SetActiveStatus(false);
				classicSceneDeferredReceiver =
					std::make_shared<Model>(
						"models/sphere/sphere.obj",
						classicSceneMotionCasterMaterial.get());
				classicSceneDeferredReceiver->SetName(
					"Required +X Shadow Receiver");
				classicSceneDeferredReceiver->SetScale(
					casterRadius / casterSourceRadius);
				classicSceneDeferredReceiver->SetPosition(
					classicSceneOptions.pointLightPosition +
						classicSceneOptions.normalizedRadius *
							glm::vec3(0.12f, 0.0f, 0.0f));
				scene.modelSource.AddModel(
					classicSceneDeferredReceiver);
			}
			else if (classicSceneOptions.shadowWorkload ==
				"replace-model-aba") {
				classicSceneMotionCaster->SetName(
					"Topology ABA Caster");
				classicSceneReplacementCaster =
					std::make_shared<Model>(
						"models/sphere/sphere.obj",
						classicSceneMotionCasterMaterial.get());
				classicSceneReplacementCaster->SetName(
					classicSceneMotionCaster->GetName());
				classicSceneReplacementCaster->SetScale(
					classicSceneMotionCaster->scale);
				classicSceneReplacementCaster->SetPosition(
					classicSceneMotionCaster->position);
				classicSceneReplacementCaster->SetRotation(
					classicSceneMotionCaster->rotation);
			}
		}

		camera.cameraPos = classicSceneOptions.cameraPosition;
		camera.cameraFront = glm::normalize(
			classicSceneOptions.cameraTarget - classicSceneOptions.cameraPosition);
		camera.up = glm::normalize(classicSceneOptions.cameraUp);
		camera.fov = classicSceneOptions.fov;
		properties.DEFER_RENDERING =
			classicSceneOptions.renderPath.find("deferred") !=
			std::string::npos;
		properties.SSAO =
			classicSceneOptions.ssaoExperiment &&
			classicSceneOptions.ssaoSamples > 0;
		properties.SSAO_KERNEL_SIZE =
			properties.SSAO
				? classicSceneOptions.ssaoSamples
				: 64;
		if (classicSceneOptions.ssaoMode == "half-raw") {
			properties.SSAO_MODE = SSAOProperty::HalfRaw;
		}
		else if (classicSceneOptions.ssaoMode == "half-bilateral") {
			properties.SSAO_MODE = SSAOProperty::HalfBilateral;
		}
		else {
			properties.SSAO_MODE = SSAOProperty::LegacyFull;
		}
		if (classicSceneOptions.ssaoExperiment) {
			properties.SSAO_RADIUS = 0.35f;
			properties.SSAO_BIAS = 0.025f;
			properties.SSAO_BILATERAL_DEPTH_SIGMA = 0.02f;
			properties.SSAO_BILATERAL_NORMAL_POWER = 32.0f;
		}
		properties.LIGHT_VOLUME =
			classicSceneOptions.renderPath == "phong-deferred-volume";
		properties.BLOOM = false;
		properties.GAMMA_CORRECTION = true;
		properties.DEBUG_MODE = false;
		properties.AUTO_RELOAD_SHADERS = false;
		properties.AUTO_RELOAD_MATERIALS = false;
		if (classicSceneOptions.ssaoExperiment) {
			for (auto& light : scene.lightSource.pointLights) {
				light.useShadowMap = false;
			}
			for (auto& light : scene.lightSource.directionLights) {
				light.useShadowMap = false;
			}
			for (auto& light : scene.lightSource.spotLights) {
				light.useShadowMap = false;
			}
		}
		std::cout << "[ClassicScene] scene=" << classicSceneOptions.sceneName
			<< " model=" << classicSceneOptions.modelPath
			<< " loadMs=" << std::fixed << std::setprecision(2)
			<< classicSceneLoadMilliseconds
			<< " meshes=" << classicSceneMeshCount
			<< " vertices=" << classicSceneVertexCount
			<< " triangles=" << classicSceneTriangleCount
			<< " sourceRadius=" << classicSceneSourceRadius
			<< " appliedScale=" << classicSceneAppliedScale
			<< " ssaoSamples="
			<< (properties.SSAO
				? properties.SSAO_KERNEL_SIZE
				: 0)
			<< " ssaoMode="
			<< SSAOProperty::ModeName(properties.SSAO_MODE)
			<< std::endl;
	}
	else if (useBuiltInMaterialScene) {
		Model::SetImportedMaterialSharingEnabled(!benchmarkUnsharedImportedMaterials);
		LoadDefaultLights(scene);
		const bool usePbrMaterial = pbrSmokeTest || benchmarkPbrMaterialScene;
		auto validationModel = std::make_shared<Model>(
			"models/backpack/backpack.obj",
			shaderManager.GetShader(
				usePbrMaterial ? ShaderManager::Pbr : ShaderManager::Phong));
		validationModel->SetName("pbr-backpack-validation");
		validationModel->SetPosition(glm::vec3(0.0f));
		validationModel->SetScale(1.0f);
		scene.modelSource.AddModel(validationModel);
		camera.cameraPos = glm::vec3(0.0f, 0.0f, 5.0f);
		camera.cameraFront = glm::vec3(0.0f, 0.0f, -1.0f);
		camera.up = glm::vec3(0.0f, 1.0f, 0.0f);
		properties.DEFER_RENDERING = false;
		properties.SSAO = false;
		properties.LIGHT_VOLUME = false;
		properties.BLOOM = false;
		properties.GAMMA_CORRECTION = true;

		if (pbrSmokeTest) {
			bool hasPbrMaterial = false;
			bool hasAlbedo = false;
			bool hasNormal = false;
			bool hasRoughness = false;
			bool hasAo = false;
			bool hasMetallicFactor = false;
			bool hasEmissiveFactor = false;
			for (const Mesh& mesh : validationModel->GetMeshes()) {
				if (!mesh.material_ptr || mesh.material_ptr->GetShaderName() != "pbr") {
					continue;
				}
				hasPbrMaterial = true;
				const auto& materialProperties = mesh.material_ptr->GetProperties();
				auto hasTexture = [&](const char* name) {
					const auto it = materialProperties.find(name);
					return it != materialProperties.end() &&
						it->second.type == MaterialPropertyType::Texture &&
						!it->second.textures.empty() &&
						it->second.textures.front().textureID != 0;
				};
				hasAlbedo = hasAlbedo || hasTexture("texture_diffuse");
				hasNormal = hasNormal || hasTexture("texture_normal");
				hasRoughness = hasRoughness || hasTexture("texture_roughness");
				hasAo = hasAo || hasTexture("texture_ao");
				const auto metallic = materialProperties.find("metallic");
				hasMetallicFactor = hasMetallicFactor ||
					(metallic != materialProperties.end() &&
						metallic->second.type == MaterialPropertyType::Float);
				const auto emissive = materialProperties.find("emissive");
				hasEmissiveFactor = hasEmissiveFactor ||
					(emissive != materialProperties.end() &&
						(emissive->second.type == MaterialPropertyType::Color ||
							emissive->second.type == MaterialPropertyType::Vec3));
			}
			pbrSmokeFailed = !(
				hasPbrMaterial &&
				hasAlbedo &&
				hasNormal &&
				hasRoughness &&
				hasAo &&
				hasMetallicFactor &&
				hasEmissiveFactor);
			std::cout << "[PBRSmoke] material=" << hasPbrMaterial
				<< " albedo=" << hasAlbedo
				<< " normal=" << hasNormal
				<< " roughness=" << hasRoughness
				<< " ao=" << hasAo
				<< " metallicFactor=" << hasMetallicFactor
				<< " emissiveFactor=" << hasEmissiveFactor << std::endl;
		}
	}
	else if (SceneStateIO::Exists(sceneStatePath)) {
		// 有存档：只初始化默认灯光占位，其它由 SceneStateIO 恢复，避免默认模型+存档模型双加载。
		LoadDefaultLights(scene);
		const bool loaded = SceneStateIO::LoadAsync(scene, camera, sceneStatePath);
		// 兼容兜底：若存档损坏/旧格式导致没有任何模型，则回退到默认场景，避免“模型全没了”。
		if (!loaded) {
			scene.lightSource.pointLights.clear();
			scene.lightSource.directionLights.clear();
			scene.lightSource.spotLights.clear();
			LoadModels(scene);
		}
		// 额外兜底：某些旧/异常存档可能把 lights 写成空数组，导致场景几乎全黑。
		if (scene.lightSource.pointLights.empty() &&
			scene.lightSource.directionLights.empty() &&
			scene.lightSource.spotLights.empty()) {
			LoadDefaultLights(scene);
		}
	}
	else {
		// 无存档：走默认场景。
		LoadModels(scene);
	}
	if (resourceSmokeTest) {
		// Use one known light of every supported shadow type so the lifetime
		// smoke test exercises 2D directional, cubemap point, and 2D spot maps.
		scene.lightSource.pointLights.clear();
		scene.lightSource.directionLights.clear();
		scene.lightSource.spotLights.clear();
		LoadDefaultLights(scene);
		scene.lightSource.AddSpotLight(SpotLight(
			glm::vec3(0.0f, 8.0f, 4.0f),
			glm::normalize(glm::vec3(0.0f, -1.0f, -0.4f)),
			glm::vec3(0.0f),
			glm::vec3(1.0f),
			glm::vec3(1.0f),
			20.0f,
			30.0f));
	}
	CubeTexture skybox("materials/skybox");
	float skyboxVertices[] = {
		// positions          
		-1.0f,  1.0f, -1.0f,
		-1.0f, -1.0f, -1.0f,
		 1.0f, -1.0f, -1.0f,
		 1.0f, -1.0f, -1.0f,
		 1.0f,  1.0f, -1.0f,
		-1.0f,  1.0f, -1.0f,

		-1.0f, -1.0f,  1.0f,
		-1.0f, -1.0f, -1.0f,
		-1.0f,  1.0f, -1.0f,
		-1.0f,  1.0f, -1.0f,
		-1.0f,  1.0f,  1.0f,
		-1.0f, -1.0f,  1.0f,

		 1.0f, -1.0f, -1.0f,
		 1.0f, -1.0f,  1.0f,
		 1.0f,  1.0f,  1.0f,
		 1.0f,  1.0f,  1.0f,
		 1.0f,  1.0f, -1.0f,
		 1.0f, -1.0f, -1.0f,

		-1.0f, -1.0f,  1.0f,
		-1.0f,  1.0f,  1.0f,
		 1.0f,  1.0f,  1.0f,
		 1.0f,  1.0f,  1.0f,
		 1.0f, -1.0f,  1.0f,
		-1.0f, -1.0f,  1.0f,

		-1.0f,  1.0f, -1.0f,
		 1.0f,  1.0f, -1.0f,
		 1.0f,  1.0f,  1.0f,
		 1.0f,  1.0f,  1.0f,
		-1.0f,  1.0f,  1.0f,
		-1.0f,  1.0f, -1.0f,

		-1.0f, -1.0f, -1.0f,
		-1.0f, -1.0f,  1.0f,
		 1.0f, -1.0f, -1.0f,
		 1.0f, -1.0f, -1.0f,
		-1.0f, -1.0f,  1.0f,
		 1.0f, -1.0f,  1.0f
	};
	unsigned int skyboxVAO,skyboxVBO;
	glGenVertexArrays(1, &skyboxVAO);
	glGenBuffers(1, &skyboxVBO);
	glBindBuffer(GL_ARRAY_BUFFER, skyboxVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(skyboxVertices), &skyboxVertices, GL_STATIC_DRAW);
	GLState::BindVertexArray(skyboxVAO);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
	GLState::BindVertexArray(0);
	scene.skyboxSource = SkyboxSource(skybox, skyboxVAO, shaderManager.GetShader(ShaderManager::Skybox));
	ImageBasedLighting imageBasedLighting;
	scene.SetImageBasedLighting(&imageBasedLighting);
	bool iblInitializationAttempted = false;
	if ((useBuiltInMaterialScene || classicSceneOptions.enabled) &&
		scene.UsesPbrMaterials()) {
		iblInitializationAttempted = true;
		if (!imageBasedLighting.Initialize(
			skybox.textureID,
			skyboxVAO,
			globalVAOs.quadVAO,
			properties.SCREEN_WIDTH,
			properties.SCREEN_HEIGHT)) {
			std::cerr << "PBR IBL initialization failed; using direct-light fallback" << std::endl;
			if (pbrSmokeTest) pbrSmokeFailed = true;
		}
	}
	
	FramebuffersManager& framebuffersMgr = FramebuffersManager::GetInstance();
	AntiAliasManager& antiAliasMgr = AntiAliasManager::GetInstance();

	GLState::BindFramebuffer(GL_FRAMEBUFFER, 0);
	//glEnable(GL_CULL_FACE);
	//glCullFace(GL_BACK);

	glEnable(GL_PROGRAM_POINT_SIZE);
	
	auto forwardRenderPass = new ForwardRenderPass();
	forwardRenderPass->Init(properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT);
	auto deferRenderPass = new DeferRenderPass();
	deferRenderPass->Init(properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT);
	// PostprocessRenderPass 当前主循环未使用（最终图直接画到 postProcessFBO），若 Init 会多占一个同类型 FBO，导致列表里多一个 Forward+Gamma
	auto postprocessRenderPass = new PostprocessRenderPass();
	postprocessRenderPass->Init(properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT);
	bool deferredPassActive = properties.DEFER_RENDERING;
	int resourceSmokeFrames = 0;
	int pbrSmokeFrames = 0;
	int classicSceneFrames = 0;
	int classicSceneTemporalCapturesCompleted = 0;
	bool classicSceneTemporalCaptureSucceeded = true;
	bool renderDocFrameCaptureActive = false;
	bool renderDocFrameCaptureCompleted = false;
	std::vector<unsigned char> pbrForwardPixels;
	std::vector<double> classicSceneFrameMilliseconds;
	std::vector<BenchmarkTimelineFrameTelemetry>
		classicSceneTimelineTelemetry;
	bool classicSceneProfilerCaptureStarted = false;
	Scene::ShadowSystemStats classicSceneMeasurementStartShadowStats;
	Scene::ShadowSystemStats classicScenePreviousFrameShadowStats;
	const glm::vec3 classicSceneBaseModelPosition =
		classicSceneMotionCaster
			? classicSceneMotionCaster->position
			: (classicSceneModel
				? classicSceneModel->position
				: glm::vec3(0.0f));
	const glm::vec3 classicSceneBaseDirection =
		scene.lightSource.directionLights.empty()
			? glm::vec3(-0.45f, -1.0f, -0.25f)
			: scene.lightSource.directionLights.front().direction;
	const glm::vec3 classicSceneBasePointPosition =
		scene.lightSource.pointLights.empty()
			? glm::vec3(0.0f)
			: scene.lightSource.pointLights.front().position;
	const int classicSceneBasePointShadowResolution =
		scene.lightSource.pointLights.empty()
			? 1024
			: scene.lightSource.pointLights.front().shadowResolution;
	const glm::vec3 classicSceneBaseSpotPosition =
		scene.lightSource.spotLights.empty()
			? classicSceneOptions.cameraPosition
			: scene.lightSource.spotLights.front().position;
	const glm::vec3 classicSceneBaseSpotDirection =
		scene.lightSource.spotLights.empty()
			? glm::normalize(
				classicSceneOptions.cameraTarget -
				classicSceneOptions.cameraPosition)
			: scene.lightSource.spotLights.front().direction;
	BenchmarkMotionTimelineConfig classicSceneTimelineConfig;
	classicSceneTimelineConfig.fixedFramesPerSecond =
		classicSceneOptions.timelineFixedFramesPerSecond;
	classicSceneTimelineConfig.cycleFrames =
		classicSceneOptions.timelineCycleFrames;
	classicSceneTimelineConfig.sceneRadius =
		classicSceneOptions.normalizedRadius;
	classicSceneTimelineConfig.cameraPositionRadiusRatio =
		classicSceneOptions.cameraTimelinePositionRadiusRatio;
	classicSceneTimelineConfig.cameraTargetRadiusRatio =
		classicSceneOptions.cameraTimelineTargetRadiusRatio;
	BenchmarkMotionBaseState classicSceneMotionBaseState;
	classicSceneMotionBaseState.pointPosition =
		classicSceneBasePointPosition;
	classicSceneMotionBaseState.casterPosition =
		classicSceneBaseModelPosition;
	classicSceneMotionBaseState.cameraPosition =
		classicSceneOptions.cameraPosition;
	classicSceneMotionBaseState.cameraTarget =
		classicSceneOptions.cameraTarget;
	classicSceneMotionBaseState.cameraUp =
		classicSceneOptions.cameraUp;
	const BenchmarkMotionProfile classicSceneMotionProfile =
		classicSceneOptions.deterministicCameraTimeline
			? BenchmarkMotionProfile::Camera
			: BenchmarkMotionTimeline::ProfileFromWorkload(
				classicSceneOptions.shadowWorkload);
	const BenchmarkMotionTimeline classicSceneMotionTimeline(
		classicSceneMotionProfile,
		classicSceneTimelineConfig,
		classicSceneMotionBaseState);
	BenchmarkMotionSample classicSceneCurrentMotionSample =
		classicSceneMotionTimeline.Sample(
			-classicSceneOptions.warmupFrames);
	bool classicSceneTopologyReplacementPerformed = false;
	auto applyClassicShadowWorkload = [&](int frameNumber) {
		if (!classicSceneOptions.enabled) {
			return;
		}
		if (classicSceneOptions.deterministicCameraTimeline) {
			const int timelineFrame =
				frameNumber -
				classicSceneOptions.warmupFrames -
				1;
			classicSceneCurrentMotionSample =
				classicSceneMotionTimeline.Sample(timelineFrame);
			const glm::vec3 cameraDirection =
				classicSceneCurrentMotionSample.cameraTarget -
				classicSceneCurrentMotionSample.cameraPosition;
			camera.cameraPos =
				classicSceneCurrentMotionSample.cameraPosition;
			if (glm::length(cameraDirection) > 0.0001f) {
				camera.cameraFront = glm::normalize(cameraDirection);
			}
			camera.up = classicSceneCurrentMotionSample.cameraUp;
			return;
		}
		if (!classicSceneOptions.shadowExperiment ||
			classicSceneOptions.shadowWorkload == "static-hit") {
			return;
		}

		if (classicSceneOptions.shadowWorkload ==
			"deferred-face-required") {
			const int timelineFrame =
				frameNumber -
				classicSceneOptions.warmupFrames -
				1;
			classicSceneCurrentMotionSample =
				classicSceneMotionTimeline.Sample(timelineFrame);
			const bool measured = timelineFrame >= 0;
			properties.POINT_SHADOW_FORCE_ALL_FACES_REQUIRED =
				!measured;
			const bool viewPositiveFace =
				measured
					? timelineFrame == 0
					: (frameNumber & 1) != 0;
			if (classicSceneMotionCaster) {
				const glm::vec3 movedCasterPosition =
					classicSceneBaseModelPosition +
					classicSceneOptions.normalizedRadius *
						glm::vec3(
							0.0f,
							measured ? 0.01f : 0.0f,
							0.0f);
				classicSceneMotionCaster->SetPosition(
					movedCasterPosition);
				classicSceneCurrentMotionSample.casterPosition =
					movedCasterPosition;
			}
			const glm::vec3 cameraPosition =
				classicSceneBasePointPosition +
				classicSceneOptions.normalizedRadius *
					glm::vec3(0.0f, 0.025f, 0.025f);
			const glm::vec3 cameraTarget =
				viewPositiveFace && classicSceneDeferredReceiver
					? classicSceneDeferredReceiver->position
					: (classicSceneMotionCaster
						? classicSceneMotionCaster->position
						: classicSceneBasePointPosition -
							glm::vec3(
								classicSceneOptions.normalizedRadius *
									0.12f,
								0.0f,
								0.0f));
			camera.cameraPos = cameraPosition;
			const glm::vec3 cameraDirection =
				cameraTarget - cameraPosition;
			if (glm::length(cameraDirection) > 0.0001f) {
				camera.cameraFront =
					glm::normalize(cameraDirection);
			}
			camera.up = glm::vec3(0.0f, 1.0f, 0.0f);
			classicSceneCurrentMotionSample.cameraPosition =
				cameraPosition;
			classicSceneCurrentMotionSample.cameraTarget =
				cameraTarget;
			classicSceneCurrentMotionSample.cameraUp = camera.up;
			classicSceneCurrentMotionSample.pointPosition =
				classicSceneBasePointPosition;
			return;
		}

		if (classicSceneOptions.shadowWorkload ==
			"replace-model-aba") {
			const int timelineFrame =
				frameNumber -
				classicSceneOptions.warmupFrames -
				1;
			classicSceneCurrentMotionSample =
				classicSceneMotionTimeline.Sample(timelineFrame);
			if (timelineFrame >= 0 &&
				!classicSceneTopologyReplacementPerformed) {
				const std::shared_ptr<Model> oldModel =
					classicSceneMotionCaster;
				if (!oldModel ||
					!classicSceneReplacementCaster ||
					!scene.modelSource.ReplaceModel(
						oldModel,
						classicSceneReplacementCaster)) {
					classicSceneFailed = true;
					std::cerr
						<< "[ClassicScene] topology ABA replacement failed"
						<< std::endl;
				}
				else {
					classicSceneMotionCaster =
						classicSceneReplacementCaster;
					classicSceneTopologyReplacementPerformed = true;
				}
			}
			if (classicSceneMotionCaster) {
				classicSceneCurrentMotionSample.casterPosition =
					classicSceneMotionCaster->position;
			}
			return;
		}

		if (BenchmarkMotionTimeline::IsTimelineWorkload(
			classicSceneOptions.shadowWorkload)) {
			const int timelineFrame =
				frameNumber -
				classicSceneOptions.warmupFrames -
				1;
			classicSceneCurrentMotionSample =
				classicSceneMotionTimeline.Sample(timelineFrame);
			const std::uint32_t trackMask =
				classicSceneCurrentMotionSample.trackMask;
			if (BenchmarkMotionTimeline::HasTrack(
				trackMask,
				BenchmarkMotionTrack::Point) &&
				!scene.lightSource.pointLights.empty()) {
				scene.lightSource.pointLights.front().SetPosition(
					classicSceneCurrentMotionSample.pointPosition);
			}
			if (BenchmarkMotionTimeline::HasTrack(
				trackMask,
				BenchmarkMotionTrack::Caster) &&
				(classicSceneMotionCaster ||
					classicSceneModel)) {
				auto& motionCaster =
					classicSceneMotionCaster
						? classicSceneMotionCaster
						: classicSceneModel;
				motionCaster->SetPosition(
					classicSceneCurrentMotionSample.casterPosition);
			}
			if (BenchmarkMotionTimeline::HasTrack(
				trackMask,
				BenchmarkMotionTrack::Camera)) {
				const glm::vec3 cameraDirection =
					classicSceneCurrentMotionSample.cameraTarget -
					classicSceneCurrentMotionSample.cameraPosition;
				camera.cameraPos =
					classicSceneCurrentMotionSample.cameraPosition;
				if (glm::length(cameraDirection) > 0.0001f) {
					camera.cameraFront =
						glm::normalize(cameraDirection);
				}
				camera.up =
					classicSceneCurrentMotionSample.cameraUp;
			}
			return;
		}

		const float phase = (frameNumber & 1) == 0 ? 1.0f : -1.0f;
		auto perturbDirection = [phase](const glm::vec3& direction) {
			const glm::vec3 base =
				glm::length(direction) > 0.0001f
					? glm::normalize(direction)
					: glm::vec3(0.0f, -1.0f, 0.0f);
			const glm::vec3 referenceAxis =
				std::abs(base.x) < 0.9f
					? glm::vec3(1.0f, 0.0f, 0.0f)
					: glm::vec3(0.0f, 1.0f, 0.0f);
			const glm::vec3 tangent =
				glm::normalize(glm::cross(base, referenceAxis));
			return glm::normalize(base + tangent * (phase * 0.001f));
		};
		auto moveDirectional = [&]() {
			if (scene.lightSource.directionLights.empty()) {
				return false;
			}
			scene.lightSource.directionLights.front().direction =
				perturbDirection(classicSceneBaseDirection);
			return true;
		};
		auto movePoint = [&]() {
			if (scene.lightSource.pointLights.empty()) {
				return false;
			}
			scene.lightSource.pointLights.front().SetPosition(
				classicSceneBasePointPosition +
				glm::vec3(
					phase * 0.002f * classicSceneOptions.worldScale,
					0.0f,
					0.0f));
			return true;
		};
		auto moveSpot = [&]() {
			if (scene.lightSource.spotLights.empty()) {
				return false;
			}
			scene.lightSource.spotLights.front().SetPosition(
				classicSceneBaseSpotPosition +
				glm::vec3(
					phase * 0.002f * classicSceneOptions.worldScale,
					0.0f,
					0.0f));
			scene.lightSource.spotLights.front().direction =
				perturbDirection(classicSceneBaseSpotDirection);
			return true;
		};
		auto moveCaster = [&]() {
			if (!classicSceneModel) {
				return false;
			}
			classicSceneModel->SetPosition(
				classicSceneBaseModelPosition +
				glm::vec3(
					phase * 0.002f * classicSceneOptions.worldScale,
					0.0f,
					0.0f));
			return true;
		};
		auto moveLocalCaster = [&]() {
			if (!classicSceneMotionCaster) {
				return false;
			}
			classicSceneMotionCaster->SetPosition(
				classicSceneBaseModelPosition +
					glm::vec3(
						phase * 0.002f *
							classicSceneOptions.worldScale,
						0.0f,
						0.0f));
			return true;
		};
		auto changeCasterMaterial = [&]() {
			if (!classicSceneOverrideMaterial) {
				return false;
			}
			classicSceneOverrideMaterial->AddProperty(
				"useAlphaCutoff",
				MaterialProperty::CreateBool(true));
			classicSceneOverrideMaterial->AddProperty(
				"alphaCutoff",
				MaterialProperty::CreateFloat(
					0.5f,
					0.0f,
					1.0f,
					0.01f));
			classicSceneOverrideMaterial->AddProperty(
				"opacity",
				MaterialProperty::CreateFloat(
					phase > 0.0f ? 1.0f : 0.0f,
					0.0f,
					1.0f,
					0.01f));
			return true;
		};
		auto reloadShadow2D = [&]() {
			return shaderManager.ReloadShader("shadow", true);
		};
		auto reloadPointShadow = [&]() {
			const bool layeredReloaded =
				shaderManager.ReloadShader("shadowCube", true);
			const bool faceReloaded =
				shaderManager.ReloadShader("shadowCubeFace", true);
			return layeredReloaded && faceReloaded;
		};
		auto resizePointShadow = [&]() {
			if (scene.lightSource.pointLights.empty()) {
				return false;
			}
			const int alternateResolution =
				classicSceneBasePointShadowResolution > 128
					? (std::max)(
						128,
						classicSceneBasePointShadowResolution / 2)
					: (std::min)(
						4096,
						classicSceneBasePointShadowResolution * 2);
			scene.lightSource.pointLights.front().shadowResolution =
				phase > 0.0f
					? classicSceneBasePointShadowResolution
					: alternateResolution;
			return true;
		};
		auto replacePointShadowTarget = [&]() {
			if (scene.lightSource.pointLights.empty()) {
				return false;
			}
			auto& light = scene.lightSource.pointLights.front();
			if (!light.shadowFBO) {
				return false;
			}
			auto& manager = FramebuffersManager::GetInstance();
			manager.ReleaseFBO(light.shadowFBO);
			light.shadowFBO = nullptr;
			manager.TrimUnusedFBOs();
			return true;
		};
		auto toggleCaster = [&]() {
			if (!classicSceneModel) {
				return false;
			}
			classicSceneModel->SetActiveStatus(phase > 0.0f);
			return true;
		};

		if (classicSceneOptions.shadowWorkload == "move-directional") {
			moveDirectional();
		}
		else if (classicSceneOptions.shadowWorkload == "move-point") {
			movePoint();
		}
		else if (classicSceneOptions.shadowWorkload == "move-spot") {
			moveSpot();
		}
		else if (classicSceneOptions.shadowWorkload == "move-caster") {
			moveCaster();
		}
		else if (
			classicSceneOptions.shadowWorkload ==
			"move-local-caster") {
			moveLocalCaster();
		}
		else if (
			classicSceneOptions.shadowWorkload ==
			"change-caster-material") {
			changeCasterMaterial();
		}
		else if (
			classicSceneOptions.shadowWorkload == "reload-shadow-2d") {
			reloadShadow2D();
		}
		else if (
			classicSceneOptions.shadowWorkload ==
			"reload-shadow-point") {
			reloadPointShadow();
		}
		else if (
			classicSceneOptions.shadowWorkload ==
			"resize-point-shadow") {
			resizePointShadow();
		}
		else if (
			classicSceneOptions.shadowWorkload ==
			"replace-point-shadow-target") {
			replacePointShadowTarget();
		}
		else if (
			classicSceneOptions.shadowWorkload == "toggle-caster") {
			toggleCaster();
		}
		else if (classicSceneOptions.shadowWorkload == "force-update") {
			bool changed = false;
			if ((classicSceneOptions.shadowLights == "directional" ||
				classicSceneOptions.shadowLights == "all")) {
				changed = moveDirectional() || changed;
			}
			if ((classicSceneOptions.shadowLights == "point" ||
					classicSceneOptions.shadowLights == "all")) {
				changed = movePoint() || changed;
			}
			if ((classicSceneOptions.shadowLights == "spot" ||
					classicSceneOptions.shadowLights == "all")) {
				changed = moveSpot() || changed;
			}
			if (!changed) {
				moveCaster();
			}
		}
	};
	bool resourceSmokeFailed = false;
	auto reportResourceState = [&](const char* stage, std::size_t expectedBusyFBOs) {
		const auto busyFBOs = FramebuffersManager::GetInstance().GetBusyFBOs();
		const auto& memoryStats = PerformanceProfiler::GetInstance().GetMemoryStats();
		const auto& renderTargets = memoryStats.categories[
			static_cast<std::size_t>(MemoryResourceType::RenderTarget)];
		const auto& meshCpu = memoryStats.categories[
			static_cast<std::size_t>(MemoryResourceType::MeshCpu)];
		const auto& meshGpu = memoryStats.categories[
			static_cast<std::size_t>(MemoryResourceType::MeshGpu)];
		const double renderTargetMiB =
			static_cast<double>(renderTargets.currentBytes) / (1024.0 * 1024.0);
		std::cout << "[ResourceSmoke] stage=" << stage
			<< " busyFBOs=" << busyFBOs.size()
			<< " renderTargetMiB=" << std::fixed << std::setprecision(2)
			<< renderTargetMiB
			<< " meshCpuMiB="
			<< static_cast<double>(meshCpu.currentBytes) / (1024.0 * 1024.0)
			<< " meshGpuMiB="
			<< static_cast<double>(meshGpu.currentBytes) / (1024.0 * 1024.0)
			<< std::endl;
		if (busyFBOs.size() != expectedBusyFBOs) {
			resourceSmokeFailed = true;
		}
	};
	double nextHotReloadPollTime = 0.0;
	PerformanceBenchmarkSession benchmarkSession(benchmarkOptions, applicationStart);
	if (benchmarkOptions.enabled) {
		GLint windowSampleBuffers = 0;
		GLint windowSamples = 0;
		glGetIntegerv(GL_SAMPLE_BUFFERS, &windowSampleBuffers);
		glGetIntegerv(GL_SAMPLES, &windowSamples);
		auto glString = [](GLenum name) -> std::string {
			const GLubyte* value = glGetString(name);
			return value ? reinterpret_cast<const char*>(value) : std::string();
		};

		int shadowCastingLights = 0;
		for (auto& light : scene.lightSource.pointLights) {
			if (light.GetActiveStatus() && light.useShadowMap) ++shadowCastingLights;
		}
		for (auto& light : scene.lightSource.directionLights) {
			if (light.GetActiveStatus() && light.useShadowMap) ++shadowCastingLights;
		}
		for (auto& light : scene.lightSource.spotLights) {
			if (light.GetActiveStatus() && light.useShadowMap) ++shadowCastingLights;
		}

		PerformanceBenchmarkMetadata metadata;
		if (submissionStressOptions.enabled) {
			metadata.scenePath = DescribeSubmissionStressScene(
				submissionStressOptions,
				submissionStressState);
		}
		else {
			metadata.scenePath = benchmarkPbrMaterialScene
				? (benchmarkUnsharedImportedMaterials
					? "builtin/backpack-pbr-unshared-materials"
					: "builtin/backpack-pbr")
				: (benchmarkPhongMaterialScene
					? "builtin/backpack-phong"
					: sceneStatePath);
		}
		metadata.glVendor = glString(GL_VENDOR);
		metadata.glRenderer = glString(GL_RENDERER);
		metadata.glVersion = glString(GL_VERSION);
#ifdef NDEBUG
		metadata.buildConfiguration = "Release";
#else
		metadata.buildConfiguration = "Debug";
#endif
#ifdef _WIN64
		metadata.architecture = "x64";
#else
		metadata.architecture = "Win32";
#endif
		metadata.width = properties.SCREEN_WIDTH;
		metadata.height = properties.SCREEN_HEIGHT;
		metadata.windowSampleBuffers = windowSampleBuffers;
		metadata.windowSamples = windowSamples;
		metadata.requestedSwapInterval = 0;
		metadata.pointLights = static_cast<int>(scene.lightSource.pointLights.size());
		metadata.directionLights = static_cast<int>(scene.lightSource.directionLights.size());
		metadata.spotLights = static_cast<int>(scene.lightSource.spotLights.size());
		metadata.shadowCastingLights = shadowCastingLights;
		metadata.bloom = properties.BLOOM;
		metadata.deferredRendering = properties.DEFER_RENDERING;
		metadata.ssao = properties.SSAO;
		metadata.forwardNormalBuffer = properties.FORWARD_NORMAL_BUFFER;
		metadata.gammaCorrection = properties.GAMMA_CORRECTION;
		metadata.autoReloadShaders = properties.AUTO_RELOAD_SHADERS;
		metadata.autoReloadMaterials = properties.AUTO_RELOAD_MATERIALS;
		metadata.inputFrozen = true;
		metadata.gpuTimingSupported = PerformanceProfiler::GetInstance().IsGpuTimingSupported();
		metadata.submissionStressScene = submissionStressOptions.enabled;
		metadata.submissionStressObjectCount =
			submissionStressState.objectCount;
		metadata.submissionStressDynamicObjectCount =
			submissionStressState.dynamicObjectCount;
		metadata.submissionStressMaterialCount =
			submissionStressState.materialCount;
		metadata.submissionStressSeed = submissionStressOptions.seed;
		metadata.opaqueSortMode =
			Scene::GetOpaqueSortModeName(scene.GetOpaqueSortMode());
		metadata.submissionStressRenderPath =
			submissionStressOptions.enabled
				? submissionStressOptions.renderPath
				: (properties.DEFER_RENDERING ? "deferred" : "forward");
		metadata.submissionStressGeometrySet =
			submissionStressOptions.geometrySet;
		metadata.submissionStressCollectionBreakdown =
			submissionStressOptions.collectionBreakdown;
		benchmarkSession.SetMetadata(metadata);
	}

	const bool minimalSubmissionStressUi =
		submissionStressOptions.enabled && benchmarkOptions.enabled;
	const bool minimalSsaoBenchmarkUi =
		classicSceneOptions.enabled && classicSceneOptions.ssaoExperiment;
	const bool minimalAutomatedUi =
		minimalSubmissionStressUi || minimalSsaoBenchmarkUi;
	const char* minimalPresentZoneName =
		minimalSubmissionStressUi
			? "Submission Stress Present"
			: "SSAO Benchmark Present";
	std::uint64_t submissionStressFrameIndex = 0;
	bool submissionStressCaptureCompleted = false;
	while (!glfwWindowShouldClose(window)) {
#ifdef _WIN32
		if (renderDocApi &&
			!renderDocFrameCaptureActive &&
			!renderDocFrameCaptureCompleted &&
			classicSceneFrames + 1 ==
				classicSceneOptions.renderDocCaptureFrame) {
			renderDocApi->StartFrameCapture(nullptr, nullptr);
			if (renderDocApi->IsFrameCapturing() == 0) {
				std::cerr
					<< "[RenderDocCapture] failed to begin frame "
					<< classicSceneOptions.renderDocCaptureFrame
					<< std::endl;
				classicSceneFailed = true;
				break;
			}
			renderDocFrameCaptureActive = true;
			const std::string captureTitle =
				classicSceneOptions.sceneName + " " +
				classicSceneOptions.ssaoMode + " " +
				std::to_string(classicSceneOptions.ssaoSamples) +
				" samples";
			renderDocApi->SetCaptureTitle(captureTitle.c_str());
			std::cout
				<< "[RenderDocCapture] begin frame="
				<< classicSceneOptions.renderDocCaptureFrame
				<< " mode=" << classicSceneOptions.ssaoMode
				<< " samples=" << classicSceneOptions.ssaoSamples
				<< std::endl;
		}
#endif
		if (classicSceneOptions.enabled &&
			!classicSceneProfilerCaptureStarted &&
			classicSceneFrames == classicSceneOptions.warmupFrames) {
			classicSceneMeasurementStartShadowStats =
				scene.GetShadowSystemStats();
			classicScenePreviousFrameShadowStats =
				classicSceneMeasurementStartShadowStats;
			PerformanceProfiler::GetInstance().BeginBenchmarkCapture(
				static_cast<std::size_t>(
					classicSceneOptions.captureFrame -
					classicSceneOptions.warmupFrames));
			classicSceneProfilerCaptureStarted = true;
		}
		const auto classicSceneFrameStart =
			PerformanceBenchmarkSession::Clock::now();
		if (benchmarkOptions.enabled &&
			!benchmarkSession.OnFrameBoundary(!SceneStateIO::HasPendingAsyncLoads())) {
			break;
		}
		{
		PERF_FRAME_SCOPE();
		applyClassicShadowWorkload(classicSceneFrames + 1);
		if (!automatedValidation) {
			editorSceneManager.ProcessPendingAction(scene, camera);
		}
		// 分帧异步恢复存档里的文件模型，减少单帧加载峰值。
		{
			PERF_CPU_SCOPE("Async Model Loads");
			SceneStateIO::UpdateAsyncLoads(scene, 1);
		}
		if (submissionStressOptions.enabled) {
			PERF_CPU_SCOPE("Submission Stress Motion");
			UpdateSubmissionStressScene(
				submissionStressState,
				submissionStressFrameIndex++);
		}
		if (submissionStressOptions.collectionBreakdown) {
			scene.ProfileCollectionBreakdown();
		}
		if (!iblInitializationAttempted && scene.UsesPbrMaterials()) {
			iblInitializationAttempted = true;
			if (!imageBasedLighting.Initialize(
				skybox.textureID,
				skyboxVAO,
				globalVAOs.quadVAO,
				properties.SCREEN_WIDTH,
				properties.SCREEN_HEIGHT)) {
				std::cerr << "PBR IBL initialization failed; using direct-light fallback" << std::endl;
				if (pbrSmokeTest) pbrSmokeFailed = true;
			}
		}
		if (pbrSmokeTest) {
			++pbrSmokeFrames;
		}
		if (resourceSmokeTest && !SceneStateIO::HasPendingAsyncLoads()) {
			++resourceSmokeFrames;
			if (resourceSmokeFrames == 30) {
				reportResourceState("forward-default", 2);
				properties.BLOOM = true;
			}
			else if (resourceSmokeFrames == 60) {
				reportResourceState("forward-bloom", 4);
				properties.DEFER_RENDERING = true;
				properties.SSAO = true;
			}
			else if (resourceSmokeFrames == 90) {
				reportResourceState("deferred-ssao-bloom", 6);
				for (auto& light : scene.lightSource.pointLights) {
					light.useShadowMap = true;
				}
				for (auto& light : scene.lightSource.directionLights) {
					light.useShadowMap = true;
				}
				for (auto& light : scene.lightSource.spotLights) {
					light.useShadowMap = true;
				}
			}
			else if (resourceSmokeFrames == 120) {
				reportResourceState("all-effects", 9);
				properties.BLOOM = false;
				properties.SSAO = false;
				properties.DEFER_RENDERING = false;
				for (auto& light : scene.lightSource.pointLights) {
					light.useShadowMap = false;
				}
				for (auto& light : scene.lightSource.directionLights) {
					light.useShadowMap = false;
				}
				for (auto& light : scene.lightSource.spotLights) {
					light.useShadowMap = false;
				}
			}
			else if (resourceSmokeFrames == 150) {
				reportResourceState("reclaimed-default", 2);
				glfwSetWindowShouldClose(window, true);
			}
		}

		//calculate FPS
		timer.Tick();
		
		std::stringstream windowTitle;
		windowTitle << "OpenGL_Learn FPS:" << timer.GetFPS();
		if (SceneStateIO::HasPendingAsyncLoads()) {
			const int pending = SceneStateIO::GetPendingAsyncLoadCount();
			const int total = SceneStateIO::GetTotalAsyncLoadCount();
			const int done = (total >= pending) ? (total - pending) : 0;
			windowTitle << " [Loading " << done << "/" << total << "]";
		}
		glfwSetWindowTitle(window, windowTitle.str().c_str());

		const double currentTime = glfwGetTime();
		if (currentTime >= nextHotReloadPollTime) {
			PERF_CPU_SCOPE("Hot Reload Polling");
			if (properties.AUTO_RELOAD_SHADERS) {
				shaderManager.ReloadChangedShaders();
			}
			if (properties.AUTO_RELOAD_MATERIALS) {
				xmlMaterialManager.ReloadChangedFiles();
			}
			const double pollInterval = (std::max)(
				0.05,
				static_cast<double>(properties.HOT_RELOAD_POLL_INTERVAL));
			nextHotReloadPollTime = currentTime + pollInterval;
		}
		// NewFrame
		{
			PERF_CPU_SCOPE("Editor UI Build");
			SetGui();

			if (!minimalAutomatedUi) {
				// ?? DockSpace??? Unity ?????
				mygui.MainDockSpace();
				mygui.Overview_UI();
				mygui.Profiler_UI();
				mygui.MotionTimeline_UI(scene, camera);

				// Settings / Scene / Materials / XML / Assets ??? Dock ? DockSpace ?
				mygui.Begin();              // Settings ??
				mygui.System_UI();
				mygui.Shadow_UI();
				mygui.Gamma_UI();
				mygui.Framebuffers_UI();
				mygui.Anti_Aliasing_UI();
				mygui.End();

				mygui.Scene_UI(scene, camera, editorSceneManager); // Scene browser + lights + models
				mygui.ModelMaterialsInspector_UI(scene);  // 选中模型的材质查看/编辑
				mygui.MaterialsInspector_UI();  // 全局材质 Inspector
				mygui.MaterialsEditor_UI();     // XML 编辑器
			}

		}
		//process input
		{
			PERF_CPU_SCOPE("Input and Frame Uniforms");
			if (!benchmarkOptions.enabled &&
				!classicSceneOptions.enabled &&
				!mygui.IsMotionTimelinePlaying()) {
				ProcessInput(window);
			}
			if (!automatedValidation) {
				mygui.UpdateMotionTimelinePreview(
					scene,
					camera,
					timer.GetDeltaTime());
			}
		//reset used texture num
		properties.ResetUsedTextureNum();
		//before pass: set uniform buffer
			SetUniformBuffer();
		}
		{
			PERF_GPU_SCOPE("GPU Frame");
#ifdef USE_SCENE_SHADER
		//first pass: render scene to framebuffer (HDR)
		FBO* sceneFBO = nullptr;
		if (properties.DEFER_RENDERING != deferredPassActive) {
			if (properties.DEFER_RENDERING) {
				forwardRenderPass->Destroy();
			}
			else {
				deferRenderPass->Destroy();
			}
			FramebuffersManager::GetInstance().TrimUnusedFBOs();
			deferredPassActive = properties.DEFER_RENDERING;
		}
		if (properties.DEFER_RENDERING) {
			deferRenderPass->Render(&scene);
			sceneFBO = deferRenderPass->GetOutputFBO();
		}
		else {
			forwardRenderPass->Render(&scene);
			sceneFBO = forwardRenderPass->GetOutputFBO();
		}
		if (!automatedValidation) {
			mygui.RecordMotionTimelineTelemetry(scene);
		}
		//second pass: postprocess (HDR + gamma + bloom) -> LDR texture (inside postprocessRenderPass)
		
		GLState::BindFramebuffer(GL_FRAMEBUFFER, 0);
		glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
		if (!properties.DEBUG_MODE) {
			if (!sceneFBO || sceneFBO->textureIDs.empty()) {
				std::cout << "no valid color attachment, skip this frame" << std::endl;
				continue;
			}
			postprocessRenderPass->Render(&scene, sceneFBO);
		}
		else {
			GLState::BindFramebuffer(GL_FRAMEBUFFER, 0);
			FBO* debugFBO = scene.GetDebugFramebuffer();
			int size = static_cast<int>(debugFBO->textureIDs.size());
			int len = 1;
			while(len*len<size){
				len++;
			}
			debugShader.use();
			for(int i = 0;i<size;i++){
				GLState::ActiveTexture(GL_TEXTURE0 + i);
				GLState::BindTexture(GL_TEXTURE_2D, debugFBO->textureIDs[i]);
				std::string uniformName = "screenTexture[" + std::to_string(i) + "]";
				debugShader.setInt(uniformName, i);
			}
			debugShader.setFloat("div", (float)len);
			GLState::BindVertexArray(globalVAOs.quadVAO);
			GLState::Disable(GL_DEPTH_TEST);
			PerformanceProfiler::GetInstance().RecordDraw(GL_TRIANGLES, 6);
			glDrawArrays(GL_TRIANGLES, 0, 6);
		}

		// ??????? FBO??????????????? ImGui ?????
		GLState::BindFramebuffer(GL_FRAMEBUFFER, 0);

		if (minimalAutomatedUi) {
			PERF_CPU_SCOPE(minimalPresentZoneName);
			PERF_GPU_SCOPE(minimalPresentZoneName);
			FBO* finalFBO = postprocessRenderPass->GetOutputFBO();
			if (finalFBO && !finalFBO->textureIDs.empty()) {
				int framebufferWidth = properties.SCREEN_WIDTH;
				int framebufferHeight = properties.SCREEN_HEIGHT;
				glfwGetFramebufferSize(
					window,
					&framebufferWidth,
					&framebufferHeight);
				// A native resize changes the default framebuffer before GLFW
				// dispatches the size callback. Skip this one diagnostic
				// present instead of blitting during that transient mismatch;
				// the callback resizes all managed targets before the next frame.
				if (framebufferWidth == properties.SCREEN_WIDTH &&
					framebufferHeight == properties.SCREEN_HEIGHT) {
					GLState::BindFramebuffer(
						GL_READ_FRAMEBUFFER,
						finalFBO->framebufferID);
					GLState::BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
					glBlitFramebuffer(
						0,
						0,
						finalFBO->width,
						finalFBO->height,
						0,
						0,
						framebufferWidth,
						framebufferHeight,
						GL_COLOR_BUFFER_BIT,
						GL_NEAREST);
					GLState::BindFramebuffer(GL_FRAMEBUFFER, 0);
				}
			}
		}
		if (classicSceneOptions.enabled &&
			!classicSceneOptions.ssaoTemporalCaptureDirectory.empty() &&
			classicSceneFrames >= classicSceneOptions.warmupFrames) {
			const int measurementFrame =
				classicSceneFrames - classicSceneOptions.warmupFrames;
			const int relativeFrame =
				measurementFrame -
				classicSceneOptions.ssaoTemporalCaptureStartFrame;
			const bool captureThisFrame =
				relativeFrame >= 0 &&
				relativeFrame %
					classicSceneOptions.ssaoTemporalCaptureStride == 0 &&
				classicSceneTemporalCapturesCompleted <
					classicSceneOptions.ssaoTemporalCaptureFrameCount;
			if (captureThisFrame) {
				const FBO* ssaoFBO =
					deferRenderPass->GetSSAOOutputFBO();
				const FBO* gbufferFBO =
					deferRenderPass->GetGBufferFBO();
				const FBO* ldrFBO =
					postprocessRenderPass->GetOutputFBO();
				bool frameCaptureSucceeded = true;
				std::ostringstream frameName;
				frameName << "frame-"
					<< std::setfill('0') << std::setw(6)
					<< measurementFrame;
				for (const SsaoTemporalCaptureRoi& roi :
					classicSceneOptions.ssaoTemporalCaptureRois) {
					const std::filesystem::path roiDirectory =
						std::filesystem::path(
							classicSceneOptions
								.ssaoTemporalCaptureDirectory) /
						roi.name;
					const std::filesystem::path prefix =
						roiDirectory / frameName.str();
					const FloatCaptureStats aoCapture =
						CaptureFramebufferPfmRegion(
							ssaoFBO,
							0,
							prefix.string() + "-ao.pfm",
							FloatCaptureSource::Red,
							roi.x,
							roi.y,
							roi.width,
							roi.height);
					const FrameCaptureStats ldrCapture =
						CaptureFramebufferPpmRegion(
							ldrFBO,
							prefix.string() + "-ldr.ppm",
							roi.x,
							roi.y,
							roi.width,
							roi.height);
					frameCaptureSucceeded =
						frameCaptureSucceeded &&
						aoCapture.valid &&
						aoCapture.width == roi.width &&
						aoCapture.height == roi.height &&
						aoCapture.channels == 1 &&
						ldrCapture.valid;
					if (classicSceneOptions
						.ssaoTemporalCaptureReferenceGuides) {
						const FloatCaptureStats depthCapture =
							CaptureFramebufferPfmRegion(
								gbufferFBO,
								0,
								prefix.string() + "-depth.pfm",
								FloatCaptureSource::Alpha,
								roi.x,
								roi.y,
								roi.width,
								roi.height);
						const FloatCaptureStats normalCapture =
							CaptureFramebufferPfmRegion(
								gbufferFBO,
								1,
								prefix.string() + "-normal.pfm",
								FloatCaptureSource::RGB,
								roi.x,
								roi.y,
								roi.width,
								roi.height);
						frameCaptureSucceeded =
							frameCaptureSucceeded &&
							depthCapture.valid &&
							depthCapture.width == roi.width &&
							depthCapture.height == roi.height &&
							depthCapture.channels == 1 &&
							normalCapture.valid &&
							normalCapture.width == roi.width &&
							normalCapture.height == roi.height &&
							normalCapture.channels == 3;
					}
				}
				for (GLenum error = glGetError();
					error != GL_NO_ERROR;
					error = glGetError()) {
					frameCaptureSucceeded = false;
					std::cerr
						<< "[SSAOTemporalCapture] OpenGL error 0x"
						<< std::hex << error << std::dec
						<< " at measurement frame "
						<< measurementFrame << std::endl;
				}
				if (!frameCaptureSucceeded) {
					classicSceneTemporalCaptureSucceeded = false;
					classicSceneFailed = true;
					std::cerr
						<< "[SSAOTemporalCapture] capture failed at "
						<< "measurement frame " << measurementFrame
						<< std::endl;
				}
				else {
					++classicSceneTemporalCapturesCompleted;
				}
			}
		}
		if (submissionStressOptions.enabled &&
			!submissionStressCaptureCompleted &&
			submissionStressFrameIndex >= 2) {
			const std::uint64_t opaqueSubmissionSignature =
				scene.ComputeOpaqueSubmissionSignature();
			benchmarkSession.SetOpaqueSubmissionSignature(
				opaqueSubmissionSignature);
			std::ostringstream signatureStream;
			signatureStream << "0x"
				<< std::hex
				<< std::setw(16)
				<< std::setfill('0')
				<< opaqueSubmissionSignature;
			std::cout
				<< "[SubmissionStress] opaqueSubmissionSignature="
				<< signatureStream.str()
				<< std::endl;

			if (!submissionStressOptions.capturePath.empty()) {
				const FrameCaptureStats capture =
					CaptureFramebufferPpm(
						postprocessRenderPass->GetOutputFBO(),
						submissionStressOptions.capturePath);
				std::cout << "[SubmissionStress] capture="
					<< submissionStressOptions.capturePath
					<< " valid="
					<< (capture.valid ? "true" : "false")
					<< " meanLuminance=" << std::fixed
					<< std::setprecision(4)
					<< capture.meanLuminance
					<< " nonBlackRatio="
					<< capture.nonBlackRatio
					<< std::endl;
			}
			submissionStressCaptureCompleted = true;
		}

		if (!minimalAutomatedUi) {
			// Viewport：默认(INDEX==0)显示最终渲染结果；否则显示所选 FBO 的指定 color/depth 附件
			unsigned int viewportTextureID = 0;
			unsigned int viewportReadFBO = 0;
			int viewportReadAttachment = 0;
			bool viewportReadIsDepth = false;
			int viewportReadWidth = properties.SCREEN_WIDTH;
			int viewportReadHeight = properties.SCREEN_HEIGHT;
			if (properties.VIEWPORT_DEBUG_FBO_INDEX == 0) {
				// 最终图（延迟+正向+后处理后的结果）
				FBO* finalFBO = postprocessRenderPass->GetOutputFBO();
				if (finalFBO && !finalFBO->textureIDs.empty()) {
					viewportTextureID = finalFBO->textureIDs[0];
					viewportReadFBO = finalFBO->framebufferID;
					viewportReadAttachment = 0;
					viewportReadIsDepth = false;
					viewportReadWidth = finalFBO->width;
					viewportReadHeight = finalFBO->height;
				}
			} else {
				std::vector<FBO*> busyFBOs = FramebuffersManager::GetInstance().GetBusyFBOs();
				int fboIdx = properties.VIEWPORT_DEBUG_FBO_INDEX - 1;
				if (fboIdx >= 0 && fboIdx < (int)busyFBOs.size()) {
					FBO* fbo = busyFBOs[fboIdx];
					if (!fbo->textureIDs.empty()
						&& properties.VIEWPORT_DEBUG_ATTACHMENT_INDEX >= 0
						&& properties.VIEWPORT_DEBUG_ATTACHMENT_INDEX < (int)fbo->textureIDs.size()) {
						viewportTextureID = fbo->textureIDs[properties.VIEWPORT_DEBUG_ATTACHMENT_INDEX];
						viewportReadFBO = fbo->framebufferID;
						viewportReadAttachment = properties.VIEWPORT_DEBUG_ATTACHMENT_INDEX;
						viewportReadIsDepth = (fbo->attr.isShadowMap && properties.VIEWPORT_DEBUG_ATTACHMENT_INDEX == 0);
						viewportReadWidth = fbo->width;
						viewportReadHeight = fbo->height;
					}
				}
			}
			{
				PERF_CPU_SCOPE("Viewport and Assets UI");
				{
					PERF_CPU_SCOPE("Viewport UI");
					mygui.SetViewportReadSource(
						viewportReadFBO,
						viewportReadAttachment,
						viewportReadIsDepth,
						viewportReadWidth,
						viewportReadHeight
					);
					mygui.Viewport_UI(viewportTextureID);
				}

				// Assets ?????? models / materials / shaders ??
				{
					PERF_CPU_SCOPE("Assets Browser UI");
					mygui.AssetsBrowser_UI();
				}
			}
		}

		//Draw GUI
		{
			PERF_CPU_SCOPE("ImGui Render");
			mygui.Render();
		}
		if (pbrSmokeTest && (pbrSmokeFrames == 30 || pbrSmokeFrames == 60)) {
			const bool deferredCapture = pbrSmokeFrames == 60;
			const std::string capturePath = deferredCapture
				? "benchmark-results/pbr-ibl/pbr-deferred.ppm"
				: "benchmark-results/pbr-ibl/pbr-forward.ppm";
			const FrameCaptureStats capture = CaptureFramebufferPpm(
				postprocessRenderPass->GetOutputFBO(),
				capturePath);
			std::cout << "[PBRSmoke] mode="
				<< (deferredCapture ? "deferred" : "forward")
				<< " iblReady=" << imageBasedLighting.IsReady()
				<< " meanLuminance=" << std::fixed << std::setprecision(4)
				<< capture.meanLuminance
				<< " nonBlackRatio=" << capture.nonBlackRatio
				<< " capture=" << capturePath << std::endl;
			if (!capture.valid || !imageBasedLighting.IsReady()) {
				pbrSmokeFailed = true;
			}
			if (!deferredCapture) {
				pbrForwardPixels = capture.pixels;
				properties.DEFER_RENDERING = true;
			}
			else {
				double meanAbsoluteDifference = 1.0;
				if (!pbrForwardPixels.empty() &&
					pbrForwardPixels.size() == capture.pixels.size()) {
					double differenceSum = 0.0;
					for (size_t i = 0; i < capture.pixels.size(); ++i) {
						differenceSum += std::abs(
							static_cast<int>(pbrForwardPixels[i]) -
							static_cast<int>(capture.pixels[i]));
					}
					meanAbsoluteDifference = differenceSum /
						(static_cast<double>(capture.pixels.size()) * 255.0);
				}
				std::cout << "[PBRSmoke] forwardDeferredMae="
					<< std::fixed << std::setprecision(6)
					<< meanAbsoluteDifference << std::endl;
				if (meanAbsoluteDifference > 0.01) {
					pbrSmokeFailed = true;
				}
				glfwSetWindowShouldClose(window, true);
			}
		}
#elif defined(USE_GEOMETRY_SHADER)
		geometryShader.use();
		GLState::BindVertexArray(VAO);
		PerformanceProfiler::GetInstance().RecordDraw(GL_POINTS, 4);
		glDrawArrays(GL_POINTS, 0, 4);
#elif defined(USE_PLANET_SHADER)
		GLState::Enable(GL_DEPTH_TEST);
		glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
		glClearStencil(0);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		planet.Draw();
#endif
		}
		//scene.ClearFBO();
		// glfw: swap buffers and poll IO events (keys pressed/released, mouse moved etc.)
		{
			PERF_CPU_SCOPE("Present and Events");
			glfwSwapBuffers(window);
#ifdef _WIN32
			if (renderDocFrameCaptureActive) {
				const bool captureSaved =
					renderDocApi->EndFrameCapture(
						nullptr,
						nullptr) != 0;
				renderDocFrameCaptureActive = false;
				renderDocFrameCaptureCompleted = captureSaved;
				if (!captureSaved) {
					std::cerr
						<< "[RenderDocCapture] failed to save frame "
						<< classicSceneOptions.renderDocCaptureFrame
						<< std::endl;
					classicSceneFailed = true;
				}
				else {
					const std::string comments =
						"Diagnostic RenderDoc capture only. Scene=" +
						classicSceneOptions.sceneName +
						"; SSAO mode=" +
						classicSceneOptions.ssaoMode +
						"; samples=" +
						std::to_string(
							classicSceneOptions.ssaoSamples) +
						"; resolution=" +
						std::to_string(properties.SCREEN_WIDTH) +
						"x" +
						std::to_string(properties.SCREEN_HEIGHT) +
						". RenderDoc timings are not formal benchmark data.";
					renderDocApi->SetCaptureFileComments(
						nullptr,
						comments.c_str());
					std::string savedCapturePath;
					const std::uint32_t captureCount =
						renderDocApi->GetNumCaptures();
					if (captureCount > 0) {
						std::uint32_t pathLength = 0;
						const std::uint32_t captureIndex =
							captureCount - 1;
						if (renderDocApi->GetCapture(
								captureIndex,
								nullptr,
								&pathLength,
								nullptr) != 0 &&
							pathLength > 0) {
							std::vector<char> pathBuffer(
								static_cast<std::size_t>(
									pathLength) + 1,
								'\0');
							std::uint32_t pathCapacity =
								static_cast<std::uint32_t>(
									pathBuffer.size());
							if (renderDocApi->GetCapture(
									captureIndex,
									pathBuffer.data(),
									&pathCapacity,
									nullptr) != 0) {
								savedCapturePath =
									pathBuffer.data();
							}
						}
					}
					std::cout
						<< "[RenderDocCapture] saved=true"
						<< " captureCount=" << captureCount
						<< " path=" << savedCapturePath
						<< std::endl;
				}
			}
#endif
			glfwPollEvents();
		}
		if (classicSceneOptions.enabled) {
			if (classicSceneOptions.gpuSynchronized) {
				glFinish();
			}
			++classicSceneFrames;
			if (classicSceneFrames > classicSceneOptions.warmupFrames) {
				const double frameMilliseconds =
					std::chrono::duration<double, std::milli>(
						PerformanceBenchmarkSession::Clock::now() -
						classicSceneFrameStart).count();
				classicSceneFrameMilliseconds.push_back(frameMilliseconds);
				PerformanceProfiler::GetInstance().RecordBenchmarkWallFrame(
					frameMilliseconds);
				if (classicSceneMotionTimeline.GetProfile() !=
					BenchmarkMotionProfile::None) {
					const Scene::ShadowSystemStats& currentShadowStats =
						scene.GetShadowSystemStats();
					BenchmarkTimelineFrameTelemetry frameTelemetry;
					frameTelemetry.measurementFrame =
						classicSceneFrames -
						classicSceneOptions.warmupFrames -
						1;
					frameTelemetry.motion =
						classicSceneCurrentMotionSample;
					frameTelemetry.wallMilliseconds =
						frameMilliseconds;
					frameTelemetry.updateCount = CounterDelta(
						classicScenePreviousFrameShadowStats.updateCount,
						currentShadowStats.updateCount);
					frameTelemetry.cacheHitCount = CounterDelta(
						classicScenePreviousFrameShadowStats.cacheHitCount,
						currentShadowStats.cacheHitCount);
					frameTelemetry.lightCacheHitCount = CounterDelta(
						classicScenePreviousFrameShadowStats.lightCacheHitCount,
						currentShadowStats.lightCacheHitCount);
					frameTelemetry.directionalLightUpdateCount =
						CounterDelta(
							classicScenePreviousFrameShadowStats
								.directionalLightUpdateCount,
							currentShadowStats.directionalLightUpdateCount);
					frameTelemetry.pointLightUpdateCount = CounterDelta(
						classicScenePreviousFrameShadowStats
							.pointLightUpdateCount,
						currentShadowStats.pointLightUpdateCount);
					frameTelemetry.pointShadowSubmissionPassCount =
						CounterDelta(
							classicScenePreviousFrameShadowStats
								.pointShadowSubmissionPassCount,
							currentShadowStats
								.pointShadowSubmissionPassCount);
					frameTelemetry.pointShadowRequiredFaceCount =
						CounterDelta(
							classicScenePreviousFrameShadowStats
								.pointShadowRequiredFaceCount,
							currentShadowStats
								.pointShadowRequiredFaceCount);
					frameTelemetry.pointShadowRenderedFaceCount =
						CounterDelta(
							classicScenePreviousFrameShadowStats
								.pointShadowRenderedFaceCount,
							currentShadowStats
								.pointShadowRenderedFaceCount);
					frameTelemetry.pointShadowFaceCacheHitCount =
						CounterDelta(
							classicScenePreviousFrameShadowStats
								.pointShadowFaceCacheHitCount,
							currentShadowStats
								.pointShadowFaceCacheHitCount);
					frameTelemetry.pointShadowDeferredFaceCount =
						CounterDelta(
							classicScenePreviousFrameShadowStats
								.pointShadowDeferredFaceCount,
							currentShadowStats
								.pointShadowDeferredFaceCount);
					frameTelemetry.pointShadowRequiredFaceMask =
						currentShadowStats
							.lastPointShadowRequiredFaceMask;
					frameTelemetry.pointShadowUpdateFaceMask =
						currentShadowStats
							.lastPointShadowUpdateFaceMask;
					frameTelemetry.spotLightUpdateCount = CounterDelta(
						classicScenePreviousFrameShadowStats
							.spotLightUpdateCount,
						currentShadowStats.spotLightUpdateCount);
					frameTelemetry.updatedLightCount =
						frameTelemetry.directionalLightUpdateCount +
						frameTelemetry.pointLightUpdateCount +
						frameTelemetry.spotLightUpdateCount;
					frameTelemetry.casterBoundsRebuildCount =
						CounterDelta(
							classicScenePreviousFrameShadowStats
								.casterBoundsRebuildCount,
							currentShadowStats
								.casterBoundsRebuildCount);
					frameTelemetry.sceneTopologyRevision =
						currentShadowStats.sceneTopologyRevision;
					frameTelemetry.sceneTopologyInvalidationCount =
						CounterDelta(
							classicScenePreviousFrameShadowStats
								.sceneTopologyInvalidationCount,
							currentShadowStats
								.sceneTopologyInvalidationCount);
					frameTelemetry.sceneTopologyModelCount =
						currentShadowStats.sceneTopologyModelCount;
					frameTelemetry.cacheCheckCpuMilliseconds =
						currentShadowStats
							.lastCacheCheckCpuMilliseconds;
					frameTelemetry.casterStateSyncCpuMilliseconds =
						currentShadowStats
							.lastCasterStateSyncCpuMilliseconds;
					frameTelemetry.pointShadowFaceDemandCpuMilliseconds =
						currentShadowStats
							.lastPointShadowFaceDemandCpuMilliseconds;
					frameTelemetry.pointShadowFaceSignatureCpuMilliseconds =
						currentShadowStats
							.lastPointShadowFaceSignatureCpuMilliseconds;
					frameTelemetry.shadowUpdateCpuMilliseconds =
						frameTelemetry.updateCount > 0
							? currentShadowStats
								.lastUpdateCpuMilliseconds
							: 0.0;
					classicSceneTimelineTelemetry.push_back(
						frameTelemetry);
					classicScenePreviousFrameShadowStats =
						currentShadowStats;
				}
			}
		}
		}
		if (classicSceneOptions.enabled &&
			classicSceneFrames == classicSceneOptions.captureFrame) {
			PerformanceProfiler::GetInstance().FinishBenchmarkCapture();
			const ProfilerBenchmarkSamples& profilerSamples =
				PerformanceProfiler::GetInstance().GetBenchmarkSamples();
			const std::size_t expectedSamples = static_cast<std::size_t>(
				classicSceneOptions.captureFrame -
				classicSceneOptions.warmupFrames);
			auto zoneSampleCount = [](
				const std::unordered_map<
					std::string,
					std::vector<double>>& zones,
				const char* name) {
				const auto it = zones.find(name);
				return it == zones.end()
					? std::size_t{ 0 }
					: it->second.size();
			};
			const bool gpuTimingSupported =
				PerformanceProfiler::GetInstance().IsGpuTimingSupported();
			const bool profilerCaptureSucceeded =
				classicSceneProfilerCaptureStarted &&
				profilerSamples.wallFrameMs.size() == expectedSamples &&
				profilerSamples.cpuFrameMs.size() == expectedSamples &&
				profilerSamples.renderStats.size() == expectedSamples &&
				(!gpuTimingSupported ||
					profilerSamples.gpuFrameMs.size() == expectedSamples);
			const std::size_t deferredCpuSamples =
				zoneSampleCount(
					profilerSamples.cpuZoneMs,
					"Deferred Pass");
			const std::size_t deferredGpuSamples =
				zoneSampleCount(
					profilerSamples.gpuZoneMs,
					"Deferred Pass");
			const std::size_t ssaoCpuSamples =
				zoneSampleCount(
					profilerSamples.cpuZoneMs,
					"SSAO Pass");
			const std::size_t ssaoGpuSamples =
				zoneSampleCount(
					profilerSamples.gpuZoneMs,
					"SSAO Pass");
			const std::size_t ssaoGenerateCpuSamples =
				zoneSampleCount(
					profilerSamples.cpuZoneMs,
					"SSAO Generate");
			const std::size_t ssaoGenerateGpuSamples =
				zoneSampleCount(
					profilerSamples.gpuZoneMs,
					"SSAO Generate");
			const std::size_t ssaoUpsampleCpuSamples =
				zoneSampleCount(
					profilerSamples.cpuZoneMs,
					"SSAO Upsample");
			const std::size_t ssaoUpsampleGpuSamples =
				zoneSampleCount(
					profilerSamples.gpuZoneMs,
					"SSAO Upsample");
			const std::size_t expectedSsaoSamples =
				classicSceneOptions.ssaoSamples > 0
					? expectedSamples
					: std::size_t{ 0 };
			const std::size_t expectedSsaoUpsampleSamples =
				classicSceneOptions.ssaoSamples > 0 &&
				classicSceneOptions.ssaoMode == "half-bilateral"
					? expectedSamples
					: std::size_t{ 0 };
			const bool ssaoZoneCaptureSucceeded =
				!classicSceneOptions.ssaoExperiment ||
				(gpuTimingSupported &&
					deferredCpuSamples == expectedSamples &&
					deferredGpuSamples == expectedSamples &&
					ssaoCpuSamples == expectedSsaoSamples &&
					ssaoGpuSamples == expectedSsaoSamples &&
					ssaoGenerateCpuSamples == expectedSsaoSamples &&
					ssaoGenerateGpuSamples == expectedSsaoSamples &&
					ssaoUpsampleCpuSamples ==
						expectedSsaoUpsampleSamples &&
					ssaoUpsampleGpuSamples ==
						expectedSsaoUpsampleSamples);
			if (!profilerCaptureSucceeded) {
				std::cerr
					<< "[ClassicScene] profiler capture count mismatch: "
					<< "expected=" << expectedSamples
					<< " wall=" << profilerSamples.wallFrameMs.size()
					<< " cpu=" << profilerSamples.cpuFrameMs.size()
					<< " gpu=" << profilerSamples.gpuFrameMs.size()
					<< " renderStats="
					<< profilerSamples.renderStats.size()
					<< std::endl;
			}
			if (!ssaoZoneCaptureSucceeded) {
				std::cerr
					<< "[SSAOBaseline] required zone count mismatch: "
					<< "expected=" << expectedSamples
					<< " deferredCpu=" << deferredCpuSamples
					<< " deferredGpu=" << deferredGpuSamples
					<< " expectedSsao=" << expectedSsaoSamples
					<< " ssaoCpu=" << ssaoCpuSamples
					<< " ssaoGpu=" << ssaoGpuSamples
					<< " generateCpu=" << ssaoGenerateCpuSamples
					<< " generateGpu=" << ssaoGenerateGpuSamples
					<< " expectedUpsample="
					<< expectedSsaoUpsampleSamples
					<< " upsampleCpu=" << ssaoUpsampleCpuSamples
					<< " upsampleGpu=" << ssaoUpsampleGpuSamples
					<< " gpuTimingSupported="
					<< (gpuTimingSupported ? "true" : "false")
					<< std::endl;
			}

			bool renderGlErrorFree = true;
			for (GLenum error = glGetError();
				error != GL_NO_ERROR;
				error = glGetError()) {
				renderGlErrorFree = false;
				std::cerr
					<< "[ClassicScene] OpenGL error before capture: 0x"
					<< std::hex << error << std::dec << std::endl;
			}

			FrameCaptureStats capture;
			if (classicSceneOptions.captureFinalFrame) {
				capture = CaptureFramebufferPpm(
					postprocessRenderPass->GetOutputFBO(),
					classicSceneOptions.capturePath);
			}
			const FBO* ssaoFBO = deferRenderPass->GetSSAOOutputFBO();
			const FBO* ssaoGenerationFBO =
				deferRenderPass->GetSSAOGenerationFBO();
			const FBO* gbufferFBO = deferRenderPass->GetGBufferFBO();
			FrameCaptureStats ssaoCapture;
			if (!classicSceneOptions.ssaoCapturePath.empty()) {
				ssaoCapture = CaptureFramebufferPpm(
					ssaoFBO,
					classicSceneOptions.ssaoCapturePath,
					true);
			}
			FloatCaptureStats ssaoFloatCapture;
			if (!classicSceneOptions.ssaoFloatCapturePath.empty()) {
				ssaoFloatCapture = CaptureFramebufferPfm(
					ssaoFBO,
					0,
					classicSceneOptions.ssaoFloatCapturePath,
					FloatCaptureSource::Red);
			}
			FloatCaptureStats ssaoRawFloatCapture;
			if (!classicSceneOptions.ssaoRawFloatCapturePath.empty()) {
				ssaoRawFloatCapture = CaptureFramebufferPfm(
					ssaoGenerationFBO,
					0,
					classicSceneOptions.ssaoRawFloatCapturePath,
					FloatCaptureSource::Red);
			}
			FloatCaptureStats ssaoDepthCapture;
			if (!classicSceneOptions.ssaoDepthCapturePath.empty()) {
				ssaoDepthCapture = CaptureFramebufferPfm(
					gbufferFBO,
					0,
					classicSceneOptions.ssaoDepthCapturePath,
					FloatCaptureSource::Alpha);
			}
			FloatCaptureStats ssaoNormalCapture;
			if (!classicSceneOptions.ssaoNormalCapturePath.empty()) {
				ssaoNormalCapture = CaptureFramebufferPfm(
					gbufferFBO,
					1,
					classicSceneOptions.ssaoNormalCapturePath,
					FloatCaptureSource::RGB);
			}
			for (GLenum error = glGetError();
				error != GL_NO_ERROR;
				error = glGetError()) {
				renderGlErrorFree = false;
				std::cerr
					<< "[ClassicScene] OpenGL error after capture: 0x"
					<< std::hex << error << std::dec << std::endl;
			}
			const bool ssaoOutputExpected =
				classicSceneOptions.ssaoExperiment &&
				classicSceneOptions.ssaoSamples > 0;
			const int fullWidth = properties.SCREEN_WIDTH;
			const int fullHeight = properties.SCREEN_HEIGHT;
			const int halfWidth = (fullWidth + 1) / 2;
			const int halfHeight = (fullHeight + 1) / 2;
			const bool halfGeneration =
				classicSceneOptions.ssaoMode != "legacy-full";
			const int expectedGenerationWidth =
				halfGeneration ? halfWidth : fullWidth;
			const int expectedGenerationHeight =
				halfGeneration ? halfHeight : fullHeight;
			const bool bilateral =
				classicSceneOptions.ssaoMode == "half-bilateral";
			const int expectedOutputWidth =
				bilateral ||
				classicSceneOptions.ssaoMode == "legacy-full"
					? fullWidth
					: halfWidth;
			const int expectedOutputHeight =
				bilateral ||
				classicSceneOptions.ssaoMode == "legacy-full"
					? fullHeight
					: halfHeight;
			auto validAoFbo = [](
				const FBO* fbo,
				int expectedWidth,
				int expectedHeight) {
				return
					fbo &&
					fbo->IsComplete() &&
					!fbo->textureIDs.empty() &&
					fbo->width == expectedWidth &&
					fbo->height == expectedHeight &&
					!fbo->attr.textureAttrs.empty() &&
					fbo->attr.textureAttrs.front().internalFormat ==
						GL_R16F;
			};
			const bool ssaoFboRelationshipValid =
				!ssaoOutputExpected ||
				(bilateral
					? (ssaoGenerationFBO &&
						ssaoFBO &&
						ssaoGenerationFBO != ssaoFBO &&
						ssaoGenerationFBO->framebufferID !=
							ssaoFBO->framebufferID)
					: (ssaoGenerationFBO == ssaoFBO));
			const bool ssaoOutputValid =
				!classicSceneOptions.ssaoExperiment ||
				(ssaoOutputExpected
					? (validAoFbo(
							ssaoFBO,
							expectedOutputWidth,
							expectedOutputHeight) &&
						validAoFbo(
							ssaoGenerationFBO,
							expectedGenerationWidth,
							expectedGenerationHeight) &&
						ssaoFboRelationshipValid)
					: ssaoFBO == nullptr &&
						ssaoGenerationFBO == nullptr);
			const bool ssaoCaptureValid =
				classicSceneOptions.ssaoCapturePath.empty() ||
				ssaoCapture.valid;
			auto validRequestedFloatCapture = [](
				const std::string& path,
				const FloatCaptureStats& captureStats,
				int expectedWidth,
				int expectedHeight,
				int expectedChannels) {
				return
					path.empty() ||
					(captureStats.valid &&
						captureStats.width == expectedWidth &&
						captureStats.height == expectedHeight &&
						captureStats.channels == expectedChannels);
			};
			const bool ssaoFloatCapturesValid =
				validRequestedFloatCapture(
					classicSceneOptions.ssaoFloatCapturePath,
					ssaoFloatCapture,
					expectedOutputWidth,
					expectedOutputHeight,
					1) &&
				validRequestedFloatCapture(
					classicSceneOptions.ssaoRawFloatCapturePath,
					ssaoRawFloatCapture,
					expectedGenerationWidth,
					expectedGenerationHeight,
					1) &&
				validRequestedFloatCapture(
					classicSceneOptions.ssaoDepthCapturePath,
					ssaoDepthCapture,
					fullWidth,
					fullHeight,
					1) &&
				validRequestedFloatCapture(
					classicSceneOptions.ssaoNormalCapturePath,
					ssaoNormalCapture,
					fullWidth,
					fullHeight,
					3);
			if (!ssaoOutputValid ||
				!ssaoCaptureValid ||
				!ssaoFloatCapturesValid ||
				!renderGlErrorFree) {
				std::cerr
					<< "[SSAO] output/capture validation failed: "
					<< "expected=" << (ssaoOutputExpected ? "true" : "false")
					<< " mode=" << classicSceneOptions.ssaoMode
					<< " outputAvailable="
					<< (ssaoFBO ? "true" : "false")
					<< " outputSize="
					<< (ssaoFBO ? ssaoFBO->width : 0)
					<< "x" << (ssaoFBO ? ssaoFBO->height : 0)
					<< " generationSize="
					<< (ssaoGenerationFBO
						? ssaoGenerationFBO->width
						: 0)
					<< "x"
					<< (ssaoGenerationFBO
						? ssaoGenerationFBO->height
						: 0)
					<< " ldrCaptureValid="
					<< (ssaoCapture.valid ? "true" : "false")
					<< " floatCapturesValid="
					<< (ssaoFloatCapturesValid ? "true" : "false")
					<< " glErrorFree="
					<< (renderGlErrorFree ? "true" : "false")
					<< std::endl;
			}
			const PointShadowCubeEvidence pointShadowCubeEvidence =
				CapturePointShadowCubeEvidence(scene);
			classicSceneFrameTiming =
				CalculateFrameTimingStats(classicSceneFrameMilliseconds);
			const bool requiresImageBasedLighting =
				classicSceneOptions.renderPath.find("pbr-") == 0;
			const bool requiresPointShadowEvidence =
				classicSceneOptions.shadowExperiment &&
				classicSceneOptions.shadowMode != "off" &&
				(classicSceneOptions.shadowLights == "point" ||
					classicSceneOptions.shadowLights == "all");
			const bool timelineCaptureSucceeded =
				classicSceneMotionTimeline.GetProfile() ==
					BenchmarkMotionProfile::None ||
				classicSceneTimelineTelemetry.size() == expectedSamples;
			const bool temporalCaptureSucceeded =
				classicSceneOptions.ssaoTemporalCaptureDirectory.empty() ||
				(classicSceneTemporalCaptureSucceeded &&
					classicSceneTemporalCapturesCompleted ==
						classicSceneOptions
							.ssaoTemporalCaptureFrameCount);
			const bool captureSucceeded =
				(!classicSceneOptions.captureFinalFrame ||
					capture.valid) &&
				profilerCaptureSucceeded &&
				ssaoZoneCaptureSucceeded &&
				ssaoOutputValid &&
				ssaoCaptureValid &&
				ssaoFloatCapturesValid &&
				renderGlErrorFree &&
				timelineCaptureSucceeded &&
				temporalCaptureSucceeded &&
				(!requiresImageBasedLighting || imageBasedLighting.IsReady()) &&
				(!requiresPointShadowEvidence ||
					pointShadowCubeEvidence.valid) &&
				!classicSceneFailed;
			if (requiresPointShadowEvidence &&
				!pointShadowCubeEvidence.valid) {
				std::cerr
					<< "[ClassicScene] point shadow cubemap evidence "
					<< "is unavailable or invalid"
					<< std::endl;
			}
			if (!timelineCaptureSucceeded) {
				std::cerr
					<< "[ClassicScene] timeline telemetry count mismatch: "
					<< "expected=" << expectedSamples
					<< " actual="
					<< classicSceneTimelineTelemetry.size()
					<< std::endl;
			}
			if (!temporalCaptureSucceeded) {
				std::cerr
					<< "[SSAOTemporalCapture] count mismatch: expected="
					<< classicSceneOptions
						.ssaoTemporalCaptureFrameCount
					<< " actual="
					<< classicSceneTemporalCapturesCompleted
					<< std::endl;
			}
			const MemoryStats memory =
				PerformanceProfiler::GetInstance().GetMemoryStats();
			float actualSpotShadowNearPlane = 0.0f;
			float actualSpotShadowFarPlane = 0.0f;
			if (!scene.lightSource.spotLights.empty()) {
				actualSpotShadowNearPlane =
					scene.lightSource.spotLights.front().near_plane;
				actualSpotShadowFarPlane =
					scene.lightSource.spotLights.front().far_plane;
			}
			if (!WriteClassicSceneResult(
				classicSceneOptions,
				scene,
				captureSucceeded,
				classicSceneLoadMilliseconds,
				classicSceneFrameTiming,
				classicSceneMeshCount,
				classicSceneVertexCount,
				classicSceneTriangleCount,
				classicSceneSourceCenter,
				classicSceneSourceRadius,
				classicSceneAppliedScale,
				capture,
				ssaoCapture,
				ssaoFloatCapture,
				ssaoRawFloatCapture,
				ssaoDepthCapture,
				ssaoNormalCapture,
				ssaoFBO,
				ssaoGenerationFBO,
				pointShadowCubeEvidence,
				memory,
				scene.GetShadowSystemStats(),
				classicSceneMeasurementStartShadowStats,
				classicSceneMotionTimeline,
				classicSceneTimelineTelemetry,
				profilerSamples,
				actualSpotShadowNearPlane,
				actualSpotShadowFarPlane)) {
				std::cerr << "[ClassicScene] failed to write result "
					<< classicSceneOptions.resultPath << std::endl;
				classicSceneFailed = true;
			}
			classicSceneCaptured = true;
			classicSceneFailed = classicSceneFailed || !captureSucceeded;
			std::cout << "[ClassicScene] scene="
				<< classicSceneOptions.sceneName
				<< " workload=" << classicSceneOptions.shadowWorkload
				<< " variant=" << classicSceneOptions.shadowVariant
				<< " iblReady=" << imageBasedLighting.IsReady()
				<< " meanLuminance=" << std::fixed << std::setprecision(4)
				<< capture.meanLuminance
				<< " nonBlackRatio=" << capture.nonBlackRatio
				<< " averageFrameMs=" << classicSceneFrameTiming.meanMilliseconds
				<< " medianFrameMs=" << classicSceneFrameTiming.medianMilliseconds
				<< " p95FrameMs=" << classicSceneFrameTiming.p95Milliseconds
				<< " p99FrameMs=" << classicSceneFrameTiming.p99Milliseconds
				<< " capture=" << classicSceneOptions.capturePath
				<< " result=" << classicSceneOptions.resultPath
				<< std::endl;
			glfwSetWindowShouldClose(window, true);
		}
	}
#ifdef _WIN32
	if (renderDocFrameCaptureActive && renderDocApi) {
		renderDocApi->DiscardFrameCapture(nullptr, nullptr);
		renderDocFrameCaptureActive = false;
		classicSceneFailed = true;
		std::cerr
			<< "[RenderDocCapture] discarded incomplete capture"
			<< std::endl;
	}
#endif
	if (classicSceneOptions.renderDocCaptureFrame > 0 &&
		!renderDocFrameCaptureCompleted) {
		classicSceneFailed = true;
		std::cerr
			<< "[RenderDocCapture] requested frame was not captured"
			<< std::endl;
	}
	if (benchmarkOptions.enabled && !benchmarkSession.IsComplete()) {
		benchmarkSession.Abort();
	}
	if (!resourceSmokeTest &&
		!pbrSmokeTest &&
		!benchmarkOptions.enabled &&
		!classicSceneOptions.enabled &&
		!submissionStressOptions.enabled) {
		mygui.RestoreTemporaryEditorState(scene, camera);
	}

	forwardRenderPass->Destroy();
	delete forwardRenderPass;
	deferRenderPass->Destroy();
	delete deferRenderPass;
	postprocessRenderPass->Destroy();
	delete postprocessRenderPass;
	if (!resourceSmokeTest &&
		!pbrSmokeTest &&
		!benchmarkOptions.enabled &&
		!classicSceneOptions.enabled &&
		!submissionStressOptions.enabled) {
		SceneStateIO::Save(scene, camera, sceneStatePath);
	}
	scene.SetSelectedModelForMaterials(nullptr);
	scene.modelSource.ClearModels();
	submissionStressState.Reset();
	classicSceneDeferredReceiver.reset();
	classicSceneReplacementCaster.reset();
	classicSceneMotionCaster.reset();
	classicSceneMotionCasterMaterial.reset();
	classicSceneModel.reset();
	classicSceneOverrideMaterial.reset();
	scene.lightSource.pointLights.clear();
	scene.lightSource.directionLights.clear();
	scene.lightSource.spotLights.clear();
	Model::DestroyMeshCache();
	scene.SetImageBasedLighting(nullptr);
	imageBasedLighting.Destroy();
	skybox.Release();
	DestroyTextureCache();
	FramebuffersManager::GetInstance().Shutdown();
	if (pbrSmokeTest) {
		const auto& memory = PerformanceProfiler::GetInstance().GetMemoryStats();
		auto currentBytes = [&](MemoryResourceType type) {
			return memory.categories[static_cast<size_t>(type)].currentBytes;
		};
		const std::uint64_t textureBytes = currentBytes(MemoryResourceType::Texture);
		const std::uint64_t meshCpuBytes = currentBytes(MemoryResourceType::MeshCpu);
		const std::uint64_t meshGpuBytes = currentBytes(MemoryResourceType::MeshGpu);
		const std::uint64_t renderTargetBytes = currentBytes(MemoryResourceType::RenderTarget);
		std::cout << "[PBRSmoke] released textureBytes=" << textureBytes
			<< " meshCpuBytes=" << meshCpuBytes
			<< " meshGpuBytes=" << meshGpuBytes
			<< " renderTargetBytes=" << renderTargetBytes << std::endl;
		if (textureBytes != 0 || meshCpuBytes != 0 || meshGpuBytes != 0 || renderTargetBytes != 0) {
			pbrSmokeFailed = true;
		}
	}
	if (classicSceneOptions.enabled) {
		const auto& memory = PerformanceProfiler::GetInstance().GetMemoryStats();
		auto currentBytes = [&](MemoryResourceType type) {
			return memory.categories[static_cast<size_t>(type)].currentBytes;
		};
		const std::uint64_t textureBytes = currentBytes(MemoryResourceType::Texture);
		const std::uint64_t meshCpuBytes = currentBytes(MemoryResourceType::MeshCpu);
		const std::uint64_t meshGpuBytes = currentBytes(MemoryResourceType::MeshGpu);
		const std::uint64_t renderTargetBytes =
			currentBytes(MemoryResourceType::RenderTarget);
		std::cout << "[ClassicScene] released textureBytes=" << textureBytes
			<< " meshCpuBytes=" << meshCpuBytes
			<< " meshGpuBytes=" << meshGpuBytes
			<< " renderTargetBytes=" << renderTargetBytes << std::endl;
		if (!classicSceneCaptured ||
			textureBytes != 0 ||
			meshCpuBytes != 0 ||
			meshGpuBytes != 0 ||
			renderTargetBytes != 0) {
			classicSceneFailed = true;
		}
	}
	PerformanceProfiler::GetInstance().Shutdown();

	glfwTerminate();
	if (resourceSmokeFailed) {
		return 2;
	}
	if (pbrSmokeFailed) {
		return 6;
	}
	if (classicSceneFailed) {
		return 7;
	}
	if (benchmarkOptions.enabled && !benchmarkSession.WasSuccessful()) {
		return 3;
	}
	return 0;
}
