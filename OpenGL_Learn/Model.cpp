#include "Model.h"
#include "GLStateCache.h"
#include "Profiler.h"
#include "XmlMaterialManager.h"
#include <assimp/Exporter.hpp>
#include <assimp/version.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>

namespace {
	// 模型缓存：相同 path(+shader) 直接复用已构建 meshes，避免重复 Assimp 解析与 CPU 构建。
	std::unordered_map<std::string, std::vector<Mesh>> g_modelMeshCache;

	bool FileExists(const std::string& path)
	{
		PerformanceProfiler::GetInstance().RecordFileSystemCheck();
		std::error_code error;
		return std::filesystem::exists(path, error);
	}

	bool UsesAmazonBistroMaterialConvention(const std::string& modelDirectory)
	{
		std::string directoryName =
			std::filesystem::path(modelDirectory).filename().string();
		std::transform(
			directoryName.begin(),
			directoryName.end(),
			directoryName.begin(),
			[](unsigned char value) {
				return static_cast<char>(std::tolower(value));
			});
		return directoryName == "bistro";
	}

	bool ContainsCaseInsensitive(
		const std::string& value,
		const std::string& needle)
	{
		std::string lowerValue = value;
		std::string lowerNeedle = needle;
		std::transform(
			lowerValue.begin(),
			lowerValue.end(),
			lowerValue.begin(),
			[](unsigned char character) {
				return static_cast<char>(std::tolower(character));
			});
		std::transform(
			lowerNeedle.begin(),
			lowerNeedle.end(),
			lowerNeedle.begin(),
			[](unsigned char character) {
				return static_cast<char>(std::tolower(character));
			});
		return lowerValue.find(lowerNeedle) != std::string::npos;
	}

	std::vector<Texture> ReclassifyTextures(
		const std::vector<Texture>& source,
		const std::string& typeName)
	{
		std::vector<Texture> textures = source;
		for (Texture& texture : textures) {
			texture.type = typeName;
		}
		return textures;
	}

	std::string BuildCompanionTexturePath(
		const std::vector<Texture>& baseColorTextures,
		const std::string& sourceSuffix,
		const std::string& destinationSuffix)
	{
		if (baseColorTextures.empty()) {
			return {};
		}

		const std::filesystem::path sourcePath(
			baseColorTextures.front().path.C_Str());
		std::string stem = sourcePath.stem().string();
		std::string lowerStem = stem;
		std::transform(
			lowerStem.begin(),
			lowerStem.end(),
			lowerStem.begin(),
			[](unsigned char value) {
				return static_cast<char>(std::tolower(value));
			});

		std::string lowerSuffix = sourceSuffix;
		std::transform(
			lowerSuffix.begin(),
			lowerSuffix.end(),
			lowerSuffix.begin(),
			[](unsigned char value) {
				return static_cast<char>(std::tolower(value));
			});
		if (lowerStem.size() < lowerSuffix.size() ||
			lowerStem.compare(
				lowerStem.size() - lowerSuffix.size(),
				lowerSuffix.size(),
				lowerSuffix) != 0) {
			return {};
		}

		stem.replace(
			stem.size() - sourceSuffix.size(),
			sourceSuffix.size(),
			destinationSuffix);
		return (
			sourcePath.parent_path() /
			(stem + sourcePath.extension().string())).string();
	}

	constexpr unsigned int kModelImportFlags =
		aiProcess_Triangulate |
		aiProcess_FlipUVs |
		aiProcess_GenSmoothNormals |
		aiProcess_CalcTangentSpace |
		aiProcess_JoinIdenticalVertices |
		aiProcess_SortByPType;
	constexpr std::uint64_t kModelImportCacheVersion = 1;

	struct HeightTextureClassification {
		bool isTangentSpaceNormal = false;
		std::string reason;
	};

	std::unordered_map<std::string, HeightTextureClassification> g_heightTextureClassifications;

