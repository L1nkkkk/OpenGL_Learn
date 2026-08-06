#include "PerformanceBenchmark.h"

#include "Profiler.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <vector>

#include "../assimp/contrib/rapidjson/include/rapidjson/prettywriter.h"
#include "../assimp/contrib/rapidjson/include/rapidjson/stringbuffer.h"

namespace {
	using JsonWriter = rapidjson::PrettyWriter<rapidjson::StringBuffer>;

	struct DistributionStats {
		std::size_t count = 0;
		double mean = 0.0;
		double minimum = 0.0;
		double maximum = 0.0;
		double median = 0.0;
		double p95 = 0.0;
		double p99 = 0.0;
	};

	struct RenderField {
		const char* name;
		std::uint64_t RenderStats::*member;
	};

	const RenderField kRenderFields[] = {
		{ "drawCalls", &RenderStats::drawCalls },
		{ "submittedVertices", &RenderStats::submittedVertices },
		{ "submittedTriangles", &RenderStats::submittedTriangles },
		{ "shaderBinds", &RenderStats::shaderBinds },
		{ "uniformUpdates", &RenderStats::uniformUpdates },
		{ "uniformLocationQueries", &RenderStats::uniformLocationQueries },
		{ "uniformLocationCacheHits", &RenderStats::uniformLocationCacheHits },
		{ "materialBinds", &RenderStats::materialBinds },
		{ "materialBindCacheHits", &RenderStats::materialBindCacheHits },
		{ "renderStateChanges", &RenderStats::renderStateChanges },
		{ "renderStateCacheHits", &RenderStats::renderStateCacheHits },
		{ "renderStateQueries", &RenderStats::renderStateQueries },
		{ "textureStateChanges", &RenderStats::textureStateChanges },
		{ "textureStateCacheHits", &RenderStats::textureStateCacheHits },
		{ "vertexArrayBinds", &RenderStats::vertexArrayBinds },
		{ "vertexArrayBindCacheHits", &RenderStats::vertexArrayBindCacheHits },
		{ "framebufferBinds", &RenderStats::framebufferBinds },
		{ "framebufferBindCacheHits", &RenderStats::framebufferBindCacheHits },
		{ "fileSystemChecks", &RenderStats::fileSystemChecks },
		{ "assetBrowserCacheHits", &RenderStats::assetBrowserCacheHits },
		{ "assetBrowserCacheMisses", &RenderStats::assetBrowserCacheMisses },
		{ "activeModels", &RenderStats::activeModels },
		{ "visibleModels", &RenderStats::visibleModels },
		{ "culledModels", &RenderStats::culledModels },
		{ "culledMeshes", &RenderStats::culledMeshes },
		{ "opaqueMeshes", &RenderStats::opaqueMeshes },
		{ "transparentMeshes", &RenderStats::transparentMeshes },
		{ "pointLightsTotal", &RenderStats::pointLightsTotal },
		{ "pointLightsActive", &RenderStats::pointLightsActive },
		{ "pointLightsSubmitted", &RenderStats::pointLightsSubmitted },
		{ "pointLightsCulled", &RenderStats::pointLightsCulled },
		{ "pointLightBoundsRect", &RenderStats::pointLightBoundsRect },
		{ "pointLightBoundsOutside", &RenderStats::pointLightBoundsOutside },
		{ "pointLightBoundsFullscreenFallback", &RenderStats::pointLightBoundsFullscreenFallback },
		{ "pointLightFallbackCameraInside", &RenderStats::pointLightFallbackCameraInside },
		{ "pointLightFallbackNearPlane", &RenderStats::pointLightFallbackNearPlane },
		{ "pointLightFallbackInvalid", &RenderStats::pointLightFallbackInvalid },
		{ "pointLightVolumeCount", &RenderStats::pointLightVolumeCount },
		{ "pointLightScreenCount", &RenderStats::pointLightScreenCount },
		{ "pointLightStencilDraws", &RenderStats::pointLightStencilDraws },
		{ "pointLightLightingVolumeDraws", &RenderStats::pointLightLightingVolumeDraws },
		{ "pointLightScreenDraws", &RenderStats::pointLightScreenDraws },
		{ "pointLightRectPixelArea", &RenderStats::pointLightRectPixelArea },
		{ "pointLightStencilClearPixelArea", &RenderStats::pointLightStencilClearPixelArea },
		{ "stencilClears", &RenderStats::stencilClears },
		{ "pointLightStencilClears", &RenderStats::pointLightStencilClears },
		{ "uiDrawCalls", &RenderStats::uiDrawCalls },
		{ "uiVertices", &RenderStats::uiVertices },
		{ "uiIndices", &RenderStats::uiIndices }
	};

