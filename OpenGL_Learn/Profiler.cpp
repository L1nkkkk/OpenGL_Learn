#include "Profiler.h"

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#include <Psapi.h>
#pragma comment(lib, "Psapi.lib")
#endif

#include <glad/glad.h>

#include <algorithm>
#include <cmath>

namespace {
	constexpr unsigned int kGlPoints = 0x0000;
	constexpr unsigned int kGlLines = 0x0001;
	constexpr unsigned int kGlLineLoop = 0x0002;
	constexpr unsigned int kGlLineStrip = 0x0003;
	constexpr unsigned int kGlTriangles = 0x0004;
	constexpr unsigned int kGlTriangleStrip = 0x0005;
	constexpr unsigned int kGlTriangleFan = 0x0006;

	std::uint64_t CountTriangles(unsigned int primitiveMode, std::uint64_t vertexCount)
	{
		switch (primitiveMode) {
		case kGlTriangles:
			return vertexCount / 3;
		case kGlTriangleStrip:
		case kGlTriangleFan:
			return vertexCount >= 3 ? vertexCount - 2 : 0;
		case kGlPoints:
		case kGlLines:
		case kGlLineLoop:
		case kGlLineStrip:
		default:
			return 0;
		}
	}
}

PerformanceProfiler& PerformanceProfiler::GetInstance()
{
	static PerformanceProfiler instance;
	return instance;
}

void PerformanceProfiler::Initialize()
{
	if (m_initialized) {
		return;
	}

	m_initialized = true;
	m_gpuTimingSupported =
		glQueryCounter != nullptr &&
		glGetQueryObjectiv != nullptr &&
		glGetQueryObjectui64v != nullptr &&
		glGenQueries != nullptr &&
		glDeleteQueries != nullptr;
	m_gpuTimingEnabled = m_gpuTimingSupported;
}

void PerformanceProfiler::Shutdown()
{
	if (!m_initialized) {
		return;
	}

	for (auto& runtime : m_gpuZoneRuntime) {
		for (auto& slot : runtime.slots) {
			if (slot.startQuery != 0) {
				glDeleteQueries(1, &slot.startQuery);
				slot.startQuery = 0;
			}
			if (slot.endQuery != 0) {
				glDeleteQueries(1, &slot.endQuery);
				slot.endQuery = 0;
			}
			slot.pending = false;
			slot.benchmarkSample = false;
		}
	}

	m_gpuZoneRuntime.clear();
	m_gpuZoneStats.clear();
	m_gpuZoneIndices.clear();
	m_gpuTimingSupported = false;
	m_gpuTimingEnabled = false;
	m_frameActive = false;
	m_benchmarkCaptureActive = false;
	m_benchmarkSamples = {};
	m_initialized = false;
}

void PerformanceProfiler::BeginFrame()
{
	if (!m_initialized) {
		Initialize();
	}

	if (m_gpuTimingSupported) {
		ResolveGpuQueries();
	}
	UpdateProcessMemoryStats();

	if (!m_enabled) {
		m_frameActive = false;
		return;
	}

	m_currentRenderStats = {};
	for (std::size_t i = 0; i < m_cpuZoneAccumulators.size(); ++i) {
		m_cpuZoneAccumulators[i] = 0.0;
		m_cpuZoneHitCounts[i] = 0;
	}

	m_frameStart = std::chrono::steady_clock::now();
	m_frameActive = true;
}

void PerformanceProfiler::UpdateProcessMemoryStats()
{
	const auto now = std::chrono::steady_clock::now();
	if (m_lastMemoryPoll.time_since_epoch().count() != 0 &&
		std::chrono::duration<double>(now - m_lastMemoryPoll).count() < 0.5) {
		return;
	}
	m_lastMemoryPoll = now;

#ifdef _WIN32
	PROCESS_MEMORY_COUNTERS_EX counters{};
	counters.cb = sizeof(counters);
	if (GetProcessMemoryInfo(
		GetCurrentProcess(),
		reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
		sizeof(counters))) {
		m_memoryStats.processWorkingSetBytes = static_cast<std::uint64_t>(counters.WorkingSetSize);
		m_memoryStats.processPrivateBytes = static_cast<std::uint64_t>(counters.PrivateUsage);
	}
#endif
}