	bool HasNormalMapNameHint(const std::filesystem::path& texturePath)
	{
		std::string stem = texturePath.stem().string();
		std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char value) {
			return static_cast<char>(std::tolower(value));
		});
		return stem.find("normal") != std::string::npos ||
			stem.rfind("n_", 0) == 0 ||
			stem.find("_nrm") != std::string::npos ||
			stem.find("-nrm") != std::string::npos ||
			(stem.size() > 2 && stem.compare(stem.size() - 2, 2, "_n") == 0) ||
			(stem.size() > 2 && stem.compare(stem.size() - 2, 2, "-n") == 0);
	}

	HeightTextureClassification ClassifyHeightTexture(
		const std::string& modelDirectory,
		const char* texturePath)
	{
		namespace fs = std::filesystem;
		fs::path resolvedPath(texturePath);
		if (!resolvedPath.is_absolute()) {
			resolvedPath = fs::path(modelDirectory) / resolvedPath;
		}
		std::error_code error;
		fs::path absolutePath = fs::absolute(resolvedPath, error);
		if (!error) {
			resolvedPath = absolutePath.lexically_normal();
		}

		const std::string cacheKey = resolvedPath.generic_string();
		const auto cached = g_heightTextureClassifications.find(cacheKey);
		if (cached != g_heightTextureClassifications.end()) {
			return cached->second;
		}

		HeightTextureClassification result;
		int width = 0;
		int height = 0;
		int channelCount = 0;
		const std::string nativePath = resolvedPath.string();
		const bool hasNormalMapNameHint = HasNormalMapNameHint(resolvedPath);
		if (stbi_info(nativePath.c_str(), &width, &height, &channelCount) == 0) {
			// Some production assets use formats stb_image cannot inspect (for
			// example DDS). In that case require explicit filename semantics.
			result.isTangentSpaceNormal = hasNormalMapNameHint;
			result.reason = hasNormalMapNameHint
				? "normal-map filename hint"
				: "image metadata unavailable and no normal-map filename hint";
		}
		else if (channelCount < 3) {
			// A single/dual-channel HEIGHT texture cannot provide the XYZ vector
			// expected by the existing tangent-space normal shader path.
			result.reason = "fewer than three color channels";
		}
		else if (hasNormalMapNameHint) {
			result.isTangentSpaceNormal = true;
			result.reason = "normal-map filename hint";
		}
		else {
			int loadedWidth = 0;
			int loadedHeight = 0;
			int loadedChannels = 0;
			unsigned char* pixels = stbi_load(
				nativePath.c_str(),
				&loadedWidth,
				&loadedHeight,
				&loadedChannels,
				3);
			if (!pixels || loadedWidth <= 0 || loadedHeight <= 0) {
				result.reason = "image pixels unavailable and no normal-map filename hint";
			}
			else {
				constexpr std::size_t kMaximumSamples = 4096;
				const std::size_t pixelCount =
					static_cast<std::size_t>(loadedWidth) * static_cast<std::size_t>(loadedHeight);
				const int sampleStride = (std::max)(
					1,
					static_cast<int>(std::sqrt(
						static_cast<double>(pixelCount) / kMaximumSamples)));
				std::size_t sampleCount = 0;
				std::size_t positiveZCount = 0;
				std::size_t blueDominantCount = 0;
				double redTotal = 0.0;
				double greenTotal = 0.0;
				double blueTotal = 0.0;
				for (int y = 0; y < loadedHeight; y += sampleStride) {
					for (int x = 0; x < loadedWidth; x += sampleStride) {
						const std::size_t offset =
							(static_cast<std::size_t>(y) * loadedWidth + x) * 3;
						const unsigned int red = pixels[offset];
						const unsigned int green = pixels[offset + 1];
						const unsigned int blue = pixels[offset + 2];
						redTotal += red;
						greenTotal += green;
						blueTotal += blue;
						positiveZCount += blue >= 128 ? 1 : 0;
						blueDominantCount += blue >= (std::max)(red, green) + 8 ? 1 : 0;
						++sampleCount;
					}
				}

				const double positiveZRatio =
					static_cast<double>(positiveZCount) / sampleCount;
				const double blueDominantRatio =
					static_cast<double>(blueDominantCount) / sampleCount;
				const double meanRed = redTotal / sampleCount;
				const double meanGreen = greenTotal / sampleCount;
				const double meanBlue = blueTotal / sampleCount;
				result.isTangentSpaceNormal =
					positiveZRatio >= 0.80 &&
					(blueDominantRatio >= 0.50 ||
						meanBlue >= (std::max)(meanRed, meanGreen) + 24.0);
				result.reason = result.isTangentSpaceNormal
					? "normal-like RGB content"
					: "RGB content is not normal-map-like";
			}
			stbi_image_free(pixels);
		}

		g_heightTextureClassifications.emplace(cacheKey, result);
		if (!result.isTangentSpaceNormal) {
			std::cout << "[Model Material] ignored HEIGHT texture as tangent-space normal: "
				<< texturePath << " (" << result.reason << ")" << std::endl;
		}
		return result;
	}

	bool IsValidScene(const aiScene* scene)
	{
		return scene &&
			!(scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) &&
			scene->mRootNode;
	}

	std::uint64_t HashCacheFingerprint(const std::string& fingerprint)
	{
		std::uint64_t hash = 14695981039346656037ull;
		for (unsigned char value : fingerprint) {
			hash ^= value;
			hash *= 1099511628211ull;
		}
		return hash;
	}

	std::filesystem::path BuildModelImportCachePath(const std::string& sourcePath)
	{
		namespace fs = std::filesystem;
		std::error_code error;
		fs::path absolutePath = fs::absolute(sourcePath, error);
		if (error) {
			return {};
		}
		absolutePath = absolutePath.lexically_normal();

		PerformanceProfiler::GetInstance().RecordFileSystemCheck();
		const std::uintmax_t sourceSize = fs::file_size(absolutePath, error);
		if (error) {
			return {};
		}
		PerformanceProfiler::GetInstance().RecordFileSystemCheck();
		const auto sourceWriteTime = fs::last_write_time(absolutePath, error);
		if (error) {
			return {};
		}

		std::ostringstream fingerprint;
		fingerprint << absolutePath.generic_string()
			<< "|size=" << sourceSize
			<< "|write=" << sourceWriteTime.time_since_epoch().count()
			<< "|cache=" << kModelImportCacheVersion
			<< "|flags=" << kModelImportFlags
			<< "|vertex=" << sizeof(Vertex)
			<< "|assimp=" << aiGetVersionMajor() << '.' << aiGetVersionMinor() << '.' << aiGetVersionPatch();

		if (absolutePath.extension() == ".obj") {
			std::ifstream source(absolutePath);
			std::string line;
			for (int lineCount = 0; lineCount < 256 && std::getline(source, line); ++lineCount) {
				if (line.rfind("mtllib ", 0) != 0) {
					continue;
				}
				std::istringstream materialLibraries(line.substr(7));
				std::string materialLibrary;
				while (materialLibraries >> materialLibrary) {
					const fs::path dependencyPath =
						(absolutePath.parent_path() / materialLibrary).lexically_normal();
					PerformanceProfiler::GetInstance().RecordFileSystemCheck();
					const std::uintmax_t dependencySize = fs::file_size(dependencyPath, error);
					if (error) {
						fingerprint << "|dependency-missing=" << dependencyPath.generic_string();
						error.clear();
						continue;
					}
					PerformanceProfiler::GetInstance().RecordFileSystemCheck();
					const auto dependencyWriteTime = fs::last_write_time(dependencyPath, error);
					if (error) {
						fingerprint << "|dependency-time-error=" << dependencyPath.generic_string();
						error.clear();
						continue;
					}
					fingerprint << "|dependency=" << dependencyPath.generic_string()
						<< ':' << dependencySize
						<< ':' << dependencyWriteTime.time_since_epoch().count();
				}
				break;
			}
		}

		std::ostringstream fileName;
		fileName << std::hex << std::setfill('0') << std::setw(16)
			<< HashCacheFingerprint(fingerprint.str()) << ".assbin";
		return fs::path("model-cache") / fileName.str();
	}

	void WriteModelImportCache(const aiScene* scene, const std::filesystem::path& cachePath)
	{
		if (!scene || cachePath.empty()) {
			return;
		}

		namespace fs = std::filesystem;
		std::error_code error;
		fs::create_directories(cachePath.parent_path(), error);
		if (error) {
			std::cout << "[Model Import Cache] cannot create directory: "
				<< error.message() << std::endl;
			return;
		}

		fs::path temporaryPath = cachePath;
		temporaryPath += ".tmp";
		Assimp::Exporter exporter;
		if (exporter.Export(scene, "assbin", temporaryPath.string()) != AI_SUCCESS) {
			std::cout << "[Model Import Cache] export failed: "
				<< exporter.GetErrorString() << std::endl;
			return;
		}

		fs::remove(cachePath, error);
		error.clear();
		fs::rename(temporaryPath, cachePath, error);
		if (error) {
			std::cout << "[Model Import Cache] publish failed: "
				<< error.message() << std::endl;
		}
	}
}

void Model::DestroyMeshCache()
{
	g_modelMeshCache.clear();
}

