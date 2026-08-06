#include "PointLightGridRuntime.h"

#include "Global.h"
#include "GLStateCache.h"
#include "Profiler.h"
#include "Scene.h"
#include "Shader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
	constexpr int kTileSize = 16;

	template <typename T>
	void HashValue(std::uint64_t& hash, const T& value)
	{
		const unsigned char* bytes =
			reinterpret_cast<const unsigned char*>(&value);
		for (std::size_t index = 0; index < sizeof(T); ++index) {
			hash ^= bytes[index];
			hash *= 1099511628211ull;
		}
	}

	void HashBytes(std::uint64_t& hash, const void* data, std::size_t size)
	{
		const unsigned char* bytes = static_cast<const unsigned char*>(data);
		for (std::size_t index = 0; index < size; ++index) {
			hash ^= bytes[index];
			hash *= 1099511628211ull;
		}
	}

	bool FiniteMatrix(const glm::mat4& matrix)
	{
		for (int column = 0; column < 4; ++column) {
			for (int row = 0; row < 4; ++row) {
				if (!std::isfinite(matrix[column][row])) return false;
			}
		}
		return true;
	}

	struct PixelRect {
		int x0 = 0;
		int y0 = 0;
		int x1 = 0;
		int y1 = 0;
		bool outside = false;
	};

	PixelRect ConservativeSphereRect(
		const glm::vec3& worldCenter,
		float radius,
		const glm::mat4& view,
		const glm::mat4& projection,
		int width,
		int height)
	{
		PixelRect result{ 0, 0, width, height, false };
		if (!std::isfinite(radius) || radius <= 0.0f ||
			!FiniteMatrix(view) || !FiniteMatrix(projection) ||
			!std::isfinite(worldCenter.x) ||
			!std::isfinite(worldCenter.y) ||
			!std::isfinite(worldCenter.z)) {
			return result;
		}

		const glm::mat4 clipFromWorld = projection * view;
		const glm::mat4 rows = glm::transpose(clipFromWorld);
		const std::array<glm::vec4, 6> planes = {
			rows[3] + rows[0], rows[3] - rows[0],
			rows[3] + rows[1], rows[3] - rows[1],
			rows[3] + rows[2], rows[3] - rows[2]
		};
		for (const glm::vec4& plane : planes) {
			const float normalLength = glm::length(glm::vec3(plane));
			if (!std::isfinite(normalLength) || normalLength <= 1e-7f) {
				return result;
			}
			const float signedDistance = glm::dot(
				plane,
				glm::vec4(worldCenter, 1.0f));
			if (signedDistance < -radius * normalLength) {
				result = {};
				result.outside = true;
				return result;
			}
		}

		const glm::vec3 center = glm::vec3(view * glm::vec4(worldCenter, 1.0f));
		const double depth = -static_cast<double>(center.z);
		if (!std::isfinite(depth) || depth - radius <= 0.1) return result;
		const double r = static_cast<double>(radius);
		const double denominator = depth * depth - r * r;
		if (!std::isfinite(denominator) || denominator <= 1e-12) return result;

		auto tangent = [&](double axisCenter, float projectionScale,
			double& minimum, double& maximum) {
			const double rootTerm =
				axisCenter * axisCenter + depth * depth - r * r;
			if (!std::isfinite(rootTerm) || rootTerm < 0.0) return false;
			const double root = std::sqrt((std::max)(0.0, rootTerm));
			const double a = static_cast<double>(projectionScale) *
				(axisCenter * depth - r * root) / denominator;
			const double b = static_cast<double>(projectionScale) *
				(axisCenter * depth + r * root) / denominator;
			if (!std::isfinite(a) || !std::isfinite(b)) return false;
			minimum = (std::min)(a, b);
			maximum = (std::max)(a, b);
			return true;
		};

		double minX = 0.0, maxX = 0.0, minY = 0.0, maxY = 0.0;
		if (!tangent(center.x, projection[0][0], minX, maxX) ||
			!tangent(center.y, projection[1][1], minY, maxY)) {
			return result;
		}
		if (maxX < -1.0 || minX > 1.0 || maxY < -1.0 || minY > 1.0) {
			result = {};
			result.outside = true;
			return result;
		}

		minX = (std::max)(-1.0, minX);
		maxX = (std::min)(1.0, maxX);
		minY = (std::max)(-1.0, minY);
		maxY = (std::min)(1.0, maxY);
		constexpr int guard = 1;
		result.x0 = (std::max)(0, (std::min)(width,
			static_cast<int>(std::floor((minX * 0.5 + 0.5) * width)) - guard));
		result.x1 = (std::max)(0, (std::min)(width,
			static_cast<int>(std::ceil((maxX * 0.5 + 0.5) * width)) + guard));
		result.y0 = (std::max)(0, (std::min)(height,
			static_cast<int>(std::floor((minY * 0.5 + 0.5) * height)) - guard));
		result.y1 = (std::max)(0, (std::min)(height,
			static_cast<int>(std::ceil((maxY * 0.5 + 0.5) * height)) + guard));
		if (result.x1 <= result.x0 || result.y1 <= result.y0) {
			result = {};
			result.outside = true;
		}
		return result;
	}

	int DepthSlice(
		float depth,
		float nearPlane,
		float farPlane,
		int sliceCount)
	{
		if (sliceCount <= 1) return 0;
		const float clamped = (std::max)(nearPlane, (std::min)(farPlane, depth));
		const float normalized =
			std::log(clamped / nearPlane) / std::log(farPlane / nearPlane);
		return (std::max)(0, (std::min)(sliceCount - 1,
			static_cast<int>(std::floor(normalized * sliceCount))));
	}
}

