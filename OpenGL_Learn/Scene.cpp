#include "Scene.h"
#include "GLStateCache.h"
#include "ImageBasedLighting.h"
#include "Profiler.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <numeric>
#include <unordered_set>

namespace {
	struct Frustum {
		std::array<glm::vec4, 6> planes{};

		bool IntersectsSphere(const glm::vec3& center, float radius) const
		{
			for (const glm::vec4& plane : planes) {
				if (glm::dot(glm::vec3(plane), center) + plane.w < -radius) {
					return false;
				}
			}
			return true;
		}

		bool IntersectsObb(
			const glm::vec3& center,
			const glm::vec3& axisX,
			const glm::vec3& axisY,
			const glm::vec3& axisZ) const
		{
			for (const glm::vec4& plane : planes) {
				const glm::vec3 normal(plane);
				const float projectedRadius =
					std::abs(glm::dot(normal, axisX)) +
					std::abs(glm::dot(normal, axisY)) +
					std::abs(glm::dot(normal, axisZ));
				if (glm::dot(normal, center) + plane.w <
					-projectedRadius) {
					return false;
				}
			}
			return true;
		}
	};

	Frustum BuildFrustum(const glm::mat4& viewProjection)
	{
		const glm::vec4 row0(
			viewProjection[0][0], viewProjection[1][0],
			viewProjection[2][0], viewProjection[3][0]);
		const glm::vec4 row1(
			viewProjection[0][1], viewProjection[1][1],
			viewProjection[2][1], viewProjection[3][1]);
		const glm::vec4 row2(
			viewProjection[0][2], viewProjection[1][2],
			viewProjection[2][2], viewProjection[3][2]);
		const glm::vec4 row3(
			viewProjection[0][3], viewProjection[1][3],
			viewProjection[2][3], viewProjection[3][3]);

		Frustum frustum;
		frustum.planes = {
			row3 + row0,
			row3 - row0,
			row3 + row1,
			row3 - row1,
			row3 + row2,
			row3 - row2
		};
		for (glm::vec4& plane : frustum.planes) {
			const float normalLength = glm::length(glm::vec3(plane));
			if (normalLength > 0.0f) {
				plane /= normalLength;
			}
		}
		return frustum;
	}

	float GetMaximumWorldScale(const glm::mat4& modelMatrix)
	{
		const float scaleX = glm::length(glm::vec3(modelMatrix[0]));
		const float scaleY = glm::length(glm::vec3(modelMatrix[1]));
		const float scaleZ = glm::length(glm::vec3(modelMatrix[2]));
		return (std::max)(scaleX, (std::max)(scaleY, scaleZ));
	}

	bool IsFiniteVector(const glm::vec3& value)
	{
		return std::isfinite(value.x) &&
			std::isfinite(value.y) &&
			std::isfinite(value.z);
	}

	bool IsFiniteMatrix(const glm::mat4& value)
	{
		for (int column = 0; column < 4; ++column) {
			for (int row = 0; row < 4; ++row) {
				if (!std::isfinite(value[column][row])) {
					return false;
				}
			}
		}
		return true;
	}

	bool IsShadowShaderReady(const std::shared_ptr<Shader>& shader)
	{
		return shader && shader->ID != 0;
	}

	void TransformBoundsToSphere(
		const glm::mat4& modelMatrix,
		const glm::vec3& localBoundsMin,
		const glm::vec3& localBoundsMax,
		glm::vec3& worldCenter,
		float& worldRadius)
	{
		const glm::vec3 localCenter =
			(localBoundsMin + localBoundsMax) * 0.5f;
		worldCenter = glm::vec3(
			modelMatrix * glm::vec4(localCenter, 1.0f));
		worldRadius = 0.0f;
		for (int corner = 0; corner < 8; ++corner) {
			const glm::vec3 localCorner(
				(corner & 1) ? localBoundsMax.x : localBoundsMin.x,
				(corner & 2) ? localBoundsMax.y : localBoundsMin.y,
				(corner & 4) ? localBoundsMax.z : localBoundsMin.z);
			const glm::vec3 worldCorner = glm::vec3(
				modelMatrix * glm::vec4(localCorner, 1.0f));
			worldRadius = (std::max)(
				worldRadius,
				glm::length(worldCorner - worldCenter));
		}
	}

	std::uint64_t NextShadowStateSyncEpoch()
	{
		static std::uint64_t epoch = 0;
		++epoch;
		if (epoch == 0) {
			++epoch;
		}
		return epoch;
	}

	struct ShadowCacheCheckTelemetry {
		Scene::ShadowSystemStats& stats;
		bool legacy = false;
		std::chrono::steady_clock::time_point start =
			std::chrono::steady_clock::now();

		~ShadowCacheCheckTelemetry()
		{
			const double elapsedMilliseconds =
				std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - start).count();
			++stats.cacheCheckCount;
			if (legacy) {
				++stats.legacySignatureCheckCount;
			}
			else {
				++stats.revisionCheckCount;
			}
			stats.lastCacheCheckCpuMilliseconds =
				elapsedMilliseconds;
			stats.totalCacheCheckCpuMilliseconds +=
				elapsedMilliseconds;
			stats.lastCacheCheckUsedLegacySignature = legacy;
		}
	};
}

void Scene::BuildMeshDrawLists()
{
	PERF_CPU_SCOPE("Build Draw Lists");
	const auto& models = modelSource.GetModels();
	m_visibleModels.clear();
	m_opaqueMeshList.clear();
	m_transparentMeshList.clear();
	m_visibleModels.reserve(models.size());
	std::uint64_t activeModelCount = 0;
	std::uint64_t visibleModelCount = 0;
	std::uint64_t culledModelCount = 0;
	std::uint64_t culledMeshCount = 0;
	const bool syncShadowState =
		!properties.SHADOW_CACHE_USE_LEGACY_SIGNATURE;
	const std::uint64_t shadowStateSyncEpoch =
		syncShadowState ? NextShadowStateSyncEpoch() : 0;
	std::size_t shadowCasterSignature = 0;
	double shadowStateSyncMilliseconds = 0.0;
	if (syncShadowState) {
		m_shadowCasterBoundsScratch.clear();
		m_shadowCasterBoundsScratch.reserve(models.size());
		hash_combine(
			shadowCasterSignature,
			modelSource.GetSceneTopologyRevision());
		hash_combine(shadowCasterSignature, models.size());
	}

	const bool useFrustumCulling = properties.FRUSTUM_CULLING && camera_ptr;
	Frustum cameraFrustum;
	if (useFrustumCulling) {
		const float aspectRatio = static_cast<float>(properties.SCREEN_WIDTH) /
			static_cast<float>((std::max)(1, properties.SCREEN_HEIGHT));
		cameraFrustum = BuildFrustum(
			camera_ptr->GetProjectionMatrix(aspectRatio) *
			camera_ptr->GetViewMatrix());
	}

	{
	PERF_CPU_SCOPE("Draw Item Collection");
	for (const auto& model : models) {
		std::uint64_t modelShadowStateRevision = 0;
		if (syncShadowState) {
			const auto syncStart = std::chrono::steady_clock::now();
			hash_combine(
				shadowCasterSignature,
				reinterpret_cast<std::uintptr_t>(model.get()));
			if (model) {
				// Refresh before camera culling: an off-camera alpha-tested mesh
				// can still cast a visible shadow.
				model->RefreshMaterialDrivenState();
				modelShadowStateRevision =
					model->SyncShadowStateRevision(
						shadowStateSyncEpoch);
				hash_combine(
					shadowCasterSignature,
					modelShadowStateRevision);
			}
			shadowStateSyncMilliseconds +=
				std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - syncStart).count();
		}
		if (!model || !model->GetAcitveStatus()) continue;
		++activeModelCount;
		const glm::mat4 modelMatrix = model->getModelMatrix();
		const glm::vec3 worldBoundsCenter = glm::vec3(
			modelMatrix * glm::vec4(model->GetLoacalCenter(), 1.0f));
		const float worldBoundsRadius =
			model->GetLocalBoundingRadius() * GetMaximumWorldScale(modelMatrix);
		if (syncShadowState) {
			m_shadowCasterBoundsScratch.push_back({
				model.get(),
				modelShadowStateRevision,
				worldBoundsCenter,
				worldBoundsRadius });
		}

		if (useFrustumCulling &&
			!cameraFrustum.IntersectsSphere(worldBoundsCenter, worldBoundsRadius)) {
			++culledModelCount;
			for (const Mesh& mesh : model->GetMeshes()) {
				if (mesh.GetActiveStatus()) {
					++culledMeshCount;
				}
			}
			continue;
		}

		++visibleModelCount;
		m_visibleModels.push_back({
			model.get(),
			modelMatrix,
			worldBoundsCenter,
			worldBoundsRadius });
		if (!syncShadowState) {
			model->RefreshMaterialDrivenState();
		}
		auto shaderPtr = model->GetShader();
		Shader* shader = shaderPtr.get();
		if (!shader) continue;

		auto appendMeshDrawItem = [&](std::vector<MeshDrawItem>& list,
			Mesh* mesh) {
			if (!mesh || !mesh->GetActiveStatus()) {
				return;
			}
			glm::vec3 meshWorldCenter = worldBoundsCenter;
			float meshWorldRadius = worldBoundsRadius;
			const glm::vec3 localBoundsMin = mesh->GetBoundsMin();
			const glm::vec3 localBoundsMax = mesh->GetBoundsMax();
			const glm::vec3 localBoundsExtent =
				(localBoundsMax - localBoundsMin) * 0.5f;
			const glm::vec3 worldBoundsAxisX =
				glm::vec3(modelMatrix[0]) * localBoundsExtent.x;
			const glm::vec3 worldBoundsAxisY =
				glm::vec3(modelMatrix[1]) * localBoundsExtent.y;
			const glm::vec3 worldBoundsAxisZ =
				glm::vec3(modelMatrix[2]) * localBoundsExtent.z;
			TransformBoundsToSphere(
				modelMatrix,
				localBoundsMin,
				localBoundsMax,
				meshWorldCenter,
				meshWorldRadius);
			const bool meshWorldBoundsValid =
				IsFiniteMatrix(modelMatrix) &&
				IsFiniteVector(localBoundsMin) &&
				IsFiniteVector(localBoundsMax) &&
				localBoundsMin.x <= localBoundsMax.x &&
				localBoundsMin.y <= localBoundsMax.y &&
				localBoundsMin.z <= localBoundsMax.z &&
				IsFiniteVector(meshWorldCenter) &&
				IsFiniteVector(worldBoundsAxisX) &&
				IsFiniteVector(worldBoundsAxisY) &&
				IsFiniteVector(worldBoundsAxisZ) &&
				std::isfinite(meshWorldRadius) &&
				meshWorldRadius >= 0.0f;
			list.push_back({
				model.get(),
				mesh,
				shader,
				0,
				modelMatrix,
				meshWorldCenter,
				worldBoundsAxisX,
				worldBoundsAxisY,
				worldBoundsAxisZ,
				meshWorldBoundsValid,
				meshWorldRadius });
		};
		for (const auto& entry : model->GetOpaqueMeshEntries()) {
			appendMeshDrawItem(m_opaqueMeshList, entry.mesh);
		}
		for (const auto& entry : model->GetTransparentMeshEntries()) {
			appendMeshDrawItem(m_transparentMeshList, entry.mesh);
		}
	}
	}

	if (syncShadowState) {
		const auto commitStart = std::chrono::steady_clock::now();
		CommitShadowCasterState(
			shadowCasterSignature,
			m_shadowCasterBoundsScratch);
		shadowStateSyncMilliseconds +=
			std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - commitStart).count();
		m_shadowCasterStatePrepared = true;
		++m_shadowStats.casterStateSyncCount;
		m_shadowStats.lastCasterStateSyncCpuMilliseconds =
			shadowStateSyncMilliseconds;
		m_shadowStats.totalCasterStateSyncCpuMilliseconds +=
			shadowStateSyncMilliseconds;
	}
	else {
		m_shadowCasterStatePrepared = false;
		m_shadowStats.lastCasterStateSyncCpuMilliseconds = 0.0;
	}

	{
		PERF_CPU_SCOPE("Opaque Draw Sorting");
		// Pointer-address sorting changes across independent processes. Group
		// ordinals therefore come from deterministic scene traversal rather than
		// pointer values. The legacy path retains the original map lookups for a
		// same-binary A/B; optimized comparators only read the precomputed key.
		if (m_opaqueSortMode == OpaqueSortMode::LegacyMapComparator) {
			std::unordered_map<Shader*, std::size_t> shaderSortOrder;
			std::unordered_map<Material*, std::size_t> materialSortOrder;
			{
				PERF_CPU_SCOPE("Opaque Legacy Order Build");
				for (MeshDrawItem& item : m_opaqueMeshList) {
					auto shaderOrder =
						shaderSortOrder.find(item.shader);
					if (shaderOrder == shaderSortOrder.end()) {
						shaderOrder = shaderSortOrder.emplace(
							item.shader,
							shaderSortOrder.size()).first;
					}
					Material* material =
						item.mesh ? item.mesh->material_ptr : nullptr;
					auto materialOrder =
						materialSortOrder.find(material);
					if (materialOrder ==
						materialSortOrder.end()) {
						materialOrder = materialSortOrder.emplace(
							material,
							materialSortOrder.size()).first;
					}
					item.opaqueSortKey =
						(static_cast<std::uint64_t>(
							shaderOrder->second) << 32) |
						static_cast<std::uint64_t>(
							materialOrder->second);
				}
			}
			{
				PERF_CPU_SCOPE("Opaque Sort Algorithm");
				std::stable_sort(
					m_opaqueMeshList.begin(),
					m_opaqueMeshList.end(),
					[&shaderSortOrder, &materialSortOrder](
						const MeshDrawItem& a,
						const MeshDrawItem& b) {
						const std::size_t aShaderOrder =
							shaderSortOrder.at(a.shader);
						const std::size_t bShaderOrder =
							shaderSortOrder.at(b.shader);
						if (aShaderOrder != bShaderOrder) {
							return aShaderOrder < bShaderOrder;
						}

						Material* aMaterial =
							a.mesh ? a.mesh->material_ptr : nullptr;
						Material* bMaterial =
							b.mesh ? b.mesh->material_ptr : nullptr;
						return materialSortOrder.at(aMaterial) <
							materialSortOrder.at(bMaterial);
					});
			}
		}
		else {
			{
				PERF_CPU_SCOPE("Opaque Sort Key Build");
				std::unordered_map<Shader*, std::uint32_t> shaderSortOrder;
				std::unordered_map<Material*, std::uint32_t>
					materialSortOrder;
				// Typical scenes have far fewer shader/material groups than
				// draws. Reserving one bucket per draw made key construction
				// itself scale with the 30k stress count.
				shaderSortOrder.reserve((std::min)(
					m_opaqueMeshList.size(),
					std::size_t{ 128 }));
				materialSortOrder.reserve((std::min)(
					m_opaqueMeshList.size(),
					std::size_t{ 1024 }));
				for (MeshDrawItem& item : m_opaqueMeshList) {
					const auto shaderResult = shaderSortOrder.try_emplace(
						item.shader,
						static_cast<std::uint32_t>(
							shaderSortOrder.size()));
					Material* material =
						item.mesh ? item.mesh->material_ptr : nullptr;
					const auto materialResult =
						materialSortOrder.try_emplace(
							material,
							static_cast<std::uint32_t>(
								materialSortOrder.size()));
					item.opaqueSortKey =
						(static_cast<std::uint64_t>(
							shaderResult.first->second) << 32) |
						static_cast<std::uint64_t>(
							materialResult.first->second);
				}
			}

			if (m_opaqueSortMode == OpaqueSortMode::KeyDirect ||
				m_opaqueMeshList.size() >
					static_cast<std::size_t>(
						(std::numeric_limits<std::uint32_t>::max)())) {
				PERF_CPU_SCOPE("Opaque Sort Algorithm");
				std::stable_sort(
					m_opaqueMeshList.begin(),
					m_opaqueMeshList.end(),
					[](const MeshDrawItem& a, const MeshDrawItem& b) {
						return a.opaqueSortKey < b.opaqueSortKey;
					});
			}
			else {
				{
					PERF_CPU_SCOPE("Opaque Sort Algorithm");
					m_opaqueSortIndices.resize(m_opaqueMeshList.size());
					std::iota(
						m_opaqueSortIndices.begin(),
						m_opaqueSortIndices.end(),
						std::uint32_t{ 0 });
					std::stable_sort(
						m_opaqueSortIndices.begin(),
						m_opaqueSortIndices.end(),
						[this](std::uint32_t a, std::uint32_t b) {
							return m_opaqueMeshList[a].opaqueSortKey <
								m_opaqueMeshList[b].opaqueSortKey;
						});
				}
				{
					PERF_CPU_SCOPE("Opaque Index Materialization");
					m_opaqueMeshSortScratch.clear();
					m_opaqueMeshSortScratch.reserve(
						m_opaqueMeshList.size());
					for (const std::uint32_t index :
						m_opaqueSortIndices) {
						m_opaqueMeshSortScratch.push_back(
							std::move(m_opaqueMeshList[index]));
					}
					m_opaqueMeshList.swap(m_opaqueMeshSortScratch);
				}
			}
		}
	}

	{
		PERF_CPU_SCOPE("Transparent Draw Sorting");
		if (camera_ptr) {
			std::sort(m_transparentMeshList.begin(), m_transparentMeshList.end(),
				[this](const MeshDrawItem& a, const MeshDrawItem& b) {
					const glm::vec3 aDelta =
						camera_ptr->cameraPos - a.worldBoundsCenter;
					const glm::vec3 bDelta =
						camera_ptr->cameraPos - b.worldBoundsCenter;
					const float da = glm::dot(aDelta, aDelta);
					const float db = glm::dot(bDelta, bDelta);
					return da > db;
				});
		}
	}

	PerformanceProfiler::GetInstance().SetSceneSubmissionStats(
		activeModelCount,
		visibleModelCount,
		culledModelCount,
		culledMeshCount,
		m_opaqueMeshList.size(),
		m_transparentMeshList.size());
}

void Scene::ProfileCollectionBreakdown()
{
	PERF_CPU_SCOPE("Collection Probe Total");
	const auto& models = modelSource.GetModels();
	const bool useFrustumCulling =
		properties.FRUSTUM_CULLING && camera_ptr;
	Frustum cameraFrustum;
	if (useFrustumCulling) {
		const float aspectRatio =
			static_cast<float>(properties.SCREEN_WIDTH) /
			static_cast<float>(
				(std::max)(1, properties.SCREEN_HEIGHT));
		cameraFrustum = BuildFrustum(
			camera_ptr->GetProjectionMatrix(aspectRatio) *
			camera_ptr->GetViewMatrix());
	}

	m_collectionProbeModels.clear();
	m_collectionProbeModels.reserve(models.size());
	{
		PERF_CPU_SCOPE(
			"Collection Probe Model Matrix Bounds Frustum");
		for (const auto& model : models) {
			if (!model || !model->GetAcitveStatus()) {
				continue;
			}
			const glm::mat4 modelMatrix = model->getModelMatrix();
			const glm::vec3 worldBoundsCenter = glm::vec3(
				modelMatrix *
				glm::vec4(model->GetLoacalCenter(), 1.0f));
			const float worldBoundsRadius =
				model->GetLocalBoundingRadius() *
				GetMaximumWorldScale(modelMatrix);
			if (useFrustumCulling &&
				!cameraFrustum.IntersectsSphere(
					worldBoundsCenter,
					worldBoundsRadius)) {
				continue;
			}
			m_collectionProbeModels.push_back({
				model.get(),
				modelMatrix,
				worldBoundsCenter,
				worldBoundsRadius });
		}
	}

	{
		PERF_CPU_SCOPE("Collection Probe Material Revision");
		for (const ModelFrameItem& modelItem :
			m_collectionProbeModels) {
			if (modelItem.model) {
				modelItem.model->RefreshMaterialDrivenState();
			}
		}
	}

	m_collectionProbePreparedItems.clear();
	m_collectionProbePreparedItems.reserve(
		m_collectionProbeModels.size());
	{
		PERF_CPU_SCOPE(
			"Collection Probe Mesh Bounds Validation");
		for (const ModelFrameItem& modelItem :
			m_collectionProbeModels) {
			Model* model = modelItem.model;
			if (!model) {
				continue;
			}
			auto shaderPtr = model->GetShader();
			Shader* shader = shaderPtr.get();
			if (!shader) {
				continue;
			}
			auto appendPreparedItem = [&](
				Mesh* mesh,
				bool transparent) {
				if (!mesh || !mesh->GetActiveStatus()) {
					return;
				}

				const glm::vec3 localBoundsMin =
					mesh->GetBoundsMin();
				const glm::vec3 localBoundsMax =
					mesh->GetBoundsMax();
				const glm::vec3 localBoundsExtent =
					(localBoundsMax - localBoundsMin) * 0.5f;
				const glm::vec3 worldBoundsAxisX =
					glm::vec3(modelItem.modelMatrix[0]) *
					localBoundsExtent.x;
				const glm::vec3 worldBoundsAxisY =
					glm::vec3(modelItem.modelMatrix[1]) *
					localBoundsExtent.y;
				const glm::vec3 worldBoundsAxisZ =
					glm::vec3(modelItem.modelMatrix[2]) *
					localBoundsExtent.z;
				glm::vec3 meshWorldCenter =
					modelItem.worldBoundsCenter;
				float meshWorldRadius =
					modelItem.worldBoundsRadius;
				TransformBoundsToSphere(
					modelItem.modelMatrix,
					localBoundsMin,
					localBoundsMax,
					meshWorldCenter,
					meshWorldRadius);
				const bool meshWorldBoundsValid =
					IsFiniteMatrix(modelItem.modelMatrix) &&
					IsFiniteVector(localBoundsMin) &&
					IsFiniteVector(localBoundsMax) &&
					localBoundsMin.x <= localBoundsMax.x &&
					localBoundsMin.y <= localBoundsMax.y &&
					localBoundsMin.z <= localBoundsMax.z &&
					IsFiniteVector(meshWorldCenter) &&
					IsFiniteVector(worldBoundsAxisX) &&
					IsFiniteVector(worldBoundsAxisY) &&
					IsFiniteVector(worldBoundsAxisZ) &&
					std::isfinite(meshWorldRadius) &&
					meshWorldRadius >= 0.0f;

				CollectionProbePreparedItem prepared;
				prepared.item.model = model;
				prepared.item.mesh = mesh;
				prepared.item.shader = shader;
				prepared.item.modelMatrix =
					modelItem.modelMatrix;
				prepared.item.worldBoundsCenter =
					meshWorldCenter;
				prepared.item.worldBoundsAxisX =
					worldBoundsAxisX;
				prepared.item.worldBoundsAxisY =
					worldBoundsAxisY;
				prepared.item.worldBoundsAxisZ =
					worldBoundsAxisZ;
				prepared.item.worldBoundsValid =
					meshWorldBoundsValid;
				prepared.item.worldBoundsRadius =
					meshWorldRadius;
				prepared.transparent = transparent;
				m_collectionProbePreparedItems.push_back(
					std::move(prepared));
			};

			for (const auto& entry :
				model->GetOpaqueMeshEntries()) {
				appendPreparedItem(entry.mesh, false);
			}
			for (const auto& entry :
				model->GetTransparentMeshEntries()) {
				appendPreparedItem(entry.mesh, true);
			}
		}
	}

	{
		PERF_CPU_SCOPE("Collection Probe DrawItem Write");
		m_collectionProbeOpaqueItems.clear();
		m_collectionProbeTransparentItems.clear();
		m_collectionProbeOpaqueItems.reserve(
			m_collectionProbePreparedItems.size());
		m_collectionProbeTransparentItems.reserve(
			m_collectionProbePreparedItems.size());
		for (const CollectionProbePreparedItem& prepared :
			m_collectionProbePreparedItems) {
			if (prepared.transparent) {
				m_collectionProbeTransparentItems.push_back(
					prepared.item);
			}
			else {
				m_collectionProbeOpaqueItems.push_back(
					prepared.item);
			}
		}
	}
}