	const char* kMemoryCategoryNames[] = {
		"texture",
		"meshCpu",
		"meshGpu",
		"renderTarget"
	};

	bool ParseNonNegativeInt(const std::string& text, int& value)
	{
		try {
			std::size_t consumed = 0;
			const long parsed = std::stol(text, &consumed, 10);
			if (consumed != text.size() || parsed < 0 || parsed > 10000000L) {
				return false;
			}
			value = static_cast<int>(parsed);
			return true;
		}
		catch (...) {
			return false;
		}
	}

	double NearestRankPercentile(const std::vector<double>& sorted, double percentile)
	{
		if (sorted.empty()) {
			return 0.0;
		}
		const double normalized = (std::max)(0.0, (std::min)(1.0, percentile));
		const std::size_t rank = static_cast<std::size_t>(
			std::ceil(normalized * static_cast<double>(sorted.size())));
		const std::size_t index = rank == 0 ? 0 : rank - 1;
		return sorted[(std::min)(index, sorted.size() - 1)];
	}

	DistributionStats Summarize(const std::vector<double>& values)
	{
		DistributionStats result;
		result.count = values.size();
		if (values.empty()) {
			return result;
		}

		std::vector<double> sorted = values;
		std::sort(sorted.begin(), sorted.end());
		result.mean = std::accumulate(values.begin(), values.end(), 0.0) /
			static_cast<double>(values.size());
		result.minimum = sorted.front();
		result.maximum = sorted.back();
		result.median = NearestRankPercentile(sorted, 0.50);
		result.p95 = NearestRankPercentile(sorted, 0.95);
		result.p99 = NearestRankPercentile(sorted, 0.99);
		return result;
	}

	void WriteDistribution(JsonWriter& writer, const std::vector<double>& values)
	{
		const DistributionStats stats = Summarize(values);
		writer.StartObject();
		writer.Key("count"); writer.Uint64(static_cast<std::uint64_t>(stats.count));
		if (stats.count == 0) {
			writer.Key("mean"); writer.Null();
			writer.Key("min"); writer.Null();
			writer.Key("max"); writer.Null();
			writer.Key("median"); writer.Null();
			writer.Key("p95"); writer.Null();
			writer.Key("p99"); writer.Null();
		}
		else {
			writer.Key("mean"); writer.Double(stats.mean);
			writer.Key("min"); writer.Double(stats.minimum);
			writer.Key("max"); writer.Double(stats.maximum);
			writer.Key("median"); writer.Double(stats.median);
			writer.Key("p95"); writer.Double(stats.p95);
			writer.Key("p99"); writer.Double(stats.p99);
		}
		writer.EndObject();
	}

	void WriteDoubleArray(JsonWriter& writer, const std::vector<double>& values)
	{
		writer.StartArray();
		for (double value : values) {
			writer.Double(value);
		}
		writer.EndArray();
	}

	std::vector<std::string> SortedKeys(
		const std::unordered_map<std::string, std::vector<double>>& values)
	{
		std::vector<std::string> keys;
		keys.reserve(values.size());
		for (const auto& [name, samples] : values) {
			(void)samples;
			keys.push_back(name);
		}
		std::sort(keys.begin(), keys.end());
		return keys;
	}