MeshGeometry::MeshGeometry(std::vector<Vertex> vertices, std::vector<unsigned int> indices)
{
	m_vertexCount = vertices.size();
	m_indexCount = indices.size();
	if (!vertices.empty()) {
		m_boundsMin = vertices.front().Position;
		m_boundsMax = vertices.front().Position;
		m_boundsCenter = vertices.front().Position;
		for (const auto& vertex : vertices) {
			m_boundsMin.x = (std::min)(m_boundsMin.x, vertex.Position.x);
			m_boundsMin.y = (std::min)(m_boundsMin.y, vertex.Position.y);
			m_boundsMin.z = (std::min)(m_boundsMin.z, vertex.Position.z);
			m_boundsMax.x = (std::max)(m_boundsMax.x, vertex.Position.x);
			m_boundsMax.y = (std::max)(m_boundsMax.y, vertex.Position.y);
			m_boundsMax.z = (std::max)(m_boundsMax.z, vertex.Position.z);

			const glm::vec3 delta = vertex.Position - m_boundsCenter;
			const float distanceSquared = glm::dot(delta, delta);
			const float radiusSquared = m_boundingRadius * m_boundingRadius;
			if (distanceSquared > radiusSquared) {
				const float distance = std::sqrt(distanceSquared);
				const float expandedRadius = (m_boundingRadius + distance) * 0.5f;
				m_boundsCenter += delta * ((expandedRadius - m_boundingRadius) / distance);
				m_boundingRadius = expandedRadius;
			}
		}
	}

	glGenVertexArrays(1, &m_vao);
	glGenBuffers(1, &m_vbo);
	if (m_indexCount != 0) {
		glGenBuffers(1, &m_ebo);
	}

	GLState::BindVertexArray(m_vao);
	glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
	glBufferData(
		GL_ARRAY_BUFFER,
		vertices.size() * sizeof(Vertex),
		vertices.empty() ? nullptr : vertices.data(),
		GL_STATIC_DRAW
	);
	if (m_ebo != 0) {
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
		glBufferData(
			GL_ELEMENT_ARRAY_BUFFER,
			indices.size() * sizeof(unsigned int),
			indices.data(),
			GL_STATIC_DRAW
		);
	}

	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Normal));
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, TexCoords));
	glEnableVertexAttribArray(3);
	glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Tangent));
	glEnableVertexAttribArray(4);
	glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Bitangent));
	GLState::BindVertexArray(0);

	const std::uint64_t stagingBytes =
		static_cast<std::uint64_t>(vertices.capacity() * sizeof(Vertex)) +
		static_cast<std::uint64_t>(indices.capacity() * sizeof(unsigned int));
	m_trackedGpuBytes =
		static_cast<std::uint64_t>(m_vertexCount * sizeof(Vertex)) +
		static_cast<std::uint64_t>(m_indexCount * sizeof(unsigned int));
	auto& profiler = PerformanceProfiler::GetInstance();
	if (stagingBytes != 0) {
		profiler.RecordMemoryAllocation(MemoryResourceType::MeshCpu, stagingBytes);
	}
	if (m_trackedGpuBytes != 0) {
		profiler.RecordMemoryAllocation(MemoryResourceType::MeshGpu, m_trackedGpuBytes);
	}

	// The VBO and compact bounds are the long-lived representation. Release the
	// upload vector immediately so cached geometry does not pin a CPU-side copy.
	std::vector<Vertex>().swap(vertices);
	std::vector<unsigned int>().swap(indices);
	if (stagingBytes != 0) {
		profiler.RecordMemoryRelease(MemoryResourceType::MeshCpu, stagingBytes);
	}
}

MeshGeometry::~MeshGeometry()
{
	auto& profiler = PerformanceProfiler::GetInstance();
	if (m_trackedGpuBytes != 0) {
		profiler.RecordMemoryRelease(MemoryResourceType::MeshGpu, m_trackedGpuBytes);
	}
	if (m_vao != 0) {
		GLState::ForgetVertexArray(m_vao);
		glDeleteVertexArrays(1, &m_vao);
	}
	if (m_vbo != 0) {
		glDeleteBuffers(1, &m_vbo);
	}
	if (m_ebo != 0) {
		glDeleteBuffers(1, &m_ebo);
	}
}

Mesh::Mesh(std::vector<Vertex> vertices,
		std::vector<unsigned int> indices,
		   Material* material,
		   std::shared_ptr<Material> ownedMaterial,
           const std::string& materialXmlPathIn,
		   bool tangentBasisReady)
{
	if (!indices.empty()) {
		const bool malformed = indices.size() % 3 != 0 ||
			std::any_of(indices.begin(), indices.end(),
				[vertexCount = vertices.size()](unsigned int index) {
					return index >= vertexCount;
				});
		if (malformed) {
			std::cout << "warning: invalid mesh indices, falling back to non-indexed draw" << std::endl;
			indices.clear();
			tangentBasisReady = false;
		}
	}
	if (!tangentBasisReady) {
		vertices = ComputeTBNVertices(vertices, indices);
	}
	m_geometry = std::make_shared<MeshGeometry>(std::move(vertices), std::move(indices));
	this->material_owner = std::move(ownedMaterial);
	if(material) {
		this->material_ptr = material;
	}
	else if (this->material_owner) {
		this->material_ptr = this->material_owner.get();
	}
	else {
		this->material_ptr = XmlMaterialManager::GetInstance().GetOrLoadMaterialByFile(materialXmlPathIn);
	}
    this->materialXmlPath = materialXmlPathIn;
	this->start_tex_index = 0;
	// 默认：Mesh 可见 / 可绘制
	SetActiveStatus(true);
}

Mesh::Mesh(std::vector<Vertex> vertices,
		std::vector<unsigned int> indices,
		Material* material,
		const std::string& materialXmlPathIn)
	: Mesh(std::move(vertices), std::move(indices), material, std::shared_ptr<Material>(), materialXmlPathIn)
{
}

unsigned int Mesh::GetVAO() const
{
	return m_geometry ? m_geometry->GetVAO() : 0;
}

std::size_t Mesh::GetVertexCount() const
{
	return m_geometry ? m_geometry->GetVertexCount() : 0;
}

std::size_t Mesh::GetIndexCount() const
{
	return m_geometry ? m_geometry->GetIndexCount() : 0;
}

std::size_t Mesh::GetDrawCount() const
{
	return m_geometry ? m_geometry->GetDrawCount() : 0;
}

bool Mesh::UsesIndices() const
{
	return m_geometry && m_geometry->UsesIndices();
}

const glm::vec3& Mesh::GetBoundsMin() const
{
	static const glm::vec3 empty(0.0f);
	return m_geometry ? m_geometry->GetBoundsMin() : empty;
}

const glm::vec3& Mesh::GetBoundsMax() const
{
	static const glm::vec3 empty(0.0f);
	return m_geometry ? m_geometry->GetBoundsMax() : empty;
}

const glm::vec3& Mesh::GetBoundsCenter() const
{
	static const glm::vec3 empty(0.0f);
	return m_geometry ? m_geometry->GetBoundsCenter() : empty;
}

