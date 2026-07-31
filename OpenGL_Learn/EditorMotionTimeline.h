#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "BenchmarkMotionTimeline.h"
#include "Camera.h"
#include "Scene.h"

struct EditorMotionFrameTelemetry {
	bool motionSampleApplied = false;
	std::uint64_t shadowUpdateCount = 0;
	std::uint64_t cacheHitCount = 0;
	std::uint64_t lightCacheHitCount = 0;
	std::uint64_t updatedLightCount = 0;
	std::uint64_t directionalLightUpdateCount = 0;
	std::uint64_t pointLightUpdateCount = 0;
	std::uint64_t spotLightUpdateCount = 0;
	std::uint64_t pointShadowLayeredUpdateCount = 0;
	std::uint64_t pointShadowSixFaceUpdateCount = 0;
	std::uint64_t pointShadowSubmissionPassCount = 0;
	std::uint64_t pointShadowRequiredFaceCount = 0;
	std::uint64_t pointShadowRenderedFaceCount = 0;
	std::uint64_t pointShadowFaceCacheHitCount = 0;
	std::uint64_t pointShadowDeferredFaceCount = 0;
	std::uint64_t shadowResourceFailureCount = 0;
	std::uint64_t conservativeShadowFallbackCount = 0;
	double shadowUpdateCpuMilliseconds = 0.0;
};

class EditorMotionTimelineController final {
public:
	static constexpr std::size_t kHistoryLength = 240;
	enum class ShadowComparisonMode {
		GlobalDirty,
		PerLight,
		PerFace
	};

	bool CaptureBaseState(Scene& scene, Camera& camera);
	void Play(Scene& scene, Camera& camera);
	void Pause();
	void StopAndRestore(Scene& scene, Camera& camera);
	void ResetToStart(Scene& scene, Camera& camera);
	void Update(Scene& scene, Camera& camera, float deltaSeconds);
	void RecordAfterRender(const Scene& scene);

	void ChangeProfile(
		Scene& scene,
		Camera& camera,
		BenchmarkMotionProfile profile);
	void ChangePointLightIndex(
		Scene& scene,
		Camera& camera,
		int index);
	void ChangeCasterIndex(
		Scene& scene,
		Camera& camera,
		int index);
	void SetFrame(Scene& scene, Camera& camera, int frameIndex);
	void SetFixedFramesPerSecond(
		Scene& scene,
		Camera& camera,
		int framesPerSecond);
	void SetCycleFrames(
		Scene& scene,
		Camera& camera,
		int cycleFrames);
	void SetSceneRadius(
		Scene& scene,
		Camera& camera,
		float sceneRadius);
	void SetPlaybackSpeed(float speed);
	void SetLooping(bool looping) { m_looping = looping; }
	void UseEstimatedSceneRadius(Scene& scene, Camera& camera);
	bool PrepareThreeLightTestRig(Scene& scene, Camera& camera);
	bool RestoreThreeLightTestRig(Scene& scene, Camera& camera);
	void SetShadowComparisonMode(
		Scene& scene,
		ShadowComparisonMode mode);

	bool IsCaptured() const { return m_captured; }
	bool IsPlaying() const { return m_playing; }
	bool IsLooping() const { return m_looping; }
	bool HasValidTargets(const Scene& scene) const;
	bool IsThreeLightTestRigActive() const {
		return m_testRigPrepared;
	}
	bool IsThreeLightTestRigReady(const Scene& scene) const;
	BenchmarkMotionProfile GetProfile() const { return m_profile; }
	int GetPointLightIndex() const { return m_pointLightIndex; }
	int GetCasterIndex() const { return m_casterIndex; }
	int GetFrame() const { return m_frameIndex; }
	float GetPlaybackSpeed() const { return m_playbackSpeed; }
	const BenchmarkMotionTimelineConfig& GetConfig() const { return m_config; }
	const BenchmarkMotionSample& GetCurrentSample() const {
		return m_currentSample;
	}
	const BenchmarkMotionBaseState& GetBaseState() const {
		return m_baseState;
	}
	const EditorMotionFrameTelemetry& GetLatestTelemetry() const {
		return m_latestTelemetry;
	}
	const EditorMotionFrameTelemetry& GetLatestMotionStepTelemetry() const {
		return m_latestMotionStepTelemetry;
	}
	const std::vector<float>& GetShadowCpuHistory() const {
		return m_shadowCpuHistory;
	}
	const std::vector<float>& GetUpdatedLightHistory() const {
		return m_updatedLightHistory;
	}
	const std::vector<float>& GetLightCacheHitHistory() const {
		return m_lightCacheHitHistory;
	}
	const std::vector<float>& GetPointSubmissionHistory() const {
		return m_pointSubmissionHistory;
	}
	const std::string& GetStatusText() const { return m_statusText; }
	const std::string& GetTestRigStatusText() const {
		return m_testRigStatusText;
	}

