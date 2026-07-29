#pragma once

#include "Light.h"
#include "Shader.h"
#include "Model.h"
#include "Camera.h"
#include <unordered_map>
#include <vector>
#include <memory>
#include <utility>
#include <algorithm>
#include <cstdint>
#include "shaderManager.h"
#include "Global.h"
#include "XmlMaterialManager.h"
#include <imgui.h>
#include <backends/imgui_impl_opengl3.h>
#include <backends/imgui_impl_glfw.h>

class ImageBasedLighting;

struct LightSource {
	unsigned int pointLightVAO;
	unsigned int vertexCount;
	std::vector<PointLight> pointLights;
	std::vector<DirectionLight> directionLights;
	std::vector<SpotLight> spotLights;

	Shader pointLightShader;
	LightSource()
		: pointLightShader("LightVertexShader.glsl", "LightFragmentShader.glsl") {
	}
	std::vector<PointLight>& GetPointLights() {
		return pointLights;
	}

	std::vector<DirectionLight>& GetDirectionLights() {
		return directionLights;
	}

	std::vector<SpotLight>& GetSpotLights() {
		return spotLights;
	}

	void AddPointLight(const PointLight& pointLight) {
		pointLights.push_back(pointLight);
	}

	void AddDirectionLight(const DirectionLight& directionLight) {
		directionLights.push_back(directionLight);
	}

	void AddSpotLight(const SpotLight& spotLight) {
		spotLights.push_back(spotLight);
	}

	void SetLightUniforms(Shader& shader) {
		constexpr size_t kMaxShaderLights = 16;
		const size_t pointLightCount =
			(std::min)(pointLights.size(), kMaxShaderLights);
		for (size_t i = 0; i < pointLightCount; ++i) {
			pointLights[i].SetLightUniforms(shader, static_cast<int>(i));
		}
		shader.setInt(
			"NR_POINT_LIGHTS",
			static_cast<int>(pointLightCount));
		const size_t directionLightCount =
			(std::min)(directionLights.size(), kMaxShaderLights);
		for (size_t i = 0; i < directionLightCount; ++i) {
			directionLights[i].SetLightUniforms(shader, static_cast<int>(i));
		}
		shader.setInt(
			"NR_DIR_LIGHTS",
			static_cast<int>(directionLightCount));
		const size_t spotLightCount =
			(std::min)(spotLights.size(), kMaxShaderLights);
		for (size_t i = 0; i < spotLightCount; ++i) {
			spotLights[i].SetLightUniforms(shader, static_cast<int>(i));
		}
		shader.setInt(
			"NR_SPOT_LIGHTS",
			static_cast<int>(spotLightCount));
	}

};

struct ModelSource {
	ModelSource() = default;

	const std::vector<std::shared_ptr<Model>>& GetModels() const {
		return models;
	}

	std::uint64_t GetSceneTopologyRevision() const {
		return sceneTopologyRevision;
	}

	void AddModel(const std::shared_ptr<Model>& model) {
		assert(model != nullptr && "AddModel: model is null");
		models.push_back(model);
		AdvanceSceneTopologyRevision();
	}

	bool DeleteModel(const std::shared_ptr<Model>& model) {
		assert(model != nullptr && "DeleteModel: model is null");
		const auto oldSize = models.size();
		models.erase(
			std::remove(models.begin(), models.end(), model),
			models.end());
		if (models.size() == oldSize) {
			return false;
		}
		AdvanceSceneTopologyRevision();
		return true;
	}

	bool ReplaceModel(
		const std::shared_ptr<Model>& oldModel,
		const std::shared_ptr<Model>& newModel) {
		assert(oldModel != nullptr && "ReplaceModel: old model is null");
		assert(newModel != nullptr && "ReplaceModel: new model is null");
		const auto iterator =
			std::find(models.begin(), models.end(), oldModel);
		if (iterator == models.end()) {
			return false;
		}
		*iterator = newModel;
		AdvanceSceneTopologyRevision();
		return true;
	}

	void ClearModels() {
		models.clear();
		// Clear is an explicit topology barrier even for an already empty source.
		AdvanceSceneTopologyRevision();
	}

private:
	void AdvanceSceneTopologyRevision() {
		++sceneTopologyRevision;
		if (sceneTopologyRevision == 0) {
			++sceneTopologyRevision;
		}
	}

	std::vector<std::shared_ptr<Model>> models;
	std::uint64_t sceneTopologyRevision = 1;
};

