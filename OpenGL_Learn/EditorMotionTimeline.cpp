#include "EditorMotionTimeline.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <glm/gtc/matrix_transform.hpp>

namespace {
	bool ProfileHasTrack(
		BenchmarkMotionProfile profile,
		BenchmarkMotionTrack track)
	{
		return BenchmarkMotionTimeline::HasTrack(
			BenchmarkMotionTimeline::TrackMask(profile),
			track);
	}

	bool ContainsModel(
		const Scene& scene,
		const std::shared_ptr<Model>& target)
	{
		if (!target) {
			return false;
		}
		const auto& models = scene.modelSource.GetModels();
		return std::find(models.begin(), models.end(), target) != models.end();
	}

	float MaximumScaleComponent(const glm::vec3& scale)
	{
		return (std::max)(
			std::abs(scale.x),
			(std::max)(std::abs(scale.y), std::abs(scale.z)));
	}
}

bool EditorMotionTimelineController::CaptureBaseState(
	Scene& scene,
	Camera& camera)
{
	if (m_captured) {
		RestoreCapturedState(scene, camera);
	}

	const auto& models = scene.modelSource.GetModels();
	m_capturedModelCount = models.size();
	m_capturedPointLightCount = scene.lightSource.pointLights.size();
	m_pointLightIndex = (std::max)(0, m_pointLightIndex);
	m_casterIndex = (std::max)(0, m_casterIndex);

	if (!models.empty()) {
		m_casterIndex = (std::min)(
			m_casterIndex,
			static_cast<int>(models.size()) - 1);
		m_caster = models[static_cast<std::size_t>(m_casterIndex)];
		m_sceneAnchor = models.front();
	}
	else {
		m_caster.reset();
		m_sceneAnchor.reset();
	}
	if (!scene.lightSource.pointLights.empty()) {
		m_pointLightIndex = (std::min)(
			m_pointLightIndex,
			static_cast<int>(scene.lightSource.pointLights.size()) - 1);
	}

	if (ProfileHasTrack(m_profile, BenchmarkMotionTrack::Point) &&
		scene.lightSource.pointLights.empty()) {
		InvalidateCapture(
			"The selected profile needs a point light, but the scene has none.");
		return false;
	}
	if (ProfileHasTrack(m_profile, BenchmarkMotionTrack::Caster) &&
		models.empty()) {
		InvalidateCapture(
			"The selected profile needs a caster model, but the scene has none.");
		return false;
	}

	m_config.sceneRadius = EstimateSceneRadius(scene);
	if (!scene.lightSource.pointLights.empty()) {
		m_baseState.pointPosition =
			scene.lightSource.pointLights[
				static_cast<std::size_t>(m_pointLightIndex)].position;
	}
	if (const std::shared_ptr<Model> caster = m_caster.lock()) {
		m_baseState.casterPosition = caster->position;
	}
	m_baseState.cameraPosition = camera.cameraPos;
	const glm::vec3 safeFront =
		glm::length(camera.cameraFront) > 0.0001f
			? glm::normalize(camera.cameraFront)
			: glm::vec3(0.0f, 0.0f, -1.0f);
	m_baseState.cameraTarget =
		camera.cameraPos + safeFront * (std::max)(1.0f, m_config.sceneRadius);
	m_baseState.cameraUp =
		glm::length(camera.up) > 0.0001f
			? glm::normalize(camera.up)
			: glm::vec3(0.0f, 1.0f, 0.0f);
	m_restoreCameraFront = camera.cameraFront;
	m_restoreCameraUp = camera.up;

	m_frameIndex = 0;
	m_frameAccumulator = 0.0;
	m_captured = true;
	m_playing = false;
	ResetTelemetry(scene);
	ApplyCurrentSample(scene, camera);
	m_statusText =
		"Base state captured. Preview uses deterministic fixed-frame samples.";
	return true;
}