void Scene::PrepareRenderData()
{
	SynchronizeSceneTopologyRevision();
	BuildMeshDrawLists();
}

const std::vector<Scene::MeshDrawItem>& Scene::GetOpaqueMeshes() const
{
	return m_opaqueMeshList;
}

const std::vector<Scene::MeshDrawItem>& Scene::GetTransparentMeshes() const
{
	return m_transparentMeshList;
}

const char* Scene::GetOpaqueSortModeName(OpaqueSortMode mode)
{
	switch (mode) {
	case OpaqueSortMode::LegacyMapComparator:
		return "legacy";
	case OpaqueSortMode::KeyDirect:
		return "key-direct";
	case OpaqueSortMode::KeyIndex:
		return "key-index";
	default:
		return "unknown";
	}
}

std::uint64_t Scene::ComputeOpaqueSubmissionSignature() const
{
	constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
	constexpr std::uint64_t kFnvPrime = 1099511628211ull;
	std::uint64_t signature = kFnvOffset;
	auto hashBytes = [&](const void* data, std::size_t size) {
		const auto* bytes =
			static_cast<const unsigned char*>(data);
		for (std::size_t index = 0; index < size; ++index) {
			signature ^= bytes[index];
			signature *= kFnvPrime;
		}
	};

	const std::uint64_t itemCount =
		static_cast<std::uint64_t>(m_opaqueMeshList.size());
	hashBytes(&itemCount, sizeof(itemCount));
	for (const MeshDrawItem& item : m_opaqueMeshList) {
		hashBytes(
			&item.opaqueSortKey,
			sizeof(item.opaqueSortKey));
		const std::uint64_t drawCount = item.mesh
			? static_cast<std::uint64_t>(
				item.mesh->GetDrawCount())
			: 0;
		hashBytes(&drawCount, sizeof(drawCount));
		for (int column = 0; column < 4; ++column) {
			for (int row = 0; row < 4; ++row) {
				std::uint32_t bits = 0;
				const float value =
					item.modelMatrix[column][row];
				static_assert(
					sizeof(bits) == sizeof(value),
					"float signature assumes 32-bit float");
				std::memcpy(&bits, &value, sizeof(bits));
				hashBytes(&bits, sizeof(bits));
			}
		}
	}
	return signature;
}

unsigned int Scene::BindImageBasedLighting(Shader& shader, unsigned int firstTextureUnit) const
{
	if (!m_imageBasedLighting) {
		shader.setBool("useIBL", false);
		return firstTextureUnit;
	}
	return m_imageBasedLighting->Bind(shader, firstTextureUnit);
}

bool Scene::UsesPbrMaterials() const
{
	for (const auto& model : modelSource.GetModels()) {
		if (!model) {
			continue;
		}
		const auto modelShader = model->GetShader();
		if (modelShader && modelShader->shaderName == "pbr") {
			return true;
		}
		for (const Mesh& mesh : model->GetMeshes()) {
			if (mesh.material_ptr && mesh.material_ptr->GetShaderName() == "pbr") {
				return true;
			}
		}
	}
	return false;
}
void Scene::Draw()
{
    PrepareRenderData();
    DrawShadowMap();
    auto attr = FramebuffersManager::GenCurrentAttr();
    fbo = FramebuffersManager::GetInstance().GetFBO(attr);
    GLState::BindFramebuffer(GL_FRAMEBUFFER, fbo->framebufferID);

    GLState::Enable(GL_DEPTH_TEST);
    GLState::Enable(GL_STENCIL_TEST);

    GLState::StencilFunc(GL_ALWAYS, 0, 0xFF);
    GLState::StencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearStencil(0);

    GLState::StencilMask(0xFF);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    GLState::StencilMask(0x00);
    //Draw scene in the following order
    if (properties.DEFER_RENDERING) {
        DrawDefferedModels();
    }
    else {
        DrawOpaqueModels();  // 先绘制所有不透明物体，记录需要outline的物体到stencil buffer
    }
    DrawNormalLines(); // 可选：绘制法线线段用于调试
    DrawPointLights();
    DrawSkybox(view);        // 绘制天空盒（使用深度测试优化，但不影响stencil buffer）
    DrawTransparentModels();  // 绘制透明物体
    DrawOutlines();
    // 最后绘制outline（禁用深度测试，基于stencil buffer绘制）
    GLState::BindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Scene::DrawDefferedModels()
{
    if (!properties.DEFER_RENDERING) return;
    if (properties.GBUFFER_POSITION_MODE ==
        GBufferPositionProperty::ReconstructFromDepth) {
        static bool warned = false;
        if (!warned) {
            std::cerr
                << "[GBufferExperiment] legacy Scene::DrawDefferedModels is outside "
                << "the reconstruction experiment; it explicitly falls back to "
                << "the existing gPosition path."
                << std::endl;
            warned = true;
        }
        // This legacy renderer is not attachment-layout aware. Disable the
        // candidate before it can expose shifted MRT semantics to later work.
        properties.GBUFFER_POSITION_MODE = GBufferPositionProperty::Explicit;
    }
    FBOAttributes attr = FramebuffersManager::GenCurrentAttr();
    //反走样不支持MSAA
    attr.aaType = AntiAliasManager::AntiAliasType::Default;
    attr.isDefer = true;
    deferFBO = FramebuffersManager::GetInstance().GetFBO(attr);
    GLState::Disable(GL_BLEND);
    GLState::BindFramebuffer(GL_FRAMEBUFFER, deferFBO->framebufferID);
    GLState::StencilMask(0x00);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    GLState::StencilMask(0x00);
    deferShader->use();
    auto& list = GetOpaqueMeshes();
    {
        MaterialBatchScope materialBatch;
        for (const auto& item : list) {
            if (!item.model || !item.mesh) continue;
            deferShader->setMat4("model", item.modelMatrix);
            item.mesh->Draw(deferShader.get());
        }
    }

    GLState::BindFramebuffer(GL_READ_FRAMEBUFFER, deferFBO->framebufferID);
    GLState::BindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo->framebufferID);
    glBlitFramebuffer(
        0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT, 0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT,
        GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST
    );

    GLState::BindFramebuffer(GL_FRAMEBUFFER, fbo->framebufferID);
    GLState::StencilMask(0xFF);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    if (!properties.LIGHT_VOLUME) {
        //默认光照计算
        GLState::Disable(GL_DEPTH_TEST);
        auto deferDrawShader = ShaderManager::GetInstance().GetShader(ShaderManager::Defer);
        deferDrawShader->use();
        SetLightUniforms(*deferDrawShader);
        properties.USED_TEXTURE_NUM = SetShadowMap(
            *deferDrawShader,
            ShadowLightBinding::AllLights,
            9);
        deferDrawShader->setVec3("viewPos", camera_ptr->cameraPos);
        GLState::ActiveTexture(GL_TEXTURE0 + properties.USED_TEXTURE_NUM);
        GLState::BindTexture(GL_TEXTURE_2D, deferFBO->textureIDs[0]);
        deferDrawShader->setInt("gPosition", properties.USED_TEXTURE_NUM++);
        GLState::ActiveTexture(GL_TEXTURE0 + properties.USED_TEXTURE_NUM);
        GLState::BindTexture(GL_TEXTURE_2D, deferFBO->textureIDs[1]);
        deferDrawShader->setInt("gNormal", properties.USED_TEXTURE_NUM++);
        GLState::ActiveTexture(GL_TEXTURE0 + properties.USED_TEXTURE_NUM);
        GLState::BindTexture(GL_TEXTURE_2D, deferFBO->textureIDs[2]);
        deferDrawShader->setInt("gAlbedoSpec", properties.USED_TEXTURE_NUM++);
        GLState::ActiveTexture(GL_TEXTURE0 + properties.USED_TEXTURE_NUM);
        GLState::BindTexture(GL_TEXTURE_2D, deferFBO->textureIDs[3]);
        deferDrawShader->setInt("gMaterial", properties.USED_TEXTURE_NUM++);

        GLState::BindVertexArray(globalVAOs.quadVAO);
        PerformanceProfiler::GetInstance().RecordDraw(GL_TRIANGLES, 6);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        GLState::Enable(GL_DEPTH_TEST);
    }
    else {
        //使用延迟渲染计算平行光照
        GLState::Disable(GL_DEPTH_TEST);
        auto deferDirDrawShader = ShaderManager::GetInstance().GetShader(ShaderManager::DeferDirLightVolume);
        deferDirDrawShader->use();
        SetLightUniforms(*deferDirDrawShader);
        properties.USED_TEXTURE_NUM = SetShadowMap(
            *deferDirDrawShader,
            ShadowLightBinding::DirectionalOnly,
            6);
        deferDirDrawShader->setVec3("viewPos", camera_ptr->cameraPos);
        GLState::ActiveTexture(GL_TEXTURE0 + properties.USED_TEXTURE_NUM);
        GLState::BindTexture(GL_TEXTURE_2D, deferFBO->textureIDs[0]);
        deferDirDrawShader->setInt("gPosition", properties.USED_TEXTURE_NUM++);
        GLState::ActiveTexture(GL_TEXTURE0 + properties.USED_TEXTURE_NUM);
        GLState::BindTexture(GL_TEXTURE_2D, deferFBO->textureIDs[1]);
        deferDirDrawShader->setInt("gNormal", properties.USED_TEXTURE_NUM++);
        GLState::ActiveTexture(GL_TEXTURE0 + properties.USED_TEXTURE_NUM);
        GLState::BindTexture(GL_TEXTURE_2D, deferFBO->textureIDs[2]);
        deferDirDrawShader->setInt("gAlbedoSpec", properties.USED_TEXTURE_NUM++);
        GLState::ActiveTexture(GL_TEXTURE0 + properties.USED_TEXTURE_NUM);
        GLState::BindTexture(GL_TEXTURE_2D, deferFBO->textureIDs[3]);
        deferDirDrawShader->setInt("gMaterial", properties.USED_TEXTURE_NUM++);
        GLState::BindVertexArray(globalVAOs.quadVAO);
        PerformanceProfiler::GetInstance().RecordDraw(GL_TRIANGLES, 6);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        //模板缓冲来计算受点光源影响的区域，减少光照计算的像素数量
        auto defaultShader = ShaderManager::GetInstance().GetShader(ShaderManager::Default);
        auto lightVolumeShader = ShaderManager::GetInstance().GetShader(ShaderManager::LightVolume);
        lightVolumeShader->use();
        lightVolumeShader->setVec3("viewPos", camera_ptr->cameraPos);
        GLState::ActiveTexture(GL_TEXTURE0);
        GLState::BindTexture(GL_TEXTURE_2D, deferFBO->textureIDs[0]);
        lightVolumeShader->setInt("gPosition", 0);
        GLState::ActiveTexture(GL_TEXTURE1);
        GLState::BindTexture(GL_TEXTURE_2D, deferFBO->textureIDs[1]);
        lightVolumeShader->setInt("gNormal", 1);
        GLState::ActiveTexture(GL_TEXTURE2);
        GLState::BindTexture(GL_TEXTURE_2D, deferFBO->textureIDs[2]);
        lightVolumeShader->setInt("gAlbedoSpec", 2);
        GLState::ActiveTexture(GL_TEXTURE3);
        GLState::BindTexture(GL_TEXTURE_2D, deferFBO->textureIDs[3]);
        lightVolumeShader->setInt("gMaterial", 3);
        GLState::Enable(GL_DEPTH_TEST);
        GLState::Enable(GL_STENCIL_TEST);
        glBlendEquation(GL_FUNC_ADD);
        GLState::Enable(GL_BLEND);
        GLState::BlendFunc(GL_ONE, GL_ONE);
        for (auto& pointLight : lightSource.pointLights) {
            if (!pointLight.GetActiveStatus()) continue;
            float radius = ComputePointLightStencilVolumeRadius(
                pointLight.constant, pointLight.linear, pointLight.quadratic, pointLight.diffuse,
                properties.LIGHT_VOLUME_CUTOFF_SCALE, properties.LIGHT_VOLUME_RADIUS_SCALE);
            const glm::vec3 savedScale = pointLight.scale;
            pointLight.SetScale(glm::vec3(radius));

            GLState::Disable(GL_BLEND);
            GLState::StencilMask(0xFF);
            glClear(GL_STENCIL_BUFFER_BIT);
            GLState::ColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            GLState::DepthMask(GL_FALSE);
            GLState::Enable(GL_DEPTH_TEST);
            GLState::DepthFunc(GL_LESS);
            GLState::Disable(GL_CULL_FACE);
            GLState::StencilFunc(GL_ALWAYS, 0, 0xFF);
            GLState::StencilOpSeparate(GL_BACK, GL_KEEP, GL_INCR_WRAP, GL_KEEP);
            GLState::StencilOpSeparate(GL_FRONT, GL_KEEP, GL_DECR_WRAP, GL_KEEP);

            defaultShader->use();
            defaultShader->setMat4("model", pointLight.getModelMatrix());
            pointLight.DrawGeometry();

            lightVolumeShader->use();
            GLState::Enable(GL_BLEND);
            GLState::BlendFunc(GL_ONE, GL_ONE);
            GLState::ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            GLState::StencilFunc(GL_NOTEQUAL, 0, 0xFF);
            GLState::StencilMask(0x00);
            GLState::Enable(GL_CULL_FACE);
            GLState::CullFace(GL_FRONT);
            GLState::DepthFunc(GL_GEQUAL);

            lightVolumeShader->setVec3("pointLight.position", pointLight.position);
            lightVolumeShader->setFloat("pointLight.constant", pointLight.constant);
            lightVolumeShader->setFloat("pointLight.linear", pointLight.linear);
            lightVolumeShader->setFloat("pointLight.quadratic", pointLight.quadratic);
            lightVolumeShader->setVec3("pointLight.ambient", pointLight.ambient);
            lightVolumeShader->setVec3("pointLight.diffuse", pointLight.diffuse);
            lightVolumeShader->setVec3("pointLight.specular", pointLight.specular);
            lightVolumeShader->setFloat("pointLight.far_plane", pointLight.far);
            GLState::ActiveTexture(GL_TEXTURE4);
            FBO* pointShadowFBO = pointLight.shadowFBO;
            const bool pointShadowSampleable =
                pointLight.useShadowMap &&
                pointLight.shadowCache.IsSampleable(pointShadowFBO);
            GLState::BindTexture(
                GL_TEXTURE_CUBE_MAP,
                pointShadowSampleable
                    ? pointShadowFBO->textureIDs[0]
                    : 0);
            lightVolumeShader->setInt("pointLight.shadowCubeMap", 4);
            lightVolumeShader->setBool(
                "pointLight.useShadowMap",
                pointShadowSampleable);
            lightVolumeShader->setMat4("model", pointLight.getModelMatrix());
            GLState::ActiveTexture(GL_TEXTURE0);
            GLState::BindTexture(GL_TEXTURE_2D, deferFBO->textureIDs[0]);
            GLState::ActiveTexture(GL_TEXTURE1);
            GLState::BindTexture(GL_TEXTURE_2D, deferFBO->textureIDs[1]);
            GLState::ActiveTexture(GL_TEXTURE2);
            GLState::BindTexture(GL_TEXTURE_2D, deferFBO->textureIDs[2]);
            GLState::ActiveTexture(GL_TEXTURE3);
            GLState::BindTexture(GL_TEXTURE_2D, deferFBO->textureIDs[3]);

            pointLight.DrawGeometry();

            pointLight.SetScale(savedScale);
            GLState::Disable(GL_BLEND);
            GLState::StencilMask(0xff);
            glClear(GL_STENCIL_BUFFER_BIT);
            GLState::StencilMask(0x00);
            GLState::CullFace(GL_BACK);
            GLState::DepthFunc(GL_LESS);
        }
        // 恢复默认状态
        GLState::StencilMask(0xFF);
        GLState::Disable(GL_BLEND);
        GLState::Disable(GL_STENCIL_TEST);
        GLState::DepthMask(GL_TRUE);
        GLState::Enable(GL_DEPTH_TEST);
        GLState::DepthFunc(GL_LESS);
        GLState::Disable(GL_CULL_FACE);
    }
    GLState::BindFramebuffer(GL_FRAMEBUFFER, fbo->framebufferID);
}

void Scene::RenderScene(Shader& shader) {
	PERF_CPU_SCOPE("Shadow Caster Submission");
	MaterialBatchScope materialBatch;
	for (const auto& model : modelSource.GetModels()) {
		if (!model || !model->GetAcitveStatus()) continue;
		shader.setMat4("model", model->getModelMatrix());
		model->Draw(&shader);
	}
}

void Scene::BuildShadowCasterDrawList() {
	if (m_shadowCasterDrawListValid &&
		m_shadowCasterDrawListRevision == m_shadowCasterRevision) {
		return;
	}
	PERF_CPU_SCOPE("Build Shadow Caster List");
	m_shadowCasterDrawList.clear();
	m_shadowCasterDrawListTriangleCount = 0;

	for (const auto& model : modelSource.GetModels()) {
		if (!model || !model->GetAcitveStatus()) {
			continue;
		}
		const glm::mat4 modelMatrix = model->getModelMatrix();
		for (Mesh& mesh : model->GetMeshes()) {
			if (!mesh.GetActiveStatus() ||
				!mesh.material_ptr ||
				mesh.GetDrawCount() == 0) {
				continue;
			}
			glm::vec3 worldBoundsCenter(0.0f);
			float worldBoundsRadius = 0.0f;
			const glm::vec3 localBoundsMin = mesh.GetBoundsMin();
			const glm::vec3 localBoundsMax = mesh.GetBoundsMax();
			const glm::vec3 localBoundsExtent =
				(localBoundsMax - localBoundsMin) * 0.5f;
			const glm::vec3 worldBoundsAxisX =
				glm::vec3(modelMatrix[0]) * localBoundsExtent.x;
			const glm::vec3 worldBoundsAxisY =
				glm::vec3(modelMatrix[1]) * localBoundsExtent.y;
			const glm::vec3 worldBoundsAxisZ =
				glm::vec3(modelMatrix[2]) * localBoundsExtent.z;
			TransformBoundsToSphere(
				modelMatrix,
				localBoundsMin,
				localBoundsMax,
				worldBoundsCenter,
				worldBoundsRadius);
			const bool worldBoundsValid =
				IsFiniteMatrix(modelMatrix) &&
				glm::abs(modelMatrix[0][3]) <= 0.001f &&
				glm::abs(modelMatrix[1][3]) <= 0.001f &&
				glm::abs(modelMatrix[2][3]) <= 0.001f &&
				glm::abs(modelMatrix[3][3] - 1.0f) <= 0.001f &&
				IsFiniteVector(localBoundsMin) &&
				IsFiniteVector(localBoundsMax) &&
				localBoundsMin.x <= localBoundsMax.x &&
				localBoundsMin.y <= localBoundsMax.y &&
				localBoundsMin.z <= localBoundsMax.z &&
				IsFiniteVector(worldBoundsCenter) &&
				IsFiniteVector(worldBoundsAxisX) &&
				IsFiniteVector(worldBoundsAxisY) &&
				IsFiniteVector(worldBoundsAxisZ) &&
				std::isfinite(worldBoundsRadius);
			m_shadowCasterDrawList.push_back({
				model.get(),
				&mesh,
				modelMatrix,
				worldBoundsCenter,
				worldBoundsAxisX,
				worldBoundsAxisY,
				worldBoundsAxisZ,
				worldBoundsValid,
				worldBoundsRadius });
			m_shadowCasterDrawListTriangleCount +=
				static_cast<std::uint64_t>(mesh.GetDrawCount() / 3u);
		}
	}
	m_shadowCasterDrawListValid = true;
	m_shadowCasterDrawListRevision = m_shadowCasterRevision;
}