class SkyboxSource {
public:
	CubeTexture* textureCubeMap;
	unsigned int cubeMapVAO;
	std::shared_ptr<Shader> skyboxShader_ptr;
	SkyboxSource(const SkyboxSource& other) {
		textureCubeMap = other.textureCubeMap;
		cubeMapVAO = other.cubeMapVAO;
		skyboxShader_ptr = other.skyboxShader_ptr;
	}
	SkyboxSource(CubeTexture& textureid,unsigned int cubeMapVao,std::shared_ptr<Shader> skyboxShader) {
		textureCubeMap = &textureid;
		cubeMapVAO = cubeMapVao;
		skyboxShader_ptr = skyboxShader;
	}
	SkyboxSource() = default;
};

class Scene {
public:
	enum class ShadowLightBinding {
		AllLights,
		DirectionalOnly
	};

	struct ShadowSystemStats {
		std::uint64_t updateCount = 0;
		std::uint64_t cacheHitCount = 0;
		std::uint64_t updatedLightCount = 0;
		std::uint64_t casterCandidateCount = 0;
		std::uint64_t casterCulledCount = 0;
		std::uint64_t casterCullingLightCount = 0;
		std::uint64_t casterDrawCount = 0;
		std::uint64_t casterTriangleCount = 0;
		std::uint64_t totalCasterCandidateCount = 0;
		std::uint64_t totalCasterCulledCount = 0;
		std::uint64_t totalCasterCullingLightCount = 0;
		std::uint64_t totalCasterDrawCount = 0;
		std::uint64_t totalCasterTriangleCount = 0;
		std::uint64_t cacheCheckCount = 0;
		std::uint64_t cacheMissCount = 0;
		std::uint64_t legacySignatureCheckCount = 0;
		std::uint64_t revisionCheckCount = 0;
		std::uint64_t casterStateSyncCount = 0;
		std::uint64_t casterBoundsRebuildCount = 0;
		std::uint64_t casterRevision = 0;
		std::uint64_t sceneTopologyRevision = 0;
		std::uint64_t sceneTopologyInvalidationCount = 0;
		std::uint64_t sceneTopologyModelCount = 0;
		std::uint64_t lightCacheHitCount = 0;
		std::uint64_t directionalLightUpdateCount = 0;
		std::uint64_t pointLightUpdateCount = 0;
		std::uint64_t pointShadowLayeredUpdateCount = 0;
		std::uint64_t pointShadowSixFaceUpdateCount = 0;
		std::uint64_t pointShadowSubmissionPassCount = 0;
		std::uint64_t pointShadowFaceCullingPassCount = 0;
		std::uint64_t pointShadowRequiredFaceCount = 0;
		std::uint64_t pointShadowRenderedFaceCount = 0;
		std::uint64_t pointShadowFaceCacheHitCount = 0;
		std::uint64_t pointShadowDeferredFaceCount = 0;
		std::uint64_t pointShadowPartialUpdateCount = 0;
		std::uint64_t pointShadowFullUpdateCount = 0;
		std::uint64_t pointShadowZeroRequiredCount = 0;
		std::uint64_t pointShadowFaceDemandCheckCount = 0;
		std::uint64_t pointShadowFaceSignatureBuildCount = 0;
		std::uint64_t spotLightUpdateCount = 0;
		std::uint64_t emptyShadowClearCount = 0;
		std::uint64_t shadowResourceFailureCount = 0;
		std::uint64_t conservativeShadowFallbackCount = 0;
		std::uint64_t spotFitCount = 0;
		std::uint64_t spotProjectionAwareFitCount = 0;
		std::uint64_t spotFitFallbackCount = 0;
		std::uint64_t totalSpotFitCandidateCount = 0;
		std::uint64_t totalSpotFitAcceptedCount = 0;
		std::uint64_t totalSpotFitRejectedCount = 0;
		std::uint64_t directionalFitCount = 0;
		std::uint64_t directionalLightAabbFitCount = 0;
		std::uint64_t directionalResolutionChangeCount = 0;
		double lastUpdateCpuMilliseconds = 0.0;
		double lastCacheCheckCpuMilliseconds = 0.0;
		double totalCacheCheckCpuMilliseconds = 0.0;
		double lastCasterStateSyncCpuMilliseconds = 0.0;
		double totalCasterStateSyncCpuMilliseconds = 0.0;
		double lastPointShadowFaceDemandCpuMilliseconds = 0.0;
		double totalPointShadowFaceDemandCpuMilliseconds = 0.0;
		double lastPointShadowFaceSignatureCpuMilliseconds = 0.0;
		double totalPointShadowFaceSignatureCpuMilliseconds = 0.0;
		double totalDirectionalFitCpuMilliseconds = 0.0;
		double totalSpotFitCpuMilliseconds = 0.0;
		float lastDirectionalFitRawWidth = 0.0f;
		float lastDirectionalFitRawHeight = 0.0f;
		float lastDirectionalFitRawDepth = 0.0f;
		float lastDirectionalFitWidth = 0.0f;
		float lastDirectionalFitHeight = 0.0f;
		float lastDirectionalFitDepth = 0.0f;
		float lastDirectionalFitTexelSizeX = 0.0f;
		float lastDirectionalFitTexelSizeY = 0.0f;
		float lastDirectionalFitUtilization = 0.0f;
		float lastDirectionalFitReferenceTexelSize = 0.0f;
		int lastDirectionalFitResolution = 0;
		std::uint64_t lastSpotFitCandidateCount = 0;
		std::uint64_t lastSpotFitAcceptedCount = 0;
		std::uint64_t lastSpotFitRejectedCount = 0;
		float lastSpotFitLegacyNear = 0.0f;
		float lastSpotFitLegacyFar = 0.0f;
		float lastSpotFitRawNear = 0.0f;
		float lastSpotFitRawFar = 0.0f;
		float lastSpotFitNear = 0.0f;
		float lastSpotFitFar = 0.0f;
		float lastSpotFitDepthSpanReduction = 0.0f;
		float lastSpotFitDepthUtilization = 0.0f;
		float lastSpotFitProjectionDepthScale = 0.0f;
		float lastSpotFitPrecisionGain = 0.0f;
		float lastSpotFitMinimumProjectedCoverageMargin = 0.0f;
		std::size_t lastSpotFitLightIndex = 0;
		bool lastSpotFitRawNearClipped = false;
		bool lastSpotFitProjectionAware = false;
		bool lastCacheCheckUsedLegacySignature = false;
		std::uint8_t lastPointShadowRequiredFaceMask = 0;
		std::uint8_t lastPointShadowUpdateFaceMask = 0;
	};

