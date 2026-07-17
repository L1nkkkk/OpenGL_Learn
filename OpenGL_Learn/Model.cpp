#include "Model.h"
#include "GLStateCache.h"
#include "Profiler.h"
#include "XmlMaterialManager.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
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
}

void Model::DestroyMeshCache()
{
	g_modelMeshCache.clear();
}

MeshGeometry::MeshGeometry(std::vector<Vertex> vertices)
{
	m_vertexCount = vertices.size();
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

	GLState::BindVertexArray(m_vao);
	glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
	glBufferData(
		GL_ARRAY_BUFFER,
		vertices.size() * sizeof(Vertex),
		vertices.empty() ? nullptr : vertices.data(),
		GL_STATIC_DRAW
	);

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

	const std::uint64_t stagingBytes = static_cast<std::uint64_t>(vertices.capacity() * sizeof(Vertex));
	m_trackedGpuBytes = static_cast<std::uint64_t>(m_vertexCount * sizeof(Vertex));
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
}

Mesh::Mesh(std::vector<Vertex> vertices,
		std::vector<unsigned int> indices,
		   Material* material,
		   std::shared_ptr<Material> ownedMaterial,
           const std::string& materialXmlPathIn)
{
	m_geometry = std::make_shared<MeshGeometry>(ComputeTBNVertices(vertices, indices));
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

void Mesh::Draw(Shader* shader)
{
    if (!material_ptr) {
        return;
    }

	// XML materials are refreshed once while preparing the scene render data.
	auto materialGaurd = MaterialGaurd(*material_ptr, shader);
	GLState::ActiveTexture(GL_TEXTURE0);
	GLState::BindVertexArray(GetVAO());
	PerformanceProfiler::GetInstance().RecordDraw(GL_TRIANGLES, GetVertexCount());
	glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(GetVertexCount()));
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
		m_shader = ShaderManager::GetInstance().GetShaderByName("phong");
	}
	std::string shaderName = (m_shader ? m_shader->shaderName : std::string("phong"));
	std::string cacheKey = path + "|shader=" + shaderName + (mat ? "|mat=custom" : "|mat=default");
	if (!mat) {
		auto it = g_modelMeshCache.find(cacheKey);
		if (it != g_modelMeshCache.end()) {
			meshes = it->second;
			directory = path.substr(0, path.find_last_of('/'));
			return;
		}
	}

	Assimp::Importer importer;
	const aiScene* scene = importer.ReadFile(path, aiProcess_Triangulate | aiProcess_FlipUVs);

	if(!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
		std::cout << "ERROR::ASSIMP:: " << importer.GetErrorString() << std::endl;
		return;
	}
	// 防止 meshes 在 push_back 时多次扩容触发移动/销毁链，先按 Assimp 总 mesh 数预留容量
	meshes.clear();
	meshes.reserve(scene->mNumMeshes);
	directory = path.substr(0, path.find_last_of('/'));
	processNode(scene->mRootNode, scene,mat);
	if (!mat) {
		g_modelMeshCache[cacheKey] = meshes;
	}
}

void Model::processNode(aiNode* node, const aiScene* scene,Material* mat)
{
	for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
		aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
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
			mat->SetRenderState({ true,true,false,BlendMode::None,CullMode::Back });
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

Mesh Model::processMesh(aiMesh* mesh, const aiScene* scene,Material* mat)
{
	std::vector<Vertex> vertices;
	std::vector<unsigned int>indices;
	Material* material = mat;
	const std::string materialShaderName = m_shader ? m_shader->shaderName : std::string("phong");
	vertices.reserve(mesh->mNumVertices);
	indices.reserve(static_cast<size_t>(mesh->mNumFaces) * 3);
	for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
		Vertex vertex;
		glm::vec3 vector;
		vector.x = mesh->mVertices[i].x;
		vector.y = mesh->mVertices[i].y;
		vector.z = mesh->mVertices[i].z;
		vertex.Position = vector;
		vector.x = mesh->mNormals[i].x;
		vector.y = mesh->mNormals[i].y;
		vector.z = mesh->mNormals[i].z;
		vertex.Normal = vector;
		if (mesh->mTextureCoords[0]) {
			glm::vec2 vec;
			vec.x = mesh->mTextureCoords[0][i].x;
			vec.y = mesh->mTextureCoords[0][i].y;
			vertex.TexCoords = vec;
		}
		else {
			vertex.TexCoords = glm::vec2(0.0f, 0.0f);
		}
		vertices.push_back(vertex);
	}
	for (unsigned int i = 0; i < mesh->mNumFaces; ++i) {
		const aiFace& face = mesh->mFaces[i];
		for (unsigned int j = 0; j < face.mNumIndices; ++j) {
			indices.push_back(face.mIndices[j]);
		}
	}
	std::cout << "[Mesh Debug] Name: " << mesh->mName.C_Str()
		<< " | Vertices: " << mesh->mNumVertices
		<< " | MatID: " << mesh->mMaterialIndex << std::endl;
	std::string xmlPath;
	if(mat) {
		return Mesh(vertices, indices, mat, std::shared_ptr<Material>(), std::string());
	}
	else if (mesh->mMaterialIndex >= 0) {
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
			std::shared_ptr<Material> ownedMaterial = std::make_shared<Material>(materialShaderName);
			material = ownedMaterial.get();
			prosessMaterial(aiMat, material);
			return Mesh(vertices, indices, material, ownedMaterial, std::string());
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
			return Mesh(vertices,indices, material, ownedMaterial, std::string());
        }
	}
	return Mesh(vertices,indices, material, nullptr, xmlPath);
}