void Scene::FitDirectionalShadowToCasterBounds(DirectionLight& light) {
	PERF_CPU_SCOPE("Directional Shadow Fit");
	const auto fitStart = std::chrono::steady_clock::now();
	++m_shadowStats.directionalFitCount;

	bool appliedLightSpaceAabb = false;
	float rawWidth = 0.0f;
	float rawHeight = 0.0f;
	float rawDepth = 0.0f;
	const int configuredResolution =
		(std::max)(1, light.shadowResolution);
	const float referenceRadius =
		(std::max)(0.5f, m_cachedShadowCasterRadius);
	const float referenceTexelSize =
		2.0f * referenceRadius * 1.08f /
		static_cast<float>(configuredResolution);

	if (properties.DIRECTIONAL_SHADOW_LIGHT_AABB_FIT &&
		light.autoFitShadow) {
		BuildShadowCasterDrawList();
		if (!m_shadowCasterDrawList.empty()) {
			glm::vec3 lightDirection =
				glm::length(light.direction) > 0.0001f
					? glm::normalize(light.direction)
					: glm::vec3(0.0f, -1.0f, 0.0f);
			glm::vec3 usableUp(0.0f, 1.0f, 0.0f);
			if (glm::abs(glm::dot(lightDirection, usableUp)) > 0.999f) {
				usableUp = glm::vec3(1.0f, 0.0f, 0.0f);
			}
			const glm::vec3 lightRight =
				glm::normalize(glm::cross(lightDirection, usableUp));
			const glm::vec3 lightUp =
				glm::normalize(glm::cross(lightRight, lightDirection));
			const glm::vec3 lightBackward = -lightDirection;

			glm::vec3 boundsMin(std::numeric_limits<float>::max());
			glm::vec3 boundsMax(std::numeric_limits<float>::lowest());
			bool foundFiniteCorner = false;
			for (const ShadowCasterDrawItem& item :
				m_shadowCasterDrawList) {
				if (!item.mesh) {
					continue;
				}
				const glm::vec3 localMin = item.mesh->GetBoundsMin();
				const glm::vec3 localMax = item.mesh->GetBoundsMax();
				for (int corner = 0; corner < 8; ++corner) {
					const glm::vec3 localCorner(
						(corner & 1) ? localMax.x : localMin.x,
						(corner & 2) ? localMax.y : localMin.y,
						(corner & 4) ? localMax.z : localMin.z);
					const glm::vec3 worldCorner = glm::vec3(
						item.modelMatrix *
						glm::vec4(localCorner, 1.0f));
					const glm::vec3 lightSpaceCorner(
						glm::dot(lightRight, worldCorner),
						glm::dot(lightUp, worldCorner),
						glm::dot(lightBackward, worldCorner));
					if (!std::isfinite(lightSpaceCorner.x) ||
						!std::isfinite(lightSpaceCorner.y) ||
						!std::isfinite(lightSpaceCorner.z)) {
						continue;
					}
					boundsMin = glm::min(boundsMin, lightSpaceCorner);
					boundsMax = glm::max(boundsMax, lightSpaceCorner);
					foundFiniteCorner = true;
				}
			}

			if (foundFiniteCorner) {
				const glm::vec3 rawSpan =
					glm::max(boundsMax - boundsMin, glm::vec3(0.0f));
				rawWidth = rawSpan.x;
				rawHeight = rawSpan.y;
				rawDepth = rawSpan.z;

				int resolution = configuredResolution;
				if (properties.DIRECTIONAL_SHADOW_DENSITY_RESOLUTION) {
					constexpr int kResolutionQuantum = 64;
					constexpr int kResolutionHysteresis = 128;
					const float longestRawAxis =
						(std::max)(rawSpan.x, rawSpan.y);
					const float requiredResolution =
						longestRawAxis /
							(std::max)(referenceTexelSize, 0.000001f) +
						36.0f;
					int quantizedResolution =
						static_cast<int>(
							std::ceil(
								requiredResolution /
								static_cast<float>(
									kResolutionQuantum))) *
						kResolutionQuantum;
					const int minimumResolution =
						(std::min)(configuredResolution, 256);
					quantizedResolution = glm::clamp(
						quantizedResolution,
						minimumResolution,
						configuredResolution);

					const int currentResolution =
						light.GetEffectiveShadowResolution();
					if (quantizedResolution > currentResolution ||
						quantizedResolution <=
							currentResolution -
								kResolutionHysteresis) {
						resolution = quantizedResolution;
					}
					else {
						resolution = currentResolution;
					}
				}
				const float resolutionFloat =
					static_cast<float>(resolution);
				const float guardTexels = (std::min)(
					18.0f,
					(std::max)(
						0.0f,
						(resolutionFloat - 1.0f) * 0.25f));
				const float usableResolution = (std::max)(
					1.0f,
					resolutionFloat - 2.0f * guardTexels);
				const float guardScale =
					resolutionFloat / usableResolution;
				const float rawHalfWidth =
					(std::max)(0.001f, rawSpan.x * 0.5f);
				const float rawHalfHeight =
					(std::max)(0.001f, rawSpan.y * 0.5f);
				const float paddedHalfWidth =
					rawHalfWidth * guardScale;
				const float paddedHalfHeight =
					rawHalfHeight * guardScale;
				const float texelSizeX =
					2.0f * paddedHalfWidth / resolutionFloat;
				const float texelSizeY =
					2.0f * paddedHalfHeight / resolutionFloat;

				const float rawCenterX =
					(boundsMin.x + boundsMax.x) * 0.5f;
				const float rawCenterY =
					(boundsMin.y + boundsMax.y) * 0.5f;
				const float centerZ =
					(boundsMin.z + boundsMax.z) * 0.5f;
				const float snappedCenterX =
					std::round(rawCenterX / texelSizeX) * texelSizeX;
				const float snappedCenterY =
					std::round(rawCenterY / texelSizeY) * texelSizeY;
				const glm::vec3 snappedWorldCenter =
					lightRight * snappedCenterX +
					lightUp * snappedCenterY +
					lightBackward * centerZ;

				const float halfDepth =
					(std::max)(0.001f, rawSpan.z * 0.5f);
				const float maximumTexelSize =
					(std::max)(texelSizeX, texelSizeY);
				const float depthMargin = (std::max)(
					0.1f,
					(std::max)(
						rawSpan.z * 0.02f,
						maximumTexelSize * 4.0f));
				const float nearPlane = 0.05f;
				const float eyeDistance =
					halfDepth + depthMargin + nearPlane;
				const float farPlane =
					nearPlane + 2.0f * (halfDepth + depthMargin);

				const int previousResolution =
					light.GetEffectiveShadowResolution();
				light.ApplyLightSpaceAabbFit(
					snappedWorldCenter,
					paddedHalfWidth,
					paddedHalfHeight,
					eyeDistance,
					nearPlane,
					farPlane,
					resolution);
				if (light.GetEffectiveShadowResolution() !=
					previousResolution) {
					++m_shadowStats.directionalResolutionChangeCount;
				}
				appliedLightSpaceAabb = true;
				++m_shadowStats.directionalLightAabbFitCount;
			}
		}
	}

	if (!appliedLightSpaceAabb) {
		light.FitShadowToBounds(
			m_cachedShadowCasterCenter,
			m_cachedShadowCasterRadius);
	}

	const int resolution = light.GetEffectiveShadowResolution();
	const float fittedWidth = light.width * 2.0f;
	const float fittedHeight =
		(light.lightSpaceAabbFitActive
			? light.fittedHalfHeight
			: light.width) * 2.0f;
	const float fittedDepth =
		(std::max)(0.0f, light.far_plane - light.near_plane);
	m_shadowStats.lastDirectionalFitRawWidth = rawWidth;
	m_shadowStats.lastDirectionalFitRawHeight = rawHeight;
	m_shadowStats.lastDirectionalFitRawDepth = rawDepth;
	m_shadowStats.lastDirectionalFitWidth = fittedWidth;
	m_shadowStats.lastDirectionalFitHeight = fittedHeight;
	m_shadowStats.lastDirectionalFitDepth = fittedDepth;
	m_shadowStats.lastDirectionalFitTexelSizeX =
		fittedWidth / static_cast<float>(resolution);
	m_shadowStats.lastDirectionalFitTexelSizeY =
		fittedHeight / static_cast<float>(resolution);
	m_shadowStats.lastDirectionalFitUtilization =
		appliedLightSpaceAabb &&
			fittedWidth > 0.0f &&
			fittedHeight > 0.0f
			? (rawWidth * rawHeight) /
				(fittedWidth * fittedHeight)
			: 0.0f;
	m_shadowStats.lastDirectionalFitReferenceTexelSize =
		referenceTexelSize;
	m_shadowStats.lastDirectionalFitResolution = resolution;
	m_shadowStats.totalDirectionalFitCpuMilliseconds +=
		std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - fitStart).count();
}

void Scene::FitSpotShadowToCasterBounds(
	SpotLight& light,
	std::size_t lightIndex,
	const glm::vec3& fallbackCenter,
	float fallbackRadius) {
	if (!light.autoFitShadow) {
		return;
	}

	PERF_CPU_SCOPE("Spot Shadow Fit");
	const auto fitStart = std::chrono::steady_clock::now();
	++m_shadowStats.spotFitCount;

	// Establish the historical scene-sphere result first. It is both the A/B
	// control and the fail-open value for any input that cannot be classified
	// conservatively.
	light.FitShadowToBounds(
		fallbackCenter,
		fallbackRadius);
	const float legacyNear = light.near_plane;
	const float legacyFar = light.far_plane;
	std::uint64_t candidateCount = 0;
	std::uint64_t acceptedCount = 0;
	std::uint64_t rejectedCount = 0;
	float rawNear = legacyNear;
	float rawFar = legacyFar;
	float depthUtilization = 1.0f;
	float minimumProjectedCoverageMargin = 0.0f;
	bool rawNearClipped = false;
	auto perspectiveDepthScale = [](float nearPlane, float farPlane) {
		const float denominator = farPlane - nearPlane;
		return denominator > 0.0f
			? farPlane * nearPlane / denominator
			: 0.0f;
	};
	const float legacyProjectionDepthScale =
		perspectiveDepthScale(legacyNear, legacyFar);
	float fittedProjectionDepthScale = legacyProjectionDepthScale;
	float precisionGain = 1.0f;

	auto finish = [&](bool projectionAware, bool fallback) {
		if (projectionAware) {
			++m_shadowStats.spotProjectionAwareFitCount;
		}
		if (fallback) {
			++m_shadowStats.spotFitFallbackCount;
		}
		m_shadowStats.totalSpotFitCandidateCount += candidateCount;
		m_shadowStats.totalSpotFitAcceptedCount += acceptedCount;
		m_shadowStats.totalSpotFitRejectedCount += rejectedCount;
		m_shadowStats.lastSpotFitCandidateCount = candidateCount;
		m_shadowStats.lastSpotFitAcceptedCount = acceptedCount;
		m_shadowStats.lastSpotFitRejectedCount = rejectedCount;
		m_shadowStats.lastSpotFitLegacyNear = legacyNear;
		m_shadowStats.lastSpotFitLegacyFar = legacyFar;
		m_shadowStats.lastSpotFitRawNear = rawNear;
		m_shadowStats.lastSpotFitRawFar = rawFar;
		m_shadowStats.lastSpotFitNear = light.near_plane;
		m_shadowStats.lastSpotFitFar = light.far_plane;
		const float legacySpan =
			(std::max)(0.0f, legacyFar - legacyNear);
		const float fittedSpan =
			(std::max)(0.0f, light.far_plane - light.near_plane);
		m_shadowStats.lastSpotFitDepthSpanReduction =
			legacySpan > 0.0f
				? 1.0f - fittedSpan / legacySpan
				: 0.0f;
		m_shadowStats.lastSpotFitDepthUtilization = depthUtilization;
		m_shadowStats.lastSpotFitProjectionDepthScale =
			fittedProjectionDepthScale;
		m_shadowStats.lastSpotFitPrecisionGain = precisionGain;
		m_shadowStats.lastSpotFitMinimumProjectedCoverageMargin =
			minimumProjectedCoverageMargin;
		m_shadowStats.lastSpotFitLightIndex = lightIndex;
		m_shadowStats.lastSpotFitRawNearClipped = rawNearClipped;
		m_shadowStats.lastSpotFitProjectionAware = projectionAware;
		m_shadowStats.totalSpotFitCpuMilliseconds +=
			std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - fitStart).count();
	};

	if (!properties.SHADOW_SPOT_CASTER_DEPTH_FIT) {
		finish(false, false);
		return;
	}

	glm::vec3 forward(0.0f);
	glm::vec3 right(0.0f);
	glm::vec3 up(0.0f);
	if (!IsFiniteVector(light.position) ||
		!std::isfinite(light.outerCutOff) ||
		!light.GetShadowViewBasis(forward, right, up)) {
		finish(false, true);
		return;
	}

	BuildShadowCasterDrawList();
	candidateCount =
		static_cast<std::uint64_t>(m_shadowCasterDrawList.size());
	if (m_shadowCasterDrawList.empty()) {
		finish(false, true);
		return;
	}

	const float halfAngle = light.GetShadowHalfAngleRadians();
	const float sinHalfAngle = std::sin(halfAngle);
	const float cosHalfAngle = std::cos(halfAngle);
	if (!std::isfinite(sinHalfAngle) ||
		!std::isfinite(cosHalfAngle) ||
		cosHalfAngle <= 0.0f) {
		finish(false, true);
		return;
	}
	const std::array<glm::vec3, 4> sidePlaneNormals = {
		forward * sinHalfAngle + right * cosHalfAngle,
		forward * sinHalfAngle - right * cosHalfAngle,
		forward * sinHalfAngle + up * cosHalfAngle,
		forward * sinHalfAngle - up * cosHalfAngle
	};

	float minimumDepth = std::numeric_limits<float>::max();
	float maximumDepth = std::numeric_limits<float>::lowest();
	bool invalidInput = false;
	for (const ShadowCasterDrawItem& item : m_shadowCasterDrawList) {
		if (!item.mesh || !item.worldBoundsValid) {
			invalidInput = true;
			break;
		}

		auto supportRadius = [&](const glm::vec3& normal) {
			return glm::abs(
					glm::dot(normal, item.worldBoundsAxisX)) +
				glm::abs(
					glm::dot(normal, item.worldBoundsAxisY)) +
				glm::abs(
					glm::dot(normal, item.worldBoundsAxisZ));
		};
		const glm::vec3 toCenter =
			item.worldBoundsCenter - light.position;
		const float forwardSupport = supportRadius(forward);
		const float centerDepth = glm::dot(forward, toCenter);
		const float classificationEpsilon = (std::max)(
			0.001f,
			1.0e-5f * (
				glm::length(toCenter) +
				glm::length(item.worldBoundsAxisX) +
				glm::length(item.worldBoundsAxisY) +
				glm::length(item.worldBoundsAxisZ)));
		if (!IsFiniteVector(toCenter) ||
			!std::isfinite(forwardSupport) ||
			!std::isfinite(centerDepth) ||
			!std::isfinite(classificationEpsilon)) {
			invalidInput = true;
			break;
		}

		bool intersectsProjection =
			centerDepth + forwardSupport >= -classificationEpsilon;
		for (const glm::vec3& planeNormal : sidePlaneNormals) {
			if (!intersectsProjection) {
				break;
			}
			const float planeSupport = supportRadius(planeNormal);
			const float signedDistance =
				glm::dot(planeNormal, toCenter);
			if (!std::isfinite(planeSupport) ||
				!std::isfinite(signedDistance)) {
				invalidInput = true;
				break;
			}
			if (signedDistance + planeSupport <
				-classificationEpsilon) {
				intersectsProjection = false;
			}
		}
		if (invalidInput) {
			break;
		}
		if (!intersectsProjection) {
			++rejectedCount;
			continue;
		}

		++acceptedCount;
		minimumDepth = (std::min)(
			minimumDepth,
			centerDepth - forwardSupport);
		maximumDepth = (std::max)(
			maximumDepth,
			centerDepth + forwardSupport);
	}

	if (invalidInput ||
		acceptedCount == 0 ||
		!std::isfinite(minimumDepth) ||
		!std::isfinite(maximumDepth) ||
		maximumDepth <= 0.0f) {
		finish(false, true);
		return;
	}

	// Bounds already enclose the complete transformed Mesh. Small absolute and
	// proportional guards absorb floating-point classification noise without
	// giving back the large irrelevant range removed by the fit.
	const float positiveMinimumDepth =
		(std::max)(0.05f, minimumDepth);
	const float depthSpan =
		(std::max)(0.1f, maximumDepth - positiveMinimumDepth);
	const float nearMargin = (std::max)(0.01f, depthSpan * 0.01f);
	const float farMargin = (std::max)(0.05f, depthSpan * 0.02f);
	const float fittedNear =
		(std::max)(0.05f, minimumDepth - nearMargin);
	const float fittedFar =
		(std::max)(fittedNear + 0.1f, maximumDepth + farMargin);
	if (!std::isfinite(fittedNear) ||
		!std::isfinite(fittedFar) ||
		fittedFar <= fittedNear) {
		finish(false, true);
		return;
	}
	rawNear = minimumDepth;
	rawFar = maximumDepth;
	rawNearClipped = minimumDepth < 0.05f;
	const float legacyDepthSpan = legacyFar - legacyNear;
	const float candidateDepthSpan = fittedFar - fittedNear;
	if (!(candidateDepthSpan + 0.001f < legacyDepthSpan)) {
		finish(false, true);
		return;
	}

	light.near_plane = fittedNear;
	light.far_plane = fittedFar;
	const float fittedSpan = fittedFar - fittedNear;
	const float coveredRawSpan =
		(std::max)(0.0f, maximumDepth - positiveMinimumDepth);
	depthUtilization =
		fittedSpan > 0.0f ? coveredRawSpan / fittedSpan : 0.0f;
	minimumProjectedCoverageMargin = (std::min)(
		positiveMinimumDepth - fittedNear,
		fittedFar - maximumDepth);
	fittedProjectionDepthScale =
		perspectiveDepthScale(fittedNear, fittedFar);
	precisionGain =
		legacyProjectionDepthScale > 0.0f
			? fittedProjectionDepthScale /
				legacyProjectionDepthScale
			: 0.0f;
	finish(true, false);
}

bool Scene::HasEnoughShadowCasterWorkForCulling() const {
	// The controlled Sponza/San Miguel crossover showed that a small caster
	// set can cost more to test than it saves. Keep the original submission
	// path below this conservative complexity floor.
	constexpr std::size_t kMinimumCasterMeshes = 512;
	constexpr std::uint64_t kMinimumCasterTriangles = 500000;
	if (std::any_of(
		m_shadowCasterDrawList.begin(),
		m_shadowCasterDrawList.end(),
		[](const ShadowCasterDrawItem& item) {
			return !item.model ||
				!item.mesh ||
				!item.worldBoundsValid;
		})) {
		return false;
	}
	return m_shadowCasterDrawList.size() >= kMinimumCasterMeshes ||
		m_shadowCasterDrawListTriangleCount >= kMinimumCasterTriangles;
}

bool Scene::ShouldUseSixFacePointShadow() {
	if (properties.POINT_SHADOW_SIX_FACE_RENDERING) {
		return true;
	}
	if (!properties.POINT_SHADOW_ADAPTIVE_RENDERING) {
		return false;
	}
	// Per-face depth readback found that the layered geometry-shader path can
	// leave five cubemap faces unwritten on the current driver. Adaptive mode
	// therefore fails closed to the independently validated six-face path.
	// Explicit "layered" remains available only as a diagnostic override.
	return true;
}

void Scene::RenderShadowCasters(
	Shader& shader,
	const glm::mat4& lightViewProjection,
	std::uint64_t trianglePassMultiplier) {
	PERF_CPU_SCOPE("Shadow Caster Submission");
	const Frustum lightFrustum = BuildFrustum(lightViewProjection);
	MaterialBatchScope materialBatch;
	Model* lastModel = nullptr;

	for (const ShadowCasterDrawItem& item : m_shadowCasterDrawList) {
		++m_pendingShadowCasterCandidateCount;
		if (!lightFrustum.IntersectsSphere(
				item.worldBoundsCenter,
				item.worldBoundsRadius)) {
			++m_pendingShadowCasterCulledCount;
			continue;
		}
		if (!item.model || !item.mesh) {
			continue;
		}
		if (item.model != lastModel) {
			shader.setMat4("model", item.modelMatrix);
			lastModel = item.model;
		}
		item.mesh->Draw(&shader);
		++m_pendingShadowCasterDrawCount;
		m_pendingShadowCasterTriangleCount +=
			static_cast<std::uint64_t>(item.mesh->GetDrawCount() / 3u) *
			trianglePassMultiplier;
	}
}

void Scene::RenderPointShadowCasters(
	Shader& shader,
	const glm::vec3& lightPosition,
	float farPlane,
	std::uint64_t trianglePassMultiplier) {
	PERF_CPU_SCOPE("Shadow Caster Submission");
	const float safeFarPlane = (std::max)(0.0f, farPlane);
	MaterialBatchScope materialBatch;
	Model* lastModel = nullptr;

	for (const ShadowCasterDrawItem& item : m_shadowCasterDrawList) {
		++m_pendingShadowCasterCandidateCount;
		const float maximumCenterDistance =
			safeFarPlane + item.worldBoundsRadius;
		const glm::vec3 offset =
			item.worldBoundsCenter - lightPosition;
		if (glm::dot(offset, offset) >
			maximumCenterDistance * maximumCenterDistance) {
			++m_pendingShadowCasterCulledCount;
			continue;
		}
		if (!item.model || !item.mesh) {
			continue;
		}
		if (item.model != lastModel) {
			shader.setMat4("model", item.modelMatrix);
			lastModel = item.model;
		}
		item.mesh->Draw(&shader);
		++m_pendingShadowCasterDrawCount;
		m_pendingShadowCasterTriangleCount +=
			static_cast<std::uint64_t>(item.mesh->GetDrawCount() / 3u) *
			trianglePassMultiplier;
	}
}

void Scene::DrawPointLights()
{
	if (!m_drawPointLightMarkers) return;
    GLState::StencilMask(0x00); // 禁用stencil写入，不影响后续的stencil记录
    glm::vec3 lightColor(1.0f);

    lightSource.pointLightShader.use();
    lightSource.pointLightShader.setVec3("lightColor", lightColor);
    for (unsigned int i = 0; i < lightSource.pointLights.size(); ++i) {
        if (!lightSource.pointLights[i].GetActiveStatus()) continue;
        lightSource.pointLights[i].Draw(&lightSource.pointLightShader);
    }
}

void Scene::DrawOpaqueModels()
{
	auto& list = GetOpaqueMeshes();
	Shader* lastShader = nullptr;
	int usedTexes = properties.USED_TEXTURE_NUM;
	MaterialBatchScope materialBatch;
	for (const auto& item : list) {
		if (!item.shader || !item.model || !item.mesh) continue;

		if (item.shader != lastShader) {
			lastShader = item.shader;
			lastShader->use();
			SetLightUniforms(*lastShader);
			properties.USED_TEXTURE_NUM = SetShadowMap(
				*lastShader,
				ShadowLightBinding::AllLights,
				11);
			GLState::ActiveTexture(GL_TEXTURE0 + properties.USED_TEXTURE_NUM++);
			usedTexes = properties.USED_TEXTURE_NUM;
			if (properties.GAMMA_CORRECTION) {
				GLState::BindTexture(GL_TEXTURE_CUBE_MAP, skyboxSource.textureCubeMap->textureGammaID);
			}
			else {
				GLState::BindTexture(GL_TEXTURE_CUBE_MAP, skyboxSource.textureCubeMap->textureID);
			}
			GLState::ActiveTexture(GL_TEXTURE0);
			lastShader->setFloat("time", static_cast<float>(glfwGetTime()));
			lastShader->setVec3("viewPos", camera_ptr->cameraPos);
			lastShader->setVec3("color", glm::vec3(0.2f));
			properties.USED_TEXTURE_NUM = BindImageBasedLighting(
				*lastShader,
				properties.USED_TEXTURE_NUM);
			usedTexes = properties.USED_TEXTURE_NUM;
		}

		properties.USED_TEXTURE_NUM = usedTexes;
		if (!item.model->IsOtherShaderUsed(OtherShaderType::outline)) {
			GLState::StencilMask(0x00);
			GLState::StencilFunc(GL_ALWAYS, 0, 0xFF);
		}
		else {
			GLState::StencilMask(0xFF);
			GLState::StencilFunc(GL_ALWAYS, 1, 0xFF);
			GLState::StencilOp(GL_KEEP, GL_REPLACE, GL_REPLACE);
		}

		lastShader->setMat4("model", item.modelMatrix);
		item.mesh->Draw(lastShader);
	}
}

void Scene::DrawTransparentModels()
{
	GLState::Enable(GL_BLEND);
	GLState::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	GLState::DepthMask(GL_FALSE);

	auto& list = GetTransparentMeshes();
	Shader* lastShader = nullptr;
	int usedTexes = properties.USED_TEXTURE_NUM;
	{
		MaterialBatchScope materialBatch;
		for (const auto& item : list) {
			if (!item.shader || !item.model || !item.mesh) continue;

			if (item.shader != lastShader) {
				lastShader = item.shader;
				lastShader->use();
				SetLightUniforms(*lastShader);
				properties.USED_TEXTURE_NUM = SetShadowMap(
					*lastShader,
					ShadowLightBinding::AllLights,
					11);
				GLState::ActiveTexture(GL_TEXTURE0 + properties.USED_TEXTURE_NUM++);
				usedTexes = properties.USED_TEXTURE_NUM;
				if (skyboxSource.textureCubeMap) {
					if (properties.GAMMA_CORRECTION) {
						GLState::BindTexture(GL_TEXTURE_CUBE_MAP, skyboxSource.textureCubeMap->textureGammaID);
					}
					else {
						GLState::BindTexture(GL_TEXTURE_CUBE_MAP, skyboxSource.textureCubeMap->textureID);
					}
				}
				GLState::ActiveTexture(GL_TEXTURE0);
				lastShader->setFloat("time", static_cast<float>(glfwGetTime()));
				if (camera_ptr) {
					lastShader->setVec3("viewPos", camera_ptr->cameraPos);
				}
				properties.USED_TEXTURE_NUM = BindImageBasedLighting(
					*lastShader,
					properties.USED_TEXTURE_NUM);
				usedTexes = properties.USED_TEXTURE_NUM;
			}
			properties.USED_TEXTURE_NUM = usedTexes;

			if (item.model->IsOtherShaderUsed(OtherShaderType::outline)) {
				GLState::StencilMask(0xFF);
				GLState::StencilFunc(GL_ALWAYS, 1, 0xFF);
				GLState::StencilOp(GL_KEEP, GL_REPLACE, GL_REPLACE);
			}
			else {
				GLState::StencilMask(0x00);
				GLState::StencilFunc(GL_ALWAYS, 0, 0xFF);
			}

			lastShader->setMat4("model", item.modelMatrix);
			item.mesh->Draw(lastShader);
		}
	}

	GLState::DepthMask(GL_TRUE);
	GLState::Disable(GL_BLEND);
}