	struct MeshDrawItem {
		Model* model = nullptr;
		Mesh* mesh = nullptr;
		Shader* shader = nullptr;
		glm::mat4 modelMatrix = glm::mat4(1.0f);
		glm::vec3 worldBoundsCenter = glm::vec3(0.0f);
		glm::vec3 worldBoundsAxisX = glm::vec3(0.0f);
		glm::vec3 worldBoundsAxisY = glm::vec3(0.0f);
		glm::vec3 worldBoundsAxisZ = glm::vec3(0.0f);
		bool worldBoundsValid = false;
		float worldBoundsRadius = 0.0f;
	};

	struct ModelFrameItem {
		Model* model = nullptr;
		glm::mat4 modelMatrix = glm::mat4(1.0f);
		glm::vec3 worldBoundsCenter = glm::vec3(0.0f);
		float worldBoundsRadius = 0.0f;
	};

	LightSource lightSource;
	ModelSource modelSource;
	SkyboxSource skyboxSource;
	Camera* camera_ptr = nullptr;

	FBO* fbo = nullptr;
	FBO* fboTemp = nullptr;
	FBO* deferFBO = nullptr;
	std::shared_ptr<Shader> deferShader;

	// 当前在 UI 中选中用于查看/编辑材质的模型（可为空）
	Model* selectedModelForMaterials = nullptr;
	void SetSelectedModelForMaterials(Model* model) { selectedModelForMaterials = model; }
	Model* GetSelectedModelForMaterials() const { return selectedModelForMaterials; }

	Scene(Camera* camera,const unsigned int& width,const unsigned int& height) {
		camera_ptr = camera;
		lightSource.pointLightVAO = globalVAOs.sphereVAO;
		lightSource.vertexCount = 262;
		deferShader = ShaderManager::GetInstance().GetShader(ShaderManager::DeferProcess);

		
	}
	void RenderScene(Shader&);
	//new api
	ModelSource& GetModelSource() {
		return modelSource;
	}

	LightSource& GetLightSource() {
		return lightSource;
	}