void EditorMotionTimelineController::Play(
	Scene& scene,
	Camera& camera)
{
	if (!m_captured && !CaptureBaseState(scene, camera)) {
		return;
	}
	if (!ValidateTargets(scene)) {
		InvalidateCapture(
			"Scene contents changed. Capture a new base state before playing.");
		return;
	}
	ApplyCurrentSample(scene, camera);
	m_playing = true;
	m_statusText =
		"Preview playing. Formal benchmark measurements remain script-driven.";
}

void EditorMotionTimelineController::Pause()
{
	m_playing = false;
	m_frameAccumulator = 0.0;
	if (m_captured) {
		m_statusText = "Preview paused at the current deterministic frame.";
	}
}

void EditorMotionTimelineController::StopAndRestore(
	Scene& scene,
	Camera& camera)
{
	const bool canRestore = m_captured && ValidateTargets(scene);
	if (canRestore) {
		RestoreCapturedState(scene, camera);
	}
	m_playing = false;
	m_captured = false;
	m_frameIndex = 0;
	m_frameAccumulator = 0.0;
	m_statusText = canRestore
		? "Original scene state restored."
		: "Preview stopped without writing to stale or missing targets.";
}

void EditorMotionTimelineController::ResetToStart(
	Scene& scene,
	Camera& camera)
{
	if (!m_captured) {
		CaptureBaseState(scene, camera);
		return;
	}
	m_playing = false;
	m_frameIndex = 0;
	m_frameAccumulator = 0.0;
	if (!ApplyCurrentSample(scene, camera)) {
		InvalidateCapture(
			"Scene contents changed. Capture a new base state before scrubbing.");
		return;
	}
	m_statusText = "Timeline reset to frame 0.";
}

void EditorMotionTimelineController::Update(
	Scene& scene,
	Camera& camera,
	float deltaSeconds)
{
	if (!m_playing) {
		return;
	}
	if (!ValidateTargets(scene)) {
		InvalidateCapture(
			"Scene contents changed during playback. Preview stopped safely.");
		return;
	}

	const double clampedDelta = (std::min)(
		0.25,
		(std::max)(0.0, static_cast<double>(deltaSeconds)));
	m_frameAccumulator +=
		clampedDelta *
		static_cast<double>(m_config.fixedFramesPerSecond) *
		static_cast<double>(m_playbackSpeed);
	const int framesToAdvance =
		static_cast<int>(std::floor(m_frameAccumulator));
	if (framesToAdvance <= 0) {
		return;
	}
	m_frameAccumulator -= static_cast<double>(framesToAdvance);
	m_frameIndex += framesToAdvance;
	if (m_frameIndex >= m_config.cycleFrames) {
		if (m_looping) {
			m_frameIndex %= m_config.cycleFrames;
		}
		else {
			m_frameIndex = m_config.cycleFrames - 1;
			m_playing = false;
		}
	}
	if (!ApplyCurrentSample(scene, camera)) {
		InvalidateCapture(
			"Timeline targets became invalid. Preview stopped safely.");
	}
}