void Scene::SetLightUniforms(Shader& shader)
{
    // 全局阴影采样配置（所有使用阴影的 shader 都共用这一套）
    shader.setInt("shadowSampleNum", properties.SHADOW_PCF_SAMPLE_NUM);
    shader.setInt("shadowSampleRings", properties.SHADOW_PCF_RING_NUM);
    shader.setInt("shadowType", properties.SHADOW_TYPE);
    shader.setInt(
        "shadowSamplingPattern",
        properties.SHADOW_SAMPLING_PATTERN);

    // 具体灯光参数与阴影贴图统一交给 LightSource/Light 对象来设置
    // 保证所有 shader 里关于 pointLights/dirLights/spotLights 的布局是一致的
    lightSource.SetLightUniforms(shader);
}

unsigned int Scene::SetShadowMap(
    Shader& shader,
    ShadowLightBinding lightBinding,
    unsigned int reservedTextureUnits) {
    // sampler2D, sampler2DShadow, and samplerCube may not alias one texture
    // unit in a linked program. Reserve one fallback unit for each sampler
    // type, then place every real shadow texture on exactly one additional
    // unit.
    constexpr unsigned int fallbackCubeUnit = 0;
    constexpr unsigned int fallbackRaw2DUnit = 1;
    constexpr unsigned int fallbackCompare2DUnit = 2;
    constexpr size_t kMaxShaderLights = 16;

    const bool hardwareCompareRequested =
        properties.SHADOW_HARDWARE_DEPTH_COMPARE &&
        properties.SHADOW_TYPE == ShadowProperty::PCF;
    const bool useLinearCompare =
        hardwareCompareRequested &&
        properties.SHADOW_TYPE == ShadowProperty::PCF &&
        properties.SHADOW_HARDWARE_LINEAR_PCF;
    unsigned int compareSampler = 0;
    if (hardwareCompareRequested) {
        compareSampler =
            FramebuffersManager::GetInstance().GetShadowCompareSampler(
                useLinearCompare);
    }
    const bool useHardwareCompare =
        hardwareCompareRequested && compareSampler != 0;
    const bool hardwareCompareFailed =
        hardwareCompareRequested && !useHardwareCompare;
    if (hardwareCompareFailed) {
        static bool warnedAboutCompareSamplerFailure = false;
        if (!warnedAboutCompareSamplerFailure) {
            std::cerr
                << "Shadow compare sampler creation failed; "
                << "2D shadows are disabled for this run."
                << std::endl;
            warnedAboutCompareSamplerFailure = true;
        }
    }

    GLState::ActiveTexture(GL_TEXTURE0 + fallbackCubeUnit);
    GLState::BindTexture(GL_TEXTURE_CUBE_MAP, 0);
    GLState::ActiveTexture(GL_TEXTURE0 + fallbackRaw2DUnit);
    GLState::BindTexture(GL_TEXTURE_2D, 0);
    GLState::ActiveTexture(GL_TEXTURE0 + fallbackCompare2DUnit);
    GLState::BindTexture(GL_TEXTURE_2D, 0);
    if (useHardwareCompare) {
        GLState::BindSampler(fallbackCompare2DUnit, compareSampler);
    }

    // A linked GLSL program may not have active samplers of different types
    // pointing at the same unit, even before a branch samples them. Reset all
    // optional non-shadow samplers on every binding pass: later material,
    // GBuffer, AO, or IBL code overwrites the uniforms it actually uses, while
    // disabled features remain on a type-compatible fallback.
    const auto setRaw2DFallback = [&](const char* name) {
        shader.setInt(name, static_cast<int>(fallbackRaw2DUnit));
    };
    const auto setCubeFallback = [&](const char* name) {
        shader.setInt(name, static_cast<int>(fallbackCubeUnit));
    };
    if (shader.shaderName == "pbr") {
        setRaw2DFallback("material.texture_diffuse1");
        setRaw2DFallback("material.texture_normal1");
        setRaw2DFallback("material.texture_metallic1");
        setRaw2DFallback("material.texture_roughness1");
        setRaw2DFallback("material.texture_ao1");
        setRaw2DFallback("material.texture_emissive1");
        setRaw2DFallback("material.texture_opacity1");
        setRaw2DFallback("brdfLUT");
        setCubeFallback("irradianceMap");
        setCubeFallback("prefilterMap");
    }
    else if (shader.shaderName == "phong") {
        setRaw2DFallback("material.texture_diffuse1");
        setRaw2DFallback("material.texture_normal1");
        setRaw2DFallback("material.texture_specular1");
        setRaw2DFallback("material.texture_opacity1");
    }
    else if (shader.shaderName == "defer") {
        setRaw2DFallback("gPosition");
        setRaw2DFallback("gNormal");
        setRaw2DFallback("gAlbedoSpec");
        setRaw2DFallback("gMaterial");
        setRaw2DFallback("gEmissive");
        setRaw2DFallback("ssaoMap");
        setRaw2DFallback("brdfLUT");
        setCubeFallback("irradianceMap");
        setCubeFallback("prefilterMap");
    }
    else if (shader.shaderName == "deferDirLightVolume") {
        setRaw2DFallback("gPosition");
        setRaw2DFallback("gNormal");
        setRaw2DFallback("gAlbedoSpec");
        setRaw2DFallback("gMaterial");
        setRaw2DFallback("ssaoMap");
    }

    unsigned int shadowMapCount = fallbackCompare2DUnit + 1;
    const unsigned int maxFragmentTextureUnits =
        GLState::GetMaxFragmentTextureUnits();
    const unsigned int firstReservedTextureUnit =
        maxFragmentTextureUnits > reservedTextureUnits
            ? maxFragmentTextureUnits - reservedTextureUnits
            : 0;
    const unsigned int shadowTextureUnitLimit =
        (std::max)(shadowMapCount, firstReservedTextureUnit);
    const auto hasShadowTextureUnit = [&]() {
        return shadowMapCount < shadowTextureUnitLimit;
    };

    shader.setInt("shadowSampleNum", properties.SHADOW_PCF_SAMPLE_NUM);
    shader.setInt("shadowSampleRings", properties.SHADOW_PCF_RING_NUM);
    shader.setInt("shadowType", properties.SHADOW_TYPE);
    shader.setInt(
        "shadowSamplingPattern",
        properties.SHADOW_SAMPLING_PATTERN);

    const size_t currentPointLightCount =
        lightBinding == ShadowLightBinding::AllLights
            ? (std::min)(
                lightSource.pointLights.size(),
                kMaxShaderLights)
            : 0;
    const size_t currentDirectionLightCount =
        (std::min)(
            lightSource.directionLights.size(),
            kMaxShaderLights);
    const size_t currentSpotLightCount =
        lightBinding == ShadowLightBinding::AllLights
            ? (std::min)(
                lightSource.spotLights.size(),
                kMaxShaderLights)
            : 0;

    // Dynamic indexing keeps every sampler element active at link time on
    // some GLSL 3.30 drivers. Initialize the complete shader-visible arrays
    // once per linked program and restore entries removed since the prior
    // binding so different sampler types can never alias a recycled unit.
    static std::unordered_map<const Shader*, ShadowSamplerBindingState>
        samplerBindingStates;
    auto& samplerState = samplerBindingStates[&shader];
    const bool initializeAllSamplerEntries =
        samplerState.programId != shader.ID ||
        samplerState.shaderRevision != shader.GetRevision();
    if (initializeAllSamplerEntries) {
        for (size_t i = 0; i < kMaxShaderLights; ++i) {
            const std::string index = std::to_string(i) + "]";
            const std::string pointBaseName = "pointLights[" + index;
            const std::string directionBaseName = "dirLights[" + index;
            const std::string spotBaseName = "spotLights[" + index;
            shader.setInt(
                pointBaseName + ".shadowCubeMap",
                static_cast<int>(fallbackCubeUnit));
            shader.setInt(
                directionBaseName + ".shadowMap",
                static_cast<int>(fallbackRaw2DUnit));
            shader.setInt(
                directionBaseName + ".shadowCompareMap",
                static_cast<int>(fallbackCompare2DUnit));
            shader.setInt(
                spotBaseName + ".shadowMap",
                static_cast<int>(fallbackRaw2DUnit));
            shader.setInt(
                spotBaseName + ".shadowCompareMap",
                static_cast<int>(fallbackCompare2DUnit));
        }
    }
    else {
        for (size_t i = currentPointLightCount;
            i < samplerState.pointCount;
            ++i) {
            const std::string baseName =
                "pointLights[" + std::to_string(i) + "]";
            shader.setInt(
                baseName + ".shadowCubeMap",
                static_cast<int>(fallbackCubeUnit));
            shader.setBool(baseName + ".useShadowMap", false);
        }
        for (size_t i = currentDirectionLightCount;
            i < samplerState.directionCount;
            ++i) {
            const std::string baseName =
                "dirLights[" + std::to_string(i) + "]";
            shader.setInt(
                baseName + ".shadowMap",
                static_cast<int>(fallbackRaw2DUnit));
            shader.setInt(
                baseName + ".shadowCompareMap",
                static_cast<int>(fallbackCompare2DUnit));
            shader.setBool(baseName + ".useShadowMap", false);
        }
        for (size_t i = currentSpotLightCount;
            i < samplerState.spotCount;
            ++i) {
            const std::string baseName =
                "spotLights[" + std::to_string(i) + "]";
            shader.setInt(
                baseName + ".shadowMap",
                static_cast<int>(fallbackRaw2DUnit));
            shader.setInt(
                baseName + ".shadowCompareMap",
                static_cast<int>(fallbackCompare2DUnit));
            shader.setBool(baseName + ".useShadowMap", false);
        }
    }
    samplerState.programId = shader.ID;
    samplerState.shaderRevision = shader.GetRevision();
    samplerState.pointCount = currentPointLightCount;
    samplerState.directionCount = currentDirectionLightCount;
    samplerState.spotCount = currentSpotLightCount;

    if (lightBinding == ShadowLightBinding::AllLights) {
        for (size_t i = 0; i < currentPointLightCount; ++i) {
            auto& pointLight = lightSource.pointLights[i];
            const std::string baseName =
                "pointLights[" + std::to_string(i) + "]";
            shader.setInt(
                baseName + ".shadowCubeMap",
                static_cast<int>(fallbackCubeUnit));
            shader.setBool(baseName + ".useShadowMap", false);
            shader.setFloat(baseName + ".far_plane", pointLight.far);
            if (!pointLight.GetActiveStatus() ||
                !pointLight.useShadowMap ||
                !hasShadowTextureUnit()) {
                continue;
            }
            FBO* pointShadowFBO = pointLight.shadowFBO;
            if (!pointLight.shadowCache.IsSampleable(pointShadowFBO)) {
                continue;
            }
            GLState::ActiveTexture(GL_TEXTURE0 + shadowMapCount);
            GLState::BindTexture(GL_TEXTURE_2D, 0);
            GLState::BindTexture(
                GL_TEXTURE_CUBE_MAP,
                pointShadowFBO->textureIDs[0]);
            shader.setInt(
                baseName + ".shadowCubeMap",
                static_cast<int>(shadowMapCount));
            shader.setBool(baseName + ".useShadowMap", true);
            ++shadowMapCount;
        }
    }

    for (size_t i = 0; i < currentDirectionLightCount; ++i) {
        auto& dirLight = lightSource.directionLights[i];
        const std::string baseName =
            "dirLights[" + std::to_string(i) + "]";
        shader.setInt(
            baseName + ".shadowMap",
            static_cast<int>(fallbackRaw2DUnit));
        shader.setInt(
            baseName + ".shadowCompareMap",
            static_cast<int>(fallbackCompare2DUnit));
        shader.setBool(baseName + ".useShadowMap", false);
        shader.setMat4(
            baseName + ".lightSpaceMatrix",
            dirLight.GetLightSpaceMatrix());
        if (!dirLight.GetActiveStatus() ||
            !dirLight.useShadowMap ||
            hardwareCompareFailed ||
            !hasShadowTextureUnit()) {
            continue;
        }
            FBO* dirShadowFBO = dirLight.shadowFBO;
            if (!dirLight.shadowCache.IsSampleable(dirShadowFBO)) {
                continue;
            }
        GLState::ActiveTexture(GL_TEXTURE0 + shadowMapCount);
        GLState::BindTexture(
            GL_TEXTURE_2D,
            dirShadowFBO->textureIDs[0]);
        GLState::BindTexture(GL_TEXTURE_CUBE_MAP, 0);
        if (useHardwareCompare) {
            // Bind the specialized sampler last: generic texture binds clear
            // any sampler object cached on this unit.
            GLState::BindSampler(shadowMapCount, compareSampler);
            shader.setInt(
                baseName + ".shadowCompareMap",
                static_cast<int>(shadowMapCount));
        }
        else {
            shader.setInt(
                baseName + ".shadowMap",
                static_cast<int>(shadowMapCount));
        }
        shader.setBool(baseName + ".useShadowMap", true);
        ++shadowMapCount;
    }

    if (lightBinding == ShadowLightBinding::AllLights) {
        for (size_t i = 0; i < currentSpotLightCount; ++i) {
            auto& spotLight = lightSource.spotLights[i];
            const std::string baseName =
                "spotLights[" + std::to_string(i) + "]";
            shader.setInt(
                baseName + ".shadowMap",
                static_cast<int>(fallbackRaw2DUnit));
            shader.setInt(
                baseName + ".shadowCompareMap",
                static_cast<int>(fallbackCompare2DUnit));
            shader.setBool(baseName + ".useShadowMap", false);
            shader.setMat4(
                baseName + ".lightSpaceMatrix",
                spotLight.GetLightSpaceMatrix());
            if (!spotLight.GetActiveStatus() ||
                !spotLight.useShadowMap ||
                hardwareCompareFailed ||
                !hasShadowTextureUnit()) {
                continue;
            }
            FBO* spotShadowFBO = spotLight.shadowFBO;
            if (!spotLight.shadowCache.IsSampleable(spotShadowFBO)) {
                continue;
            }
            GLState::ActiveTexture(GL_TEXTURE0 + shadowMapCount);
            GLState::BindTexture(
                GL_TEXTURE_2D,
                spotShadowFBO->textureIDs[0]);
            GLState::BindTexture(GL_TEXTURE_CUBE_MAP, 0);
            if (useHardwareCompare) {
                GLState::BindSampler(shadowMapCount, compareSampler);
                shader.setInt(
                    baseName + ".shadowCompareMap",
                    static_cast<int>(shadowMapCount));
            }
            else {
                shader.setInt(
                    baseName + ".shadowMap",
                    static_cast<int>(shadowMapCount));
            }
            shader.setBool(baseName + ".useShadowMap", true);
            ++shadowMapCount;
        }
    }

    GLState::ActiveTexture(GL_TEXTURE0);
    return shadowMapCount;
}

void Scene::DrawSkybox(glm::mat4 view)
{
    GLState::DepthFunc(GL_LEQUAL);
    // Skybox 只影响颜色，不参与深度信息（否则会污染 SSAO 这类基于 depth 的后处理结果）
    GLState::DepthMask(GL_FALSE);
    GLState::StencilMask(0x00); // Disable writing to stencil buffer for skybox
    skyboxSource.skyboxShader_ptr->use();
    GLState::BindVertexArray(skyboxSource.cubeMapVAO);
    GLState::ActiveTexture(GL_TEXTURE0 + properties.USED_TEXTURE_NUM);
    GLState::BindTexture(GL_TEXTURE_2D, 0);
    if (properties.GAMMA_CORRECTION)
        GLState::BindTexture(GL_TEXTURE_CUBE_MAP, skyboxSource.textureCubeMap->textureGammaID);
    else
        GLState::BindTexture(GL_TEXTURE_CUBE_MAP, skyboxSource.textureCubeMap->textureID);
    skyboxSource.skyboxShader_ptr->setInt("skybox", properties.USED_TEXTURE_NUM++);
    skyboxSource.skyboxShader_ptr->setMat4("skyboxView", glm::mat4(glm::mat3(view))); // Remove translation from the view matrix
    PerformanceProfiler::GetInstance().RecordDraw(GL_TRIANGLES, 36);
    glDrawArrays(GL_TRIANGLES, 0, 36);

    GLState::DepthFunc(GL_LESS);
    GLState::DepthMask(GL_TRUE);
    GLState::StencilMask(0xFF); // Re-enable stencil mask
}

void Scene::DrawOutlines()
{
    GLState::StencilFunc(GL_NOTEQUAL, 1, 0xFF);
    GLState::StencilMask(0x00);
    GLState::Disable(GL_DEPTH_TEST);
    {
		MaterialBatchScope materialBatch;
		for (const auto& frameItem : m_visibleModels) {
			Model* model = frameItem.model;
			if (!model) continue;
			if (!model->IsOtherShaderUsed(OtherShaderType::outline)) continue;

			std::shared_ptr<Shader> outlineShader;
			if (!(outlineShader = model->GetOtherShader(OtherShaderType::outline))) {
				std::cout << "Outline shader is null!" << std::endl;
				continue;
			}
			outlineShader->use();
			outlineShader->setVec3("Color", model->outlineColor);

			glm::mat4 moveToOrigin = glm::translate(glm::mat4(1.0f), -model->GetLoacalCenter());
			glm::mat4 scale = glm::scale(glm::mat4(1.0f), glm::vec3(1.f + model->outlineWidth));
			glm::mat4 moveBack = glm::translate(glm::mat4(1.0f), model->GetLoacalCenter());

			outlineShader->setMat4(
				"model",
				frameItem.modelMatrix * moveBack * scale * moveToOrigin);
			for (Mesh& mesh : model->GetMeshes()) {
				if (mesh.GetActiveStatus()) {
					mesh.Draw(outlineShader.get());
				}
			}
		}
	}

    GLState::StencilMask(0xFF);
    GLState::Enable(GL_DEPTH_TEST);
}

void Scene::DrawNormalLines()
{
    //glStencilMask(0x00); // Disable writing to stencil buffer
	MaterialBatchScope materialBatch;
	for (const auto& frameItem : m_visibleModels) {
		Model* model = frameItem.model;
		if (!model) continue;
		if (!model->IsOtherShaderUsed(OtherShaderType::normalLines)) continue;
		std::shared_ptr<Shader> normalLineShader;
		if (!(normalLineShader = model->GetOtherShader(OtherShaderType::normalLines))) {
			std::cout << "Normal line shader is null!" << std::endl;
			continue;
		}
		normalLineShader->use();
		normalLineShader->setFloat("MAGNITUDE", OtherShader::normalLineMagnitude);
		normalLineShader->setMat4("model", frameItem.modelMatrix);
		for (Mesh& mesh : model->GetMeshes()) {
			if (mesh.GetActiveStatus()) {
				mesh.Draw(normalLineShader.get());
			}
		}
	}
    //glStencilMask(0xFF); // Re-enable stencil mask
}

bool Scene::ComputeShadowCasterBounds(glm::vec3& center, float& radius) const {
	glm::vec3 boundsMin(std::numeric_limits<float>::max());
	glm::vec3 boundsMax(std::numeric_limits<float>::lowest());
	bool foundCaster = false;

	for (const auto& model : modelSource.GetModels()) {
		if (!model || !model->GetAcitveStatus()) {
			continue;
		}
		const glm::mat4 modelMatrix = model->getModelMatrix();
		if (!IsFiniteMatrix(modelMatrix)) {
			center = glm::vec3(0.0f);
			radius = 0.0f;
			return false;
		}
		const glm::vec3 modelCenter = glm::vec3(
			modelMatrix * glm::vec4(model->GetLoacalCenter(), 1.0f));
		const float modelRadius =
			model->GetLocalBoundingRadius() * GetMaximumWorldScale(modelMatrix);
		if (!IsFiniteVector(modelCenter) ||
			!std::isfinite(modelRadius) ||
			modelRadius < 0.0f) {
			center = glm::vec3(0.0f);
			radius = 0.0f;
			return false;
		}
		const glm::vec3 radiusVector((std::max)(modelRadius, 0.001f));
		boundsMin = glm::min(boundsMin, modelCenter - radiusVector);
		boundsMax = glm::max(boundsMax, modelCenter + radiusVector);
		foundCaster = true;
	}
	if (!foundCaster) {
		center = glm::vec3(0.0f);
		radius = 0.0f;
		return false;
	}

	center = (boundsMin + boundsMax) * 0.5f;
	radius = 0.0f;
	for (const auto& model : modelSource.GetModels()) {
		if (!model || !model->GetAcitveStatus()) {
			continue;
		}
		const glm::mat4 modelMatrix = model->getModelMatrix();
		const glm::vec3 modelCenter = glm::vec3(
			modelMatrix * glm::vec4(model->GetLoacalCenter(), 1.0f));
		const float modelRadius =
			model->GetLocalBoundingRadius() * GetMaximumWorldScale(modelMatrix);
		radius = (std::max)(radius, glm::length(modelCenter - center) + modelRadius);
	}
	radius = (std::max)(radius, 0.5f);
	return IsFiniteVector(center) && std::isfinite(radius);
}

bool Scene::HasActiveShadowCasters() const {
	const auto& models = modelSource.GetModels();
	return std::any_of(
		models.begin(),
		models.end(),
		[](const std::shared_ptr<Model>& model) {
			return model && model->GetAcitveStatus();
		});
}

void Scene::CommitShadowCasterState(
	std::size_t signature,
	const std::vector<ShadowCasterBoundItem>& bounds) {
	if (m_shadowCasterStateInitialized &&
		signature == m_shadowCasterStateSignature) {
		return;
	}

	m_shadowCasterStateInitialized = true;
	m_shadowCasterStateSignature = signature;
	++m_shadowCasterRevision;
	m_shadowStats.casterRevision = m_shadowCasterRevision;
	++m_shadowStats.casterBoundsRebuildCount;
	m_shadowCacheValid = false;
	m_shadowCasterStateReliable = true;

	if (bounds.empty()) {
		m_shadowCasterBoundsValid = false;
		m_cachedShadowCasterCenter = glm::vec3(0.0f);
		m_cachedShadowCasterRadius = 0.0f;
		return;
	}

	glm::vec3 boundsMin(std::numeric_limits<float>::max());
	glm::vec3 boundsMax(std::numeric_limits<float>::lowest());
	for (const ShadowCasterBoundItem& item : bounds) {
		if (!IsFiniteVector(item.center) ||
			!std::isfinite(item.radius) ||
			item.radius < 0.0f) {
			m_shadowCasterStateReliable = false;
			m_shadowCasterBoundsValid = false;
			m_cachedShadowCasterCenter = glm::vec3(0.0f);
			m_cachedShadowCasterRadius = 0.0f;
			return;
		}
		const glm::vec3 radiusVector((std::max)(item.radius, 0.001f));
		boundsMin = glm::min(boundsMin, item.center - radiusVector);
		boundsMax = glm::max(boundsMax, item.center + radiusVector);
	}

	m_cachedShadowCasterCenter = (boundsMin + boundsMax) * 0.5f;
	m_cachedShadowCasterRadius = 0.0f;
	for (const ShadowCasterBoundItem& item : bounds) {
		m_cachedShadowCasterRadius = (std::max)(
			m_cachedShadowCasterRadius,
			glm::length(item.center - m_cachedShadowCasterCenter) +
				item.radius);
	}
	m_cachedShadowCasterRadius =
		(std::max)(m_cachedShadowCasterRadius, 0.5f);
	m_shadowCasterBoundsValid =
		IsFiniteVector(m_cachedShadowCasterCenter) &&
		std::isfinite(m_cachedShadowCasterRadius);
	m_shadowCasterStateReliable = m_shadowCasterBoundsValid;
}