	SkyboxSource& GetSkyboxSource() {
		return skyboxSource;
	}
	unsigned int SetShadowMap(
		Shader&,
		ShadowLightBinding lightBinding = ShadowLightBinding::AllLights,
		unsigned int reservedTextureUnits = 0);
	void SetImageBasedLighting(ImageBasedLighting* imageBasedLighting) {
		m_imageBasedLighting = imageBasedLighting;
	}
	unsigned int BindImageBasedLighting(Shader& shader, unsigned int firstTextureUnit) const;
	bool UsesPbrMaterials() const;

	//old api
	void DrawPointLights();
	void DrawOpaqueModels();
	void DrawTransparentModels();
	void Draw();

	void DrawDefferedModels();

	void SetLightUniforms(Shader& shader);

	void DrawSkybox(glm::mat4 view);
	void DrawOutlines();
	void DrawNormalLines();
	
	void DrawShadowMap();
	const ShadowSystemStats& GetShadowSystemStats() const {
		return m_shadowStats;
	}
	std::uint64_t GetSceneTopologyRevision() const {
		return modelSource.GetSceneTopologyRevision();
	}
	void InvalidateShadowCache();
	void ClearContent();

	void SetSceneGui();

	void Blur(int,FBO*);
	void ClearFBO();

	FBO* GetNeedShowFramebuffer();

	FBO* GetDebugFramebuffer() {
		return deferFBO;
	}

	// Build render submission once after scene/editor updates and before executing passes.
	void PrepareRenderData();
	const std::vector<MeshDrawItem>& GetOpaqueMeshes() const;
	const std::vector<MeshDrawItem>& GetTransparentMeshes() const;

private:
	struct ShadowCasterBoundItem {
		const Model* model = nullptr;
		std::uint64_t revision = 0;
		glm::vec3 center = glm::vec3(0.0f);
		float radius = 0.0f;
	};

	struct ShadowCasterDrawItem {
		Model* model = nullptr;
		Mesh* mesh = nullptr;
		glm::mat4 modelMatrix = glm::mat4(1.0f);
		glm::vec3 worldBoundsCenter = glm::vec3(0.0f);
		glm::vec3 worldBoundsAxisX = glm::vec3(0.0f);
		glm::vec3 worldBoundsAxisY = glm::vec3(0.0f);
		glm::vec3 worldBoundsAxisZ = glm::vec3(0.0f);
		bool worldBoundsValid = false;
		float worldBoundsRadius = 0.0f;
	};

	struct ShadowLightUpdateSelection {
		std::vector<std::uint8_t> direction;
		std::vector<std::uint8_t> point;
		std::vector<std::uint8_t> spot;
		std::vector<std::size_t> directionSignature;
		std::vector<std::size_t> pointSignature;
		std::vector<std::size_t> spotSignature;
		std::vector<std::uint8_t> pointRequiredFaceMask;
		std::vector<std::uint8_t> pointUpdateFaceMask;
		std::vector<std::array<std::size_t, 6>>
			pointFaceSignatures;
	};

	struct ShadowSamplerBindingState {
		unsigned int programId = 0;
		std::uint64_t shaderRevision = 0;
		std::size_t pointCount = 0;
		std::size_t directionCount = 0;
		std::size_t spotCount = 0;
	};