void PerformanceProfiler::EndFrame()
{
	if (!m_frameActive) {
		return;
	}

	const auto now = std::chrono::steady_clock::now();
	const double frameMs = std::chrono::duration<double, std::milli>(now - m_frameStart).count();
	AddCpuSample("CPU Frame", frameMs);
	FinalizeCpuZones();

	m_lastRenderStats = m_currentRenderStats;
	AddFrameHistorySample(m_cpuFrameHistory, frameMs);
	if (m_benchmarkCaptureActive) {
		m_benchmarkSamples.cpuFrameMs.push_back(frameMs);
		m_benchmarkSamples.renderStats.push_back(m_currentRenderStats);
		m_benchmarkSamples.memoryStats.push_back(m_memoryStats);
	}
	UpdateFrameSummary();
	m_frameActive = false;
}

void PerformanceProfiler::SetGpuTimingEnabled(bool enabled)
{
	m_gpuTimingEnabled = enabled && m_gpuTimingSupported;
}

std::size_t PerformanceProfiler::GetOrCreateCpuZone(const char* name)
{
	const std::string key = name ? name : "Unnamed CPU Zone";
	const auto it = m_cpuZoneIndices.find(key);
	if (it != m_cpuZoneIndices.end()) {
		return it->second;
	}

	const std::size_t index = m_cpuZoneStats.size();
	m_cpuZoneIndices.emplace(key, index);
	m_cpuZoneStats.push_back({ key });
	m_cpuZoneAccumulators.push_back(0.0);
	m_cpuZoneHitCounts.push_back(0);
	return index;
}

std::size_t PerformanceProfiler::GetOrCreateGpuZone(const char* name)
{
	const std::string key = name ? name : "Unnamed GPU Zone";
	const auto it = m_gpuZoneIndices.find(key);
	if (it != m_gpuZoneIndices.end()) {
		return it->second;
	}

	const std::size_t index = m_gpuZoneStats.size();
	m_gpuZoneIndices.emplace(key, index);
	m_gpuZoneStats.push_back({ key });
	m_gpuZoneRuntime.emplace_back();

	for (auto& slot : m_gpuZoneRuntime.back().slots) {
		glGenQueries(1, &slot.startQuery);
		glGenQueries(1, &slot.endQuery);
	}

	return index;
}

void PerformanceProfiler::AddCpuSample(const char* name, double elapsedMs)
{
	if (!IsCollecting()) {
		return;
	}

	const std::size_t index = GetOrCreateCpuZone(name);
	m_cpuZoneAccumulators[index] += elapsedMs;
	++m_cpuZoneHitCounts[index];
}

PerformanceProfiler::GpuScopeToken PerformanceProfiler::BeginGpuScope(const char* name)
{
	GpuScopeToken token;
	if (!IsCollecting() || !m_gpuTimingEnabled || !m_gpuTimingSupported) {
		return token;
	}

	const std::size_t zoneIndex = GetOrCreateGpuZone(name);
	auto& runtime = m_gpuZoneRuntime[zoneIndex];

	for (std::size_t offset = 0; offset < kGpuQueryLatency; ++offset) {
		const std::size_t slotIndex = (runtime.nextSlot + offset) % kGpuQueryLatency;
		auto& slot = runtime.slots[slotIndex];
		if (slot.pending) {
			continue;
		}

		glQueryCounter(slot.startQuery, GL_TIMESTAMP);
		slot.benchmarkSample = m_benchmarkCaptureActive;
		runtime.nextSlot = (slotIndex + 1) % kGpuQueryLatency;
		token.zoneIndex = zoneIndex;
		token.slotIndex = slotIndex;
		token.active = true;
		return token;
	}

	return token;
}

void PerformanceProfiler::EndGpuScope(const GpuScopeToken& token)
{
	if (!token.active ||
		token.zoneIndex >= m_gpuZoneRuntime.size() ||
		token.slotIndex >= kGpuQueryLatency) {
		return;
	}

	auto& slot = m_gpuZoneRuntime[token.zoneIndex].slots[token.slotIndex];
	glQueryCounter(slot.endQuery, GL_TIMESTAMP);
	slot.pending = true;
}