void Scene::RefreshShadowCasterStateFallback() {
	const auto syncStart = std::chrono::steady_clock::now();
	const std::uint64_t shadowStateSyncEpoch =
		NextShadowStateSyncEpoch();
	std::size_t signature = 0;
	const auto& models = modelSource.GetModels();
	hash_combine(
		signature,
		modelSource.GetSceneTopologyRevision());
	hash_combine(signature, models.size());
	m_shadowCasterBoundsScratch.clear();
	m_shadowCasterBoundsScratch.reserve(models.size());

	for (const auto& model : models) {
		hash_combine(
			signature,
			reinterpret_cast<std::uintptr_t>(model.get()));
		if (!model) {
			continue;
		}
		model->RefreshMaterialDrivenState();
		const std::uint64_t modelShadowStateRevision =
			model->SyncShadowStateRevision(shadowStateSyncEpoch);
		hash_combine(
			signature,
			modelShadowStateRevision);
		if (!model->GetAcitveStatus()) {
			continue;
		}
		const glm::mat4 modelMatrix = model->getModelMatrix();
		m_shadowCasterBoundsScratch.push_back({
			model.get(),
			modelShadowStateRevision,
			glm::vec3(
				modelMatrix * glm::vec4(model->GetLoacalCenter(), 1.0f)),
			model->GetLocalBoundingRadius() *
				GetMaximumWorldScale(modelMatrix) });
	}

	CommitShadowCasterState(signature, m_shadowCasterBoundsScratch);
	m_shadowCasterStatePrepared = true;
	const double elapsedMilliseconds =
		std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - syncStart).count();
	++m_shadowStats.casterStateSyncCount;
	m_shadowStats.lastCasterStateSyncCpuMilliseconds =
		elapsedMilliseconds;
	m_shadowStats.totalCasterStateSyncCpuMilliseconds +=
		elapsedMilliseconds;
}

std::size_t Scene::BuildShadowRevisionSignature(
	std::uint64_t shadowShaderRevision,
	std::uint64_t pointShadowShaderRevision) const {
	std::size_t signature = 0;
	hash_combine(
		signature,
		modelSource.GetSceneTopologyRevision());
	hash_combine(signature, m_shadowCasterStateSignature);
	hash_combine(signature, m_shadowCasterRevision);
	hash_combine(signature, shadowShaderRevision);
	hash_combine(signature, pointShadowShaderRevision);
	hash_combine(signature, properties.POINT_SHADOW_ADAPTIVE_RENDERING);
	hash_combine(signature, properties.POINT_SHADOW_SIX_FACE_RENDERING);
	hash_combine(signature, properties.POINT_SHADOW_FACE_CULLING);
	hash_combine(signature, properties.SHADOW_SPATIAL_CASTER_CACHE);
	hash_combine(signature, properties.POINT_SHADOW_PER_FACE_CACHE);
	hash_combine(
		signature,
		properties.DIRECTIONAL_SHADOW_LIGHT_AABB_FIT);
	hash_combine(
		signature,
		properties.DIRECTIONAL_SHADOW_DENSITY_RESOLUTION);
	hash_combine(
		signature,
		properties.SHADOW_SPOT_CASTER_DEPTH_FIT);

	auto hashFloat = [&](float value) {
		hash_combine(signature, value);
	};
	auto hashVec3 = [&](const glm::vec3& value) {
		hashFloat(value.x);
		hashFloat(value.y);
		hashFloat(value.z);
	};

	hash_combine(signature, lightSource.directionLights.size());
	for (const auto& light : lightSource.directionLights) {
		const bool enabled = light.m_active && light.useShadowMap;
		hash_combine(signature, enabled);
		if (!enabled) {
			continue;
		}
		hash_combine(signature, light.shadowResolution);
		hash_combine(
			signature,
			light.GetEffectiveShadowResolution());
		hash_combine(signature, light.autoFitShadow);
		hashVec3(light.direction);
		hashVec3(light.shadowCenter);
		hashFloat(light.near_plane);
		hashFloat(light.far_plane);
		hashFloat(light.distance);
		hashFloat(light.width);
		hash_combine(signature, light.lightSpaceAabbFitActive);
		hashFloat(light.fittedHalfHeight);
	}

	hash_combine(signature, lightSource.pointLights.size());
	for (const auto& light : lightSource.pointLights) {
		const bool enabled = light.m_active && light.useShadowMap;
		hash_combine(signature, enabled);
		if (!enabled) {
			continue;
		}
		hash_combine(signature, light.shadowResolution);
		hash_combine(signature, light.autoFitShadow);
		hashVec3(light.position);
		hashFloat(light.near);
		hashFloat(light.far);
	}

	hash_combine(signature, lightSource.spotLights.size());
	for (const auto& light : lightSource.spotLights) {
		const bool enabled = light.m_active && light.useShadowMap;
		hash_combine(signature, enabled);
		if (!enabled) {
			continue;
		}
		hash_combine(signature, light.shadowResolution);
		hash_combine(signature, light.autoFitShadow);
		hashVec3(light.position);
		hashVec3(light.direction);
		hashFloat(light.outerCutOff);
		hashFloat(light.near_plane);
		hashFloat(light.far_plane);
	}
	return signature;
}

std::size_t Scene::BuildDirectionalShadowRevisionSignature(
	const DirectionLight& light,
	std::uint64_t shadowShaderRevision) const {
	std::size_t signature = 0;
	hash_combine(
		signature,
		modelSource.GetSceneTopologyRevision());
	if (properties.SHADOW_SPATIAL_CASTER_CACHE &&
		!light.autoFitShadow) {
		hash_combine(
			signature,
			BuildSpatialShadowCasterSignature(
				light.GetLightSpaceMatrix()));
	}
	else {
		hash_combine(signature, m_shadowCasterStateSignature);
		hash_combine(signature, m_shadowCasterRevision);
	}
	hash_combine(signature, shadowShaderRevision);
	hash_combine(
		signature,
		properties.DIRECTIONAL_SHADOW_LIGHT_AABB_FIT);
	hash_combine(
		signature,
		properties.DIRECTIONAL_SHADOW_DENSITY_RESOLUTION);
	hash_combine(signature, light.shadowResolution);
	hash_combine(
		signature,
		light.GetEffectiveShadowResolution());
	hash_combine(signature, light.autoFitShadow);
	hash_combine(signature, light.direction.x);
	hash_combine(signature, light.direction.y);
	hash_combine(signature, light.direction.z);
	hash_combine(signature, light.shadowCenter.x);
	hash_combine(signature, light.shadowCenter.y);
	hash_combine(signature, light.shadowCenter.z);
	hash_combine(signature, light.near_plane);
	hash_combine(signature, light.far_plane);
	hash_combine(signature, light.distance);
	hash_combine(signature, light.width);
	hash_combine(signature, light.lightSpaceAabbFitActive);
	hash_combine(signature, light.fittedHalfHeight);
	return signature;
}

std::size_t Scene::BuildPointShadowRevisionSignature(
	const PointLight& light,
	std::uint64_t pointShadowShaderRevision) const {
	std::size_t signature = 0;
	hash_combine(
		signature,
		modelSource.GetSceneTopologyRevision());
	if (properties.SHADOW_SPATIAL_CASTER_CACHE &&
		!light.autoFitShadow) {
		std::size_t casterSignature = 0;
		std::uint64_t acceptedCasterCount = 0;
		const float safeFar = (std::max)(0.0f, light.far);
		for (const ShadowCasterBoundItem& item :
			m_shadowCasterBoundsScratch) {
			const float range = safeFar + item.radius;
			const glm::vec3 offset = item.center - light.position;
			if (glm::dot(offset, offset) > range * range) {
				continue;
			}
			hash_combine(
				casterSignature,
				reinterpret_cast<std::uintptr_t>(item.model));
			hash_combine(casterSignature, item.revision);
			hash_combine(casterSignature, item.center.x);
			hash_combine(casterSignature, item.center.y);
			hash_combine(casterSignature, item.center.z);
			hash_combine(casterSignature, item.radius);
			++acceptedCasterCount;
		}
		hash_combine(casterSignature, acceptedCasterCount);
		hash_combine(signature, casterSignature);
	}
	else {
		hash_combine(signature, m_shadowCasterStateSignature);
		hash_combine(signature, m_shadowCasterRevision);
	}
	hash_combine(signature, pointShadowShaderRevision);
	hash_combine(signature, properties.POINT_SHADOW_ADAPTIVE_RENDERING);
	hash_combine(signature, properties.POINT_SHADOW_SIX_FACE_RENDERING);
	hash_combine(signature, properties.POINT_SHADOW_FACE_CULLING);
	hash_combine(signature, properties.SHADOW_SPATIAL_CASTER_CACHE);
	hash_combine(signature, properties.POINT_SHADOW_PER_FACE_CACHE);
	hash_combine(signature, light.shadowResolution);
	hash_combine(signature, light.autoFitShadow);
	hash_combine(signature, light.position.x);
	hash_combine(signature, light.position.y);
	hash_combine(signature, light.position.z);
	hash_combine(signature, light.near);
	hash_combine(signature, light.far);
	return signature;
}

std::size_t Scene::BuildSpotShadowRevisionSignature(
	const SpotLight& light,
	std::uint64_t shadowShaderRevision) const {
	std::size_t signature = 0;
	hash_combine(
		signature,
		modelSource.GetSceneTopologyRevision());
	if (properties.SHADOW_SPATIAL_CASTER_CACHE &&
		!light.autoFitShadow) {
		hash_combine(
			signature,
			BuildSpatialShadowCasterSignature(
				light.GetLightSpaceMatrix()));
	}
	else {
		hash_combine(signature, m_shadowCasterStateSignature);
		hash_combine(signature, m_shadowCasterRevision);
	}
	hash_combine(signature, shadowShaderRevision);
	hash_combine(
		signature,
		properties.SHADOW_SPOT_CASTER_DEPTH_FIT);
	hash_combine(signature, light.shadowResolution);
	hash_combine(signature, light.autoFitShadow);
	hash_combine(signature, light.position.x);
	hash_combine(signature, light.position.y);
	hash_combine(signature, light.position.z);
	hash_combine(signature, light.direction.x);
	hash_combine(signature, light.direction.y);
	hash_combine(signature, light.direction.z);
	hash_combine(signature, light.outerCutOff);
	hash_combine(signature, light.near_plane);
	hash_combine(signature, light.far_plane);
	return signature;
}

std::size_t Scene::BuildSpatialShadowCasterSignature(
	const glm::mat4& lightViewProjection) const {
	if (!m_shadowCasterStateReliable ||
		!IsFiniteMatrix(lightViewProjection)) {
		std::size_t fallback = 0;
		hash_combine(
			fallback,
			modelSource.GetSceneTopologyRevision());
		hash_combine(fallback, m_shadowCasterStateSignature);
		hash_combine(fallback, m_shadowCasterRevision);
		return fallback;
	}

	const Frustum lightFrustum = BuildFrustum(lightViewProjection);
	std::size_t signature = 0;
	hash_combine(
		signature,
		modelSource.GetSceneTopologyRevision());
	std::uint64_t acceptedCasterCount = 0;
	for (const ShadowCasterBoundItem& item :
		m_shadowCasterBoundsScratch) {
		if (!lightFrustum.IntersectsSphere(
				item.center,
				item.radius)) {
			continue;
		}
		hash_combine(
			signature,
			reinterpret_cast<std::uintptr_t>(item.model));
		hash_combine(signature, item.revision);
		hash_combine(signature, item.center.x);
		hash_combine(signature, item.center.y);
		hash_combine(signature, item.center.z);
		hash_combine(signature, item.radius);
		++acceptedCasterCount;
	}
	hash_combine(signature, acceptedCasterCount);
	return signature;
}

std::array<std::size_t, 6>
Scene::BuildPointShadowFaceRevisionSignatures(
	const PointLight& light,
	std::uint64_t pointShadowShaderRevision,
	const std::array<glm::mat4, 6>& lightSpaceMatrices) const {
	std::array<std::size_t, 6> signatures{};
	for (std::size_t face = 0; face < signatures.size(); ++face) {
		std::size_t signature = 0;
		hash_combine(
			signature,
			modelSource.GetSceneTopologyRevision());
		hash_combine(
			signature,
			BuildSpatialShadowCasterSignature(
				lightSpaceMatrices[face]));
		hash_combine(signature, pointShadowShaderRevision);
		hash_combine(
			signature,
			properties.POINT_SHADOW_ADAPTIVE_RENDERING);
		hash_combine(
			signature,
			properties.POINT_SHADOW_SIX_FACE_RENDERING);
		hash_combine(
			signature,
			properties.POINT_SHADOW_FACE_CULLING);
		hash_combine(
			signature,
			properties.POINT_SHADOW_PER_FACE_CACHE);
		hash_combine(signature, light.shadowResolution);
		hash_combine(signature, light.autoFitShadow);
		hash_combine(signature, light.position.x);
		hash_combine(signature, light.position.y);
		hash_combine(signature, light.position.z);
		hash_combine(signature, light.near);
		hash_combine(signature, light.far);
		hash_combine(signature, face);
		signatures[face] = signature;
	}
	return signatures;
}

std::uint8_t Scene::ComputePointShadowRequiredFaceMask(
	const PointLight& light) const {
	if (properties.POINT_SHADOW_FORCE_ALL_FACES_REQUIRED) {
		return 0x3fu;
	}
	if (!camera_ptr) {
		return 0x3fu;
	}
	if (m_opaqueMeshList.empty() &&
		m_transparentMeshList.empty()) {
		return 0u;
	}

	float filterRadiusTexels = 0.0f;
	if (properties.SHADOW_TYPE == ShadowProperty::PCF) {
		filterRadiusTexels = 2.0f;
	}
	else if (properties.SHADOW_TYPE == ShadowProperty::PCSS) {
		filterRadiusTexels = 16.0f;
	}
	const float safeResolution = static_cast<float>(
		(std::max)(1, light.shadowResolution));
	const float angularPaddingRadians =
		std::atan(2.0f * filterRadiusTexels / safeResolution);
	const float fieldOfViewDegrees =
		90.25f +
		glm::degrees(2.0f * angularPaddingRadians);
	const float safeNear = 0.001f;
	const float safeFar = (std::max)(
		safeNear + 0.001f,
		light.far);
	const glm::mat4 projection = glm::perspective(
		glm::radians(fieldOfViewDegrees),
		1.0f,
		safeNear,
		safeFar);
	const std::array<glm::vec3, 6> directions = {
		glm::vec3(1.0f, 0.0f, 0.0f),
		glm::vec3(-1.0f, 0.0f, 0.0f),
		glm::vec3(0.0f, 1.0f, 0.0f),
		glm::vec3(0.0f, -1.0f, 0.0f),
		glm::vec3(0.0f, 0.0f, 1.0f),
		glm::vec3(0.0f, 0.0f, -1.0f)
	};
	const std::array<glm::vec3, 6> upVectors = {
		glm::vec3(0.0f, -1.0f, 0.0f),
		glm::vec3(0.0f, -1.0f, 0.0f),
		glm::vec3(0.0f, 0.0f, 1.0f),
		glm::vec3(0.0f, 0.0f, -1.0f),
		glm::vec3(0.0f, -1.0f, 0.0f),
		glm::vec3(0.0f, -1.0f, 0.0f)
	};
	std::array<Frustum, 6> faceFrusta{};
	for (std::size_t face = 0; face < faceFrusta.size(); ++face) {
		faceFrusta[face] = BuildFrustum(
			projection *
			glm::lookAt(
				light.position,
				light.position + directions[face],
				upVectors[face]));
	}

	std::uint8_t requiredMask = 0;
	const float aspectRatio =
		static_cast<float>(properties.SCREEN_WIDTH) /
		static_cast<float>(
			(std::max)(1, properties.SCREEN_HEIGHT));
	const Frustum cameraFrustum = BuildFrustum(
		camera_ptr->GetProjectionMatrix(aspectRatio) *
		camera_ptr->GetViewMatrix());
	bool invalidReceiverBounds = false;
	auto classifyReceivers = [&](const std::vector<MeshDrawItem>& receivers) {
		for (const MeshDrawItem& receiver : receivers) {
			if (!receiver.model ||
				!receiver.mesh ||
				!IsFiniteVector(receiver.worldBoundsCenter) ||
				!std::isfinite(receiver.worldBoundsRadius) ||
				receiver.worldBoundsRadius < 0.0f) {
				invalidReceiverBounds = true;
				return;
			}
			const bool cameraVisible =
				receiver.worldBoundsValid
					? cameraFrustum.IntersectsObb(
						receiver.worldBoundsCenter,
						receiver.worldBoundsAxisX,
						receiver.worldBoundsAxisY,
						receiver.worldBoundsAxisZ)
					: cameraFrustum.IntersectsSphere(
						receiver.worldBoundsCenter,
						receiver.worldBoundsRadius);
			if (!cameraVisible) {
				continue;
			}
			const glm::vec3 lightOffset =
				receiver.worldBoundsCenter - light.position;
			const float maximumDistance =
				safeFar + receiver.worldBoundsRadius;
			if (glm::dot(lightOffset, lightOffset) >
				maximumDistance * maximumDistance) {
				continue;
			}
			for (std::size_t face = 0;
				face < faceFrusta.size();
				++face) {
				const std::uint8_t faceBit =
					static_cast<std::uint8_t>(1u << face);
				if ((requiredMask & faceBit) != 0) {
					continue;
				}
				const bool faceVisible =
					receiver.worldBoundsValid
						? faceFrusta[face].IntersectsObb(
							receiver.worldBoundsCenter,
							receiver.worldBoundsAxisX,
							receiver.worldBoundsAxisY,
							receiver.worldBoundsAxisZ)
						: faceFrusta[face].IntersectsSphere(
							receiver.worldBoundsCenter,
							receiver.worldBoundsRadius);
				if (faceVisible) {
					requiredMask = static_cast<std::uint8_t>(
						requiredMask | faceBit);
				}
			}
			if (requiredMask == 0x3fu) {
				return;
			}
		}
	};
	classifyReceivers(m_opaqueMeshList);
	if (!invalidReceiverBounds && requiredMask != 0x3fu) {
		classifyReceivers(m_transparentMeshList);
	}
	if (invalidReceiverBounds) {
		return 0x3fu;
	}
	return requiredMask;
}

bool Scene::IsPointShadowPerFaceCacheEnabled() const {
	return properties.SHADOW_PER_LIGHT_CACHE &&
		properties.POINT_SHADOW_PER_FACE_CACHE &&
		(properties.POINT_SHADOW_ADAPTIVE_RENDERING ||
			properties.POINT_SHADOW_SIX_FACE_RENDERING);
}

std::size_t Scene::BuildShadowCacheSignature() const {
	std::size_t signature = 0;
	hash_combine(
		signature,
		modelSource.GetSceneTopologyRevision());
	hash_combine(
		signature,
		properties.DIRECTIONAL_SHADOW_LIGHT_AABB_FIT);
	hash_combine(
		signature,
		properties.DIRECTIONAL_SHADOW_DENSITY_RESOLUTION);
	hash_combine(signature, properties.POINT_SHADOW_ADAPTIVE_RENDERING);
	hash_combine(signature, properties.POINT_SHADOW_SIX_FACE_RENDERING);
	hash_combine(signature, properties.POINT_SHADOW_FACE_CULLING);
	hash_combine(signature, properties.SHADOW_SPATIAL_CASTER_CACHE);
	hash_combine(signature, properties.POINT_SHADOW_PER_FACE_CACHE);
	hash_combine(
		signature,
		properties.SHADOW_SPOT_CASTER_DEPTH_FIT);
	auto hashFloat = [&](float value) {
		hash_combine(signature, value);
	};
	auto hashVec3 = [&](const glm::vec3& value) {
		hashFloat(value.x);
		hashFloat(value.y);
		hashFloat(value.z);
	};
	auto hashMatrix = [&](const glm::mat4& value) {
		for (int column = 0; column < 4; ++column) {
			for (int row = 0; row < 4; ++row) {
				hashFloat(value[column][row]);
			}
		}
	};

	std::unordered_set<const Material*> hashedMaterials;
	for (const auto& model : modelSource.GetModels()) {
		hash_combine(signature, reinterpret_cast<std::uintptr_t>(model.get()));
		if (!model) {
			continue;
		}
		hash_combine(signature, model->GetAcitveStatus());
		if (!model->GetAcitveStatus()) {
			continue;
		}
		hashMatrix(model->getModelMatrix());
		for (const Mesh& mesh : model->GetMeshes()) {
			hash_combine(signature, mesh.GetActiveStatus());
			if (!mesh.GetActiveStatus()) {
				continue;
			}
			hash_combine(signature, mesh.GetVAO());
			hash_combine(signature, mesh.GetDrawCount());
			const Material* material = mesh.material_ptr;
			hash_combine(signature, reinterpret_cast<std::uintptr_t>(material));
			if (!material || !hashedMaterials.insert(material).second) {
				continue;
			}
			for (const auto& [name, property] : material->GetProperties()) {
				std::size_t propertyHash = std::hash<std::string>{}(name);
				hash_combine(propertyHash, static_cast<int>(property.type));
				switch (property.type) {
				case MaterialPropertyType::Float:
					hash_combine(propertyHash, property.scalarValue.floatValue);
					break;
				case MaterialPropertyType::Int:
					hash_combine(propertyHash, property.scalarValue.intValue);
					break;
				case MaterialPropertyType::Bool:
					hash_combine(propertyHash, property.scalarValue.boolValue);
					break;
				case MaterialPropertyType::Vec2:
					hash_combine(propertyHash, property.vec2Value.x);
					hash_combine(propertyHash, property.vec2Value.y);
					break;
				case MaterialPropertyType::Vec3:
				case MaterialPropertyType::Color:
					hash_combine(propertyHash, property.vec3Value.x);
					hash_combine(propertyHash, property.vec3Value.y);
					hash_combine(propertyHash, property.vec3Value.z);
					break;
				case MaterialPropertyType::Vec4:
					hash_combine(propertyHash, property.vec4Value.x);
					hash_combine(propertyHash, property.vec4Value.y);
					hash_combine(propertyHash, property.vec4Value.z);
					hash_combine(propertyHash, property.vec4Value.w);
					break;
				case MaterialPropertyType::Texture:
					for (const Texture& texture : property.textures) {
						hash_combine(propertyHash, texture.textureID);
					}
					break;
				}
				signature ^= propertyHash + 0x9e3779b9 +
					(signature << 6) + (signature >> 2);
			}
		}
	}

	for (const auto& light : lightSource.directionLights) {
		hash_combine(signature, light.m_active);
		hash_combine(signature, light.useShadowMap);
		hash_combine(signature, light.shadowResolution);
		hash_combine(
			signature,
			light.GetEffectiveShadowResolution());
		hashVec3(light.direction);
		hashVec3(light.shadowCenter);
		hashFloat(light.near_plane);
		hashFloat(light.far_plane);
		hashFloat(light.distance);
		hashFloat(light.width);
		hash_combine(signature, light.lightSpaceAabbFitActive);
		hashFloat(light.fittedHalfHeight);
	}
	for (const auto& light : lightSource.pointLights) {
		hash_combine(signature, light.m_active);
		hash_combine(signature, light.useShadowMap);
		hash_combine(signature, light.shadowResolution);
		hashVec3(light.position);
		hashFloat(light.near);
		hashFloat(light.far);
	}
	for (const auto& light : lightSource.spotLights) {
		hash_combine(signature, light.m_active);
		hash_combine(signature, light.useShadowMap);
		hash_combine(signature, light.shadowResolution);
		hashVec3(light.position);
		hashVec3(light.direction);
		hashFloat(light.outerCutOff);
		hashFloat(light.near_plane);
		hashFloat(light.far_plane);
	}
	const auto shadowShader =
		ShaderManager::GetInstance().GetShader(ShaderManager::Shadow);
	const auto shadowCubeShader =
		ShaderManager::GetInstance().GetShader(ShaderManager::ShadowCube);
	const auto shadowCubeFaceShader =
		ShaderManager::GetInstance().GetShader(
			ShaderManager::ShadowCubeFace);
	hash_combine(
		signature,
		shadowShader ? shadowShader->GetRevision() : 0u);
	hash_combine(
		signature,
		shadowCubeShader ? shadowCubeShader->GetRevision() : 0u);
	hash_combine(
		signature,
		shadowCubeFaceShader ? shadowCubeFaceShader->GetRevision() : 0u);
	return signature;
}