void EditorMotionTimelineController::RecordAfterRender(
	const Scene& scene)
{
	const Scene::ShadowSystemStats& current = scene.GetShadowSystemStats();
	if (!m_hasShadowTelemetryBaseline) {
		m_previousShadowStats = current;
		m_hasShadowTelemetryBaseline = true;
		return;
	}

	m_latestTelemetry.shadowUpdateCount = CounterDelta(
		m_previousShadowStats.updateCount,
		current.updateCount);
	m_latestTelemetry.motionSampleApplied =
		m_motionSampleAppliedSinceLastRender;
	m_latestTelemetry.cacheHitCount = CounterDelta(
		m_previousShadowStats.cacheHitCount,
		current.cacheHitCount);
	m_latestTelemetry.lightCacheHitCount = CounterDelta(
		m_previousShadowStats.lightCacheHitCount,
		current.lightCacheHitCount);
	m_latestTelemetry.directionalLightUpdateCount = CounterDelta(
		m_previousShadowStats.directionalLightUpdateCount,
		current.directionalLightUpdateCount);
	m_latestTelemetry.pointLightUpdateCount = CounterDelta(
		m_previousShadowStats.pointLightUpdateCount,
		current.pointLightUpdateCount);
	m_latestTelemetry.spotLightUpdateCount = CounterDelta(
		m_previousShadowStats.spotLightUpdateCount,
		current.spotLightUpdateCount);
	m_latestTelemetry.updatedLightCount =
		m_latestTelemetry.directionalLightUpdateCount +
		m_latestTelemetry.pointLightUpdateCount +
		m_latestTelemetry.spotLightUpdateCount;
	m_latestTelemetry.pointShadowLayeredUpdateCount = CounterDelta(
		m_previousShadowStats.pointShadowLayeredUpdateCount,
		current.pointShadowLayeredUpdateCount);
	m_latestTelemetry.pointShadowSixFaceUpdateCount = CounterDelta(
		m_previousShadowStats.pointShadowSixFaceUpdateCount,
		current.pointShadowSixFaceUpdateCount);
	m_latestTelemetry.pointShadowSubmissionPassCount = CounterDelta(
		m_previousShadowStats.pointShadowSubmissionPassCount,
		current.pointShadowSubmissionPassCount);
	m_latestTelemetry.shadowResourceFailureCount = CounterDelta(
		m_previousShadowStats.shadowResourceFailureCount,
		current.shadowResourceFailureCount);
	m_latestTelemetry.conservativeShadowFallbackCount = CounterDelta(
		m_previousShadowStats.conservativeShadowFallbackCount,
		current.conservativeShadowFallbackCount);
	m_latestTelemetry.shadowUpdateCpuMilliseconds =
		m_latestTelemetry.shadowUpdateCount > 0
			? current.lastUpdateCpuMilliseconds
			: 0.0;

	PushHistory(
		m_shadowCpuHistory,
		static_cast<float>(
			m_latestTelemetry.shadowUpdateCpuMilliseconds));
	PushHistory(
		m_updatedLightHistory,
		static_cast<float>(m_latestTelemetry.updatedLightCount));
	PushHistory(
		m_lightCacheHitHistory,
		static_cast<float>(m_latestTelemetry.lightCacheHitCount));
	PushHistory(
		m_pointSubmissionHistory,
		static_cast<float>(
			m_latestTelemetry.pointShadowSubmissionPassCount));
	if (m_motionSampleAppliedSinceLastRender) {
		m_latestMotionStepTelemetry = m_latestTelemetry;
	}
	m_motionSampleAppliedSinceLastRender = false;
	m_previousShadowStats = current;
}

void EditorMotionTimelineController::ChangeProfile(
	Scene& scene,
	Camera& camera,
	BenchmarkMotionProfile profile)
{
	if (profile == m_profile) {
		return;
	}
	if (m_captured) {
		RestoreCapturedState(scene, camera);
	}
	m_profile = profile;
	m_captured = false;
	m_playing = false;
	m_frameIndex = 0;
	m_frameAccumulator = 0.0;
	m_statusText = "Profile changed. Capture the base state for this track set.";
}

void EditorMotionTimelineController::ChangePointLightIndex(
	Scene& scene,
	Camera& camera,
	int index)
{
	if (index == m_pointLightIndex) {
		return;
	}
	if (m_captured) {
		RestoreCapturedState(scene, camera);
	}
	m_pointLightIndex = (std::max)(0, index);
	m_captured = false;
	m_playing = false;
	m_frameIndex = 0;
	m_statusText = "Point-light target changed. Capture a new base state.";
}

void EditorMotionTimelineController::ChangeCasterIndex(
	Scene& scene,
	Camera& camera,
	int index)
{
	if (index == m_casterIndex) {
		return;
	}
	if (m_captured) {
		RestoreCapturedState(scene, camera);
	}
	m_casterIndex = (std::max)(0, index);
	m_captured = false;
	m_playing = false;
	m_frameIndex = 0;
	m_statusText = "Caster target changed. Capture a new base state.";
}