float Mesh::GetBoundingRadius() const
{
	return m_geometry ? m_geometry->GetBoundingRadius() : 0.0f;
}

std::uint64_t Mesh::SyncShadowStateRevision(
	std::uint64_t syncEpoch) const
{
	std::size_t signature = 0;
	hash_combine(signature, m_active);
	if (m_active) {
		hash_combine(signature, GetVAO());
		hash_combine(signature, GetDrawCount());
		hash_combine(
			signature,
			reinterpret_cast<std::uintptr_t>(material_ptr));
		if (material_ptr) {
			hash_combine(
				signature,
				material_ptr->GetShadowStateRevision(syncEpoch));
		}
	}
	if (!m_shadowStateInitialized ||
		signature != m_shadowStateSignature) {
		m_shadowStateInitialized = true;
		m_shadowStateSignature = signature;
		++m_shadowStateRevision;
	}
	return m_shadowStateRevision;
}

void Mesh::Draw(Shader* shader, bool forcePbrMaterial)
{
    if (!material_ptr) {
        return;
    }

	// XML materials are refreshed once while preparing the scene render data.
	auto materialGaurd = MaterialGaurd(*material_ptr, shader);
	if (forcePbrMaterial && shader && shader->shaderName == "deferProcess") {
		shader->setBool("material.usePBR", true);
	}
	GLState::ActiveTexture(GL_TEXTURE0);
	DrawGeometry();
}

void Mesh::DrawGeometry()
{
	GLState::BindVertexArray(GetVAO());
	PerformanceProfiler::GetInstance().RecordDraw(GL_TRIANGLES, GetDrawCount());
	if (UsesIndices()) {
		glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(GetIndexCount()), GL_UNSIGNED_INT, nullptr);
	}
	else {
		glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(GetVertexCount()));
	}
}

void Model::DrawGeometry()
{
	for (Mesh& mesh : meshes) {
		if (!mesh.GetActiveStatus()) {
			continue;
		}
		mesh.DrawGeometry();
	}
}

void Model::Draw(Shader* shader, unsigned int start_tex_index )
{
	if (shader) {
		shader->use();
		shader->setMat4("model", getModelMatrix());
	}
	else if (m_shader) {
		m_shader->use();
		m_shader->setMat4("model", getModelMatrix());
	}
	else {
		return;
	}
	auto& properties = SystemProperties::GetInstance();
	int usedTextures = properties.USED_TEXTURE_NUM;
	MaterialBatchScope materialBatch;
	for (unsigned int i = 0; i < meshes.size(); ++i) {
		if (!meshes[i].GetActiveStatus()) {
			continue;
		}
		meshes[i].start_tex_index = start_tex_index;
		meshes[i].Draw(shader);
		properties.USED_TEXTURE_NUM = usedTextures;
	}
}

void Model::loadModel(std::string path,Material* mat)
{
	if (!m_shader && !mat) {
		m_shader = ShaderManager::GetInstance().GetShaderByName("pbr");
	}
	std::string shaderName = (m_shader ? m_shader->shaderName : std::string("pbr"));
	std::string cacheKey = path + "|shader=" + shaderName + (mat ? "|mat=custom" : "|mat=default");
	if (!mat) {
		auto it = g_modelMeshCache.find(cacheKey);
		if (it != g_modelMeshCache.end()) {
			meshes = it->second;
			directory = std::filesystem::path(path).parent_path().generic_string();
			return;
		}
	}

	Assimp::Importer importer;
	const std::filesystem::path importCachePath = BuildModelImportCachePath(path);
	const aiScene* scene = nullptr;
	bool importCacheHit = false;
	if (!importCachePath.empty() && FileExists(importCachePath.string())) {
		scene = importer.ReadFile(importCachePath.string(), 0);
		importCacheHit = IsValidScene(scene);
		if (!importCacheHit) {
			std::cout << "[Model Import Cache] invalid entry, rebuilding "
				<< importCachePath.string() << std::endl;
		}
	}

	if (!importCacheHit) {
		scene = importer.ReadFile(path, kModelImportFlags);
		if (IsValidScene(scene)) {
			WriteModelImportCache(scene, importCachePath);
		}
	}
	PerformanceProfiler::GetInstance().RecordModelImportCacheLookup(importCacheHit);
	std::cout << "[Model Import Cache] " << (importCacheHit ? "hit " : "miss ")
		<< path << std::endl;

	if(!IsValidScene(scene)) {
		std::cout << "ERROR::ASSIMP:: " << importer.GetErrorString() << std::endl;
		return;
	}
	// 防止 meshes 在 push_back 时多次扩容触发移动/销毁链，先按 Assimp 总 mesh 数预留容量
	meshes.clear();
	meshes.reserve(scene->mNumMeshes);
	directory = std::filesystem::path(path).parent_path().generic_string();
	processNode(scene->mRootNode, scene,mat);
	if (!mat) {
		g_modelMeshCache[cacheKey] = meshes;
	}
}

void Model::processNode(aiNode* node, const aiScene* scene,Material* mat)
{
	if (!node || !scene) {
		return;
	}
	for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
		const unsigned int meshIndex = node->mMeshes[i];
		if (meshIndex >= scene->mNumMeshes) {
			std::cerr << "[Model] skipping invalid mesh index " << meshIndex
				<< " (mesh count " << scene->mNumMeshes << ')' << std::endl;
			continue;
		}
		aiMesh* mesh = scene->mMeshes[meshIndex];
		if (!mesh ||
			!mesh->HasPositions() ||
			mesh->mNumVertices == 0 ||
			(mesh->mPrimitiveTypes & aiPrimitiveType_TRIANGLE) == 0) {
			continue;
		}
		meshes.emplace_back(processMesh(mesh, scene,mat));
	}
	for (unsigned int i = 0; i < node->mNumChildren; ++i) {
		processNode(node->mChildren[i], scene,mat);
	}
}

void Model::BuildMeshLists()
{
	m_opaqueMeshes.clear();
	m_transparentMeshes.clear();

	for (auto& mesh : meshes) {
		Material* mat = mesh.material_ptr;
		bool isTransparent = false;
		bool isCutout = false;

		if (mat) {
			const auto& props = mat->GetProperties();
			auto itUseCutout = props.find("useAlphaCutoff");
			if (itUseCutout != props.end() && itUseCutout->second.type == MaterialPropertyType::Bool) {
				isCutout = itUseCutout->second.scalarValue.boolValue;
			}
			auto itCutoff = props.find("alphaCutoff");
			if (itCutoff != props.end() && itCutoff->second.type == MaterialPropertyType::Float) {
				isCutout = isCutout || (itCutoff->second.scalarValue.floatValue > 0.0f);
			}
			auto it = props.find("opacity");
			if (it != props.end() && it->second.type == MaterialPropertyType::Float) {
				float op = it->second.scalarValue.floatValue;
				if (op < 0.999f) isTransparent = true;
			}
		}
		if (isCutout) {
			isTransparent = false;
			RenderState cutoutState = mat->GetRenderState();
			cutoutState.depthTest = true;
			cutoutState.depthWrite = true;
			cutoutState.stencilTest = false;
			cutoutState.blendMode = BlendMode::None;
			mat->SetRenderState(cutoutState);
		}
		else if (isTransparent) {
			mat->SetRenderState({ true,false,false,BlendMode::AlphaBlend,CullMode::None});
		}
		MeshEntry entry{ &mesh, mat };
		if (isTransparent) m_transparentMeshes.push_back(entry);
		else m_opaqueMeshes.push_back(entry);
	}
}