void PerformanceProfiler::ResolveGpuQueries(bool waitForResults)
{
	for (std::size_t zoneIndex = 0; zoneIndex < m_gpuZoneRuntime.size(); ++zoneIndex) {
		auto& runtime = m_gpuZoneRuntime[zoneIndex];
		// nextSlot points just past the newest issued query. Walking from it in a
		// ring preserves submission order when several tail queries resolve at once.
		for (std::size_t offset = 0; offset < kGpuQueryLatency; ++offset) {
			const std::size_t slotIndex = (runtime.nextSlot + offset) % kGpuQueryLatency;
			auto& slot = runtime.slots[slotIndex];
			if (!slot.pending) {
				continue;
			}

			if (!waitForResults) {
				int available = GL_FALSE;
				glGetQueryObjectiv(slot.endQuery, GL_QUERY_RESULT_AVAILABLE, &available);
				if (available != GL_TRUE) {
					continue;
				}
			}

			GLuint64 startTimestamp = 0;
			GLuint64 endTimestamp = 0;
			glGetQueryObjectui64v(slot.startQuery, GL_QUERY_RESULT, &startTimestamp);
			glGetQueryObjectui64v(slot.endQuery, GL_QUERY_RESULT, &endTimestamp);
			const bool benchmarkSample = slot.benchmarkSample;
			slot.pending = false;
			slot.benchmarkSample = false;

			if (endTimestamp < startTimestamp) {
				continue;
			}

			const double elapsedMs = static_cast<double>(endTimestamp - startTimestamp) / 1000000.0;
			UpdateZoneStats(m_gpuZoneStats[zoneIndex], elapsedMs);
			if (benchmarkSample) {
				const std::string& zoneName = m_gpuZoneStats[zoneIndex].name;
				if (zoneName == "GPU Frame") {
					m_benchmarkSamples.gpuFrameMs.push_back(elapsedMs);
				}
				else {
					m_benchmarkSamples.gpuZoneMs[zoneName].push_back(elapsedMs);
				}
			}
			if (m_gpuZoneStats[zoneIndex].name == "GPU Frame") {
				m_frameSummary.gpuFrameMs = elapsedMs;
				AddFrameHistorySample(m_gpuFrameHistory, elapsedMs);
			}
		}
	}

	UpdateFrameSummary();
}

void PerformanceProfiler::FinalizeCpuZones()
{
	for (std::size_t i = 0; i < m_cpuZoneStats.size(); ++i) {
		if (m_cpuZoneHitCounts[i] == 0) {
			m_cpuZoneStats[i].latestMs = 0.0;
			continue;
		}
		UpdateZoneStats(m_cpuZoneStats[i], m_cpuZoneAccumulators[i]);
		if (m_benchmarkCaptureActive && m_cpuZoneStats[i].name != "CPU Frame") {
			m_benchmarkSamples.cpuZoneMs[m_cpuZoneStats[i].name].push_back(
				m_cpuZoneAccumulators[i]);
		}
	}
}

void PerformanceProfiler::BeginBenchmarkCapture(std::size_t expectedFrameCount)
{
	if (!m_initialized) {
		Initialize();
	}

	// Drain warm-up timestamps before tagging measured scopes. This synchronization
	// happens outside the measured interval and prevents warm-up samples from
	// consuming the small asynchronous query ring during the first capture frames.
	if (m_gpuTimingSupported) {
		glFinish();
		ResolveGpuQueries(true);
	}

	m_benchmarkSamples = {};
	m_benchmarkSamples.wallFrameMs.reserve(expectedFrameCount);
	m_benchmarkSamples.cpuFrameMs.reserve(expectedFrameCount);
	m_benchmarkSamples.gpuFrameMs.reserve(expectedFrameCount);
	m_benchmarkSamples.renderStats.reserve(expectedFrameCount);
	m_benchmarkSamples.memoryStats.reserve(expectedFrameCount);
	for (const auto& stats : m_cpuZoneStats) {
		if (stats.name != "CPU Frame") {
			m_benchmarkSamples.cpuZoneMs[stats.name].reserve(expectedFrameCount);
		}
	}
	for (const auto& stats : m_gpuZoneStats) {
		if (stats.name != "GPU Frame") {
			m_benchmarkSamples.gpuZoneMs[stats.name].reserve(expectedFrameCount);
		}
	}
	m_benchmarkCaptureActive = true;
}

void PerformanceProfiler::RecordBenchmarkWallFrame(double elapsedMs)
{
	if (m_benchmarkCaptureActive) {
		m_benchmarkSamples.wallFrameMs.push_back(elapsedMs);
	}
}