void Scene::InvalidatePerLightShadowCaches() {
	for (auto& light : lightSource.directionLights) {
		light.shadowCache.Invalidate();
	}
	for (auto& light : lightSource.pointLights) {
		light.shadowCache.Invalidate();
		light.shadowFaceCache.Invalidate();
	}
	for (auto& light : lightSource.spotLights) {
		light.shadowCache.Invalidate();
	}
}

void Scene::InvalidateShadowCache() {
	m_shadowCacheValid = false;
	InvalidatePerLightShadowCaches();
}

void Scene::SynchronizeSceneTopologyRevision() {
	const std::uint64_t currentRevision =
		modelSource.GetSceneTopologyRevision();
	m_shadowStats.sceneTopologyRevision = currentRevision;
	m_shadowStats.sceneTopologyModelCount =
		modelSource.GetModels().size();
	if (m_observedSceneTopologyRevision == 0) {
		m_observedSceneTopologyRevision = currentRevision;
		return;
	}
	if (m_observedSceneTopologyRevision == currentRevision) {
		return;
	}

	m_observedSceneTopologyRevision = currentRevision;
	++m_shadowStats.sceneTopologyInvalidationCount;
	m_shadowCacheValid = false;
	InvalidatePerLightShadowCaches();
	m_shadowCasterStateInitialized = false;
	m_shadowCasterStatePrepared = false;
	m_shadowCasterStateReliable = true;
	m_shadowCasterBoundsValid = false;
	m_shadowCasterDrawListValid = false;
	m_shadowCasterDrawListRevision = 0;
}

void Scene::SynchronizeShadowCacheGranularity(
	bool perLightCacheEnabled) {
	if (m_shadowCacheGranularityInitialized &&
		m_shadowCacheUsedPerLight == perLightCacheEnabled) {
		return;
	}
	m_shadowCacheGranularityInitialized = true;
	m_shadowCacheUsedPerLight = perLightCacheEnabled;
	m_shadowCacheValid = false;
	InvalidatePerLightShadowCaches();
}

void Scene::DisableEnabledShadowContent() {
	auto disable = [](auto& lights) {
		for (auto& light : lights) {
			if (light.m_active && light.useShadowMap) {
				light.shadowCache.Invalidate();
			}
		}
	};
	disable(lightSource.directionLights);
	disable(lightSource.pointLights);
	disable(lightSource.spotLights);
	for (auto& light : lightSource.pointLights) {
		if (light.m_active && light.useShadowMap) {
			light.shadowFaceCache.Invalidate();
		}
	}
}

bool Scene::CommitEnabledShadowContent() {
	auto targetsReady = [](const auto& lights) {
		for (const auto& light : lights) {
			if (!light.m_active || !light.useShadowMap) {
				continue;
			}
			if (!ShadowMapCacheState::IsTargetReady(light.shadowFBO)) {
				return false;
			}
		}
		return true;
	};
	if (!targetsReady(lightSource.directionLights) ||
		!targetsReady(lightSource.pointLights) ||
		!targetsReady(lightSource.spotLights)) {
		DisableEnabledShadowContent();
		return false;
	}

	auto commit = [](auto& lights) {
		for (auto& light : lights) {
			if (light.m_active && light.useShadowMap) {
				light.shadowCache.CommitContent(light.shadowFBO);
			}
		}
	};
	commit(lightSource.directionLights);
	commit(lightSource.pointLights);
	commit(lightSource.spotLights);
	return true;
}

bool Scene::AreEnabledShadowMapsSampleable() const {
	auto sampleable = [](const auto& lights) {
		for (const auto& light : lights) {
			if (!light.m_active || !light.useShadowMap) {
				continue;
			}
			if (!light.shadowCache.IsSampleable(light.shadowFBO)) {
				return false;
			}
		}
		return true;
	};
	return sampleable(lightSource.directionLights) &&
		sampleable(lightSource.pointLights) &&
		sampleable(lightSource.spotLights);
}

void Scene::UpdateShadowCasterStats(
	std::uint64_t renderedLightCount,
	std::uint64_t trianglePassMultiplier) {
	if (properties.SHADOW_CASTER_CULLING) {
		if (m_pendingUnculledRenderedLightCount > 0) {
			std::uint64_t meshCount = 0;
			std::uint64_t triangleCount = 0;
			for (const auto& model : modelSource.GetModels()) {
				if (!model || !model->GetAcitveStatus()) {
					continue;
				}
				for (const Mesh& mesh : model->GetMeshes()) {
					if (!mesh.GetActiveStatus() ||
						!mesh.material_ptr ||
						mesh.GetDrawCount() == 0) {
						continue;
					}
					++meshCount;
					triangleCount += mesh.GetDrawCount() / 3u;
				}
			}
			const std::uint64_t unculledDrawCount =
				meshCount * m_pendingUnculledRenderedLightCount;
			m_pendingShadowCasterCandidateCount += unculledDrawCount;
			m_pendingShadowCasterDrawCount += unculledDrawCount;
			m_pendingShadowCasterTriangleCount +=
				triangleCount *
				m_pendingUnculledTrianglePassMultiplier;
		}
		m_shadowStats.casterCandidateCount =
			m_pendingShadowCasterCandidateCount;
		m_shadowStats.casterCulledCount =
			m_pendingShadowCasterCulledCount;
		m_shadowStats.casterCullingLightCount =
			m_pendingShadowCasterCullingLightCount;
		m_shadowStats.casterDrawCount =
			m_pendingShadowCasterDrawCount;
		m_shadowStats.casterTriangleCount =
			m_pendingShadowCasterTriangleCount;
	}
	else {
		std::uint64_t meshCount = 0;
		std::uint64_t triangleCount = 0;
		for (const auto& model : modelSource.GetModels()) {
			if (!model || !model->GetAcitveStatus()) {
				continue;
			}
			for (const Mesh& mesh : model->GetMeshes()) {
				if (!mesh.GetActiveStatus() ||
					!mesh.material_ptr ||
					mesh.GetDrawCount() == 0) {
					continue;
				}
				++meshCount;
				triangleCount += mesh.GetDrawCount() / 3u;
			}
		}
		m_shadowStats.casterCandidateCount =
			meshCount * m_pendingShadowDrawPassMultiplier;
		m_shadowStats.casterCulledCount = 0;
		m_shadowStats.casterCullingLightCount = 0;
		m_shadowStats.casterDrawCount =
			meshCount * m_pendingShadowDrawPassMultiplier;
		m_shadowStats.casterTriangleCount =
			triangleCount * trianglePassMultiplier;
	}
	m_shadowStats.totalCasterCandidateCount +=
		m_shadowStats.casterCandidateCount;
	m_shadowStats.totalCasterCulledCount +=
		m_shadowStats.casterCulledCount;
	m_shadowStats.totalCasterCullingLightCount +=
		m_shadowStats.casterCullingLightCount;
	m_shadowStats.totalCasterDrawCount +=
		m_shadowStats.casterDrawCount;
	m_shadowStats.totalCasterTriangleCount +=
		m_shadowStats.casterTriangleCount;
}

void Scene::RenderShadowMapUpdate(
	std::uint64_t& renderedLightCount,
	std::uint64_t& trianglePassMultiplier,
	ShadowLightUpdateSelection* selection,
	bool clearOnly) {
	m_pendingShadowCasterCandidateCount = 0;
	m_pendingShadowCasterCulledCount = 0;
	m_pendingShadowCasterCullingLightCount = 0;
	m_pendingShadowCasterDrawCount = 0;
	m_pendingShadowCasterTriangleCount = 0;
	m_pendingUnculledRenderedLightCount = 0;
	m_pendingUnculledTrianglePassMultiplier = 0;
	m_pendingShadowDrawPassMultiplier = 0;
	PERF_GPU_SCOPE("Shadow Map Update");
	GLState::Enable(GL_DEPTH_TEST);
	GLState::DepthMask(GL_TRUE);
	GLState::Disable(GL_STENCIL_TEST);
	GLState::Disable(GL_BLEND);
	GLState::Disable(GL_CULL_FACE);
	GLState::Enable(GL_POLYGON_OFFSET_FILL);
	glPolygonOffset(1.5f, 4.0f);
	glClearDepth(1.0f);
	GLState::ColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	GLState::StencilMask(0x00);

	const auto restoreShadowRenderState = [&]() {
		GLState::Disable(GL_POLYGON_OFFSET_FILL);
		GLState::ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		GLState::StencilMask(0xFF);
		GLState::BindFramebuffer(GL_FRAMEBUFFER, 0);
		glViewport(0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT);
		GLState::Disable(GL_CULL_FACE);
	};
	const auto selectedForUpdate = [](
		const std::vector<std::uint8_t>* states,
		std::size_t index) {
		return !states ||
			(index < states->size() && (*states)[index] != 0);
	};

	if (clearOnly) {
		for (std::size_t index = 0;
			index < lightSource.directionLights.size();
			++index) {
			auto& light = lightSource.directionLights[index];
			if (!selectedForUpdate(
				selection ? &selection->direction : nullptr,
				index) ||
				!light.GetActiveStatus() ||
				!light.useShadowMap) {
				continue;
			}
			FBO* target = light.EnsureShadowFBO();
			if (!ShadowMapCacheState::IsTargetReady(target)) {
				++m_shadowStats.shadowResourceFailureCount;
				continue;
			}
			glViewport(0, 0, target->width, target->height);
			GLState::BindFramebuffer(GL_FRAMEBUFFER, target->framebufferID);
			glClear(GL_DEPTH_BUFFER_BIT);
			++renderedLightCount;
			++m_shadowStats.directionalLightUpdateCount;
			++m_shadowStats.emptyShadowClearCount;
			if (selection) {
				selection->direction[index] = 2;
			}
		}
		for (std::size_t index = 0;
			index < lightSource.pointLights.size();
			++index) {
			auto& light = lightSource.pointLights[index];
			if (!selectedForUpdate(
				selection ? &selection->point : nullptr,
				index) ||
				!light.GetActiveStatus() ||
				!light.useShadowMap) {
				continue;
			}
			FBO* target = light.EnsureShadowFBO();
			if (!ShadowMapCacheState::IsTargetReady(target)) {
				++m_shadowStats.shadowResourceFailureCount;
				continue;
			}
			glViewport(0, 0, target->width, target->height);
			GLState::BindFramebuffer(GL_FRAMEBUFFER, target->framebufferID);
			glClear(GL_DEPTH_BUFFER_BIT);
			++renderedLightCount;
			++m_shadowStats.pointLightUpdateCount;
			++m_shadowStats.emptyShadowClearCount;
			if (selection) {
				selection->point[index] = 2;
			}
		}
		for (std::size_t index = 0;
			index < lightSource.spotLights.size();
			++index) {
			auto& light = lightSource.spotLights[index];
			if (!selectedForUpdate(
				selection ? &selection->spot : nullptr,
				index) ||
				!light.GetActiveStatus() ||
				!light.useShadowMap) {
				continue;
			}
			FBO* target = light.EnsureShadowFBO();
			if (!ShadowMapCacheState::IsTargetReady(target)) {
				++m_shadowStats.shadowResourceFailureCount;
				continue;
			}
			glViewport(0, 0, target->width, target->height);
			GLState::BindFramebuffer(GL_FRAMEBUFFER, target->framebufferID);
			glClear(GL_DEPTH_BUFFER_BIT);
			++renderedLightCount;
			++m_shadowStats.spotLightUpdateCount;
			++m_shadowStats.emptyShadowClearCount;
			if (selection) {
				selection->spot[index] = 2;
			}
		}
		restoreShadowRenderState();
		return;
	}

	auto hasRequestedUpdate = [](const std::vector<std::uint8_t>& states) {
		return std::any_of(
			states.begin(),
			states.end(),
			[](std::uint8_t state) { return state != 0; });
	};
	const bool updateDirectional =
		selection
			? hasRequestedUpdate(selection->direction)
			: std::any_of(
				lightSource.directionLights.begin(),
				lightSource.directionLights.end(),
				[](const DirectionLight& light) {
					return light.m_active && light.useShadowMap;
				});
	const bool updatePoint =
		selection
			? hasRequestedUpdate(selection->point)
			: std::any_of(
				lightSource.pointLights.begin(),
				lightSource.pointLights.end(),
				[](const PointLight& light) {
					return light.m_active && light.useShadowMap;
				});
	const bool updateSpot =
		selection
			? hasRequestedUpdate(selection->spot)
			: std::any_of(
				lightSource.spotLights.begin(),
				lightSource.spotLights.end(),
				[](const SpotLight& light) {
					return light.m_active && light.useShadowMap;
				});

	auto shadowShader =
		ShaderManager::GetInstance().GetShader(ShaderManager::Shadow);
	if (IsShadowShaderReady(shadowShader) && updateDirectional) {
		PERF_GPU_SCOPE("Directional Shadow Update");
		shadowShader->use();
		for (std::size_t index = 0;
			index < lightSource.directionLights.size();
			++index) {
			auto& light = lightSource.directionLights[index];
			if (selection &&
				(index >= selection->direction.size() ||
					selection->direction[index] == 0)) {
				continue;
			}
			if (!light.GetActiveStatus() || !light.useShadowMap) continue;
			FBO* shadowFBO = light.EnsureShadowFBO();
			if (!shadowFBO) {
				++m_shadowStats.shadowResourceFailureCount;
				continue;
			}
			glViewport(0, 0, shadowFBO->width, shadowFBO->height);
			GLState::BindFramebuffer(
				GL_FRAMEBUFFER, shadowFBO->framebufferID);
			glClear(GL_DEPTH_BUFFER_BIT);
			const glm::mat4 lightSpaceMatrix =
				light.GetLightSpaceMatrix();
			shadowShader->setMat4(
				"lightSpaceMatrix", lightSpaceMatrix);
			bool useCasterCulling = false;
			if (properties.SHADOW_CASTER_CULLING &&
				!light.autoFitShadow) {
				BuildShadowCasterDrawList();
				useCasterCulling =
					HasEnoughShadowCasterWorkForCulling();
			}
			if (useCasterCulling) {
				++m_pendingShadowCasterCullingLightCount;
				RenderShadowCasters(
					*shadowShader, lightSpaceMatrix, 1u);
			}
			else {
				RenderScene(*shadowShader);
				if (properties.SHADOW_CASTER_CULLING) {
					++m_pendingUnculledRenderedLightCount;
					++m_pendingUnculledTrianglePassMultiplier;
				}
			}
			++renderedLightCount;
			++trianglePassMultiplier;
			++m_pendingShadowDrawPassMultiplier;
			++m_shadowStats.directionalLightUpdateCount;
			if (selection) {
				selection->direction[index] = 2;
			}
		}
	}

	if (IsShadowShaderReady(shadowShader) && updateSpot) {
		PERF_GPU_SCOPE("Spot Shadow Update");
		shadowShader->use();
		for (std::size_t index = 0;
			index < lightSource.spotLights.size();
			++index) {
			auto& light = lightSource.spotLights[index];
			if (selection &&
				(index >= selection->spot.size() ||
					selection->spot[index] == 0)) {
				continue;
			}
			if (!light.GetActiveStatus() || !light.useShadowMap) continue;
			FBO* shadowFBO = light.EnsureShadowFBO();
			if (!shadowFBO) {
				++m_shadowStats.shadowResourceFailureCount;
				continue;
			}
			glViewport(0, 0, shadowFBO->width, shadowFBO->height);
			GLState::BindFramebuffer(
				GL_FRAMEBUFFER, shadowFBO->framebufferID);
			glClear(GL_DEPTH_BUFFER_BIT);
			const glm::mat4 lightSpaceMatrix =
				light.GetLightSpaceMatrix();
			shadowShader->setMat4(
				"lightSpaceMatrix", lightSpaceMatrix);
			bool useCasterCulling = false;
			if (properties.SHADOW_CASTER_CULLING) {
				BuildShadowCasterDrawList();
				useCasterCulling =
					HasEnoughShadowCasterWorkForCulling();
			}
			if (useCasterCulling) {
				++m_pendingShadowCasterCullingLightCount;
				RenderShadowCasters(
					*shadowShader, lightSpaceMatrix, 1u);
			}
			else {
				RenderScene(*shadowShader);
				if (properties.SHADOW_CASTER_CULLING) {
					++m_pendingUnculledRenderedLightCount;
					++m_pendingUnculledTrianglePassMultiplier;
				}
			}
			++renderedLightCount;
			++trianglePassMultiplier;
			++m_pendingShadowDrawPassMultiplier;
			++m_shadowStats.spotLightUpdateCount;
			if (selection) {
				selection->spot[index] = 2;
			}
		}
	}

	auto shadowCubeShader =
		ShaderManager::GetInstance().GetShader(ShaderManager::ShadowCube);
	auto shadowCubeFaceShader =
		ShaderManager::GetInstance().GetShader(
			ShaderManager::ShadowCubeFace);
	const bool useSixFacePointShadow =
		ShouldUseSixFacePointShadow();
	auto pointShadowShader =
		useSixFacePointShadow
			? shadowCubeFaceShader
			: shadowCubeShader;
	if (IsShadowShaderReady(pointShadowShader) && updatePoint) {
		PERF_GPU_SCOPE("Point Shadow Update");
		pointShadowShader->use();
		for (std::size_t index = 0;
			index < lightSource.pointLights.size();
			++index) {
			auto& light = lightSource.pointLights[index];
			if (selection &&
				(index >= selection->point.size() ||
					selection->point[index] == 0)) {
				continue;
			}
			if (!light.GetActiveStatus() || !light.useShadowMap) continue;
			FBO* shadowFBO = light.EnsureShadowFBO();
			if (!shadowFBO) {
				++m_shadowStats.shadowResourceFailureCount;
				continue;
			}
			glViewport(0, 0, shadowFBO->width, shadowFBO->height);
			auto& lightSpaceMatrices = light.GetLightSpaceMatrices();
			pointShadowShader->setFloat("far_plane", light.far);
			pointShadowShader->setVec3("lightPos", light.position);

			if (useSixFacePointShadow) {
				std::uint8_t faceUpdateMask = 0x3fu;
				if (selection &&
					index < selection->pointUpdateFaceMask.size()) {
					faceUpdateMask =
						selection->pointUpdateFaceMask[index];
				}
				if (faceUpdateMask == 0) {
					continue;
				}
				std::uint64_t faceUpdateCount = 0;
				for (std::uint8_t mask = faceUpdateMask;
					mask != 0;
					mask = static_cast<std::uint8_t>(mask >> 1u)) {
					faceUpdateCount += mask & 1u;
				}
				std::array<unsigned int, 6> faceFramebuffers{};
				bool faceTargetsReady = true;
				for (int face = 0; face < 6; ++face) {
					faceFramebuffers[static_cast<std::size_t>(face)] =
						shadowFBO->GetCubeFaceFramebuffer(face);
					faceTargetsReady =
						faceTargetsReady &&
						faceFramebuffers[static_cast<std::size_t>(face)] != 0;
				}
				if (!faceTargetsReady) {
					++m_shadowStats.shadowResourceFailureCount;
					continue;
				}

				const bool partialFaceUpdate =
					faceUpdateMask != 0x3fu;
				if (!partialFaceUpdate) {
					// A complete rebuild can clear every layer once. Partial
					// updates must preserve cached faces and clear only the
					// selected face FBO immediately before drawing it.
					GLState::BindFramebuffer(
						GL_FRAMEBUFFER,
						shadowFBO->framebufferID);
					glClear(GL_DEPTH_BUFFER_BIT);
				}

				const bool useFaceCulling =
					properties.SHADOW_CASTER_CULLING &&
					properties.POINT_SHADOW_FACE_CULLING;
				if (useFaceCulling) {
					BuildShadowCasterDrawList();
				}
				for (int face = 0; face < 6; ++face) {
					const std::uint8_t faceBit =
						static_cast<std::uint8_t>(1u << face);
					if ((faceUpdateMask & faceBit) == 0) {
						continue;
					}
					GLState::BindFramebuffer(
						GL_FRAMEBUFFER,
						faceFramebuffers[static_cast<std::size_t>(face)]);
					if (partialFaceUpdate) {
						glClear(GL_DEPTH_BUFFER_BIT);
					}
					pointShadowShader->setMat4(
						"shadowMatrix",
						lightSpaceMatrices[face]);
					if (useFaceCulling) {
						++m_pendingShadowCasterCullingLightCount;
						RenderShadowCasters(
							*pointShadowShader,
							lightSpaceMatrices[face],
							1u);
					}
					else {
						RenderScene(*pointShadowShader);
					}
				}
				if (!useFaceCulling &&
					properties.SHADOW_CASTER_CULLING) {
					m_pendingUnculledRenderedLightCount +=
						faceUpdateCount;
					m_pendingUnculledTrianglePassMultiplier +=
						faceUpdateCount;
				}
				m_pendingShadowDrawPassMultiplier +=
					faceUpdateCount;
				++m_shadowStats.pointShadowSixFaceUpdateCount;
				m_shadowStats.pointShadowSubmissionPassCount +=
					faceUpdateCount;
				m_shadowStats.pointShadowRenderedFaceCount +=
					faceUpdateCount;
				if (partialFaceUpdate) {
					++m_shadowStats.pointShadowPartialUpdateCount;
				}
				else {
					++m_shadowStats.pointShadowFullUpdateCount;
				}
				if (useFaceCulling) {
					m_shadowStats.pointShadowFaceCullingPassCount +=
						faceUpdateCount;
				}
				trianglePassMultiplier += faceUpdateCount;
			}
			else {
				GLState::BindFramebuffer(
					GL_FRAMEBUFFER, shadowFBO->framebufferID);
				glClear(GL_DEPTH_BUFFER_BIT);
				for (int face = 0; face < 6; ++face) {
					pointShadowShader->setMat4(
						"shadowMatrices[" + std::to_string(face) + "]",
						lightSpaceMatrices[face]);
				}
				bool useCasterCulling = false;
				if (properties.SHADOW_CASTER_CULLING &&
					!light.autoFitShadow) {
					BuildShadowCasterDrawList();
					useCasterCulling =
						HasEnoughShadowCasterWorkForCulling();
				}
				if (useCasterCulling) {
					++m_pendingShadowCasterCullingLightCount;
					RenderPointShadowCasters(
						*pointShadowShader,
						light.position,
						light.far,
						6u);
				}
				else {
					RenderScene(*pointShadowShader);
					if (properties.SHADOW_CASTER_CULLING) {
						++m_pendingUnculledRenderedLightCount;
						m_pendingUnculledTrianglePassMultiplier += 6u;
					}
				}
				++m_pendingShadowDrawPassMultiplier;
				++m_shadowStats.pointShadowLayeredUpdateCount;
				++m_shadowStats.pointShadowSubmissionPassCount;
				m_shadowStats.pointShadowRenderedFaceCount += 6u;
				trianglePassMultiplier += 6u;
			}
			++renderedLightCount;
			++m_shadowStats.pointLightUpdateCount;
			if (selection) {
				selection->point[index] = 2;
			}
		}
	}

	restoreShadowRenderState();
}

