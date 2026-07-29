#pragma once

#include <cstdint>
#include <string>

#include <glm/glm.hpp>

enum class BenchmarkMotionProfile {
	None,
	Point,
	Caster,
	Camera,
	PointCamera,
	Mixed,
	CacheThreeWay
};

enum class BenchmarkMotionTrack : std::uint32_t {
	None = 0u,
	Point = 1u << 0u,
	Caster = 1u << 1u,
	Camera = 1u << 2u
};

struct BenchmarkMotionTimelineConfig {
	int fixedFramesPerSecond = 60;
	int cycleFrames = 600;
	float sceneRadius = 1.0f;
	float pointHorizontalRadiusRatio = 0.10f;
	float pointVerticalRadiusRatio = 0.025f;
	float casterHorizontalRadiusRatio = 0.025f;
	float casterVerticalRadiusRatio = 0.008f;
	float cameraPositionRadiusRatio = 0.05f;
	float cameraTargetRadiusRatio = 0.01f;
};

struct BenchmarkMotionBaseState {
	glm::vec3 pointPosition = glm::vec3(0.0f);
	glm::vec3 casterPosition = glm::vec3(0.0f);
	glm::vec3 cameraPosition = glm::vec3(0.0f, 0.0f, 1.0f);
	glm::vec3 cameraTarget = glm::vec3(0.0f);
	glm::vec3 cameraUp = glm::vec3(0.0f, 1.0f, 0.0f);
};

struct BenchmarkMotionSample {
	int frameIndex = 0;
	int cycleFrame = 0;
	double fixedTimeSeconds = 0.0;
	double normalizedPhase = 0.0;
	std::uint32_t trackMask = 0u;
	glm::vec3 pointPosition = glm::vec3(0.0f);
	glm::vec3 casterPosition = glm::vec3(0.0f);
	glm::vec3 cameraPosition = glm::vec3(0.0f, 0.0f, 1.0f);
	glm::vec3 cameraTarget = glm::vec3(0.0f);
	glm::vec3 cameraUp = glm::vec3(0.0f, 1.0f, 0.0f);
};

class BenchmarkMotionTimeline final {
public:
	BenchmarkMotionTimeline(
		BenchmarkMotionProfile profile,
		const BenchmarkMotionTimelineConfig& config,
		const BenchmarkMotionBaseState& baseState);

	BenchmarkMotionSample Sample(int frameIndex) const;

	BenchmarkMotionProfile GetProfile() const {
		return m_profile;
	}
	const BenchmarkMotionTimelineConfig& GetConfig() const {
		return m_config;
	}
	const BenchmarkMotionBaseState& GetBaseState() const {
		return m_baseState;
	}
	std::uint32_t GetTrackMask() const;

	static bool IsTimelineWorkload(const std::string& workload);
	static BenchmarkMotionProfile ProfileFromWorkload(
		const std::string& workload);
	static const char* ProfileName(BenchmarkMotionProfile profile);
	static std::uint32_t TrackMask(BenchmarkMotionProfile profile);
	static bool HasTrack(
		std::uint32_t trackMask,
		BenchmarkMotionTrack track);

private:
	BenchmarkMotionProfile m_profile = BenchmarkMotionProfile::None;
	BenchmarkMotionTimelineConfig m_config;
	BenchmarkMotionBaseState m_baseState;
};
