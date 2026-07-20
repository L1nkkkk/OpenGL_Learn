#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct ProfilerZoneStats {
	std::string name;
	double latestMs = 0.0;
	double averageMs = 0.0;
	double peakMs = 0.0;
	std::uint64_t sampleCount = 0;
};

struct ProfilerFrameSummary {
	double cpuFrameMs = 0.0;
	double gpuFrameMs = 0.0;
	double averageCpuFrameMs = 0.0;
	double averageGpuFrameMs = 0.0;
	double cpuP95Ms = 0.0;
	double cpuP99Ms = 0.0;
};

struct RenderStats {
	std::uint64_t drawCalls = 0;
	std::uint64_t submittedVertices = 0;
	std::uint64_t submittedTriangles = 0;
	std::uint64_t shaderBinds = 0;
	std::uint64_t uniformUpdates = 0;
	std::uint64_t uniformLocationQueries = 0;
	std::uint64_t uniformLocationCacheHits = 0;
	std::uint64_t materialBinds = 0;
	std::uint64_t materialBindCacheHits = 0;
	std::uint64_t renderStateChanges = 0;
	std::uint64_t renderStateCacheHits = 0;
	std::uint64_t renderStateQueries = 0;
	std::uint64_t textureStateChanges = 0;
	std::uint64_t textureStateCacheHits = 0;
	std::uint64_t vertexArrayBinds = 0;
	std::uint64_t vertexArrayBindCacheHits = 0;
	std::uint64_t framebufferBinds = 0;
	std::uint64_t framebufferBindCacheHits = 0;
	std::uint64_t fileSystemChecks = 0;
	std::uint64_t activeModels = 0;
	std::uint64_t visibleModels = 0;
	std::uint64_t culledModels = 0;
	std::uint64_t culledMeshes = 0;
	std::uint64_t opaqueMeshes = 0;
	std::uint64_t transparentMeshes = 0;
	std::uint64_t uiDrawCalls = 0;
	std::uint64_t uiVertices = 0;
	std::uint64_t uiIndices = 0;
};

enum class MemoryResourceType : std::size_t {
	Texture = 0,
	MeshCpu,
	MeshGpu,
	RenderTarget,
	Count
};

struct MemoryCategoryStats {
	std::uint64_t currentBytes = 0;
	std::uint64_t peakBytes = 0;
	std::uint64_t resourceCount = 0;
};

struct MemoryStats {
	std::uint64_t processWorkingSetBytes = 0;
	std::uint64_t processPrivateBytes = 0;
	std::array<MemoryCategoryStats, static_cast<std::size_t>(MemoryResourceType::Count)> categories{};
	std::uint64_t textureCacheHits = 0;
	std::uint64_t textureCacheMisses = 0;
	std::uint64_t modelImportCacheHits = 0;
	std::uint64_t modelImportCacheMisses = 0;
};

struct ProfilerBenchmarkSamples {
	std::vector<double> wallFrameMs;
	std::vector<double> cpuFrameMs;
	std::vector<double> gpuFrameMs;
	std::unordered_map<std::string, std::vector<double>> cpuZoneMs;
	std::unordered_map<std::string, std::vector<double>> gpuZoneMs;
	std::vector<RenderStats> renderStats;
	std::vector<MemoryStats> memoryStats;
};

class PerformanceProfiler {
public:
	struct GpuScopeToken {
		std::size_t zoneIndex = static_cast<std::size_t>(-1);
		std::size_t slotIndex = static_cast<std::size_t>(-1);
		bool active = false;
	};

	static PerformanceProfiler& GetInstance();

	PerformanceProfiler(const PerformanceProfiler&) = delete;
	PerformanceProfiler& operator=(const PerformanceProfiler&) = delete;

	void Initialize();
	void Shutdown();

	void BeginFrame();
	void EndFrame();

	void SetEnabled(bool enabled) { m_enabled = enabled; }
	bool IsEnabled() const { return m_enabled; }
	bool IsCollecting() const { return m_frameActive && m_enabled; }

	void SetGpuTimingEnabled(bool enabled);
	bool IsGpuTimingEnabled() const { return m_gpuTimingEnabled; }
	bool IsGpuTimingSupported() const { return m_gpuTimingSupported; }

	void AddCpuSample(const char* name, double elapsedMs);
	GpuScopeToken BeginGpuScope(const char* name);
	void EndGpuScope(const GpuScopeToken& token);

	void RecordDraw(unsigned int primitiveMode, std::uint64_t vertexCount, std::uint64_t instanceCount = 1);
	void RecordShaderBind();
	void RecordUniformUpdate();
	void RecordUniformLocationLookup(bool cacheHit);
	void RecordMaterialBind(bool cacheHit);
	void RecordRenderStateChange(bool cacheHit);
	void RecordRenderStateQuery();
	void RecordTextureStateChange(bool cacheHit);
	void RecordVertexArrayBind(bool cacheHit);
	void RecordFramebufferBind(bool cacheHit);
	void RecordFileSystemCheck();
	void SetSceneSubmissionStats(
		std::uint64_t activeModels,
		std::uint64_t visibleModels,
		std::uint64_t culledModels,
		std::uint64_t culledMeshes,
		std::uint64_t opaqueMeshes,
		std::uint64_t transparentMeshes);
	void RecordUiDrawData(std::uint64_t drawCalls, std::uint64_t vertices, std::uint64_t indices);
	void RecordMemoryAllocation(MemoryResourceType type, std::uint64_t bytes);
	void RecordMemoryRelease(MemoryResourceType type, std::uint64_t bytes);
	void RecordTextureCacheLookup(bool cacheHit);
	void RecordModelImportCacheLookup(bool cacheHit);
	void BeginBenchmarkCapture(std::size_t expectedFrameCount);
	void RecordBenchmarkWallFrame(double elapsedMs);
	void FinishBenchmarkCapture();
	bool IsBenchmarkCaptureActive() const { return m_benchmarkCaptureActive; }
	const ProfilerBenchmarkSamples& GetBenchmarkSamples() const { return m_benchmarkSamples; }