	void WriteZoneSummary(
		JsonWriter& writer,
		const std::unordered_map<std::string, std::vector<double>>& zones)
	{
		writer.StartObject();
		for (const std::string& name : SortedKeys(zones)) {
			writer.Key(name.c_str());
			WriteDistribution(writer, zones.at(name));
		}
		writer.EndObject();
	}

	void WriteZoneSamples(
		JsonWriter& writer,
		const std::unordered_map<std::string, std::vector<double>>& zones)
	{
		writer.StartObject();
		for (const std::string& name : SortedKeys(zones)) {
			writer.Key(name.c_str());
			WriteDoubleArray(writer, zones.at(name));
		}
		writer.EndObject();
	}

	std::vector<double> ExtractRenderField(
		const std::vector<RenderStats>& samples,
		std::uint64_t RenderStats::*member)
	{
		std::vector<double> values;
		values.reserve(samples.size());
		for (const RenderStats& sample : samples) {
			values.push_back(static_cast<double>(sample.*member));
		}
		return values;
	}

	template <typename Getter>
	std::vector<double> ExtractMemoryField(
		const std::vector<MemoryStats>& samples,
		Getter getter)
	{
		std::vector<double> values;
		values.reserve(samples.size());
		for (const MemoryStats& sample : samples) {
			values.push_back(static_cast<double>(getter(sample)));
		}
		return values;
	}

	void WriteRenderSummary(JsonWriter& writer, const std::vector<RenderStats>& samples)
	{
		writer.StartObject();
		for (const RenderField& field : kRenderFields) {
			writer.Key(field.name);
			WriteDistribution(writer, ExtractRenderField(samples, field.member));
		}
		writer.EndObject();
	}

	void WriteRenderSamples(JsonWriter& writer, const std::vector<RenderStats>& samples)
	{
		writer.StartArray();
		for (const RenderStats& sample : samples) {
			writer.StartObject();
			for (const RenderField& field : kRenderFields) {
				writer.Key(field.name);
				writer.Uint64(sample.*(field.member));
			}
			writer.EndObject();
		}
		writer.EndArray();
	}

	void WriteMemorySummary(JsonWriter& writer, const std::vector<MemoryStats>& samples)
	{
		writer.StartObject();
		writer.Key("processWorkingSetBytes");
		WriteDistribution(writer, ExtractMemoryField(samples,
			[](const MemoryStats& value) { return value.processWorkingSetBytes; }));
		writer.Key("processPrivateBytes");
		WriteDistribution(writer, ExtractMemoryField(samples,
			[](const MemoryStats& value) { return value.processPrivateBytes; }));
		writer.Key("textureCacheHits");
		WriteDistribution(writer, ExtractMemoryField(samples,
			[](const MemoryStats& value) { return value.textureCacheHits; }));
		writer.Key("textureCacheMisses");
		WriteDistribution(writer, ExtractMemoryField(samples,
			[](const MemoryStats& value) { return value.textureCacheMisses; }));
		writer.Key("modelImportCacheHits");
		WriteDistribution(writer, ExtractMemoryField(samples,
			[](const MemoryStats& value) { return value.modelImportCacheHits; }));
		writer.Key("modelImportCacheMisses");
		WriteDistribution(writer, ExtractMemoryField(samples,
			[](const MemoryStats& value) { return value.modelImportCacheMisses; }));

		writer.Key("categories");
		writer.StartObject();
		for (std::size_t index = 0; index < static_cast<std::size_t>(MemoryResourceType::Count); ++index) {
			writer.Key(kMemoryCategoryNames[index]);
			writer.StartObject();
			writer.Key("currentBytes");
			WriteDistribution(writer, ExtractMemoryField(samples,
				[index](const MemoryStats& value) { return value.categories[index].currentBytes; }));
			writer.Key("peakBytes");
			WriteDistribution(writer, ExtractMemoryField(samples,
				[index](const MemoryStats& value) { return value.categories[index].peakBytes; }));
			writer.Key("resourceCount");
			WriteDistribution(writer, ExtractMemoryField(samples,
				[index](const MemoryStats& value) { return value.categories[index].resourceCount; }));
			writer.EndObject();
		}
		writer.EndObject();
		writer.EndObject();
	}