void Model::RefreshMaterialDrivenState()
{
	auto& materialManager = XmlMaterialManager::GetInstance();
	const size_t materialRevision = materialManager.GetMaterialRevision();
	if (m_lastAppliedMaterialRevision == materialRevision) {
		return;
	}

	for (auto& mesh : meshes) {
		if (!mesh.materialXmlPath.empty()) {
			if (Material* xmlMaterial = materialManager.GetOrLoadMaterialByFile(mesh.materialXmlPath)) {
				mesh.material_ptr = xmlMaterial;
			}
		}
	}

	for (const auto& mesh : meshes) {
		if (mesh.material_ptr) {
			auto shader = ShaderManager::GetInstance().GetShaderByName(mesh.material_ptr->GetShaderName());
			if (shader) {
				m_shader = shader;
			}
			break;
		}
	}

	BuildMeshLists();
	m_lastAppliedMaterialRevision = materialRevision;
}

std::uint64_t Model::SyncShadowStateRevision(
	std::uint64_t syncEpoch)
{
	std::size_t signature = 0;
	hash_combine(signature, m_active);
	if (m_active) {
		hash_combine(signature, GetTransformRevision());
		hash_combine(signature, meshes.size());
		for (const Mesh& mesh : meshes) {
			hash_combine(
				signature,
				mesh.SyncShadowStateRevision(syncEpoch));
			hash_combine(signature, mesh.GetShadowStateSignature());
		}
	}
	if (!m_shadowStateInitialized ||
		signature != m_shadowStateSignature) {
		m_shadowStateInitialized = true;
		m_shadowStateSignature = signature;
		++m_shadowStateRevision;
	}
	return m_shadowStateRevision;
}

Mesh Model::processMesh(aiMesh* mesh, const aiScene* scene,Material* mat)
{
	std::vector<Vertex> vertices;
	std::vector<unsigned int>indices;
	Material* material = mat;
	const std::string materialShaderName = m_shader ? m_shader->shaderName : std::string("phong");
	const bool tangentBasisReady = mesh->HasTangentsAndBitangents();
	vertices.reserve(mesh->mNumVertices);
	indices.reserve(static_cast<size_t>(mesh->mNumFaces) * 3);
	for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
		Vertex vertex;
		glm::vec3 vector;
		vector.x = mesh->mVertices[i].x;
		vector.y = mesh->mVertices[i].y;
		vector.z = mesh->mVertices[i].z;
		vertex.Position = vector;
		if (mesh->HasNormals()) {
			vector.x = mesh->mNormals[i].x;
			vector.y = mesh->mNormals[i].y;
			vector.z = mesh->mNormals[i].z;
			vertex.Normal = vector;
		}
		else {
			vertex.Normal = glm::vec3(0.0f, 1.0f, 0.0f);
		}
		if (mesh->mTextureCoords[0]) {
			glm::vec2 vec;
			vec.x = mesh->mTextureCoords[0][i].x;
			vec.y = mesh->mTextureCoords[0][i].y;
			vertex.TexCoords = vec;
		}
		else {
			vertex.TexCoords = glm::vec2(0.0f, 0.0f);
		}
		if (tangentBasisReady) {
			vertex.Tangent = glm::vec3(
				mesh->mTangents[i].x,
				mesh->mTangents[i].y,
				mesh->mTangents[i].z);
			vertex.Bitangent = glm::vec3(
				mesh->mBitangents[i].x,
				mesh->mBitangents[i].y,
				mesh->mBitangents[i].z);
		}
		vertices.push_back(vertex);
	}
	for (unsigned int i = 0; i < mesh->mNumFaces; ++i) {
		const aiFace& face = mesh->mFaces[i];
		if (face.mNumIndices != 3 ||
			face.mIndices[0] >= mesh->mNumVertices ||
			face.mIndices[1] >= mesh->mNumVertices ||
			face.mIndices[2] >= mesh->mNumVertices) {
			continue;
		}
		indices.push_back(face.mIndices[0]);
		indices.push_back(face.mIndices[1]);
		indices.push_back(face.mIndices[2]);
	}
	std::cout << "[Mesh Debug] Name: " << mesh->mName.C_Str()
		<< " | Vertices: " << mesh->mNumVertices
		<< " | Indices: " << indices.size()
		<< " | MatID: " << mesh->mMaterialIndex << std::endl;
	std::string xmlPath;
	if(mat) {
		return Mesh(std::move(vertices), std::move(indices), mat, std::shared_ptr<Material>(), std::string(), tangentBasisReady);
	}
	else if (mesh->mMaterialIndex < scene->mNumMaterials &&
		scene->mMaterials[mesh->mMaterialIndex]) {
		aiMaterial* aiMat = scene->mMaterials[mesh->mMaterialIndex];

		aiString aiName;
		if (aiMat->Get(AI_MATKEY_NAME, aiName) == AI_SUCCESS) {
            // 约定：一个材质对应一个 xml 文件，如 materials/Wood.xml
			xmlPath = "materials/";
			xmlPath += aiName.C_Str();
			xmlPath += ".xml";
			if (FileExists(xmlPath)) {
				material = XmlMaterialManager::GetInstance().GetOrLoadMaterialByFile(xmlPath);
			}
		}

		if (!material) {
			std::shared_ptr<Material> ownedMaterial;
			if (s_importedMaterialSharingEnabled) {
				auto cachedMaterial = m_importedMaterials.find(mesh->mMaterialIndex);
				if (cachedMaterial != m_importedMaterials.end()) {
					ownedMaterial = cachedMaterial->second;
				}
			}
			if (!ownedMaterial) {
				ownedMaterial = std::make_shared<Material>(materialShaderName);
				prosessMaterial(aiMat, ownedMaterial.get());
				if (s_importedMaterialSharingEnabled) {
					m_importedMaterials.emplace(mesh->mMaterialIndex, ownedMaterial);
				}
			}
			material = ownedMaterial.get();
			return Mesh(
				std::move(vertices),
				std::move(indices),
				material,
				std::move(ownedMaterial),
				std::string(),
				tangentBasisReady);
		}
	}
	else {
        // 没有关联 aiMaterial，则尝试使用一个默认 XML 材质（如 materials/Default.xml）
        xmlPath = "materials/Default.xml";
        if (FileExists(xmlPath)) {
            material = XmlMaterialManager::GetInstance().GetOrLoadMaterialByFile(xmlPath);
        }
		if (!material) {
			std::shared_ptr<Material> ownedMaterial = std::make_shared<Material>(materialShaderName);
			material = ownedMaterial.get();
			return Mesh(std::move(vertices), std::move(indices), material, ownedMaterial, std::string(), tangentBasisReady);
        }
	}
	return Mesh(std::move(vertices), std::move(indices), material, nullptr, xmlPath, tangentBasisReady);
}