void PerformanceProfiler::FinishBenchmarkCapture()
{
	if (!m_benchmarkCaptureActive) {
		return;
	}

	m_benchmarkCaptureActive = false;
	if (m_gpuTimingSupported) {
		// The measured frames have already ended. Waiting here only guarantees that
		// every timestamp belonging to the capture reaches the exported report.
		glFinish();
		ResolveGpuQueries(true);
	}
}

void PerformanceProfiler::UpdateZoneStats(ProfilerZoneStats& stats, double elapsedMs)
{
	constexpr double kSmoothing = 0.1;
	stats.latestMs = elapsedMs;
	if (stats.sampleCount == 0) {
		stats.averageMs = elapsedMs;
		stats.peakMs = elapsedMs;
	}
	else {
		stats.averageMs += (elapsedMs - stats.averageMs) * kSmoothing;
		stats.peakMs = (std::max)(stats.peakMs, elapsedMs);
	}
	++stats.sampleCount;
}

void PerformanceProfiler::RecordDraw(unsigned int primitiveMode, std::uint64_t vertexCount, std::uint64_t instanceCount)
{
	if (!IsCollecting()) {
		return;
	}

	++m_currentRenderStats.drawCalls;
	m_currentRenderStats.submittedVertices += vertexCount * instanceCount;
	m_currentRenderStats.submittedTriangles += CountTriangles(primitiveMode, vertexCount) * instanceCount;
}

void PerformanceProfiler::SetDeferredPointLightStats(
	std::uint64_t total,
	std::uint64_t active,
	std::uint64_t submitted,
	std::uint64_t culled)
{
	if (!IsCollecting()) return;
	m_currentRenderStats.pointLightsTotal = total;
	m_currentRenderStats.pointLightsActive = active;
	m_currentRenderStats.pointLightsSubmitted = submitted;
	m_currentRenderStats.pointLightsCulled = culled;
}

void PerformanceProfiler::SetDeferredPointLightPathStats(
	std::uint64_t boundsRect,
	std::uint64_t boundsOutside,
	std::uint64_t boundsFullscreenFallback,
	std::uint64_t fallbackCameraInside,
	std::uint64_t fallbackNearPlane,
	std::uint64_t fallbackInvalid,
	std::uint64_t volumeCount,
	std::uint64_t screenCount,
	std::uint64_t stencilDraws,
	std::uint64_t lightingVolumeDraws,
	std::uint64_t screenDraws,
	std::uint64_t rectPixelArea,
	std::uint64_t stencilClearPixelArea)
{
	if (!IsCollecting()) return;
	auto& stats = m_currentRenderStats;
	stats.pointLightBoundsRect = boundsRect;
	stats.pointLightBoundsOutside = boundsOutside;
	stats.pointLightBoundsFullscreenFallback = boundsFullscreenFallback;
	stats.pointLightFallbackCameraInside = fallbackCameraInside;
	stats.pointLightFallbackNearPlane = fallbackNearPlane;
	stats.pointLightFallbackInvalid = fallbackInvalid;
	stats.pointLightVolumeCount = volumeCount;
	stats.pointLightScreenCount = screenCount;
	stats.pointLightStencilDraws = stencilDraws;
	stats.pointLightLightingVolumeDraws = lightingVolumeDraws;
	stats.pointLightScreenDraws = screenDraws;
	stats.pointLightRectPixelArea = rectPixelArea;
	stats.pointLightStencilClearPixelArea = stencilClearPixelArea;
}

void PerformanceProfiler::RecordStencilClear(bool pointLightPhase)
{
	if (!IsCollecting()) return;
	++m_currentRenderStats.stencilClears;
	if (pointLightPhase) {
		++m_currentRenderStats.pointLightStencilClears;
	}
	else {
		++m_currentRenderStats.fixedStencilClears;
	}
}

void PerformanceProfiler::RecordShaderBind()
{
	if (IsCollecting()) {
		++m_currentRenderStats.shaderBinds;
	}
}

void PerformanceProfiler::RecordUniformUpdate()
{
	if (IsCollecting()) {
		++m_currentRenderStats.uniformUpdates;
	}
}

void PerformanceProfiler::RecordUniformLocationLookup(bool cacheHit)
{
	if (!IsCollecting()) {
		return;
	}

	if (cacheHit) {
		++m_currentRenderStats.uniformLocationCacheHits;
	}
	else {
		++m_currentRenderStats.uniformLocationQueries;
	}
}