	void WriteMemorySamples(JsonWriter& writer, const std::vector<MemoryStats>& samples)
	{
		writer.StartArray();
		for (const MemoryStats& sample : samples) {
			writer.StartObject();
			writer.Key("processWorkingSetBytes"); writer.Uint64(sample.processWorkingSetBytes);
			writer.Key("processPrivateBytes"); writer.Uint64(sample.processPrivateBytes);
			writer.Key("textureCacheHits"); writer.Uint64(sample.textureCacheHits);
			writer.Key("textureCacheMisses"); writer.Uint64(sample.textureCacheMisses);
			writer.Key("modelImportCacheHits"); writer.Uint64(sample.modelImportCacheHits);
			writer.Key("modelImportCacheMisses"); writer.Uint64(sample.modelImportCacheMisses);
			writer.Key("categories");
			writer.StartObject();
			for (std::size_t index = 0; index < static_cast<std::size_t>(MemoryResourceType::Count); ++index) {
				const MemoryCategoryStats& category = sample.categories[index];
				writer.Key(kMemoryCategoryNames[index]);
				writer.StartObject();
				writer.Key("currentBytes"); writer.Uint64(category.currentBytes);
				writer.Key("peakBytes"); writer.Uint64(category.peakBytes);
				writer.Key("resourceCount"); writer.Uint64(category.resourceCount);
				writer.EndObject();
			}
			writer.EndObject();
			writer.EndObject();
		}
		writer.EndArray();
	}

	std::string CurrentUtcTimestamp()
	{
		const std::time_t now = std::time(nullptr);
		std::tm utc{};
#ifdef _WIN32
		gmtime_s(&utc, &now);
#else
		gmtime_r(&now, &utc);
#endif
		std::ostringstream stream;
		stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
		return stream.str();
	}
}

bool ParsePerformanceBenchmarkOptions(
	int argc,
	char** argv,
	PerformanceBenchmarkOptions& options,
	std::string& errorMessage)
{
	bool sawBenchmarkOption = false;
	for (int i = 1; i < argc; ++i) {
		const std::string argument = argv[i];
		if (argument == "--performance-benchmark") {
			options.enabled = true;
			continue;
		}

		auto readValue = [&](const char* optionName, std::string& value) -> bool {
			if (argument != optionName) {
				return false;
			}
			sawBenchmarkOption = true;
			if (i + 1 >= argc) {
				errorMessage = std::string(optionName) + " requires a value";
				return true;
			}
			value = argv[++i];
			return true;
		};

		std::string value;
		if (readValue("--benchmark-warmup-frames", value)) {
			if (!errorMessage.empty()) return false;
			if (!ParseNonNegativeInt(value, options.warmupFrames)) {
				errorMessage = "--benchmark-warmup-frames must be an integer from 0 to 10000000";
				return false;
			}
			continue;
		}
		if (readValue("--benchmark-sample-frames", value)) {
			if (!errorMessage.empty()) return false;
			if (!ParseNonNegativeInt(value, options.sampleFrames) || options.sampleFrames == 0) {
				errorMessage = "--benchmark-sample-frames must be an integer from 1 to 10000000";
				return false;
			}
			continue;
		}
		if (readValue("--benchmark-output", value)) {
			if (!errorMessage.empty()) return false;
			options.outputPath = value;
			continue;
		}
		if (readValue("--benchmark-label", value)) {
			if (!errorMessage.empty()) return false;
			options.label = value;
			continue;
		}
	}

	if (sawBenchmarkOption && !options.enabled) {
		errorMessage = "benchmark options require --performance-benchmark";
		return false;
	}
	if (options.enabled && options.outputPath.empty()) {
		errorMessage = "--benchmark-output must not be empty";
		return false;
	}
	return true;
}