bool PointLightGridRuntime::Prepare(
	Scene& scene,
	const glm::mat4& view,
	const glm::mat4& projection,
	int viewportWidth,
	int viewportHeight,
	int sliceCount,
	bool forceRebuild,
	float nearPlane,
	float farPlane)
{
	viewportWidth = (std::max)(1, viewportWidth);
	viewportHeight = (std::max)(1, viewportHeight);
	if (sliceCount < 1 || sliceCount > 16) {
		m_stats = {};
		m_stats.overflow = true;
		m_stats.error = "point-light grid slice count must be in [1,16]";
		return false;
	}
	std::uint64_t signature = 1469598103934665603ull;
	{
	PERF_CPU_SCOPE("Point Light Grid Cache Check");
	HashValue(signature, viewportWidth);
	HashValue(signature, viewportHeight);
	HashValue(signature, sliceCount);
	HashValue(signature, nearPlane);
	HashValue(signature, farPlane);
	HashValue(
		signature,
		SystemProperties::GetInstance().LIGHT_VOLUME_CUTOFF_SCALE);
	HashValue(
		signature,
		SystemProperties::GetInstance().LIGHT_VOLUME_RADIUS_SCALE);
	HashValue(
		signature,
		SystemProperties::GetInstance().POINT_LIGHT_OFFSCREEN_CULLING);
	HashBytes(signature, &view[0][0], sizeof(glm::mat4));
	HashBytes(signature, &projection[0][0], sizeof(glm::mat4));
	const std::uint64_t sourceCount = scene.GetLightSource().pointLights.size();
	HashValue(signature, sourceCount);
	for (const PointLight& light : scene.GetLightSource().pointLights) {
		HashValue(signature, light.m_active);
		HashValue(signature, light.position);
		HashValue(signature, light.constant);
		HashValue(signature, light.linear);
		HashValue(signature, light.quadratic);
		HashValue(signature, light.ambient);
		HashValue(signature, light.diffuse);
		HashValue(signature, light.specular);
		HashValue(signature, light.useShadowMap);
	}
	}

	m_stats.rebuiltThisFrame = false;
	m_stats.cacheHit = false;
	m_stats.uploadedBytesThisFrame = 0;
	m_stats.inputSignature = signature;
	const bool rebuild = forceRebuild || !m_hasCachedInput ||
		signature != m_cachedInputSignature;

	{
		PERF_CPU_SCOPE("Point Light Grid Build");
		if (rebuild && !Build(
			scene, view, projection, viewportWidth, viewportHeight,
			sliceCount, nearPlane, farPlane)) {
			return false;
		}
	}
	{
		PERF_CPU_SCOPE("Point Light Grid Upload");
		if (rebuild && !Upload()) return false;
	}
	if (rebuild) {
		m_cachedInputSignature = signature;
		m_hasCachedInput = true;
		m_stats.rebuiltThisFrame = true;
		++m_stats.buildCount;
		++m_stats.uploadCount;
	}
	else {
		m_stats.cacheHit = true;
		++m_stats.cacheHitCount;
	}
	m_stats.inputSignature = signature;
	return m_stats.valid;
}