void PerformanceProfiler::RecordMaterialBind(bool cacheHit)
{
	if (!IsCollecting()) {
		return;
	}

	if (cacheHit) {
		++m_currentRenderStats.materialBindCacheHits;
	}
	else {
		++m_currentRenderStats.materialBinds;
	}
}

void PerformanceProfiler::RecordRenderStateChange(bool cacheHit)
{
	if (!IsCollecting()) {
		return;
	}

	if (cacheHit) {
		++m_currentRenderStats.renderStateCacheHits;
	}
	else {
		++m_currentRenderStats.renderStateChanges;
	}
}

void PerformanceProfiler::RecordRenderStateQuery()
{
	if (IsCollecting()) {
		++m_currentRenderStats.renderStateQueries;
	}
}

void PerformanceProfiler::RecordTextureStateChange(bool cacheHit)
{
	if (!IsCollecting()) {
		return;
	}

	if (cacheHit) {
		++m_currentRenderStats.textureStateCacheHits;
	}
	else {
		++m_currentRenderStats.textureStateChanges;
	}
}

void PerformanceProfiler::RecordVertexArrayBind(bool cacheHit)
{
	if (!IsCollecting()) {
		return;
	}

	if (cacheHit) {
		++m_currentRenderStats.vertexArrayBindCacheHits;
	}
	else {
		++m_currentRenderStats.vertexArrayBinds;
	}
}

void PerformanceProfiler::RecordFramebufferBind(bool cacheHit)
{
	if (!IsCollecting()) {
		return;
	}

	if (cacheHit) {
		++m_currentRenderStats.framebufferBindCacheHits;
	}
	else {
		++m_currentRenderStats.framebufferBinds;
	}
}

void PerformanceProfiler::RecordFileSystemCheck()
{
	if (IsCollecting()) {
		++m_currentRenderStats.fileSystemChecks;
	}
}

void PerformanceProfiler::RecordAssetBrowserCacheLookup(bool cacheHit)
{
	if (!IsCollecting()) {
		return;
	}

	if (cacheHit) {
		++m_currentRenderStats.assetBrowserCacheHits;
	}
	else {
		++m_currentRenderStats.assetBrowserCacheMisses;
	}
}

void PerformanceProfiler::SetSceneSubmissionStats(
	std::uint64_t activeModels,
	std::uint64_t visibleModels,
	std::uint64_t culledModels,
	std::uint64_t culledMeshes,
	std::uint64_t opaqueMeshes,
	std::uint64_t transparentMeshes)
{
	if (!IsCollecting()) {
		return;
	}
	m_currentRenderStats.activeModels = activeModels;
	m_currentRenderStats.visibleModels = visibleModels;
	m_currentRenderStats.culledModels = culledModels;
	m_currentRenderStats.culledMeshes = culledMeshes;
	m_currentRenderStats.opaqueMeshes = opaqueMeshes;
	m_currentRenderStats.transparentMeshes = transparentMeshes;
}

void PerformanceProfiler::RecordUiDrawData(
	std::uint64_t drawCalls,
	std::uint64_t vertices,
	std::uint64_t indices)
{
	if (!IsCollecting()) {
		return;
	}
	m_currentRenderStats.uiDrawCalls = drawCalls;
	m_currentRenderStats.uiVertices = vertices;
	m_currentRenderStats.uiIndices = indices;
}

void PerformanceProfiler::RecordMemoryAllocation(MemoryResourceType type, std::uint64_t bytes)
{
	const std::size_t index = static_cast<std::size_t>(type);
	if (index >= m_memoryStats.categories.size()) {
		return;
	}
	auto& category = m_memoryStats.categories[index];
	category.currentBytes += bytes;
	category.peakBytes = (std::max)(category.peakBytes, category.currentBytes);
	++category.resourceCount;
}

void PerformanceProfiler::RecordMemoryRelease(MemoryResourceType type, std::uint64_t bytes)
{
	const std::size_t index = static_cast<std::size_t>(type);
	if (index >= m_memoryStats.categories.size()) {
		return;
	}
	auto& category = m_memoryStats.categories[index];
	category.currentBytes = bytes >= category.currentBytes ? 0 : category.currentBytes - bytes;
	if (category.resourceCount > 0) {
		--category.resourceCount;
	}
}

void PerformanceProfiler::RecordTextureCacheLookup(bool cacheHit)
{
	if (cacheHit) {
		++m_memoryStats.textureCacheHits;
	}
	else {
		++m_memoryStats.textureCacheMisses;
	}
}