	const ProfilerFrameSummary& GetFrameSummary() const { return m_frameSummary; }
	const RenderStats& GetRenderStats() const { return m_lastRenderStats; }
	const std::vector<ProfilerZoneStats>& GetCpuZoneStats() const { return m_cpuZoneStats; }
	const std::vector<ProfilerZoneStats>& GetGpuZoneStats() const { return m_gpuZoneStats; }
	const std::vector<float>& GetCpuFrameHistory() const { return m_cpuFrameHistory; }
	const std::vector<float>& GetGpuFrameHistory() const { return m_gpuFrameHistory; }
	const MemoryStats& GetMemoryStats() const { return m_memoryStats; }

	void ResetStatistics();

private:
	static constexpr std::size_t kGpuQueryLatency = 4;
	static constexpr std::size_t kFrameHistorySize = 240;

	struct GpuQuerySlot {
		unsigned int startQuery = 0;
		unsigned int endQuery = 0;
		bool pending = false;
		bool benchmarkSample = false;
	};

	struct GpuZoneRuntime {
		std::array<GpuQuerySlot, kGpuQueryLatency> slots{};
		std::size_t nextSlot = 0;
	};

	PerformanceProfiler() = default;

	std::size_t GetOrCreateCpuZone(const char* name);
	std::size_t GetOrCreateGpuZone(const char* name);
	void ResolveGpuQueries(bool waitForResults = false);
	void FinalizeCpuZones();
	void UpdateZoneStats(ProfilerZoneStats& stats, double elapsedMs);
	void AddFrameHistorySample(std::vector<float>& history, double elapsedMs);
	void UpdateFrameSummary();
	void UpdateProcessMemoryStats();
	static double CalculateAverage(const std::vector<float>& values);
	static double CalculatePercentile(const std::vector<float>& values, double percentile);

	bool m_initialized = false;
	bool m_enabled = true;
	bool m_frameActive = false;
	bool m_gpuTimingSupported = false;
	bool m_gpuTimingEnabled = true;

	std::chrono::steady_clock::time_point m_frameStart{};

	std::unordered_map<std::string, std::size_t> m_cpuZoneIndices;
	std::vector<ProfilerZoneStats> m_cpuZoneStats;
	std::vector<double> m_cpuZoneAccumulators;
	std::vector<std::uint32_t> m_cpuZoneHitCounts;

	std::unordered_map<std::string, std::size_t> m_gpuZoneIndices;
	std::vector<ProfilerZoneStats> m_gpuZoneStats;
	std::vector<GpuZoneRuntime> m_gpuZoneRuntime;

	std::vector<float> m_cpuFrameHistory;
	std::vector<float> m_gpuFrameHistory;
	ProfilerFrameSummary m_frameSummary;
	RenderStats m_currentRenderStats;
	RenderStats m_lastRenderStats;
	MemoryStats m_memoryStats;
	std::chrono::steady_clock::time_point m_lastMemoryPoll{};
	bool m_benchmarkCaptureActive = false;
	ProfilerBenchmarkSamples m_benchmarkSamples;
};

class ProfilerFrameScope {
public:
	ProfilerFrameScope();
	~ProfilerFrameScope();

	ProfilerFrameScope(const ProfilerFrameScope&) = delete;
	ProfilerFrameScope& operator=(const ProfilerFrameScope&) = delete;
};

class CpuProfileScope {
public:
	explicit CpuProfileScope(const char* name);
	~CpuProfileScope();

	CpuProfileScope(const CpuProfileScope&) = delete;
	CpuProfileScope& operator=(const CpuProfileScope&) = delete;

private:
	const char* m_name = nullptr;
	std::chrono::steady_clock::time_point m_start{};
	bool m_active = false;
};

class GpuProfileScope {
public:
	explicit GpuProfileScope(const char* name);
	~GpuProfileScope();

	GpuProfileScope(const GpuProfileScope&) = delete;
	GpuProfileScope& operator=(const GpuProfileScope&) = delete;

private:
	PerformanceProfiler::GpuScopeToken m_token;
};

#define PERF_DETAIL_JOIN_IMPL(a, b) a##b
#define PERF_DETAIL_JOIN(a, b) PERF_DETAIL_JOIN_IMPL(a, b)
#define PERF_FRAME_SCOPE() ProfilerFrameScope PERF_DETAIL_JOIN(perfFrameScope_, __LINE__)
#define PERF_CPU_SCOPE(name) CpuProfileScope PERF_DETAIL_JOIN(perfCpuScope_, __LINE__)(name)
#define PERF_GPU_SCOPE(name) GpuProfileScope PERF_DETAIL_JOIN(perfGpuScope_, __LINE__)(name)
