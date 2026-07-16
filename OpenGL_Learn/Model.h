#pragma once

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <string>
#include <vector>
#include <tuple>
#include <unordered_map>
#include <utility>
#include "Global.h"
#include "Shader.h"
#include "Material.h"
#include "stb_image.h"


struct Vertex {
	glm::vec3 Position;
	glm::vec3 Normal;
	glm::vec2 TexCoords;
	glm::vec3 Tangent;
	glm::vec3 Bitangent;
	Vertex() = default;
	Vertex(glm::vec3 Pos,glm::vec3 Nor,glm::vec2 Tex):
		Position(Pos),Normal(Nor),TexCoords(Tex){}
};

class Mesh {
public:
	std::vector<Vertex> vertices;

	Material* material_ptr;
	std::shared_ptr<Material> material_owner;
	unsigned int start_tex_index;
    // 可选：该 Mesh 对应的材质 XML 路径（如 "materials/Wood.xml"），用于懒加载 + 热重载
    std::string materialXmlPath;

	Mesh(std::vector<Vertex> vertices,
		std::vector<unsigned int> indices,
		 Material* material,
		 std::shared_ptr<Material> ownedMaterial = nullptr,
         const std::string& materialXmlPathIn = std::string());
	Mesh(std::vector<Vertex> vertices,
		std::vector<unsigned int> indices,
		Material* material,
		const std::string& materialXmlPathIn);

	Mesh(const Mesh& other);
	Mesh& operator=(const Mesh& other);
	Mesh(Mesh&& other) noexcept;
	Mesh& operator=(Mesh&& other) noexcept;
	~Mesh();

	void Draw(Shader* shader = nullptr);

	unsigned int GetVAO() {
		return VAO;
	}

	bool GetActiveStatus() const {
		return m_active;
	}

	void SetActiveStatus(bool value) {
		m_active = value;
	}

private:
	bool m_active = true;
	unsigned int VAO, VBO, EBO;
	void ReleaseGL();
	void setupMesh();
};

class Model : public BaseObject {
public:
	// Model 的“数据来源”用于 Scene Save/Load：文件加载 or 程序生成。
	enum class DataSourceType {
		File,
		Generated
	};

	DataSourceType GetDataSourceType() const { return m_dataSourceType; }
	const std::string& GetDataSourceFilePath() const { return m_dataSourceFilePath; }
	const std::string& GetDataSourceGeneratorId() const { return m_dataSourceGeneratorId; }

	void SetDataSourceFile(const std::string& path) {
		m_dataSourceType = DataSourceType::File;
		m_dataSourceFilePath = path;
		m_dataSourceGeneratorId.clear();
	}

	void SetDataSourceGenerated(const std::string& generatorId) {
		m_dataSourceType = DataSourceType::Generated;
		m_dataSourceGeneratorId = generatorId;
		m_dataSourceFilePath.clear();
	}

	struct MeshEntry {
		Mesh* mesh = nullptr;
		Material* material = nullptr;
	};

	Model(std::string path) {
		position = glm::vec3(0.0f);
		rotation = glm::vec3(0.0f);
		m_dataSourceType = DataSourceType::File;
		m_dataSourceFilePath = path;
		loadModel(path);
		BuildMeshLists();
		localCenter = CalculateLocalCenter();
		name = "model" + std::to_string(count++);
	}
	Model(std::string path,std::shared_ptr<Shader> shader) {
		position = glm::vec3(0.0f);
		rotation = glm::vec3(0.0f);
		m_shader = shader;
		m_dataSourceType = DataSourceType::File;
		m_dataSourceFilePath = path;
		loadModel(path);
		BuildMeshLists();
		localCenter = CalculateLocalCenter();
		name = "model" + std::to_string(count++);
	}
	Model(std::string path, glm::mat4 matrix) {
		position = glm::vec3(0.0f);
		rotation = glm::vec3(0.0f);
		m_dataSourceType = DataSourceType::File;
		m_dataSourceFilePath = path;
		loadModel(path);
		BuildMeshLists();
		localCenter = CalculateLocalCenter();
		setModelMatrix(matrix);
		name = "model" + std::to_string(count++);
	}
	Model(std::vector<Mesh> inputMeshes) {
		position = glm::vec3(0.0f);
		rotation = glm::vec3(0.0f);
		m_dataSourceType = DataSourceType::Generated;
		meshes = std::move(inputMeshes);
		BuildMeshLists();
		localCenter = CalculateLocalCenter();
		name = "model" + std::to_string(count++);
	}
	Model(std::vector<Mesh> inputMeshes, glm::mat4 matrix) {
		position = glm::vec3(0.0f);
		rotation = glm::vec3(0.0f);
		m_dataSourceType = DataSourceType::Generated;
		meshes = std::move(inputMeshes);
		BuildMeshLists();
		localCenter = CalculateLocalCenter();
		setModelMatrix(matrix);
		name = "model" + std::to_string(count++);
	}