	static float EstimateSceneRadius(const Scene& scene);

private:
	struct ShadowLightFlags {
		bool active = true;
		bool useShadowMap = false;
	};

	bool ValidateTargets(const Scene& scene) const;
	bool ValidateTestRigScene(const Scene& scene) const;
	bool ApplyCurrentSample(Scene& scene, Camera& camera);
	void RestoreCapturedState(Scene& scene, Camera& camera);
	void InvalidateCapture(const std::string& reason);
	void ResetTelemetry(const Scene& scene);
	void ClearTestRigState();
	static std::uint64_t CounterDelta(
		std::uint64_t previous,
		std::uint64_t current);
	static void PushHistory(std::vector<float>& history, float value);

	BenchmarkMotionProfile m_profile =
		BenchmarkMotionProfile::PointCamera;
	BenchmarkMotionTimelineConfig m_config;
	BenchmarkMotionBaseState m_baseState;
	BenchmarkMotionSample m_currentSample;
	std::weak_ptr<Model> m_caster;
	std::weak_ptr<Model> m_sceneAnchor;
	std::size_t m_capturedModelCount = 0;
	std::size_t m_capturedPointLightCount = 0;
	int m_pointLightIndex = 0;
	int m_casterIndex = 0;
	int m_frameIndex = 0;
	float m_playbackSpeed = 1.0f;
	double m_frameAccumulator = 0.0;
	bool m_captured = false;
	bool m_playing = false;
	bool m_looping = true;
	glm::vec3 m_restoreCameraFront = glm::vec3(0.0f, 0.0f, -1.0f);
	glm::vec3 m_restoreCameraUp = glm::vec3(0.0f, 1.0f, 0.0f);
	Scene::ShadowSystemStats m_previousShadowStats;
	bool m_hasShadowTelemetryBaseline = false;
	bool m_motionSampleAppliedSinceLastRender = false;
	EditorMotionFrameTelemetry m_latestTelemetry;
	EditorMotionFrameTelemetry m_latestMotionStepTelemetry;
	std::vector<float> m_shadowCpuHistory;
	std::vector<float> m_updatedLightHistory;
	std::vector<float> m_lightCacheHitHistory;
	std::vector<float> m_pointSubmissionHistory;
	std::string m_statusText =
		"Capture the current scene state before previewing a track.";

	bool m_testRigPrepared = false;
	bool m_testRigAddedDirectionalLight = false;
	bool m_testRigAddedSpotLight = false;
	bool m_testRigPreviousShadowCacheDisabled = false;
	bool m_testRigPreviousPerLightCache = true;
	bool m_testRigPreviousSpatialCasterCache = false;
	bool m_testRigPreviousPointPerFaceCache = false;
	bool m_testRigPreviousForceAllPointFaces = false;
	bool m_testRigPreviousPointAdaptiveRendering = true;
	bool m_testRigPreviousPointSixFaceRendering = false;
	std::size_t m_testRigModelCount = 0;
	std::size_t m_testRigOriginalPointLightCount = 0;
	std::size_t m_testRigOriginalDirectionalLightCount = 0;
	std::size_t m_testRigOriginalSpotLightCount = 0;
	std::size_t m_testRigPreparedDirectionalLightCount = 0;
	std::size_t m_testRigPreparedSpotLightCount = 0;
	int m_testRigPointLightIndex = 0;
	int m_testRigDirectionalLightIndex = 0;
	int m_testRigSpotLightIndex = 0;
	std::weak_ptr<Model> m_testRigSceneAnchor;
	std::shared_ptr<Material> m_testRigMotionCasterMaterial;
	std::shared_ptr<Model> m_testRigMotionCaster;
	std::vector<ShadowLightFlags> m_testRigPointLightFlags;
	std::vector<ShadowLightFlags> m_testRigDirectionalLightFlags;
	std::vector<ShadowLightFlags> m_testRigSpotLightFlags;
	std::string m_testRigStatusText =
		"Prepare a temporary three-light rig for a reproducible A/B/C preview.";
};
