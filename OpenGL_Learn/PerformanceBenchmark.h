#pragma once

#include <chrono>
#include <cstdint>
#include <string>

struct PerformanceBenchmarkOptions {
	bool enabled = false;
	int warmupFrames = 300;
	int sampleFrames = 1200;
	std::string outputPath = "benchmark-results/runtime-benchmark.json";
	std::string label = "unlabeled";
};

struct PerformanceBenchmarkMetadata {
	std::string scenePath;
	std::string glVendor;
	std::string glRenderer;
	std::string glVersion;
	std::string buildConfiguration;
	std::string architecture;
	int width = 0;
	int height = 0;
	int windowSampleBuffers = 0;
	int windowSamples = 0;
	int requestedSwapInterval = 0;
	int pointLights = 0;
	int directionLights = 0;
	int spotLights = 0;
	int shadowCastingLights = 0;
	bool bloom = false;
	bool deferredRendering = false;
	bool ssao = false;
	bool forwardNormalBuffer = false;
	bool gammaCorrection = false;
	bool autoReloadShaders = false;
	bool autoReloadMaterials = false;
	bool inputFrozen = true;
	bool gpuTimingSupported = false;
	bool submissionStressScene = false;
	int submissionStressObjectCount = 0;
	int submissionStressDynamicObjectCount = 0;
	int submissionStressMaterialCount = 0;
	std::uint32_t submissionStressSeed = 0;
	std::string opaqueSortMode;
	std::string submissionStressRenderPath;
	std::string submissionStressGeometrySet;
	bool submissionStressCollectionBreakdown = false;
	std::string opaqueSubmissionSignature;
	bool opaqueSubmissionSignatureValid = false;
};

bool ParsePerformanceBenchmarkOptions(
	int argc,
	char** argv,
	PerformanceBenchmarkOptions& options,
	std::string& errorMessage);

class PerformanceBenchmarkSession {
public:
	using Clock = std::chrono::steady_clock;

	PerformanceBenchmarkSession(
		const PerformanceBenchmarkOptions& options,
		Clock::time_point applicationStart);

	void SetMetadata(const PerformanceBenchmarkMetadata& metadata);
	void SetOpaqueSubmissionSignature(std::uint64_t signature);
	bool OnFrameBoundary(bool sceneReady);
	void Abort();

	bool IsComplete() const { return m_complete; }
	bool WasSuccessful() const { return m_success; }
	double GetLoadReadyMs() const { return m_loadReadyMs; }

private:
	bool WriteReport();

	PerformanceBenchmarkOptions m_options;
	PerformanceBenchmarkMetadata m_metadata;
	Clock::time_point m_applicationStart{};
	Clock::time_point m_previousFrameStart{};
	int m_warmupFramesCompleted = 0;
	int m_sampleFramesCompleted = 0;
	double m_loadReadyMs = 0.0;
	bool m_loadReadyRecorded = false;
	bool m_previousFrameWasWarmup = false;
	bool m_previousFrameWasSample = false;
	bool m_captureStarted = false;
	bool m_complete = false;
	bool m_success = false;
};