void Model::prosessMaterial(aiMaterial* mat,Material* material)
{
	aiColor3D color(0.0f, 0.0f, 0.0f);
	float shininess = 0.0f;
	float opacity = 1.0f;
	if (mat->Get(AI_MATKEY_COLOR_AMBIENT, color) == AI_SUCCESS)
		material->AddProperty("ambient", MaterialProperty::CreateVec3(glm::vec3(color.r, color.g, color.b)));
	if (mat->Get(AI_MATKEY_COLOR_DIFFUSE, color) == AI_SUCCESS)
		material->AddProperty("diffuse", MaterialProperty::CreateVec3(glm::vec3(color.r, color.g, color.b)));
	if (mat->Get(AI_MATKEY_COLOR_SPECULAR, color) == AI_SUCCESS)
		material->AddProperty("specular", MaterialProperty::CreateVec3(glm::vec3(color.r, color.g, color.b)));
	if (mat->Get(AI_MATKEY_SHININESS, shininess) == AI_SUCCESS)
		material->AddProperty("shininess", MaterialProperty::CreateFloat(shininess));
	if (mat->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS)
		material->AddProperty("opacity", MaterialProperty::CreateFloat(opacity));
	
	material->AddProperty("texture_diffuse", MaterialProperty::CreateTexture(loadMaterialTextures(mat, aiTextureType_DIFFUSE, "texture_diffuse")));
	material->AddProperty("texture_specular", MaterialProperty::CreateTexture(loadMaterialTextures(mat, aiTextureType_SPECULAR, "texture_specular")));
	material->AddProperty("texture_normal", MaterialProperty::CreateTexture(loadMaterialTextures(mat, aiTextureType_NORMALS, "texture_normal")));
	// For cloth/card-like alpha textures use cutout by default to avoid transparent sorting artifacts.
	bool autoCutout = opacity < 0.999f;
	material->AddProperty("useAlphaCutoff", MaterialProperty::CreateBool(autoCutout));
	material->AddProperty("alphaCutoff", MaterialProperty::CreateFloat(autoCutout ? 0.4f : 0.0f, 0.0f, 1.0f, 0.01f));

	material->AddProperty("useBloom", MaterialProperty::CreateBool(false));
}

std::vector<Texture> Model::loadMaterialTextures(aiMaterial* mat, aiTextureType type, std::string typeName)
{
	std::vector<Texture> textures;
	for (unsigned int i = 0; i < mat->GetTextureCount(type); i++)
	{
		aiString str;
		mat->GetTexture(type, i, &str);
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
			const bool srgb = typeName == "texture_diffuse";
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
	// Assimp Triangulate 理论上应满足 size%3==0，但仍做防御：
	// - 若 indices 不合法/为空，则保持原 vertices，避免生成空数组触发后续 UB。
	if (vertices.empty() || indices.empty()) {
		return vertices;
	}
	if (indices.size() % 3 != 0) {
		std::cout << "warning: indices size is not multiple of 3, skip TBN recompute" << std::endl;
		return vertices;
	}

	for (unsigned int idx : indices) {
		if (idx >= vertices.size()) {
			std::cout << "warning: index out of range, skip TBN recompute" << std::endl;
			return vertices;
		}
	}

	std::vector<Vertex> ret;
	ret.reserve(indices.size());
	for (size_t i = 0; i < indices.size(); i += 3) {
		auto aPoint = vertices[indices[i]];
		auto bPoint = vertices[indices[i + 1]];
		auto cPoint = vertices[indices[i + 2]];
		ComputeTBN(aPoint, bPoint, cPoint);
		ret.push_back(aPoint);
		ret.push_back(bPoint);
		ret.push_back(cPoint);
	}
	return ret;
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