	Model(std::string path, Material* mat) {
		position = glm::vec3(0.0f);
		rotation = glm::vec3(0.0f);
		m_dataSourceType = DataSourceType::File;
		m_dataSourceFilePath = path;
		
		m_shader = ShaderManager::GetInstance().GetShaderByName(mat->GetShaderName());
		loadModel(path,mat);
		BuildMeshLists();
		localCenter = CalculateLocalCenter();
		name = "model" + std::to_string(count++);
	}

	void Draw(Shader* shader = nullptr, unsigned int start_tex_index = 0);

	std::unordered_map<int,bool> otherShaderUse;
	std::unordered_map<int,std::shared_ptr<Shader>> otherShaderPtr;
	glm::vec3 outlineColor = glm::vec3(0.0f);
	float outlineWidth = 0.05f;

	std::shared_ptr<Shader> GetShader() {
		return m_shader;
	}

	void SetShader(std::shared_ptr<Shader> shader) {
		m_shader = shader;
	}

	glm::vec3 GetWorldPosition() {
		return glm::vec3(modelMatrix[3]);
	}

	glm::vec3 GetLoacalCenter() {
		return localCenter;
	}

	float GetLocalBoundingRadius() const {
		return localBoundingRadius;
	}

	void AddOtherShader(OtherShaderType type, std::shared_ptr<Shader> shader) {
		otherShaderUse[static_cast<int>(type)] = false;
		otherShaderPtr[static_cast<int>(type)] = shader;
	}

	std::shared_ptr<Shader> GetOtherShader(OtherShaderType type) {
		return otherShaderPtr[static_cast<int>(type)];
	}

	bool IsOtherShaderUsed(OtherShaderType type) {
		return otherShaderUse[static_cast<int>(type)];
	}

	std::vector<Mesh>& GetMeshes() {
		return meshes;
	}

	void RefreshMaterialDrivenState();
	const std::vector<MeshEntry>& GetOpaqueMeshEntries() const { return m_opaqueMeshes; }
	const std::vector<MeshEntry>& GetTransparentMeshEntries() const { return m_transparentMeshes; }

	unsigned int GetTextureID(int index) {
		return (properties.GAMMA_CORRECTION)?textures_loaded[index].textureGammaID: textures_loaded[index].textureID;
	}

	std::string GetName() {
		return name;
	}

	void SetName(std::string name) {
		this->name = name;
	}

	bool GetAcitveStatus() {
		return m_active;
	}

	void SetActiveStatus(bool val) {
		m_active = val;
	}
private:
	std::shared_ptr<Shader> m_shader;
	std::vector<Texture> textures_loaded;
	std::string directory;
	glm::vec3 localCenter;
	float localBoundingRadius = 0.0f;
	std::string name;
	DataSourceType m_dataSourceType = DataSourceType::Generated;
	std::string m_dataSourceFilePath;
	std::string m_dataSourceGeneratorId;
	size_t m_lastAppliedMaterialRevision = static_cast<size_t>(-1);
	inline static unsigned int count = 0;
protected:
	std::vector<Mesh> meshes;
	std::vector<MeshEntry> m_opaqueMeshes;
	std::vector<MeshEntry> m_transparentMeshes;
	void loadModel(std::string path,Material* mat = nullptr);
	Mesh processMesh(aiMesh* mesh, const aiScene* scene, Material* mat = nullptr);
	void processNode(aiNode* node, const aiScene* scene, Material* mat = nullptr);
	void BuildMeshLists();
	std::vector<Texture> loadMaterialTextures(aiMaterial* mat, aiTextureType type, std::string typeName);
	void prosessMaterial(aiMaterial* mat,Material* material);
	glm::vec3 CalculateLocalCenter();

	SystemProperties& properties = SystemProperties::GetInstance();
};

std::vector<Vertex> ComputeTBNVertices(std::vector<Vertex>& vertices, std::vector<unsigned int> indices);

void ComputeTBN(Vertex&, Vertex&, Vertex&);