bool PointLightGridRuntime::Build(
	Scene& scene,
	const glm::mat4& view,
	const glm::mat4& projection,
	int viewportWidth,
	int viewportHeight,
	int sliceCount,
	float nearPlane,
	float farPlane)
{
	const std::uint64_t previousBuildCount = m_stats.buildCount;
	const std::uint64_t previousUploadCount = m_stats.uploadCount;
	const std::uint64_t previousCacheHitCount = m_stats.cacheHitCount;
	m_stats = {};
	m_stats.buildCount = previousBuildCount;
	m_stats.uploadCount = previousUploadCount;
	m_stats.cacheHitCount = previousCacheHitCount;
	m_stats.clustered = sliceCount > 1;
	m_stats.tileSize = kTileSize;
	m_stats.sliceCount = sliceCount;
	m_stats.tilesX = (viewportWidth + kTileSize - 1) / kTileSize;
	m_stats.tilesY = (viewportHeight + kTileSize - 1) / kTileSize;
	glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &m_stats.maxTextureBufferTexels);

	const std::uint64_t cells64 =
		static_cast<std::uint64_t>(m_stats.tilesX) *
		static_cast<std::uint64_t>(m_stats.tilesY) *
		static_cast<std::uint64_t>(m_stats.sliceCount);
	if (cells64 == 0 || cells64 > (std::numeric_limits<std::uint32_t>::max)() ||
		cells64 > static_cast<std::uint64_t>(m_stats.maxTextureBufferTexels)) {
		m_stats.overflow = true;
		m_stats.error = "point-light grid metadata exceeds texture-buffer capacity";
		return false;
	}
	m_stats.logicalCells = cells64;
	const std::size_t cellCount = static_cast<std::size_t>(cells64);

	m_spans.clear();
	m_spanTiles.clear();
	m_lightTexels.clear();
	m_spans.reserve(scene.GetLightSource().pointLights.size());
	m_spanTiles.reserve(scene.GetLightSource().pointLights.size() * 64u);
	m_lightTexels.reserve(scene.GetLightSource().pointLights.size() * 4u);
	std::vector<std::uint32_t> counts(cellCount, 0u);

	std::uint32_t packedLightIndex = 0;
	{
		PERF_CPU_SCOPE("Point Light Grid Bounds");
	for (const PointLight& light : scene.GetLightSource().pointLights) {
		if (!light.m_active) continue;
		if (light.useShadowMap) {
			m_stats.error =
				"Tile/Cluster benchmark runtime requires unshadowed point lights";
			return false;
		}
		const float radius = ComputePointLightStencilVolumeRadius(
			light.constant,
			light.linear,
			light.quadratic,
			light.diffuse,
			SystemProperties::GetInstance().LIGHT_VOLUME_CUTOFF_SCALE,
			SystemProperties::GetInstance().LIGHT_VOLUME_RADIUS_SCALE);
		if (!std::isfinite(radius) || radius <= 0.0f) {
			m_stats.error = "invalid point-light effective radius";
			return false;
		}

		m_lightTexels.push_back(glm::vec4(light.position, radius * radius));
		m_lightTexels.push_back(glm::vec4(light.ambient, light.constant));
		m_lightTexels.push_back(glm::vec4(light.diffuse, light.linear));
		m_lightTexels.push_back(glm::vec4(light.specular, light.quadratic));

		constexpr float membershipEpsilon = 1.0e-6f;
		const float guardedRadius = radius + membershipEpsilon;
		const glm::vec3 viewCenter = glm::vec3(
			view * glm::vec4(light.position, 1.0f));
		const float centerDepth = -viewCenter.z;
		if (!std::isfinite(centerDepth) ||
			centerDepth + guardedRadius < nearPlane ||
			centerDepth - guardedRadius > farPlane) {
			++packedLightIndex;
			continue;
		}
		PixelRect rect = ConservativeSphereRect(
			light.position, guardedRadius, view, projection,
			viewportWidth, viewportHeight);
		if (rect.outside) {
			// Match the frozen offscreen-culling switch. Formal Tile/Cluster
			// boundary runs keep it off, so an outside sphere still goes through
			// the same four tile-side-plane test used by Phase A. This preserves
			// identical membership/CSR inputs across the offline and runtime
			// experiments instead of silently adding a second optimization.
			if (SystemProperties::GetInstance().POINT_LIGHT_OFFSCREEN_CULLING) {
				++packedLightIndex;
				continue;
			}
			rect = { 0, 0, viewportWidth, viewportHeight, false };
		}

		LightSpan span;
		span.sourceIndex = packedLightIndex;
		const int minTileX = (std::max)(0, rect.x0 / kTileSize);
		const int maxTileX = (std::min)(m_stats.tilesX - 1,
			(std::max)(rect.x0, rect.x1 - 1) / kTileSize);
		const int minTileY = (std::max)(0, rect.y0 / kTileSize);
		const int maxTileY = (std::min)(m_stats.tilesY - 1,
			(std::max)(rect.y0, rect.y1 - 1) / kTileSize);
		span.firstTile = static_cast<std::uint32_t>(m_spanTiles.size());
		for (int tileY = minTileY; tileY <= maxTileY; ++tileY) {
			const int y0 = tileY * kTileSize;
			const int y1 = (std::min)(viewportHeight, y0 + kTileSize);
			const float ndcY0 =
				2.0f * static_cast<float>(y0) / viewportHeight - 1.0f;
			const float ndcY1 =
				2.0f * static_cast<float>(y1) / viewportHeight - 1.0f;
			const float c = ndcY0 / projection[1][1];
			const float d = ndcY1 / projection[1][1];
			for (int tileX = minTileX; tileX <= maxTileX; ++tileX) {
				const int x0 = tileX * kTileSize;
				const int x1 = (std::min)(viewportWidth, x0 + kTileSize);
				const float ndcX0 =
					2.0f * static_cast<float>(x0) / viewportWidth - 1.0f;
				const float ndcX1 =
					2.0f * static_cast<float>(x1) / viewportWidth - 1.0f;
				const float a = ndcX0 / projection[0][0];
				const float b = ndcX1 / projection[0][0];
				const std::array<glm::vec3, 4> normals = {
					glm::normalize(glm::vec3(1.0f, 0.0f, a)),
					glm::normalize(glm::vec3(-1.0f, 0.0f, -b)),
					glm::normalize(glm::vec3(0.0f, 1.0f, c)),
					glm::normalize(glm::vec3(0.0f, -1.0f, -d))
				};
				bool intersects = true;
				for (const glm::vec3& normal : normals) {
					if (glm::dot(normal, viewCenter) < -guardedRadius) {
						intersects = false;
						break;
					}
				}
				if (intersects) {
					m_spanTiles.push_back(static_cast<std::uint32_t>(
						tileY * m_stats.tilesX + tileX));
				}
			}
		}
		span.tileCount = static_cast<std::uint32_t>(
			m_spanTiles.size() - span.firstTile);
		span.minSlice = 0;
		span.maxSlice = m_stats.sliceCount - 1;
		if (sliceCount > 1) {
			const float minimumDepth = centerDepth - guardedRadius;
			const float maximumDepth = centerDepth + guardedRadius;
			if (!std::isfinite(centerDepth) || maximumDepth < nearPlane ||
				minimumDepth > farPlane) {
				++packedLightIndex;
				continue;
			}
			span.minSlice = DepthSlice(
				(std::max)(nearPlane, minimumDepth), nearPlane, farPlane,
				sliceCount);
			span.maxSlice = DepthSlice(
				(std::min)(farPlane, maximumDepth), nearPlane, farPlane,
				sliceCount);
		}
		m_spans.push_back(span);

		++packedLightIndex;
	}
	}
	m_stats.lightCount = packedLightIndex;
	if (m_lightTexels.size() >
		static_cast<std::size_t>(m_stats.maxTextureBufferTexels)) {
		m_stats.overflow = true;
		m_stats.error = "point-light payload exceeds texture-buffer capacity";
		return false;
	}

	{
		PERF_CPU_SCOPE("Point Light Grid Count");
		for (const LightSpan& span : m_spans) {
			for (int slice = span.minSlice; slice <= span.maxSlice; ++slice) {
				const std::size_t sliceBase =
					static_cast<std::size_t>(slice) *
					m_stats.tilesX * m_stats.tilesY;
				for (std::uint32_t tileOrdinal = 0;
					tileOrdinal < span.tileCount; ++tileOrdinal) {
					const std::uint32_t tile =
						m_spanTiles[span.firstTile + tileOrdinal];
					std::uint32_t& count = counts[sliceBase + tile];
					if (count == (std::numeric_limits<std::uint32_t>::max)()) {
						m_stats.overflow = true;
						m_stats.error = "point-light grid cell count overflow";
						return false;
					}
					++count;
				}
			}
		}
	}

	std::uint64_t totalIndices64 = 0;
	{
		PERF_CPU_SCOPE("Point Light Grid Prefix");
		m_metadata.assign(cellCount, glm::uvec2(0u));
	for (std::size_t cell = 0; cell < cellCount; ++cell) {
		m_metadata[cell].x = static_cast<std::uint32_t>(totalIndices64);
		m_metadata[cell].y = counts[cell];
		totalIndices64 += counts[cell];
		if (counts[cell] > 0) ++m_stats.nonEmptyCells;
		m_stats.maximumLightsPerCell =
			(std::max)(m_stats.maximumLightsPerCell,
				static_cast<std::uint64_t>(counts[cell]));
		if (totalIndices64 > (std::numeric_limits<std::uint32_t>::max)() ||
			totalIndices64 >
				static_cast<std::uint64_t>(m_stats.maxTextureBufferTexels)) {
			m_stats.overflow = true;
			m_stats.error = "point-light grid index buffer exceeds capacity";
			return false;
		}
	}
	}
	m_stats.totalIndices = totalIndices64;
	m_stats.averageLightsPerCell = cellCount > 0
		? static_cast<double>(totalIndices64) / static_cast<double>(cellCount)
		: 0.0;
	{
		PERF_CPU_SCOPE("Point Light Grid Fill");
		m_indices.assign(static_cast<std::size_t>(totalIndices64), 0u);
		std::vector<std::uint32_t> cursors(cellCount);
		for (std::size_t cell = 0; cell < cellCount; ++cell) {
			cursors[cell] = m_metadata[cell].x;
		}
	for (const LightSpan& span : m_spans) {
		for (int slice = span.minSlice; slice <= span.maxSlice; ++slice) {
			const std::size_t sliceBase =
				static_cast<std::size_t>(slice) *
				m_stats.tilesX * m_stats.tilesY;
			for (std::uint32_t tileOrdinal = 0;
				tileOrdinal < span.tileCount; ++tileOrdinal) {
				const std::size_t cell = sliceBase +
					m_spanTiles[span.firstTile + tileOrdinal];
				m_indices[cursors[cell]++] = span.sourceIndex;
			}
		}
	}
	}

	m_stats.metadataBytes = m_metadata.size() * sizeof(glm::uvec2);
	m_stats.indexBytes = m_indices.size() * sizeof(std::uint32_t);
	m_stats.lightBytes = m_lightTexels.size() * sizeof(glm::vec4);
	m_stats.residentBytes = m_stats.metadataBytes + m_stats.indexBytes +
		m_stats.lightBytes;
	std::uint64_t csrHash = 1469598103934665603ull;
	if (!m_metadata.empty()) {
		HashBytes(csrHash, m_metadata.data(), m_stats.metadataBytes);
	}
	if (!m_indices.empty()) {
		HashBytes(csrHash, m_indices.data(), m_stats.indexBytes);
	}
	m_stats.csrSignature = csrHash;
	m_stats.valid = true;
	return true;
}