void Scene::DrawShadowMapPerLight() {
	auto& framebufferManager = FramebuffersManager::GetInstance();
	const auto shadowShader =
		ShaderManager::GetInstance().GetShader(ShaderManager::Shadow);
	const auto shadowCubeShader =
		ShaderManager::GetInstance().GetShader(ShaderManager::ShadowCube);
	const auto shadowCubeFaceShader =
		ShaderManager::GetInstance().GetShader(
			ShaderManager::ShadowCubeFace);
	const auto pointShadowShader =
		ShouldUseSixFacePointShadow()
			? shadowCubeFaceShader
			: shadowCubeShader;
	const bool shadowShaderReady =
		IsShadowShaderReady(shadowShader);
	const bool pointShadowShaderReady =
		IsShadowShaderReady(pointShadowShader);
	ShadowLightUpdateSelection selection;
	selection.direction.assign(
		lightSource.directionLights.size(), 0);
	selection.point.assign(lightSource.pointLights.size(), 0);
	selection.spot.assign(lightSource.spotLights.size(), 0);
	selection.directionSignature.assign(
		lightSource.directionLights.size(), 0);
	selection.pointSignature.assign(
		lightSource.pointLights.size(), 0);
	selection.spotSignature.assign(
		lightSource.spotLights.size(), 0);
	selection.pointRequiredFaceMask.assign(
		lightSource.pointLights.size(), 0x3fu);
	selection.pointUpdateFaceMask.assign(
		lightSource.pointLights.size(), 0x3fu);
	selection.pointFaceSignatures.resize(
		lightSource.pointLights.size());

	std::uint64_t enabledLightCount = 0;
	std::uint64_t lightCacheHitCount = 0;
	bool clearOnly = false;
	const bool usePointFaceCache =
		IsPointShadowPerFaceCacheEnabled();
	{
		PERF_CPU_SCOPE("Shadow Cache Check");
		ShadowCacheCheckTelemetry telemetry{ m_shadowStats, false };

		bool releasedShadowTarget = false;
		auto releaseIfDisabled = [&](auto& light) {
			const bool enabled =
				light.GetActiveStatus() && light.useShadowMap;
			if (!enabled) {
				light.shadowCache.Invalidate();
				if (light.shadowFBO) {
					framebufferManager.ReleaseFBO(light.shadowFBO);
					light.shadowFBO = nullptr;
					releasedShadowTarget = true;
				}
			}
			return enabled;
		};

		for (auto& light : lightSource.directionLights) {
			if (releaseIfDisabled(light)) {
				++enabledLightCount;
			}
		}
		for (auto& light : lightSource.pointLights) {
			if (releaseIfDisabled(light)) {
				++enabledLightCount;
			}
			else {
				light.shadowFaceCache.Invalidate();
			}
		}
		for (auto& light : lightSource.spotLights) {
			if (releaseIfDisabled(light)) {
				++enabledLightCount;
			}
		}
		if (releasedShadowTarget) {
			framebufferManager.TrimUnusedFBOs();
		}
		if (enabledLightCount == 0) {
			m_shadowCacheValid = false;
			m_shadowCasterStatePrepared = false;
			return;
		}

		if (!m_shadowCasterStatePrepared) {
			RefreshShadowCasterStateFallback();
		}
		m_shadowCasterStatePrepared = false;
		if (!m_shadowCasterStateReliable) {
			m_shadowCacheValid = false;
			DisableEnabledShadowContent();
			++m_shadowStats.cacheMissCount;
			++m_shadowStats.conservativeShadowFallbackCount;
			return;
		}
		clearOnly = !m_shadowCasterBoundsValid;

		const std::uint64_t shadowShaderRevision =
			shadowShaderReady ? shadowShader->GetRevision() : 0u;
		const std::uint64_t pointShadowShaderRevision =
			pointShadowShaderReady
				? pointShadowShader->GetRevision()
				: 0u;

		for (std::size_t index = 0;
			index < lightSource.directionLights.size();
			++index) {
			auto& light = lightSource.directionLights[index];
			if (!light.GetActiveStatus() || !light.useShadowMap) continue;
			FBO* target = light.EnsureShadowFBO();
			if ((!clearOnly && !shadowShaderReady) || !target) {
				light.shadowCache.Invalidate();
				++m_shadowStats.shadowResourceFailureCount;
				continue;
			}
			const std::size_t currentSignature =
				BuildDirectionalShadowRevisionSignature(
					light, shadowShaderRevision);
			if (light.shadowCache.IsCacheHit(
				currentSignature,
				target)) {
				++lightCacheHitCount;
				continue;
			}
			light.shadowCache.Invalidate();
			if (!clearOnly) {
				FitDirectionalShadowToCasterBounds(light);
			}
			selection.direction[index] = 1;
			selection.directionSignature[index] =
				BuildDirectionalShadowRevisionSignature(
					light, shadowShaderRevision);
		}

		for (std::size_t index = 0;
			index < lightSource.pointLights.size();
			++index) {
			auto& light = lightSource.pointLights[index];
			if (!light.GetActiveStatus() || !light.useShadowMap) continue;
			FBO* target = light.EnsureShadowFBO();
			if ((!clearOnly && !pointShadowShaderReady) || !target) {
				light.shadowCache.Invalidate();
				light.shadowFaceCache.Invalidate();
				++m_shadowStats.shadowResourceFailureCount;
				continue;
			}
			if (!clearOnly && usePointFaceCache) {
				light.shadowFaceCache.SynchronizeTarget(target);
				if (!light.shadowFaceCache.MatchesTarget(target)) {
					light.shadowCache.Invalidate();
					++m_shadowStats.shadowResourceFailureCount;
					continue;
				}

				light.FitShadowToBounds(
					m_cachedShadowCasterCenter,
					m_cachedShadowCasterRadius);
				const auto& lightSpaceMatrices =
					light.GetLightSpaceMatrices();

				const auto signatureStart =
					std::chrono::steady_clock::now();
				const auto faceSignatures =
					BuildPointShadowFaceRevisionSignatures(
						light,
						pointShadowShaderRevision,
						lightSpaceMatrices);
				const double signatureMilliseconds =
					std::chrono::duration<double, std::milli>(
						std::chrono::steady_clock::now() -
						signatureStart).count();
				++m_shadowStats.pointShadowFaceSignatureBuildCount;
				m_shadowStats.lastPointShadowFaceSignatureCpuMilliseconds =
					signatureMilliseconds;
				m_shadowStats.totalPointShadowFaceSignatureCpuMilliseconds +=
					signatureMilliseconds;

				const auto demandStart =
					std::chrono::steady_clock::now();
				const std::uint8_t requiredMask =
					ComputePointShadowRequiredFaceMask(light);
				const double demandMilliseconds =
					std::chrono::duration<double, std::milli>(
						std::chrono::steady_clock::now() -
						demandStart).count();
				++m_shadowStats.pointShadowFaceDemandCheckCount;
				m_shadowStats.lastPointShadowFaceDemandCpuMilliseconds =
					demandMilliseconds;
				m_shadowStats.totalPointShadowFaceDemandCpuMilliseconds +=
					demandMilliseconds;

				const std::uint8_t staleMask =
					light.shadowFaceCache.BuildMissMask(
						0x3fu,
						faceSignatures,
						target);
				std::uint8_t updateMask =
					static_cast<std::uint8_t>(
						requiredMask & staleMask);
				if (requiredMask != 0 &&
					!light.shadowCache.IsSampleable(target)) {
					updateMask = requiredMask;
				}
				const std::uint8_t hitMask =
					static_cast<std::uint8_t>(
						requiredMask & ~updateMask);
				const std::uint8_t deferredMask =
					static_cast<std::uint8_t>(
						staleMask & ~requiredMask);
				const auto countFaces = [](std::uint8_t mask) {
					std::uint64_t count = 0;
					for (; mask != 0; mask =
						static_cast<std::uint8_t>(mask >> 1u)) {
						count += mask & 1u;
					}
					return count;
				};
				m_shadowStats.pointShadowRequiredFaceCount +=
					countFaces(requiredMask);
				m_shadowStats.pointShadowFaceCacheHitCount +=
					countFaces(hitMask);
				m_shadowStats.pointShadowDeferredFaceCount +=
					countFaces(deferredMask);
				m_shadowStats.lastPointShadowRequiredFaceMask =
					requiredMask;
				m_shadowStats.lastPointShadowUpdateFaceMask =
					updateMask;
				if (requiredMask == 0) {
					++m_shadowStats.pointShadowZeroRequiredCount;
				}

				selection.pointRequiredFaceMask[index] =
					requiredMask;
				selection.pointUpdateFaceMask[index] =
					updateMask;
				selection.pointFaceSignatures[index] =
					faceSignatures;
				if (updateMask == 0) {
					++lightCacheHitCount;
					continue;
				}
				selection.point[index] = 1;
				continue;
			}
			const std::size_t currentSignature =
				BuildPointShadowRevisionSignature(
					light, pointShadowShaderRevision);
			if (light.shadowCache.IsCacheHit(
				currentSignature,
				target)) {
				++lightCacheHitCount;
				continue;
			}
			light.shadowCache.Invalidate();
			if (!clearOnly) {
				light.FitShadowToBounds(
					m_cachedShadowCasterCenter,
					m_cachedShadowCasterRadius);
			}
			selection.point[index] = 1;
			selection.pointSignature[index] =
				BuildPointShadowRevisionSignature(
					light, pointShadowShaderRevision);
		}

		for (std::size_t index = 0;
			index < lightSource.spotLights.size();
			++index) {
			auto& light = lightSource.spotLights[index];
			if (!light.GetActiveStatus() || !light.useShadowMap) continue;
			FBO* target = light.EnsureShadowFBO();
			if ((!clearOnly && !shadowShaderReady) || !target) {
				light.shadowCache.Invalidate();
				++m_shadowStats.shadowResourceFailureCount;
				continue;
			}
			const std::size_t currentSignature =
				BuildSpotShadowRevisionSignature(
					light, shadowShaderRevision);
			if (light.shadowCache.IsCacheHit(
				currentSignature,
				target)) {
				++lightCacheHitCount;
				continue;
			}
			light.shadowCache.Invalidate();
			if (!clearOnly) {
				FitSpotShadowToCasterBounds(
					light,
					index,
					m_cachedShadowCasterCenter,
					m_cachedShadowCasterRadius);
			}
			selection.spot[index] = 1;
			selection.spotSignature[index] =
				BuildSpotShadowRevisionSignature(
					light, shadowShaderRevision);
		}

		m_shadowStats.lightCacheHitCount += lightCacheHitCount;
		if (lightCacheHitCount == enabledLightCount) {
			++m_shadowStats.cacheHitCount;
			return;
		}
	}

	++m_shadowStats.cacheMissCount;
	const bool hasSelectedUpdate =
		std::any_of(
			selection.direction.begin(),
			selection.direction.end(),
			[](std::uint8_t state) { return state != 0; }) ||
		std::any_of(
			selection.point.begin(),
			selection.point.end(),
			[](std::uint8_t state) { return state != 0; }) ||
		std::any_of(
			selection.spot.begin(),
			selection.spot.end(),
			[](std::uint8_t state) { return state != 0; });
	if (!hasSelectedUpdate) {
		return;
	}

	const auto updateStart = std::chrono::steady_clock::now();
	std::uint64_t renderedLightCount = 0;
	std::uint64_t trianglePassMultiplier = 0;
	RenderShadowMapUpdate(
		renderedLightCount,
		trianglePassMultiplier,
		&selection,
		!m_shadowCasterBoundsValid);

	for (std::size_t index = 0;
		index < selection.direction.size();
		++index) {
		if (selection.direction[index] == 2) {
			auto& light = lightSource.directionLights[index];
			light.shadowCache.Commit(
				selection.directionSignature[index],
				light.shadowFBO);
		}
		else if (selection.direction[index] == 1) {
			lightSource.directionLights[index].shadowCache.Invalidate();
		}
	}
	for (std::size_t index = 0;
		index < selection.point.size();
		++index) {
		if (selection.point[index] == 2) {
			auto& light = lightSource.pointLights[index];
			if (usePointFaceCache && !clearOnly) {
				const std::uint8_t renderedMask =
					selection.pointUpdateFaceMask[index];
				light.shadowFaceCache.Commit(
					renderedMask,
					selection.pointFaceSignatures[index],
					light.shadowFBO);
				light.shadowCache.CommitContent(light.shadowFBO);
			}
			else {
				light.shadowCache.Commit(
					selection.pointSignature[index],
					light.shadowFBO);
				light.shadowFaceCache.Invalidate();
			}
		}
		else if (selection.point[index] == 1) {
			auto& light = lightSource.pointLights[index];
			light.shadowCache.Invalidate();
		}
	}
	for (std::size_t index = 0;
		index < selection.spot.size();
		++index) {
		if (selection.spot[index] == 2) {
			auto& light = lightSource.spotLights[index];
			light.shadowCache.Commit(
				selection.spotSignature[index],
				light.shadowFBO);
		}
		else if (selection.spot[index] == 1) {
			lightSource.spotLights[index].shadowCache.Invalidate();
		}
	}

	if (renderedLightCount > 0) {
		++m_shadowStats.updateCount;
	}
	m_shadowStats.updatedLightCount = renderedLightCount;
	UpdateShadowCasterStats(renderedLightCount, trianglePassMultiplier);
	m_shadowStats.lastUpdateCpuMilliseconds =
		std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - updateStart).count();
}

void Scene::DrawShadowMapRevision() {
	if (!m_shadowCacheStrategyInitialized ||
		m_shadowCacheUsedLegacySignature ||
		m_shadowCacheDisabledLastFrame) {
		m_shadowCacheStrategyInitialized = true;
		m_shadowCacheUsedLegacySignature = false;
		m_shadowCacheDisabledLastFrame = false;
		m_shadowCacheValid = false;
		InvalidatePerLightShadowCaches();
	}
	SynchronizeShadowCacheGranularity(
		properties.SHADOW_PER_LIGHT_CACHE);
	const bool spatialCastersEnabled =
		properties.SHADOW_PER_LIGHT_CACHE &&
		properties.SHADOW_SPATIAL_CASTER_CACHE;
	const bool pointFacesEnabled =
		IsPointShadowPerFaceCacheEnabled();
	if (!m_shadowCacheFeatureStateInitialized ||
		m_shadowCacheUsedSpatialCasters != spatialCastersEnabled ||
		m_shadowCacheUsedPointFaces != pointFacesEnabled) {
		m_shadowCacheFeatureStateInitialized = true;
		m_shadowCacheUsedSpatialCasters = spatialCastersEnabled;
		m_shadowCacheUsedPointFaces = pointFacesEnabled;
		m_shadowCacheValid = false;
		InvalidatePerLightShadowCaches();
	}
	if (properties.SHADOW_PER_LIGHT_CACHE) {
		DrawShadowMapPerLight();
		return;
	}
	DrawShadowMapRevisionGlobal(false);
}

void Scene::DrawShadowMapRevisionGlobal(bool forceUpdate) {
	auto& framebufferManager = FramebuffersManager::GetInstance();
	const auto shadowShader =
		ShaderManager::GetInstance().GetShader(ShaderManager::Shadow);
	const auto shadowCubeShader =
		ShaderManager::GetInstance().GetShader(ShaderManager::ShadowCube);
	const auto shadowCubeFaceShader =
		ShaderManager::GetInstance().GetShader(
			ShaderManager::ShadowCubeFace);
	const auto pointShadowShader =
		ShouldUseSixFacePointShadow()
			? shadowCubeFaceShader
			: shadowCubeShader;
	const bool shadowShaderReady =
		IsShadowShaderReady(shadowShader);
	const bool pointShadowShaderReady =
		IsShadowShaderReady(pointShadowShader);
	std::size_t signature = 0;
	std::uint64_t enabledLightCount = 0;
	bool targetsReady = true;
	bool clearOnly = false;
	bool hasDirectionalShadow = false;
	bool hasPointShadow = false;
	bool hasSpotShadow = false;

	{
		PERF_CPU_SCOPE("Shadow Cache Check");
		ShadowCacheCheckTelemetry telemetry{ m_shadowStats, false };

		bool releasedShadowTarget = false;
		auto releaseIfDisabled = [&](auto& light) {
			const bool enabled =
				light.GetActiveStatus() && light.useShadowMap;
			if (!enabled) {
				light.shadowCache.Invalidate();
				if (light.shadowFBO) {
					framebufferManager.ReleaseFBO(light.shadowFBO);
					light.shadowFBO = nullptr;
					releasedShadowTarget = true;
				}
			}
			return enabled;
		};

		for (auto& light : lightSource.directionLights) {
			if (releaseIfDisabled(light)) {
				++enabledLightCount;
				hasDirectionalShadow = true;
			}
		}
		for (auto& light : lightSource.pointLights) {
			if (releaseIfDisabled(light)) {
				++enabledLightCount;
				hasPointShadow = true;
			}
			else {
				light.shadowFaceCache.Invalidate();
			}
		}
		for (auto& light : lightSource.spotLights) {
			if (releaseIfDisabled(light)) {
				++enabledLightCount;
				hasSpotShadow = true;
			}
		}
		if (releasedShadowTarget) {
			framebufferManager.TrimUnusedFBOs();
			m_shadowCacheValid = false;
		}
		if (enabledLightCount == 0) {
			m_shadowCacheValid = false;
			m_shadowCasterStatePrepared = false;
			return;
		}

		if (!m_shadowCasterStatePrepared) {
			RefreshShadowCasterStateFallback();
		}
		m_shadowCasterStatePrepared = false;
		if (!m_shadowCasterStateReliable) {
			m_shadowCacheValid = false;
			DisableEnabledShadowContent();
			++m_shadowStats.cacheMissCount;
			++m_shadowStats.conservativeShadowFallbackCount;
			return;
		}
		clearOnly = !m_shadowCasterBoundsValid;
		if (!clearOnly) {
			const bool shadersReady =
				(!hasDirectionalShadow || shadowShaderReady) &&
				(!hasPointShadow || pointShadowShaderReady) &&
				(!hasSpotShadow || shadowShaderReady);
			if (!shadersReady) {
				targetsReady = false;
				++m_shadowStats.shadowResourceFailureCount;
			}
		}

		bool targetsChanged = false;
		auto ensureShadowTarget = [&](auto& light) {
			FBO* const previousTarget = light.shadowFBO;
			const unsigned int previousFramebuffer =
				previousTarget ? previousTarget->framebufferID : 0u;
			const unsigned int previousTexture =
				previousTarget && !previousTarget->textureIDs.empty()
					? previousTarget->textureIDs.front()
					: 0u;
			const int previousWidth =
				previousTarget ? previousTarget->width : 0;
			const int previousHeight =
				previousTarget ? previousTarget->height : 0;
			const std::uint64_t previousGeneration =
				previousTarget
					? previousTarget->GetResourceGeneration()
					: 0u;

			FBO* const target = light.EnsureShadowFBO();
			if (!target) {
				targetsReady = false;
				++m_shadowStats.shadowResourceFailureCount;
				return;
			}
			targetsChanged =
				targetsChanged ||
				previousTarget != target ||
				previousFramebuffer != target->framebufferID ||
				previousTexture !=
					(target->textureIDs.empty()
						? 0u
						: target->textureIDs.front()) ||
				previousWidth != target->width ||
				previousHeight != target->height ||
				previousGeneration !=
					target->GetResourceGeneration();
		};

		for (auto& light : lightSource.directionLights) {
			if (!light.GetActiveStatus() || !light.useShadowMap) continue;
			ensureShadowTarget(light);
		}
		for (auto& light : lightSource.pointLights) {
			if (!light.GetActiveStatus() || !light.useShadowMap) continue;
			ensureShadowTarget(light);
		}
		for (auto& light : lightSource.spotLights) {
			if (!light.GetActiveStatus() || !light.useShadowMap) continue;
			ensureShadowTarget(light);
		}

		const std::size_t currentSignature =
			forceUpdate
				? 0u
				: BuildShadowRevisionSignature(
					shadowShaderReady ? shadowShader->GetRevision() : 0u,
					pointShadowShaderReady
						? pointShadowShader->GetRevision()
						: 0u);
		if (!forceUpdate &&
			!targetsChanged &&
			targetsReady &&
			m_shadowCacheValid &&
			AreEnabledShadowMapsSampleable() &&
			currentSignature == m_shadowCacheSignature) {
			++m_shadowStats.cacheHitCount;
			return;
		}

		if (!clearOnly) {
			for (auto& light : lightSource.directionLights) {
				if (!light.GetActiveStatus() || !light.useShadowMap) continue;
				FitDirectionalShadowToCasterBounds(light);
			}
			for (auto& light : lightSource.pointLights) {
				if (!light.GetActiveStatus() || !light.useShadowMap) continue;
				light.FitShadowToBounds(
					m_cachedShadowCasterCenter,
					m_cachedShadowCasterRadius);
			}
			for (std::size_t index = 0;
				index < lightSource.spotLights.size();
				++index) {
				auto& light = lightSource.spotLights[index];
				if (!light.GetActiveStatus() || !light.useShadowMap) continue;
				FitSpotShadowToCasterBounds(
					light,
					index,
					m_cachedShadowCasterCenter,
					m_cachedShadowCasterRadius);
			}
		}

		signature =
			forceUpdate
				? 0u
				: BuildShadowRevisionSignature(
					shadowShaderReady ? shadowShader->GetRevision() : 0u,
					pointShadowShaderReady
						? pointShadowShader->GetRevision()
						: 0u);
	}

	++m_shadowStats.cacheMissCount;
	const auto updateStart = std::chrono::steady_clock::now();
	std::uint64_t renderedLightCount = 0;
	std::uint64_t trianglePassMultiplier = 0;
	DisableEnabledShadowContent();
	RenderShadowMapUpdate(
		renderedLightCount,
		trianglePassMultiplier,
		nullptr,
		clearOnly);

	const bool contentCommitted =
		targetsReady &&
		renderedLightCount == enabledLightCount &&
		CommitEnabledShadowContent();
	m_shadowCacheSignature = signature;
	m_shadowCacheValid = !forceUpdate && contentCommitted;
	if (!contentCommitted) {
		DisableEnabledShadowContent();
	}
	if (renderedLightCount > 0) {
		++m_shadowStats.updateCount;
	}
	m_shadowStats.updatedLightCount = renderedLightCount;
	UpdateShadowCasterStats(renderedLightCount, trianglePassMultiplier);
	m_shadowStats.lastUpdateCpuMilliseconds =
		std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - updateStart).count();
}

