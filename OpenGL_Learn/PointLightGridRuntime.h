#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glad/glad.h>
#include <glm/glm.hpp>

class Scene;
class Shader;

struct PointLightGridRuntimeStats {
	bool valid = false;
	bool clustered = false;
	bool rebuiltThisFrame = false;
	bool cacheHit = false;
	bool overflow = false;
	int tileSize = 16;
	int sliceCount = 1;
	int tilesX = 0;
	int tilesY = 0;
	std::uint64_t logicalCells = 0;
	std::uint64_t nonEmptyCells = 0;
	std::uint64_t lightCount = 0;
	std::uint64_t totalIndices = 0;
	std::uint64_t maximumLightsPerCell = 0;
	double averageLightsPerCell = 0.0;
	std::uint64_t metadataBytes = 0;
	std::uint64_t indexBytes = 0;
	std::uint64_t lightBytes = 0;
	std::uint64_t residentBytes = 0;
	std::uint64_t uploadedBytesThisFrame = 0;
	std::uint64_t buildCount = 0;
	std::uint64_t uploadCount = 0;
	std::uint64_t cacheHitCount = 0;
	std::uint64_t inputSignature = 0;
	std::uint64_t csrSignature = 0;
	GLint maxTextureBufferTexels = 0;
	std::string error;
};

// OpenGL 3.3 point-light grid shared by the Tile16 and Cluster16 paths.
// Both variants use the same CSR builder, TBO layout, upload path, light
// payload, and fullscreen shader. The only experimental variable is whether
// the XY cell is additionally split into logarithmic view-depth slices.
class PointLightGridRuntime {
public:
	bool Prepare(
		Scene& scene,
		const glm::mat4& view,
		const glm::mat4& projection,
		int viewportWidth,
		int viewportHeight,
		int sliceCount,
		bool forceRebuild,
		float nearPlane,
		float farPlane);

	void Bind(Shader& shader, unsigned int firstTextureUnit) const;
	void Destroy();

	const PointLightGridRuntimeStats& GetStats() const { return m_stats; }
	bool HasLights() const { return m_stats.valid && m_stats.lightCount > 0; }

private:
	struct LightSpan {
		std::uint32_t sourceIndex = 0;
		std::uint32_t firstTile = 0;
		std::uint32_t tileCount = 0;
		int minSlice = 0;
		int maxSlice = 0;
	};

	bool Build(
		Scene& scene,
		const glm::mat4& view,
		const glm::mat4& projection,
		int viewportWidth,
		int viewportHeight,
		int sliceCount,
		float nearPlane,
		float farPlane);
	bool Upload();
	void EnsureObjects();

	std::vector<glm::uvec2> m_metadata;
	std::vector<std::uint32_t> m_indices;
	std::vector<glm::vec4> m_lightTexels;
	std::vector<LightSpan> m_spans;
	std::vector<std::uint32_t> m_spanTiles;

	GLuint m_metadataBuffer = 0;
	GLuint m_metadataTexture = 0;
	GLuint m_indexBuffer = 0;
	GLuint m_indexTexture = 0;
	GLuint m_lightBuffer = 0;
	GLuint m_lightTexture = 0;

	std::uint64_t m_cachedInputSignature = 0;
	bool m_hasCachedInput = false;
	PointLightGridRuntimeStats m_stats;
};