void Model::prosessMaterial(aiMaterial* mat,Material* material)
{
	aiColor3D color(0.0f, 0.0f, 0.0f);
	aiColor3D diffuseColor(1.0f, 1.0f, 1.0f);
	aiColor3D emissiveColor(0.0f, 0.0f, 0.0f);
	float shininess = 0.0f;
	float opacity = 1.0f;
	aiString importedMaterialName;
	mat->Get(AI_MATKEY_NAME, importedMaterialName);
	const bool usesBistroConvention =
		UsesAmazonBistroMaterialConvention(directory);
	const bool bistroDoubleSidedMaterial =
		usesBistroConvention &&
		ContainsCaseInsensitive(importedMaterialName.C_Str(), ".doublesided");
	int assimpTwoSided = 0;
	const bool importedDoubleSided =
		bistroDoubleSidedMaterial ||
		(mat->Get(AI_MATKEY_TWOSIDED, assimpTwoSided) == AI_SUCCESS &&
			assimpTwoSided != 0);
	if (mat->Get(AI_MATKEY_COLOR_AMBIENT, color) == AI_SUCCESS)
		material->AddProperty("ambient", MaterialProperty::CreateVec3(glm::vec3(color.r, color.g, color.b)));
	if (mat->Get(AI_MATKEY_COLOR_DIFFUSE, diffuseColor) == AI_SUCCESS)
		material->AddProperty("diffuse", MaterialProperty::CreateVec3(glm::vec3(diffuseColor.r, diffuseColor.g, diffuseColor.b)));
	if (mat->Get(AI_MATKEY_COLOR_SPECULAR, color) == AI_SUCCESS)
		material->AddProperty("specular", MaterialProperty::CreateVec3(glm::vec3(color.r, color.g, color.b)));
	if (mat->Get(AI_MATKEY_SHININESS, shininess) == AI_SUCCESS)
		material->AddProperty("shininess", MaterialProperty::CreateFloat(shininess));
	if (mat->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS)
		material->AddProperty("opacity", MaterialProperty::CreateFloat(opacity));

	std::vector<Texture> diffuseTextures = loadMaterialTextures(
		mat,
		aiTextureType_BASE_COLOR,
		"texture_diffuse");
	if (diffuseTextures.empty()) {
		diffuseTextures = loadMaterialTextures(mat, aiTextureType_DIFFUSE, "texture_diffuse");
	}
	material->AddProperty("texture_diffuse", MaterialProperty::CreateTexture(diffuseTextures));
	auto specularTextures = loadMaterialTextures(
		mat,
		aiTextureType_SPECULAR,
		"texture_specular");
	material->AddProperty(
		"texture_specular",
		MaterialProperty::CreateTexture(specularTextures));
	auto normalTextures = loadMaterialTextures(mat, aiTextureType_NORMALS, "texture_normal");
	if (normalTextures.empty()) {
		// Assimp exposes OBJ map_Bump as HEIGHT for both real height maps and
		// tangent-space normal maps. Only promote textures with reliable normal
		// evidence; feeding grayscale height into the XYZ shader path corrupts
		// lighting instead of producing bump mapping.
		normalTextures = loadMaterialTextures(
			mat,
			aiTextureType_HEIGHT,
			"texture_normal",
			true);
	}
	material->AddProperty("texture_normal", MaterialProperty::CreateTexture(normalTextures));
	auto opacityTextures = loadMaterialTextures(
		mat,
		aiTextureType_OPACITY,
		"texture_opacity");
	material->AddProperty("texture_opacity", MaterialProperty::CreateTexture(opacityTextures));

	if (material->GetShaderName() == "pbr") {
		aiColor4D baseColor(diffuseColor.r, diffuseColor.g, diffuseColor.b, opacity);
		if (mat->Get(AI_MATKEY_BASE_COLOR, baseColor) == AI_SUCCESS) {
			diffuseColor = aiColor3D(baseColor.r, baseColor.g, baseColor.b);
			opacity *= baseColor.a;
		}
		float metallic = 0.0f;
		float roughness = shininess > 0.0f
			? std::sqrt(2.0f / (shininess + 2.0f))
			: 0.5f;
		mat->Get(AI_MATKEY_METALLIC_FACTOR, metallic);
		mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness);
		mat->Get(AI_MATKEY_COLOR_EMISSIVE, emissiveColor);

		auto loadConventionalTexture = [this](
			const std::vector<std::string>& candidates,
			const std::string& typeName,
			bool srgb) {
			std::vector<Texture> textures;
			for (const std::string& candidate : candidates) {
				if (!FileExists(directory + '/' + candidate)) {
					continue;
				}
				for (const Texture& loaded : textures_loaded) {
					if (loaded.path.C_Str() == candidate) {
						Texture reused = loaded;
						reused.type = typeName;
						textures.push_back(reused);
						return textures;
					}
				}

				Texture texture{};
				texture.textureID = TextureFromFile(candidate.c_str(), directory, false, srgb);
				if (texture.textureID == 0) {
					return textures;
				}
				texture.textureGammaID = texture.textureID;
				texture.type = typeName;
				texture.path = candidate;
				textures.push_back(texture);
				textures_loaded.push_back(texture);
				return textures;
			}
			return textures;
		};

		auto metallicTextures = loadMaterialTextures(
			mat,
			aiTextureType_METALNESS,
			"texture_metallic");
		auto roughnessTextures = loadMaterialTextures(
			mat,
			aiTextureType_DIFFUSE_ROUGHNESS,
			"texture_roughness");
		auto aoTextures = loadMaterialTextures(
			mat,
			aiTextureType_AMBIENT_OCCLUSION,
			"texture_ao");
		auto emissiveTextures = loadMaterialTextures(
			mat,
			aiTextureType_EMISSION_COLOR,
			"texture_emissive");
		if (emissiveTextures.empty()) {
			emissiveTextures = loadMaterialTextures(mat, aiTextureType_EMISSIVE, "texture_emissive");
		}
		if (usesBistroConvention && !specularTextures.empty()) {
			// Bistro's texture named "Specular" is actually an ORM payload:
			// R = occlusion amount, G = roughness, B = metalness.
			metallicTextures = ReclassifyTextures(
				specularTextures,
				"texture_metallic");
			roughnessTextures = ReclassifyTextures(
				specularTextures,
				"texture_roughness");
			aoTextures = ReclassifyTextures(
				specularTextures,
				"texture_ao");
			// MTL has no metallic/roughness factors for this convention. Unit
			// factors preserve the authored channel values instead of scaling them
			// by the legacy Ks/Ns defaults.
			metallic = 1.0f;
			roughness = 1.0f;
		}
		if (emissiveTextures.empty() && usesBistroConvention) {
			const std::string companionEmissive = BuildCompanionTexturePath(
				diffuseTextures,
				"_BaseColor",
				"_Emissive");
			if (!companionEmissive.empty()) {
				emissiveTextures = loadConventionalTexture(
					{ companionEmissive },
					"texture_emissive",
					true);
				if (!emissiveTextures.empty() &&
					emissiveColor.r <= 0.0f &&
					emissiveColor.g <= 0.0f &&
					emissiveColor.b <= 0.0f) {
					emissiveColor = aiColor3D(1.0f, 1.0f, 1.0f);
				}
			}
		}
		if (metallicTextures.empty()) {
			metallicTextures = loadConventionalTexture(
				{ "metallic.png", "metallic.jpg", "metalness.png", "metalness.jpg" },
				"texture_metallic",
				false);
		}
		if (roughnessTextures.empty()) {
			roughnessTextures = loadConventionalTexture(
				{ "roughness.png", "roughness.jpg" },
				"texture_roughness",
				false);
		}
		if (aoTextures.empty()) {
			aoTextures = loadConventionalTexture(
				{ "ao.png", "ao.jpg", "ambient_occlusion.png", "ambient_occlusion.jpg" },
				"texture_ao",
				false);
		}
		if (emissiveTextures.empty()) {
			emissiveTextures = loadConventionalTexture(
				{ "emissive.png", "emissive.jpg", "emission.png", "emission.jpg" },
				"texture_emissive",
				true);
		}

		const bool packedMetallicRoughness =
			!metallicTextures.empty() &&
			!roughnessTextures.empty() &&
			std::strcmp(
				metallicTextures.front().path.C_Str(),
				roughnessTextures.front().path.C_Str()) == 0;
		material->AddProperty("albedo", MaterialProperty::CreateColor(glm::vec3(
			diffuseColor.r,
			diffuseColor.g,
			diffuseColor.b)));
		material->AddProperty("metallic", MaterialProperty::CreateFloat(
			glm::clamp(metallic, 0.0f, 1.0f), 0.0f, 1.0f, 0.01f));
		material->AddProperty("roughness", MaterialProperty::CreateFloat(
			glm::clamp(roughness, 0.04f, 1.0f), 0.04f, 1.0f, 0.01f));
		material->AddProperty("ao", MaterialProperty::CreateFloat(1.0f, 0.0f, 1.0f, 0.01f));
		material->AddProperty("emissive", MaterialProperty::CreateColor(glm::vec3(
			emissiveColor.r,
			emissiveColor.g,
			emissiveColor.b)));
		material->AddProperty("metallicRoughnessPacked", MaterialProperty::CreateBool(packedMetallicRoughness));
		material->AddProperty(
			"occlusionMapStoresOcclusion",
			MaterialProperty::CreateBool(usesBistroConvention));
		material->AddProperty("texture_metallic", MaterialProperty::CreateTexture(metallicTextures));
		material->AddProperty("texture_roughness", MaterialProperty::CreateTexture(roughnessTextures));
		material->AddProperty("texture_ao", MaterialProperty::CreateTexture(aoTextures));
		material->AddProperty("texture_emissive", MaterialProperty::CreateTexture(emissiveTextures));
		material->AddProperty("opacity", MaterialProperty::CreateFloat(opacity));
	}
	// OBJ map_d is often a separate grayscale image rather than alpha embedded
	// in the diffuse texture. Amazon Bistro instead stores foliage coverage in
	// BaseColor alpha and identifies those materials with a .DoubleSided suffix.
	const bool hasOpacityTexture = !opacityTextures.empty();
	const bool hasBistroBaseColorCutout =
		bistroDoubleSidedMaterial && !diffuseTextures.empty();
	const bool autoCutout =
		opacity < 0.999f || hasOpacityTexture || hasBistroBaseColorCutout;
	material->AddProperty("useAlphaCutoff", MaterialProperty::CreateBool(autoCutout));
	material->AddProperty("alphaCutoff", MaterialProperty::CreateFloat(autoCutout ? 0.4f : 0.0f, 0.0f, 1.0f, 0.01f));
	if (importedDoubleSided) {
		RenderState renderState = material->GetRenderState();
		renderState.cullMode = CullMode::None;
		material->SetRenderState(renderState);
	}

	material->AddProperty("useBloom", MaterialProperty::CreateBool(false));
}