PerformanceBenchmarkSession::PerformanceBenchmarkSession(
	const PerformanceBenchmarkOptions& options,
	Clock::time_point applicationStart)
	: m_options(options)
	, m_applicationStart(applicationStart)
{
}

void PerformanceBenchmarkSession::SetMetadata(const PerformanceBenchmarkMetadata& metadata)
{
	m_metadata = metadata;
}

void PerformanceBenchmarkSession::SetOpaqueSubmissionSignature(
	std::uint64_t signature)
{
	std::ostringstream stream;
	stream << "0x"
		<< std::hex
		<< std::setw(16)
		<< std::setfill('0')
		<< signature;
	m_metadata.opaqueSubmissionSignature = stream.str();
	m_metadata.opaqueSubmissionSignatureValid = true;
}

bool PerformanceBenchmarkSession::OnFrameBoundary(bool sceneReady)
{
	if (!m_options.enabled) {
		return true;
	}
	if (m_complete) {
		return false;
	}

	const Clock::time_point boundary = Clock::now();
	if (m_previousFrameWasWarmup) {
		++m_warmupFramesCompleted;
		m_previousFrameWasWarmup = false;
	}
	if (m_previousFrameWasSample) {
		const double wallFrameMs = std::chrono::duration<double, std::milli>(
			boundary - m_previousFrameStart).count();
		PerformanceProfiler::GetInstance().RecordBenchmarkWallFrame(wallFrameMs);
		++m_sampleFramesCompleted;
		m_previousFrameWasSample = false;
	}

	if (m_sampleFramesCompleted >= m_options.sampleFrames) {
		PerformanceProfiler::GetInstance().FinishBenchmarkCapture();
		m_captureStarted = false;
		m_success = WriteReport();
		m_complete = true;
		return false;
	}

	if (!sceneReady) {
		m_previousFrameStart = boundary;
		return true;
	}

	if (!m_loadReadyRecorded) {
		m_loadReadyMs = std::chrono::duration<double, std::milli>(
			boundary - m_applicationStart).count();
		m_loadReadyRecorded = true;
		std::cout << "[PerformanceBenchmark] load ready at "
			<< std::fixed << std::setprecision(3) << m_loadReadyMs << " ms" << std::endl;
	}

	if (m_warmupFramesCompleted < m_options.warmupFrames) {
		m_previousFrameWasWarmup = true;
		m_previousFrameStart = boundary;
		return true;
	}

	if (!m_captureStarted) {
		PerformanceProfiler::GetInstance().BeginBenchmarkCapture(
			static_cast<std::size_t>(m_options.sampleFrames));
		m_captureStarted = true;
		std::cout << "[PerformanceBenchmark] sampling " << m_options.sampleFrames
			<< " frames after " << m_options.warmupFrames << " warm-up frames" << std::endl;
		// BeginBenchmarkCapture drains warm-up GPU queries outside the measured range.
		m_previousFrameStart = Clock::now();
	}
	else {
		m_previousFrameStart = boundary;
	}
	m_previousFrameWasSample = true;
	return true;
}

void PerformanceBenchmarkSession::Abort()
{
	if (m_captureStarted) {
		PerformanceProfiler::GetInstance().FinishBenchmarkCapture();
		m_captureStarted = false;
	}
	m_complete = true;
	m_success = false;
}

