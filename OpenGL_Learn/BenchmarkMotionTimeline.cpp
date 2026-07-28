#include "BenchmarkMotionTimeline.h"

#include <algorithm>
#include <cmath>

namespace {
	constexpr double kTwoPi = 6.28318530717958647692;

	int PositiveModulo(int value, int modulus)
	{
		const int remainder = value % modulus;
		return remainder < 0 ? remainder + modulus : remainder;
	}

	float Wave(double phase)
	{
		return static_cast<float>(std::sin(phase));
	}

	float Orbit(double phase)
	{
		return static_cast<float>(std::cos(phase));
	}
}

BenchmarkMotionTimeline::BenchmarkMotionTimeline(
	BenchmarkMotionProfile profile,
	const BenchmarkMotionTimelineConfig& config,
	const BenchmarkMotionBaseState& baseState)
	: m_profile(profile),
	m_config(config),
	m_baseState(baseState)
{
	m_config.fixedFramesPerSecond =
		(std::max)(1, m_config.fixedFramesPerSecond);
	m_config.cycleFrames = (std::max)(1, m_config.cycleFrames);
	m_config.sceneRadius =
		(std::max)(0.0001f, std::abs(m_config.sceneRadius));
	if (glm::length(m_baseState.cameraUp) < 0.0001f) {
		m_baseState.cameraUp = glm::vec3(0.0f, 1.0f, 0.0f);
	}
	else {
		m_baseState.cameraUp = glm::normalize(m_baseState.cameraUp);
	}
}

BenchmarkMotionSample BenchmarkMotionTimeline::Sample(int frameIndex) const
{
	BenchmarkMotionSample sample;
	sample.frameIndex = frameIndex;
	sample.cycleFrame =
		PositiveModulo(frameIndex, m_config.cycleFrames);
	sample.fixedTimeSeconds =
		static_cast<double>(frameIndex) /
		static_cast<double>(m_config.fixedFramesPerSecond);
	sample.normalizedPhase =
		static_cast<double>(sample.cycleFrame) /
		static_cast<double>(m_config.cycleFrames);
	sample.trackMask = GetTrackMask();
	sample.pointPosition = m_baseState.pointPosition;
	sample.casterPosition = m_baseState.casterPosition;
	sample.cameraPosition = m_baseState.cameraPosition;
	sample.cameraTarget = m_baseState.cameraTarget;
	sample.cameraUp = m_baseState.cameraUp;

	const double angle = sample.normalizedPhase * kTwoPi;
	const float radius = m_config.sceneRadius;
	if (HasTrack(sample.trackMask, BenchmarkMotionTrack::Point)) {
		sample.pointPosition += radius * glm::vec3(
			m_config.pointHorizontalRadiusRatio * Orbit(angle),
			m_config.pointVerticalRadiusRatio * Wave(angle * 2.0),
			m_config.pointHorizontalRadiusRatio * 0.8f * Wave(angle));
	}
	if (HasTrack(sample.trackMask, BenchmarkMotionTrack::Caster)) {
		sample.casterPosition += radius * glm::vec3(
			m_config.casterHorizontalRadiusRatio * Wave(angle),
			m_config.casterVerticalRadiusRatio * Wave(angle * 2.0),
			m_config.casterHorizontalRadiusRatio * 0.8f * Orbit(angle));
	}
	if (HasTrack(sample.trackMask, BenchmarkMotionTrack::Camera)) {
		sample.cameraPosition += radius * glm::vec3(
			m_config.cameraPositionRadiusRatio * Wave(angle),
			m_config.cameraPositionRadiusRatio * 0.4f * Wave(angle * 2.0),
			m_config.cameraPositionRadiusRatio * 0.8f * Orbit(angle));
		const double targetAngle = angle + kTwoPi / 6.0;
		sample.cameraTarget += radius * glm::vec3(
			m_config.cameraTargetRadiusRatio * Wave(targetAngle),
			m_config.cameraTargetRadiusRatio * 0.5f * Wave(angle * 2.0),
			m_config.cameraTargetRadiusRatio * Orbit(targetAngle));
	}

	return sample;
}

std::uint32_t BenchmarkMotionTimeline::GetTrackMask() const
{
	return TrackMask(m_profile);
}

bool BenchmarkMotionTimeline::IsTimelineWorkload(
	const std::string& workload)
{
	return ProfileFromWorkload(workload) != BenchmarkMotionProfile::None;
}

BenchmarkMotionProfile BenchmarkMotionTimeline::ProfileFromWorkload(
	const std::string& workload)
{
	if (workload == "timeline-point") {
		return BenchmarkMotionProfile::Point;
	}
	if (workload == "timeline-caster") {
		return BenchmarkMotionProfile::Caster;
	}
	if (workload == "timeline-camera") {
		return BenchmarkMotionProfile::Camera;
	}
	if (workload == "timeline-mixed") {
		return BenchmarkMotionProfile::Mixed;
	}
	return BenchmarkMotionProfile::None;
}

const char* BenchmarkMotionTimeline::ProfileName(
	BenchmarkMotionProfile profile)
{
	switch (profile) {
	case BenchmarkMotionProfile::Point:
		return "point";
	case BenchmarkMotionProfile::Caster:
		return "caster";
	case BenchmarkMotionProfile::Camera:
		return "camera";
	case BenchmarkMotionProfile::Mixed:
		return "mixed";
	default:
		return "none";
	}
}

std::uint32_t BenchmarkMotionTimeline::TrackMask(
	BenchmarkMotionProfile profile)
{
	switch (profile) {
	case BenchmarkMotionProfile::Point:
		return static_cast<std::uint32_t>(BenchmarkMotionTrack::Point);
	case BenchmarkMotionProfile::Caster:
		return static_cast<std::uint32_t>(BenchmarkMotionTrack::Caster);
	case BenchmarkMotionProfile::Camera:
		return static_cast<std::uint32_t>(BenchmarkMotionTrack::Camera);
	case BenchmarkMotionProfile::Mixed:
		return
			static_cast<std::uint32_t>(BenchmarkMotionTrack::Point) |
			static_cast<std::uint32_t>(BenchmarkMotionTrack::Caster) |
			static_cast<std::uint32_t>(BenchmarkMotionTrack::Camera);
	default:
		return 0u;
	}
}

bool BenchmarkMotionTimeline::HasTrack(
	std::uint32_t trackMask,
	BenchmarkMotionTrack track)
{
	return (
		trackMask &
		static_cast<std::uint32_t>(track)) != 0u;
}