void Scene::DrawShadowMap() {
	PERF_CPU_SCOPE("Shadow Maps");
	SynchronizeSceneTopologyRevision();
	if (properties.SHADOW_CACHE_DISABLED) {
		if (!m_shadowCacheStrategyInitialized ||
			!m_shadowCacheDisabledLastFrame) {
			m_shadowCacheStrategyInitialized = true;
			m_shadowCacheUsedLegacySignature = false;
			m_shadowCacheDisabledLastFrame = true;
			m_shadowCacheGranularityInitialized = false;
			m_shadowCacheValid = false;
			InvalidatePerLightShadowCaches();
		}
		DrawShadowMapRevisionGlobal(true);
		return;
	}
	if (!properties.SHADOW_CACHE_USE_LEGACY_SIGNATURE) {
		DrawShadowMapRevision();
		return;
	}

	std::size_t signature = 0;
	std::uint64_t enabledLightCount = 0;
	bool targetsReady = true;
	bool clearOnly = false;
	{
		PERF_CPU_SCOPE("Shadow Cache Check");
		ShadowCacheCheckTelemetry telemetry{ m_shadowStats, true };
		if (!m_shadowCacheStrategyInitialized ||
			!m_shadowCacheUsedLegacySignature ||
			m_shadowCacheDisabledLastFrame) {
			m_shadowCacheStrategyInitialized = true;
			m_shadowCacheUsedLegacySignature = true;
			m_shadowCacheDisabledLastFrame = false;
			m_shadowCacheValid = false;
			InvalidatePerLightShadowCaches();
		}
		m_shadowCasterStatePrepared = false;

		auto& framebufferManager =
			FramebuffersManager::GetInstance();
		bool releasedShadowTarget = false;
		bool hasDirectionalShadow = false;
		bool hasPointShadow = false;
		bool hasSpotShadow = false;
		auto releaseIfDisabled = [&](auto& light) {
			const bool enabled =
				light.GetActiveStatus() && light.useShadowMap;
			if (!enabled) {
				light.shadowCache.Invalidate();
				if (light.shadowFBO) {
					framebufferManager.ReleaseFBO(light.shadowFBO);
					light.shadowFBO = nullptr;
					releasedShadowTarget = true;
				}
			}
			return enabled;
		};
		for (auto& light : lightSource.directionLights) {
			if (releaseIfDisabled(light)) {
				hasDirectionalShadow = true;
				++enabledLightCount;
			}
		}
		for (auto& light : lightSource.pointLights) {
			if (releaseIfDisabled(light)) {
				hasPointShadow = true;
				++enabledLightCount;
			}
			else {
				light.shadowFaceCache.Invalidate();
			}
		}
		for (auto& light : lightSource.spotLights) {
			if (releaseIfDisabled(light)) {
				hasSpotShadow = true;
				++enabledLightCount;
			}
		}
		if (releasedShadowTarget) {
			framebufferManager.TrimUnusedFBOs();
			m_shadowCacheValid = false;
		}
		if (enabledLightCount == 0) {
			m_shadowCacheValid = false;
			return;
		}

		glm::vec3 casterCenter(0.0f);
		float casterRadius = 0.0f;
		const bool hasActiveCasters = HasActiveShadowCasters();
		const bool casterBoundsValid =
			ComputeShadowCasterBounds(casterCenter, casterRadius);
		if (hasActiveCasters && !casterBoundsValid) {
			m_shadowCacheValid = false;
			DisableEnabledShadowContent();
			++m_shadowStats.cacheMissCount;
			++m_shadowStats.conservativeShadowFallbackCount;
			return;
		}
		clearOnly = !hasActiveCasters;

		const auto shadowShader =
			ShaderManager::GetInstance().GetShader(
				ShaderManager::Shadow);
		const auto shadowCubeShader =
			ShaderManager::GetInstance().GetShader(
				ShaderManager::ShadowCube);
		const auto shadowCubeFaceShader =
			ShaderManager::GetInstance().GetShader(
				ShaderManager::ShadowCubeFace);
		const auto pointShadowShader =
			ShouldUseSixFacePointShadow()
				? shadowCubeFaceShader
				: shadowCubeShader;
		const bool shadowShaderReady =
			IsShadowShaderReady(shadowShader);
		const bool pointShadowShaderReady =
			IsShadowShaderReady(pointShadowShader);
		if (!clearOnly) {
			const bool shadersReady =
				(!hasDirectionalShadow || shadowShaderReady) &&
				(!hasPointShadow || pointShadowShaderReady) &&
				(!hasSpotShadow || shadowShaderReady);
			if (!shadersReady) {
				targetsReady = false;
				++m_shadowStats.shadowResourceFailureCount;
			}
		}

		m_shadowCasterDrawListValid = false;
		for (auto& light : lightSource.directionLights) {
			if (!light.GetActiveStatus() || !light.useShadowMap) continue;
			if (!clearOnly) {
				FitDirectionalShadowToCasterBounds(light);
			}
			if (!light.EnsureShadowFBO()) {
				targetsReady = false;
				++m_shadowStats.shadowResourceFailureCount;
			}
		}
		for (auto& light : lightSource.pointLights) {
			if (!light.GetActiveStatus() || !light.useShadowMap) continue;
			if (!clearOnly) {
				light.FitShadowToBounds(casterCenter, casterRadius);
			}
			if (!light.EnsureShadowFBO()) {
				targetsReady = false;
				++m_shadowStats.shadowResourceFailureCount;
			}
		}
		// The legacy cache does not publish caster revisions. Rebuild the
		// projection-aware source list so moving geometry cannot reuse stale
		// bounds in this diagnostic path.
		for (std::size_t index = 0;
			index < lightSource.spotLights.size();
			++index) {
			auto& light = lightSource.spotLights[index];
			if (!light.GetActiveStatus() || !light.useShadowMap) continue;
			if (!clearOnly) {
				FitSpotShadowToCasterBounds(
					light,
					index,
					casterCenter,
					casterRadius);
			}
			if (!light.EnsureShadowFBO()) {
				targetsReady = false;
				++m_shadowStats.shadowResourceFailureCount;
			}
		}

		signature = BuildShadowCacheSignature();
		if (targetsReady &&
			m_shadowCacheValid &&
			AreEnabledShadowMapsSampleable() &&
			signature == m_shadowCacheSignature) {
			++m_shadowStats.cacheHitCount;
			return;
		}
	}

	++m_shadowStats.cacheMissCount;
	const auto updateStart = std::chrono::steady_clock::now();
	std::uint64_t renderedLightCount = 0;
	std::uint64_t trianglePassMultiplier = 0;
	DisableEnabledShadowContent();
	RenderShadowMapUpdate(
		renderedLightCount,
		trianglePassMultiplier,
		nullptr,
		clearOnly);

	m_shadowCacheSignature = signature;
	m_shadowCacheValid =
		targetsReady &&
		renderedLightCount == enabledLightCount &&
		CommitEnabledShadowContent();
	if (!m_shadowCacheValid) {
		DisableEnabledShadowContent();
	}
	if (renderedLightCount > 0) {
		++m_shadowStats.updateCount;
	}
	m_shadowStats.updatedLightCount = renderedLightCount;
	UpdateShadowCasterStats(renderedLightCount, trianglePassMultiplier);
	m_shadowStats.lastUpdateCpuMilliseconds =
		std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - updateStart).count();
}

FBO* Scene::GetNeedShowFramebuffer() {
    FBO* ret = fbo;
    if (FramebuffersManager::GetInstance().useType == FBO::Default_FrameRenderType) {
        if (fbo->attr.aaType == AntiAliasManager::MSAA) {
            FramebuffersManager::GetInstance().ReleaseFBO(fboTemp);
            FBOAttributes attr;
            attr.isBloom = properties.BLOOM;
            attr.isHDR = properties.USE_HDR;
            fboTemp = FramebuffersManager::GetInstance().GetFBO(attr);
            GLState::BindFramebuffer(GL_READ_FRAMEBUFFER, fbo->framebufferID);
            GLState::BindFramebuffer(GL_DRAW_FRAMEBUFFER, fboTemp->framebufferID);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            glBlitFramebuffer(0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT, 0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            if (fbo->attr.isBloom) {
                glReadBuffer(GL_COLOR_ATTACHMENT1);
                glDrawBuffer(GL_COLOR_ATTACHMENT1);
                glBlitFramebuffer(0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT, 0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            }
            if (fbo->attr.isBloom) {
                GLuint attachments[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
                glDrawBuffers(2, attachments);
            }
            else {
                glDrawBuffer(GL_COLOR_ATTACHMENT0);
            }
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            GLState::BindFramebuffer(GL_FRAMEBUFFER, 0);
            if (properties.BLOOM) {
                Blur(properties.BLOOM_BLUR_ITERATIONS, fboTemp);
            }
            ret = fboTemp;
        }
        else if (fbo->attr.isBloom) {
            Blur(properties.BLOOM_BLUR_ITERATIONS, fbo);
            ret = fbo;
        }
        else ret = fbo;
    }
    else if (FramebuffersManager::GetInstance().useType == FBO::ShadowMap_FrameRenderType) {
        if (lightSource.directionLights.empty()) {
            return nullptr;
        }
        auto& light = lightSource.directionLights[0];
        return light.shadowCache.IsSampleable(light.shadowFBO)
            ? light.shadowFBO
            : nullptr;
    }
    else if (FramebuffersManager::GetInstance().useType == FBO::BrightColor_FrameRenderType) {
        if (properties.BLOOM) {
            ret = fbo;
        }
        else {
            std::cout << "NO BLOOM USED" << std::endl;
            ret = fbo;
        }
    }
    return ret;
}

void Scene::ClearFBO() {
    FramebuffersManager::GetInstance().ReleaseFBO(fbo);
    FramebuffersManager::GetInstance().ReleaseFBO(fboTemp);
    FramebuffersManager::GetInstance().ReleaseFBO(deferFBO);
}

void Scene::ClearContent()
{
    auto& framebufferManager = FramebuffersManager::GetInstance();
    for (auto& light : lightSource.pointLights) {
        if (light.shadowFBO) {
            framebufferManager.ReleaseFBO(light.shadowFBO);
            light.shadowFBO = nullptr;
        }
    }
    for (auto& light : lightSource.directionLights) {
        if (light.shadowFBO) {
            framebufferManager.ReleaseFBO(light.shadowFBO);
            light.shadowFBO = nullptr;
        }
    }
    for (auto& light : lightSource.spotLights) {
        if (light.shadowFBO) {
            framebufferManager.ReleaseFBO(light.shadowFBO);
            light.shadowFBO = nullptr;
        }
    }

    SetSelectedModelForMaterials(nullptr);
    modelSource.ClearModels();
    lightSource.pointLights.clear();
    lightSource.directionLights.clear();
    lightSource.spotLights.clear();
    m_visibleModels.clear();
    m_opaqueMeshList.clear();
    m_transparentMeshList.clear();
    m_shadowCacheValid = false;
    m_shadowCacheSignature = 0;
    m_shadowCacheStrategyInitialized = false;
    m_shadowCacheUsedLegacySignature = false;
    m_shadowCacheGranularityInitialized = false;
    m_shadowCacheUsedPerLight = false;
    m_shadowCacheFeatureStateInitialized = false;
    m_shadowCacheUsedSpatialCasters = false;
    m_shadowCacheUsedPointFaces = false;
    m_shadowCasterStateInitialized = false;
    m_shadowCasterStatePrepared = false;
    m_shadowCasterStateReliable = true;
    m_shadowCasterBoundsValid = false;
    m_shadowCasterStateSignature = 0;
    m_shadowCasterRevision = 0;
    m_observedSceneTopologyRevision =
        modelSource.GetSceneTopologyRevision();
    m_cachedShadowCasterCenter = glm::vec3(0.0f);
    m_cachedShadowCasterRadius = 0.0f;
    m_shadowCasterBoundsScratch.clear();
    m_shadowCasterDrawList.clear();
    m_shadowCasterDrawListValid = false;
    m_shadowCasterDrawListRevision = 0;
    m_shadowCasterDrawListTriangleCount = 0;
    m_pendingShadowCasterCandidateCount = 0;
    m_pendingShadowCasterCulledCount = 0;
    m_pendingShadowCasterCullingLightCount = 0;
    m_pendingShadowCasterDrawCount = 0;
    m_pendingShadowCasterTriangleCount = 0;
    m_pendingUnculledRenderedLightCount = 0;
    m_pendingUnculledTrianglePassMultiplier = 0;
    m_pendingShadowDrawPassMultiplier = 0;
    m_shadowStats = {};
    m_shadowStats.sceneTopologyRevision =
        m_observedSceneTopologyRevision;
    m_shadowStats.sceneTopologyModelCount =
        modelSource.GetModels().size();
    framebufferManager.TrimUnusedFBOs();
}

void Scene::Blur(int times, FBO* fbo) {
    if (times <= 0) return;
    auto bulrShader = ShaderManager::GetInstance().GetShader(ShaderManager::Bulr);
    FBO* fbos[2];
    FBOAttributes attr;
    attr.isHDR = properties.USE_HDR;
    fbos[0] = FramebuffersManager::GetInstance().GetFBO(attr);
    fbos[1] = FramebuffersManager::GetInstance().GetFBO(attr);
    GLboolean horizontal = true, first_iteration = true;
    GLuint amount = times << 1;
    bulrShader->use();
    for (GLuint i = 0; i < amount; i++)
    {
        GLState::BindFramebuffer(GL_FRAMEBUFFER, fbos[i % 2]->framebufferID);
        bulrShader->setBool("horizontal", horizontal);
        GLState::ActiveTexture(GL_TEXTURE0);
        GLState::BindTexture(
            GL_TEXTURE_2D, first_iteration ? fbo->textureIDs[1] : fbos[(i + 1) % 2]->textureIDs[0]
        );
        bulrShader->setInt("image", 0);
        PerformanceProfiler::GetInstance().RecordDraw(GL_TRIANGLES, 6);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        horizontal = !horizontal;
        if (first_iteration)
            first_iteration = false;
    }
    GLState::BindFramebuffer(GL_READ_FRAMEBUFFER, fbos[1]->framebufferID);
    GLState::BindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo->framebufferID);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glDrawBuffer(GL_COLOR_ATTACHMENT1);
    glBlitFramebuffer(0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT, 0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    GLuint attachments[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    glDrawBuffers(2, attachments);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    GLState::BindFramebuffer(GL_FRAMEBUFFER, 0);
    FramebuffersManager::GetInstance().ReleaseFBO(fbos[0]);
    FramebuffersManager::GetInstance().ReleaseFBO(fbos[1]);
}

void Scene::SetSceneGui()
{
    if (ImGui::CollapsingHeader("Light Settings")) {
        if (ImGui::TreeNode("Direction Lights")) {
            for (size_t i = 0; i < lightSource.directionLights.size(); ++i) {
                std::string label = "Direction Light " + std::to_string(i);
                if (ImGui::TreeNode(label.c_str())) {
                    ImGui::Checkbox("Active", &lightSource.directionLights[i].m_active);
                    ImGui::ColorEdit3("Ambient", &lightSource.directionLights[i].ambient[0]);
                    ImGui::ColorEdit3("Diffuse", &lightSource.directionLights[i].diffuse[0]);
                    ImGui::ColorEdit3("Specular", &lightSource.directionLights[i].specular[0]);
                    ImGui::DragFloat3("Direction", &lightSource.directionLights[i].direction[0], 0.1f);
                    ImGui::Checkbox("useShadow", &lightSource.directionLights[i].useShadowMap);
                    ImGui::Checkbox("Auto Fit Shadow", &lightSource.directionLights[i].autoFitShadow);
                    ImGui::DragInt("Shadow Resolution", &lightSource.directionLights[i].shadowResolution, 16.0f, 256, 4096);
                    if (!lightSource.directionLights[i].autoFitShadow) {
                        ImGui::DragFloat3("Shadow Center", &lightSource.directionLights[i].shadowCenter[0], 0.1f);
                        ImGui::DragFloat("distance", &lightSource.directionLights[i].distance, 0.1f, 0.1f, 200.0f);
                        ImGui::DragFloat("nearPlane", &lightSource.directionLights[i].near_plane, 0.1f, 0.01f, 100.0f);
                        ImGui::DragFloat("farPlane", &lightSource.directionLights[i].far_plane, 0.1f, 0.1f, 400.0f);
                        ImGui::DragFloat("shadowWidth", &lightSource.directionLights[i].width, 0.1f, 0.1f, 200.f);
                    }
                    ImGui::TreePop();
                }
            }
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Point Lights")) {
            for (size_t i = 0; i < lightSource.pointLights.size(); ++i) {
                std::string label = "Point Light " + std::to_string(i);
                if (ImGui::TreeNode(label.c_str())) {
                    ImGui::Checkbox("Active", &lightSource.pointLights[i].m_active);
                    ImGui::ColorEdit3("Ambient", &lightSource.pointLights[i].ambient[0]);
                    ImGui::ColorEdit3("Diffuse", &lightSource.pointLights[i].diffuse[0]);
                    ImGui::ColorEdit3("Specular", &lightSource.pointLights[i].specular[0]);
                    ImGui::DragFloat3("Position", &lightSource.pointLights[i].position[0], 0.1f);
                    ImGui::DragFloat3("Scale", &lightSource.pointLights[i].scale[0], 0.01f, 0.01f, 100.0f);
                    ImGui::DragFloat("Constant", &lightSource.pointLights[i].constant, 0.01f, 0.0f, 10.0f);
                    ImGui::DragFloat("Linear", &lightSource.pointLights[i].linear, 0.001f, 0.0f, 1.0f);
                    ImGui::DragFloat("Quadratic", &lightSource.pointLights[i].quadratic, 0.0001f, 0.0f, 1.0f);
                    ImGui::Checkbox("useShadow", &lightSource.pointLights[i].useShadowMap);
                    ImGui::Checkbox("Auto Fit Shadow", &lightSource.pointLights[i].autoFitShadow);
                    ImGui::DragInt("Shadow Resolution", &lightSource.pointLights[i].shadowResolution, 16.0f, 256, 4096);
                    if (!lightSource.pointLights[i].autoFitShadow) {
                        ImGui::DragFloat("nearPlane", &lightSource.pointLights[i].near, 0.1f, 0.01f, 10.0f);
                        ImGui::DragFloat("farPlane", &lightSource.pointLights[i].far, 0.1f, 0.1f, 400.0f);
                    }
                    ImGui::TreePop();
                }
            }
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Spot Lights")) {
            for (size_t i = 0; i < lightSource.spotLights.size(); ++i) {
                std::string label = "Spot Light " + std::to_string(i);
                if (ImGui::TreeNode(label.c_str())) {
                    auto& light = lightSource.spotLights[i];
                    ImGui::Checkbox("Active", &light.m_active);
                    ImGui::ColorEdit3("Ambient", &light.ambient[0]);
                    ImGui::ColorEdit3("Diffuse", &light.diffuse[0]);
                    ImGui::ColorEdit3("Specular", &light.specular[0]);
                    ImGui::DragFloat3("Position", &light.position[0], 0.1f);
                    ImGui::DragFloat3("Direction", &light.direction[0], 0.1f);
                    ImGui::DragFloat("Inner Cutoff", &light.cutOff, 0.1f, 0.1f, 89.0f);
                    ImGui::DragFloat("Outer Cutoff", &light.outerCutOff, 0.1f, 0.1f, 89.0f);
                    ImGui::Checkbox("useShadow", &light.useShadowMap);
                    ImGui::Checkbox("Auto Fit Shadow", &light.autoFitShadow);
                    ImGui::DragInt("Shadow Resolution", &light.shadowResolution, 16.0f, 256, 4096);
                    if (!light.autoFitShadow) {
                        ImGui::DragFloat("nearPlane", &light.near_plane, 0.1f, 0.01f, 10.0f);
                        ImGui::DragFloat("farPlane", &light.far_plane, 0.1f, 0.1f, 400.0f);
                    }
                    ImGui::TreePop();
                }
            }
            ImGui::TreePop();
        }
    }

	const char* options[] = { "Phong", "PBR", "Mirror", "Explode" };
    int optionCount = sizeof(options) / sizeof(options[0]);

    if (ImGui::CollapsingHeader("Model Settings")) {
        if (ImGui::TreeNode("Models")) {
            for (const auto& model : modelSource.GetModels()) {
                std::string label = "Model: " + model->GetName();
                if (ImGui::TreeNode(label.c_str())) {
                    if (ImGui::Button("View Materials")) {
                        SetSelectedModelForMaterials(model.get());
                    }
                    ImGui::SameLine();
                    ImGui::Checkbox("Active", &model->m_active);
                    ImGui::DragFloat3("Position", &model->position[0], 0.1f);
                    ImGui::DragFloat3("Rotation", &model->rotation[0], 0.5f);
                    ImGui::DragFloat3("Scale", &model->scale[0], 0.01f, 0.01f, 10.0f);
                    if (ImGui::TreeNode("Other Shader Use")) {
                        for (auto& [key, value] : model->otherShaderUse)
                        {
                            ImGui::Checkbox(OtherShader::OtherShaderTypeToString(static_cast<OtherShaderType>(key)).c_str(), &value);
                        }
                        ImGui::TreePop();
                    }
                    ImGui::DragFloat("Outline Width", &model->outlineWidth, 0.01f, 0.0f, 0.5f);
                    ImGui::ColorEdit3("Outline Color", &model->outlineColor[0]);
                    ImGui::DragFloat("NormalLine Width", &OtherShader::normalLineMagnitude, 0.01f, 0.0f, 0.4f);

					auto shaderPtr = model->GetShader();
					int curShaderIdx = shaderPtr ? ShaderManager::GetInstance().GetShaderIndexByShader(shaderPtr) : -1;
					int selectedOption = 0;
					if (curShaderIdx == ShaderManager::Pbr) selectedOption = 1;
					else if (curShaderIdx == ShaderManager::Mirror) selectedOption = 2;
					else if (curShaderIdx == ShaderManager::Explode) selectedOption = 3;
					if (ImGui::Combo("Shader Type", &selectedOption, options, optionCount)) {
                        switch (selectedOption) {
                        case 0:
                            if (curShaderIdx != ShaderManager::Phong) model->SetShader(ShaderManager::GetInstance().GetShader(ShaderManager::Phong));
                            break;
						case 1:
							if (curShaderIdx != ShaderManager::Pbr) model->SetShader(ShaderManager::GetInstance().GetShader(ShaderManager::Pbr));
							break;
						case 2:
							if (curShaderIdx != ShaderManager::Mirror) model->SetShader(ShaderManager::GetInstance().GetShader(ShaderManager::Mirror));
							break;
						case 3:
                            if (curShaderIdx != ShaderManager::Explode) model->SetShader(ShaderManager::GetInstance().GetShader(ShaderManager::Explode));
                            break;
                        }
                    }
                    ImGui::TreePop();
                }
            }
            ImGui::TreePop();
        }

    }
}