void EditorMotionTimelineController::SetFrame(
	Scene& scene,
	Camera& camera,
	int frameIndex)
{
	if (!m_captured) {
		if (!CaptureBaseState(scene, camera)) {
			return;
		}
	}
	m_frameIndex = (std::max)(
		0,
		(std::min)(frameIndex, m_config.cycleFrames - 1));
	m_frameAccumulator = 0.0;
	if (!ApplyCurrentSample(scene, camera)) {
		InvalidateCapture(
			"Scene contents changed. Capture a new base state before scrubbing.");
	}
}

void EditorMotionTimelineController::SetFixedFramesPerSecond(
	Scene& scene,
	Camera& camera,
	int framesPerSecond)
{
	m_config.fixedFramesPerSecond =
		(std::max)(1, framesPerSecond);
	if (m_captured) {
		ApplyCurrentSample(scene, camera);
	}
}

void EditorMotionTimelineController::SetCycleFrames(
	Scene& scene,
	Camera& camera,
	int cycleFrames)
{
	m_config.cycleFrames = (std::max)(2, cycleFrames);
	m_frameIndex = (std::min)(m_frameIndex, m_config.cycleFrames - 1);
	m_frameAccumulator = 0.0;
	if (m_captured) {
		ApplyCurrentSample(scene, camera);
	}
}

void EditorMotionTimelineController::SetSceneRadius(
	Scene& scene,
	Camera& camera,
	float sceneRadius)
{
	m_config.sceneRadius =
		(std::max)(0.0001f, std::abs(sceneRadius));
	if (m_captured) {
		ApplyCurrentSample(scene, camera);
	}
}

void EditorMotionTimelineController::SetPlaybackSpeed(float speed)
{
	m_playbackSpeed = (std::max)(0.1f, (std::min)(speed, 4.0f));
}

void EditorMotionTimelineController::UseEstimatedSceneRadius(
	Scene& scene,
	Camera& camera)
{
	SetSceneRadius(scene, camera, EstimateSceneRadius(scene));
}

bool EditorMotionTimelineController::HasValidTargets(
	const Scene& scene) const
{
	return m_captured && ValidateTargets(scene);
}

float EditorMotionTimelineController::EstimateSceneRadius(
	const Scene& scene)
{
	glm::vec3 minimum(std::numeric_limits<float>::max());
	glm::vec3 maximum(-std::numeric_limits<float>::max());
	bool hasBounds = false;

	for (const std::shared_ptr<Model>& model :
		scene.modelSource.GetModels()) {
		if (!model || !model->m_active) {
			continue;
		}
		const glm::mat4 modelMatrix = model->getModelMatrix();
		const glm::vec3 worldCenter = glm::vec3(
			modelMatrix * glm::vec4(model->GetLoacalCenter(), 1.0f));
		const float worldRadius =
			(std::max)(0.0f, model->GetLocalBoundingRadius()) *
			MaximumScaleComponent(model->scale);
		const glm::vec3 extent(worldRadius);
		minimum = glm::min(minimum, worldCenter - extent);
		maximum = glm::max(maximum, worldCenter + extent);
		hasBounds = true;
	}

	if (!hasBounds) {
		return 1.0f;
	}
	const float radius = glm::length(maximum - minimum) * 0.5f;
	return (std::max)(1.0f, radius);
}

bool EditorMotionTimelineController::ValidateTargets(
	const Scene& scene) const
{
	if (!m_captured ||
		scene.modelSource.GetModels().size() != m_capturedModelCount ||
		scene.lightSource.pointLights.size() != m_capturedPointLightCount) {
		return false;
	}
	if (m_capturedModelCount > 0) {
		const std::shared_ptr<Model> anchor = m_sceneAnchor.lock();
		if (!ContainsModel(scene, anchor)) {
			return false;
		}
	}
	if (ProfileHasTrack(m_profile, BenchmarkMotionTrack::Point) &&
		(m_pointLightIndex < 0 ||
			static_cast<std::size_t>(m_pointLightIndex) >=
				scene.lightSource.pointLights.size())) {
		return false;
	}
	if (ProfileHasTrack(m_profile, BenchmarkMotionTrack::Caster)) {
		const std::shared_ptr<Model> caster = m_caster.lock();
		if (!ContainsModel(scene, caster)) {
			return false;
		}
	}
	return true;
}