bool PerformanceBenchmarkSession::WriteReport()
{
	const ProfilerBenchmarkSamples& samples =
		PerformanceProfiler::GetInstance().GetBenchmarkSamples();
	const std::size_t expectedSamples = static_cast<std::size_t>(m_options.sampleFrames);
	const bool captureValid =
		samples.wallFrameMs.size() == expectedSamples &&
		samples.cpuFrameMs.size() == expectedSamples &&
		samples.renderStats.size() == expectedSamples &&
		samples.memoryStats.size() == expectedSamples &&
		(!m_metadata.gpuTimingSupported || samples.gpuFrameMs.size() == expectedSamples);

	rapidjson::StringBuffer buffer;
	JsonWriter writer(buffer);
	writer.SetIndent(' ', 2);
	writer.StartObject();
	writer.Key("schemaVersion"); writer.Int(1);
	writer.Key("generatedAtUtc"); writer.String(CurrentUtcTimestamp().c_str());
	writer.Key("label"); writer.String(m_options.label.c_str());
	writer.Key("scene"); writer.String(m_metadata.scenePath.c_str());
	writer.Key("loadReadyMs"); writer.Double(m_loadReadyMs);

	writer.Key("capture");
	writer.StartObject();
	writer.Key("warmupFrames"); writer.Int(m_options.warmupFrames);
	writer.Key("requestedSampleFrames"); writer.Int(m_options.sampleFrames);
	writer.Key("capturedWallFrames"); writer.Uint64(samples.wallFrameMs.size());
	writer.Key("capturedCpuFrames"); writer.Uint64(samples.cpuFrameMs.size());
	writer.Key("capturedGpuFrames"); writer.Uint64(samples.gpuFrameMs.size());
	writer.Key("valid"); writer.Bool(captureValid);
	writer.Key("percentileMethod"); writer.String("nearest-rank");
	writer.EndObject();

	writer.Key("environment");
	writer.StartObject();
	writer.Key("buildConfiguration"); writer.String(m_metadata.buildConfiguration.c_str());
	writer.Key("architecture"); writer.String(m_metadata.architecture.c_str());
	writer.Key("glVendor"); writer.String(m_metadata.glVendor.c_str());
	writer.Key("glRenderer"); writer.String(m_metadata.glRenderer.c_str());
	writer.Key("glVersion"); writer.String(m_metadata.glVersion.c_str());
	writer.Key("width"); writer.Int(m_metadata.width);
	writer.Key("height"); writer.Int(m_metadata.height);
	writer.Key("windowSampleBuffers"); writer.Int(m_metadata.windowSampleBuffers);
	writer.Key("windowSamples"); writer.Int(m_metadata.windowSamples);
	writer.Key("requestedSwapInterval"); writer.Int(m_metadata.requestedSwapInterval);
	writer.Key("gpuTimingSupported"); writer.Bool(m_metadata.gpuTimingSupported);
	writer.EndObject();

	writer.Key("settings");
	writer.StartObject();
	writer.Key("bloom"); writer.Bool(m_metadata.bloom);
	writer.Key("deferredRendering"); writer.Bool(m_metadata.deferredRendering);
	writer.Key("ssao"); writer.Bool(m_metadata.ssao);
	writer.Key("forwardNormalBuffer"); writer.Bool(m_metadata.forwardNormalBuffer);
	writer.Key("gammaCorrection"); writer.Bool(m_metadata.gammaCorrection);
	writer.Key("autoReloadShaders"); writer.Bool(m_metadata.autoReloadShaders);
	writer.Key("autoReloadMaterials"); writer.Bool(m_metadata.autoReloadMaterials);
	writer.Key("inputFrozen"); writer.Bool(m_metadata.inputFrozen);
	writer.Key("pointLights"); writer.Int(m_metadata.pointLights);
	writer.Key("directionLights"); writer.Int(m_metadata.directionLights);
	writer.Key("spotLights"); writer.Int(m_metadata.spotLights);
	writer.Key("shadowCastingLights"); writer.Int(m_metadata.shadowCastingLights);
	writer.Key("opaqueSortMode");
	writer.String(m_metadata.opaqueSortMode.c_str());
	writer.Key("opaqueSubmissionSignature");
	if (m_metadata.opaqueSubmissionSignatureValid) {
		writer.String(
			m_metadata.opaqueSubmissionSignature.c_str());
	}
	else {
		writer.Null();
	}
	writer.Key("submissionStressScene");
	writer.Bool(m_metadata.submissionStressScene);
	if (m_metadata.submissionStressScene) {
		writer.Key("submissionStressObjectCount");
		writer.Int(m_metadata.submissionStressObjectCount);
		writer.Key("submissionStressDynamicObjectCount");
		writer.Int(m_metadata.submissionStressDynamicObjectCount);
		writer.Key("submissionStressMaterialCount");
		writer.Int(m_metadata.submissionStressMaterialCount);
		writer.Key("submissionStressSeed");
		writer.Uint(m_metadata.submissionStressSeed);
		writer.Key("submissionStressRenderPath");
		writer.String(m_metadata.submissionStressRenderPath.c_str());
		writer.Key("submissionStressGeometrySet");
		writer.String(
			m_metadata.submissionStressGeometrySet.c_str());
		writer.Key("submissionStressCollectionBreakdown");
		writer.Bool(
			m_metadata.submissionStressCollectionBreakdown);
	}
	writer.EndObject();

	writer.Key("summary");
	writer.StartObject();
	writer.Key("wallFrameMs"); WriteDistribution(writer, samples.wallFrameMs);
	writer.Key("cpuFrameMs"); WriteDistribution(writer, samples.cpuFrameMs);
	writer.Key("gpuFrameMs"); WriteDistribution(writer, samples.gpuFrameMs);
	writer.Key("cpuZonesMs"); WriteZoneSummary(writer, samples.cpuZoneMs);
	writer.Key("gpuZonesMs"); WriteZoneSummary(writer, samples.gpuZoneMs);
	writer.Key("renderStats"); WriteRenderSummary(writer, samples.renderStats);
	writer.Key("memory"); WriteMemorySummary(writer, samples.memoryStats);
	writer.EndObject();

	writer.Key("samples");
	writer.StartObject();
	writer.Key("wallFrameMs"); WriteDoubleArray(writer, samples.wallFrameMs);
	writer.Key("cpuFrameMs"); WriteDoubleArray(writer, samples.cpuFrameMs);
	writer.Key("gpuFrameMs"); WriteDoubleArray(writer, samples.gpuFrameMs);
	writer.Key("cpuZonesMs"); WriteZoneSamples(writer, samples.cpuZoneMs);
	writer.Key("gpuZonesMs"); WriteZoneSamples(writer, samples.gpuZoneMs);
	writer.Key("renderStats"); WriteRenderSamples(writer, samples.renderStats);
	writer.Key("memory"); WriteMemorySamples(writer, samples.memoryStats);
	writer.EndObject();
	writer.EndObject();

	std::error_code directoryError;
	const std::filesystem::path outputPath(m_options.outputPath);
	if (outputPath.has_parent_path()) {
		std::filesystem::create_directories(outputPath.parent_path(), directoryError);
		if (directoryError) {
			std::cerr << "[PerformanceBenchmark] failed to create output directory: "
				<< directoryError.message() << std::endl;
			return false;
		}
	}

	std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
	if (!output) {
		std::cerr << "[PerformanceBenchmark] failed to open output: "
			<< outputPath.string() << std::endl;
		return false;
	}
	output.write(buffer.GetString(), static_cast<std::streamsize>(buffer.GetSize()));
	output.put('\n');
	output.close();
	if (!output) {
		std::cerr << "[PerformanceBenchmark] failed to write output: "
			<< outputPath.string() << std::endl;
		return false;
	}

	const DistributionStats wall = Summarize(samples.wallFrameMs);
	const DistributionStats cpu = Summarize(samples.cpuFrameMs);
	const DistributionStats gpu = Summarize(samples.gpuFrameMs);
	std::cout << "[PerformanceBenchmark] report=" << outputPath.string()
		<< " wallMedianMs=" << wall.median
		<< " cpuMedianMs=" << cpu.median
		<< " gpuMedianMs=" << gpu.median
		<< " cpuSamples=" << cpu.count
		<< " gpuSamples=" << gpu.count
		<< " valid=" << (captureValid ? "true" : "false")
		<< std::endl;
	if (!captureValid) {
		std::cerr << "[PerformanceBenchmark] capture is incomplete; report must not be used for A/B"
			<< std::endl;
	}
	return captureValid;
}