void PerformanceProfiler::RecordModelImportCacheLookup(bool cacheHit)
{
	if (cacheHit) {
		++m_memoryStats.modelImportCacheHits;
	}
	else {
		++m_memoryStats.modelImportCacheMisses;
	}
}

void PerformanceProfiler::AddFrameHistorySample(std::vector<float>& history, double elapsedMs)
{
	if (history.size() >= kFrameHistorySize) {
		history.erase(history.begin());
	}
	history.push_back(static_cast<float>(elapsedMs));
}

double PerformanceProfiler::CalculateAverage(const std::vector<float>& values)
{
	if (values.empty()) {
		return 0.0;
	}

	double total = 0.0;
	for (float value : values) {
		total += value;
	}
	return total / static_cast<double>(values.size());
}

double PerformanceProfiler::CalculatePercentile(const std::vector<float>& values, double percentile)
{
	if (values.empty()) {
		return 0.0;
	}

	std::vector<float> sorted = values;
	std::sort(sorted.begin(), sorted.end());
	const double normalized = (std::max)(0.0, (std::min)(1.0, percentile));
	const std::size_t rank = static_cast<std::size_t>(
		std::ceil(normalized * static_cast<double>(sorted.size())));
	const std::size_t index = rank == 0 ? 0 : rank - 1;
	return sorted[(std::min)(index, sorted.size() - 1)];
}

void PerformanceProfiler::UpdateFrameSummary()
{
	if (!m_cpuFrameHistory.empty()) {
		m_frameSummary.cpuFrameMs = m_cpuFrameHistory.back();
	}
	if (!m_gpuFrameHistory.empty()) {
		m_frameSummary.gpuFrameMs = m_gpuFrameHistory.back();
	}
	m_frameSummary.averageCpuFrameMs = CalculateAverage(m_cpuFrameHistory);
	m_frameSummary.averageGpuFrameMs = CalculateAverage(m_gpuFrameHistory);
	m_frameSummary.cpuP95Ms = CalculatePercentile(m_cpuFrameHistory, 0.95);
	m_frameSummary.cpuP99Ms = CalculatePercentile(m_cpuFrameHistory, 0.99);
}

void PerformanceProfiler::ResetStatistics()
{
	m_cpuFrameHistory.clear();
	m_gpuFrameHistory.clear();
	m_frameSummary = {};
	m_lastRenderStats = {};
	m_memoryStats.textureCacheHits = 0;
	m_memoryStats.textureCacheMisses = 0;
	m_memoryStats.modelImportCacheHits = 0;
	m_memoryStats.modelImportCacheMisses = 0;
	for (auto& category : m_memoryStats.categories) {
		category.peakBytes = category.currentBytes;
	}

	for (auto& stats : m_cpuZoneStats) {
		stats.latestMs = 0.0;
		stats.averageMs = 0.0;
		stats.peakMs = 0.0;
		stats.sampleCount = 0;
	}
	for (auto& stats : m_gpuZoneStats) {
		stats.latestMs = 0.0;
		stats.averageMs = 0.0;
		stats.peakMs = 0.0;
		stats.sampleCount = 0;
	}
}

ProfilerFrameScope::ProfilerFrameScope()
{
	PerformanceProfiler::GetInstance().BeginFrame();
}

ProfilerFrameScope::~ProfilerFrameScope()
{
	PerformanceProfiler::GetInstance().EndFrame();
}

CpuProfileScope::CpuProfileScope(const char* name)
	: m_name(name)
{
	m_active = PerformanceProfiler::GetInstance().IsCollecting();
	if (m_active) {
		m_start = std::chrono::steady_clock::now();
	}
}

CpuProfileScope::~CpuProfileScope()
{
	if (!m_active) {
		return;
	}

	const auto now = std::chrono::steady_clock::now();
	const double elapsedMs = std::chrono::duration<double, std::milli>(now - m_start).count();
	PerformanceProfiler::GetInstance().AddCpuSample(m_name, elapsedMs);
}

GpuProfileScope::GpuProfileScope(const char* name)
	: m_token(PerformanceProfiler::GetInstance().BeginGpuScope(name))
{
}

GpuProfileScope::~GpuProfileScope()
{
	PerformanceProfiler::GetInstance().EndGpuScope(m_token);
}