bool EditorMotionTimelineController::ApplyCurrentSample(
	Scene& scene,
	Camera& camera)
{
	if (!ValidateTargets(scene)) {
		return false;
	}

	const BenchmarkMotionTimeline timeline(
		m_profile,
		m_config,
		m_baseState);
	m_currentSample = timeline.Sample(m_frameIndex);
	if (BenchmarkMotionTimeline::HasTrack(
			m_currentSample.trackMask,
			BenchmarkMotionTrack::Point)) {
		scene.lightSource.pointLights[
			static_cast<std::size_t>(m_pointLightIndex)].SetPosition(
				m_currentSample.pointPosition);
	}
	if (BenchmarkMotionTimeline::HasTrack(
			m_currentSample.trackMask,
			BenchmarkMotionTrack::Caster)) {
		const std::shared_ptr<Model> caster = m_caster.lock();
		if (!caster) {
			return false;
		}
		caster->SetPosition(m_currentSample.casterPosition);
	}
	if (BenchmarkMotionTimeline::HasTrack(
			m_currentSample.trackMask,
			BenchmarkMotionTrack::Camera)) {
		const glm::vec3 direction =
			m_currentSample.cameraTarget -
			m_currentSample.cameraPosition;
		camera.cameraPos = m_currentSample.cameraPosition;
		if (glm::length(direction) > 0.0001f) {
			camera.cameraFront = glm::normalize(direction);
		}
		camera.up = m_currentSample.cameraUp;
	}
	m_motionSampleAppliedSinceLastRender = true;
	return true;
}

void EditorMotionTimelineController::RestoreCapturedState(
	Scene& scene,
	Camera& camera)
{
	if (!ValidateTargets(scene)) {
		return;
	}
	if (ProfileHasTrack(m_profile, BenchmarkMotionTrack::Point) &&
		!scene.lightSource.pointLights.empty() &&
		m_pointLightIndex >= 0 &&
		static_cast<std::size_t>(m_pointLightIndex) <
			scene.lightSource.pointLights.size()) {
		scene.lightSource.pointLights[
			static_cast<std::size_t>(m_pointLightIndex)].SetPosition(
			m_baseState.pointPosition);
	}
	if (ProfileHasTrack(m_profile, BenchmarkMotionTrack::Caster)) {
		if (const std::shared_ptr<Model> caster = m_caster.lock()) {
			caster->SetPosition(m_baseState.casterPosition);
		}
	}
	if (ProfileHasTrack(m_profile, BenchmarkMotionTrack::Camera)) {
		camera.cameraPos = m_baseState.cameraPosition;
		camera.cameraFront = m_restoreCameraFront;
		camera.up = m_restoreCameraUp;
	}
}

void EditorMotionTimelineController::InvalidateCapture(
	const std::string& reason)
{
	m_playing = false;
	m_captured = false;
	m_frameAccumulator = 0.0;
	m_statusText = reason;
}

void EditorMotionTimelineController::ResetTelemetry(
	const Scene& scene)
{
	m_previousShadowStats = scene.GetShadowSystemStats();
	m_hasShadowTelemetryBaseline = true;
	m_motionSampleAppliedSinceLastRender = false;
	m_latestTelemetry = EditorMotionFrameTelemetry{};
	m_latestMotionStepTelemetry = EditorMotionFrameTelemetry{};
	m_shadowCpuHistory.clear();
	m_updatedLightHistory.clear();
	m_lightCacheHitHistory.clear();
	m_pointSubmissionHistory.clear();
}

std::uint64_t EditorMotionTimelineController::CounterDelta(
	std::uint64_t previous,
	std::uint64_t current)
{
	return current >= previous ? current - previous : current;
}

void EditorMotionTimelineController::PushHistory(
	std::vector<float>& history,
	float value)
{
	if (history.size() == kHistoryLength) {
		history.erase(history.begin());
	}
	history.push_back(value);
}