	void BuildMeshDrawLists();
	bool ComputeShadowCasterBounds(glm::vec3& center, float& radius) const;
	std::size_t BuildShadowCacheSignature() const;
	std::size_t BuildShadowRevisionSignature(
		std::uint64_t shadowShaderRevision,
		std::uint64_t pointShadowShaderRevision) const;
	std::size_t BuildDirectionalShadowRevisionSignature(
		const DirectionLight& light,
		std::uint64_t shadowShaderRevision) const;
	std::size_t BuildPointShadowRevisionSignature(
		const PointLight& light,
		std::uint64_t pointShadowShaderRevision) const;
	std::size_t BuildSpotShadowRevisionSignature(
		const SpotLight& light,
		std::uint64_t shadowShaderRevision) const;
	std::size_t BuildSpatialShadowCasterSignature(
		const glm::mat4& lightViewProjection) const;
	std::array<std::size_t, 6>
		BuildPointShadowFaceRevisionSignatures(
			const PointLight& light,
			std::uint64_t pointShadowShaderRevision,
			const std::array<glm::mat4, 6>& lightSpaceMatrices) const;
	std::uint8_t ComputePointShadowRequiredFaceMask(
		const PointLight& light) const;
	bool IsPointShadowPerFaceCacheEnabled() const;
	void CommitShadowCasterState(
		std::size_t signature,
		const std::vector<ShadowCasterBoundItem>& bounds);
	void RefreshShadowCasterStateFallback();
	void DrawShadowMapRevision();
	void DrawShadowMapRevisionGlobal(bool forceUpdate);
	void DrawShadowMapPerLight();
	void InvalidatePerLightShadowCaches();
	void SynchronizeSceneTopologyRevision();
	void SynchronizeShadowCacheGranularity(bool perLightCacheEnabled);
	void DisableEnabledShadowContent();
	bool CommitEnabledShadowContent();
	bool AreEnabledShadowMapsSampleable() const;
	bool HasActiveShadowCasters() const;
	void RenderShadowMapUpdate(
		std::uint64_t& renderedLightCount,
		std::uint64_t& trianglePassMultiplier,
		ShadowLightUpdateSelection* selection = nullptr,
		bool clearOnly = false);
	void BuildShadowCasterDrawList();
	void FitDirectionalShadowToCasterBounds(DirectionLight& light);
	void FitSpotShadowToCasterBounds(
		SpotLight& light,
		std::size_t lightIndex,
		const glm::vec3& fallbackCenter,
		float fallbackRadius);
	bool HasEnoughShadowCasterWorkForCulling() const;
	bool ShouldUseSixFacePointShadow();
	void RenderShadowCasters(
		Shader& shader,
		const glm::mat4& lightViewProjection,
		std::uint64_t trianglePassMultiplier);
	void RenderPointShadowCasters(
		Shader& shader,
		const glm::vec3& lightPosition,
		float farPlane,
		std::uint64_t trianglePassMultiplier);
	void UpdateShadowCasterStats(
		std::uint64_t renderedLightCount,
		std::uint64_t trianglePassMultiplier);
	std::vector<ModelFrameItem> m_visibleModels;
	std::vector<MeshDrawItem> m_opaqueMeshList;
	std::vector<MeshDrawItem> m_transparentMeshList;
	ImageBasedLighting* m_imageBasedLighting = nullptr;
	bool m_shadowCacheValid = false;
	std::size_t m_shadowCacheSignature = 0;
	bool m_shadowCacheStrategyInitialized = false;
	bool m_shadowCacheUsedLegacySignature = false;
	bool m_shadowCacheDisabledLastFrame = false;
	bool m_shadowCacheGranularityInitialized = false;
	bool m_shadowCacheUsedPerLight = false;
	bool m_shadowCacheFeatureStateInitialized = false;
	bool m_shadowCacheUsedSpatialCasters = false;
	bool m_shadowCacheUsedPointFaces = false;
	bool m_shadowCasterStateInitialized = false;
	bool m_shadowCasterStatePrepared = false;
	bool m_shadowCasterStateReliable = true;
	bool m_shadowCasterBoundsValid = false;
	std::size_t m_shadowCasterStateSignature = 0;
	std::uint64_t m_shadowCasterRevision = 0;
	std::uint64_t m_observedSceneTopologyRevision = 0;
	glm::vec3 m_cachedShadowCasterCenter = glm::vec3(0.0f);
	float m_cachedShadowCasterRadius = 0.0f;
	std::vector<ShadowCasterBoundItem> m_shadowCasterBoundsScratch;
	std::vector<ShadowCasterDrawItem> m_shadowCasterDrawList;
	bool m_shadowCasterDrawListValid = false;
	std::uint64_t m_shadowCasterDrawListRevision = 0;
	std::uint64_t m_shadowCasterDrawListTriangleCount = 0;
	std::uint64_t m_pendingShadowCasterCandidateCount = 0;
	std::uint64_t m_pendingShadowCasterCulledCount = 0;
	std::uint64_t m_pendingShadowCasterCullingLightCount = 0;
	std::uint64_t m_pendingShadowCasterDrawCount = 0;
	std::uint64_t m_pendingShadowCasterTriangleCount = 0;
	std::uint64_t m_pendingUnculledRenderedLightCount = 0;
	std::uint64_t m_pendingUnculledTrianglePassMultiplier = 0;
	std::uint64_t m_pendingShadowDrawPassMultiplier = 0;
	ShadowSystemStats m_shadowStats;
	glm::mat4 view;
	glm::mat4 projection;
	SystemProperties& properties = SystemProperties::GetInstance();
};