std::vector<Texture> Model::loadMaterialTextures(
	aiMaterial* mat,
	aiTextureType type,
	std::string typeName,
	bool requireTangentSpaceNormalEvidence)
{
	std::vector<Texture> textures;
	for (unsigned int i = 0; i < mat->GetTextureCount(type); i++)
	{
		aiString str;
		mat->GetTexture(type, i, &str);
		if (requireTangentSpaceNormalEvidence &&
			!ClassifyHeightTexture(directory, str.C_Str()).isTangentSpaceNormal) {
			continue;
		}
		bool skip = false;
		for (unsigned int j = 0; j < textures_loaded.size(); j++)
		{

			if (std::strcmp(textures_loaded[j].path.C_Str(), str.C_Str()) == 0)
			{
				textures.push_back(textures_loaded[j]);
				skip = true;
				break;
			}
		}
		if (!skip)
		{   // ???????????��?????????????
			Texture texture;
			const bool srgb = typeName == "texture_diffuse" || typeName == "texture_emissive";
			texture.textureID = TextureFromFile(str.C_Str(), directory, false, srgb);
			texture.textureGammaID = texture.textureID;
			texture.type = typeName;
			texture.path = str.C_Str();
			textures.push_back(texture);
			textures_loaded.push_back(texture); // ?????????????????
		}
	}
	return textures;
}