void PointLightGridRuntime::EnsureObjects()
{
	if (m_metadataBuffer == 0) glGenBuffers(1, &m_metadataBuffer);
	if (m_metadataTexture == 0) glGenTextures(1, &m_metadataTexture);
	if (m_indexBuffer == 0) glGenBuffers(1, &m_indexBuffer);
	if (m_indexTexture == 0) glGenTextures(1, &m_indexTexture);
	if (m_lightBuffer == 0) glGenBuffers(1, &m_lightBuffer);
	if (m_lightTexture == 0) glGenTextures(1, &m_lightTexture);
}

bool PointLightGridRuntime::Upload()
{
	if (!m_stats.valid) return false;
	EnsureObjects();

	glBindBuffer(GL_TEXTURE_BUFFER, m_metadataBuffer);
	glBufferData(
		GL_TEXTURE_BUFFER,
		static_cast<GLsizeiptr>(m_stats.metadataBytes),
		m_metadata.empty() ? nullptr : m_metadata.data(),
		GL_DYNAMIC_DRAW);
	glBindTexture(GL_TEXTURE_BUFFER, m_metadataTexture);
	glTexBuffer(GL_TEXTURE_BUFFER, GL_RG32UI, m_metadataBuffer);

	// A zero-candidate or zero-light case never fetches these buffers, but a
	// one-element backing store keeps the texture-buffer object complete.
	const std::uint32_t dummyIndex = 0;
	glBindBuffer(GL_TEXTURE_BUFFER, m_indexBuffer);
	glBufferData(
		GL_TEXTURE_BUFFER,
		static_cast<GLsizeiptr>(m_indices.empty()
			? sizeof(dummyIndex)
			: m_stats.indexBytes),
		m_indices.empty() ? &dummyIndex : m_indices.data(),
		GL_DYNAMIC_DRAW);
	glBindTexture(GL_TEXTURE_BUFFER, m_indexTexture);
	glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, m_indexBuffer);

	const glm::vec4 dummyLight(0.0f);
	glBindBuffer(GL_TEXTURE_BUFFER, m_lightBuffer);
	glBufferData(
		GL_TEXTURE_BUFFER,
		static_cast<GLsizeiptr>(m_lightTexels.empty()
			? sizeof(dummyLight)
			: m_stats.lightBytes),
		m_lightTexels.empty() ? &dummyLight : m_lightTexels.data(),
		GL_DYNAMIC_DRAW);
	glBindTexture(GL_TEXTURE_BUFFER, m_lightTexture);
	glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, m_lightBuffer);
	glBindBuffer(GL_TEXTURE_BUFFER, 0);
	glBindTexture(GL_TEXTURE_BUFFER, 0);
	m_stats.uploadedBytesThisFrame = m_stats.residentBytes;
	return true;
}