glm::vec3 Model::CalculateLocalCenter()
{
	bool first = true;
	glm::vec3 boundsMin(0.0f);
	glm::vec3 boundsMax(0.0f);
	for (const auto& mesh : meshes) {
		if (mesh.GetVertexCount() == 0) {
			continue;
		}
		if (first) {
			first = false;
			boundsMin = mesh.GetBoundsMin();
			boundsMax = mesh.GetBoundsMax();
		}
		else {
			const glm::vec3& meshMin = mesh.GetBoundsMin();
			const glm::vec3& meshMax = mesh.GetBoundsMax();
			boundsMin.x = (std::min)(boundsMin.x, meshMin.x);
			boundsMin.y = (std::min)(boundsMin.y, meshMin.y);
			boundsMin.z = (std::min)(boundsMin.z, meshMin.z);
			boundsMax.x = (std::max)(boundsMax.x, meshMax.x);
			boundsMax.y = (std::max)(boundsMax.y, meshMax.y);
			boundsMax.z = (std::max)(boundsMax.z, meshMax.z);
		}
	}

	if (first) {
		localBoundingRadius = 0.0f;
		return glm::vec3(0.0f);
	}

	const glm::vec3 center = (boundsMin + boundsMax) * 0.5f;
	float radius = 0.0f;
	for (const auto& mesh : meshes) {
		if (mesh.GetVertexCount() != 0) {
			// Combining the per-mesh spheres remains conservative, so releasing
			// source vertices cannot introduce false-positive frustum culls.
			const float meshRadius = glm::length(mesh.GetBoundsCenter() - center) +
				mesh.GetBoundingRadius();
			radius = (std::max)(radius, meshRadius);
		}
	}
	localBoundingRadius = radius;
	return center;
}

  std::vector<Vertex> ComputeTBNVertices(
	std::vector<Vertex>& vertices,
	const std::vector<unsigned int>& indices) {
	if (vertices.empty()) {
		return std::move(vertices);
	}

	if (indices.empty()) {
		if (vertices.size() % 3 == 0) {
			for (size_t i = 0; i < vertices.size(); i += 3) {
				ComputeTBN(vertices[i], vertices[i + 1], vertices[i + 2]);
			}
		}
		return std::move(vertices);
	}

	std::vector<glm::vec3> tangentSums(vertices.size(), glm::vec3(0.0f));
	std::vector<glm::vec3> bitangentSums(vertices.size(), glm::vec3(0.0f));
	constexpr float determinantEpsilon = 1.0e-8f;
	for (size_t i = 0; i + 2 < indices.size(); i += 3) {
		const unsigned int i0 = indices[i];
		const unsigned int i1 = indices[i + 1];
		const unsigned int i2 = indices[i + 2];
		const Vertex& v0 = vertices[i0];
		const Vertex& v1 = vertices[i1];
		const Vertex& v2 = vertices[i2];
		const glm::vec3 edge1 = v1.Position - v0.Position;
		const glm::vec3 edge2 = v2.Position - v0.Position;
		const glm::vec2 deltaUv1 = v1.TexCoords - v0.TexCoords;
		const glm::vec2 deltaUv2 = v2.TexCoords - v0.TexCoords;
		const float determinant = deltaUv1.x * deltaUv2.y - deltaUv2.x * deltaUv1.y;
		if (std::abs(determinant) <= determinantEpsilon) {
			continue;
		}

		const float inverseDeterminant = 1.0f / determinant;
		const glm::vec3 tangent = inverseDeterminant *
			(deltaUv2.y * edge1 - deltaUv1.y * edge2);
		const glm::vec3 bitangent = inverseDeterminant *
			(-deltaUv2.x * edge1 + deltaUv1.x * edge2);
		for (unsigned int index : { i0, i1, i2 }) {
			tangentSums[index] += tangent;
			bitangentSums[index] += bitangent;
		}
	}

	constexpr float vectorLengthEpsilon = 1.0e-12f;
	for (size_t i = 0; i < vertices.size(); ++i) {
		glm::vec3 normal = vertices[i].Normal;
		if (glm::dot(normal, normal) <= vectorLengthEpsilon) {
			normal = glm::vec3(0.0f, 1.0f, 0.0f);
		}
		normal = glm::normalize(normal);

		glm::vec3 tangent = tangentSums[i] - normal * glm::dot(normal, tangentSums[i]);
		glm::vec3 bitangent = bitangentSums[i] - normal * glm::dot(normal, bitangentSums[i]);
		if (glm::dot(tangent, tangent) <= vectorLengthEpsilon &&
			glm::dot(bitangent, bitangent) > vectorLengthEpsilon) {
			tangent = glm::cross(normal, bitangent);
		}
		if (glm::dot(tangent, tangent) <= vectorLengthEpsilon) {
			const glm::vec3 axis = std::abs(normal.y) < 0.999f
				? glm::vec3(0.0f, 1.0f, 0.0f)
				: glm::vec3(1.0f, 0.0f, 0.0f);
			tangent = glm::cross(normal, axis);
		}
		tangent = glm::normalize(tangent);

		bitangent -= tangent * glm::dot(tangent, bitangent);
		if (glm::dot(bitangent, bitangent) <= vectorLengthEpsilon) {
			bitangent = glm::cross(tangent, normal);
		}
		else {
			bitangent = glm::normalize(bitangent);
		}

		vertices[i].Tangent = tangent;
		vertices[i].Bitangent = bitangent;
	}
	return std::move(vertices);
}

void ComputeTBN(Vertex& aPoint, Vertex& bPoint, Vertex& cPoint) {
	auto edge1 = bPoint.Position - aPoint.Position;
	auto edge2 = cPoint.Position - aPoint.Position;
	auto deltaUV1 = bPoint.TexCoords - aPoint.TexCoords;
	auto deltaUV2 = cPoint.TexCoords - aPoint.TexCoords;

	GLfloat f = 1.0f / (deltaUV1.x * deltaUV2.y - deltaUV2.x * deltaUV1.y);
	glm::vec3 tangent, bitangent;
	tangent.x = f * (deltaUV2.y * edge1.x - deltaUV1.y * edge2.x);
	tangent.y = f * (deltaUV2.y * edge1.y - deltaUV1.y * edge2.y);
	tangent.z = f * (deltaUV2.y * edge1.z - deltaUV1.y * edge2.z);
	tangent = glm::normalize(tangent);

	bitangent.x = f * (-deltaUV2.x * edge1.x + deltaUV1.x * edge2.x);
	bitangent.y = f * (-deltaUV2.x * edge1.y + deltaUV1.x * edge2.y);
	bitangent.z = f * (-deltaUV2.x * edge1.z + deltaUV1.x * edge2.z);
	bitangent = glm::normalize(bitangent - glm::dot(bitangent,tangent)*tangent);

	aPoint.Tangent = tangent;
	bPoint.Tangent = tangent;
	cPoint.Tangent = tangent;
	aPoint.Bitangent = bitangent;
	bPoint.Bitangent = bitangent;
	cPoint.Bitangent = bitangent;
	
	return;
}