void PointLightGridRuntime::Bind(
	Shader& shader,
	unsigned int firstTextureUnit) const
{
	GLState::ActiveTexture(GL_TEXTURE0 + firstTextureUnit);
	GLState::BindTexture(GL_TEXTURE_BUFFER, m_metadataTexture);
	shader.setInt("gridMetadata", static_cast<int>(firstTextureUnit));
	GLState::ActiveTexture(GL_TEXTURE0 + firstTextureUnit + 1u);
	GLState::BindTexture(GL_TEXTURE_BUFFER, m_indexTexture);
	shader.setInt("gridIndices", static_cast<int>(firstTextureUnit + 1u));
	GLState::ActiveTexture(GL_TEXTURE0 + firstTextureUnit + 2u);
	GLState::BindTexture(GL_TEXTURE_BUFFER, m_lightTexture);
	shader.setInt("gridLights", static_cast<int>(firstTextureUnit + 2u));
	shader.setInt("gridMode", m_stats.sliceCount > 1 ? 1 : 0);
	shader.setInt("gridTilesX", m_stats.tilesX);
	shader.setInt("gridTilesY", m_stats.tilesY);
	shader.setInt("gridSliceCount", m_stats.sliceCount);
}

void PointLightGridRuntime::Destroy()
{
	if (m_metadataTexture) glDeleteTextures(1, &m_metadataTexture);
	if (m_indexTexture) glDeleteTextures(1, &m_indexTexture);
	if (m_lightTexture) glDeleteTextures(1, &m_lightTexture);
	if (m_metadataBuffer) glDeleteBuffers(1, &m_metadataBuffer);
	if (m_indexBuffer) glDeleteBuffers(1, &m_indexBuffer);
	if (m_lightBuffer) glDeleteBuffers(1, &m_lightBuffer);
	m_metadataTexture = m_indexTexture = m_lightTexture = 0;
	m_metadataBuffer = m_indexBuffer = m_lightBuffer = 0;
	m_metadata.clear();
	m_indices.clear();
	m_lightTexels.clear();
	m_spans.clear();
	m_spanTiles.clear();
	m_hasCachedInput = false;
	m_stats = {};
}
