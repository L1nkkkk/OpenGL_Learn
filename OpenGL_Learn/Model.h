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
	unsigned int start_tex_index;

	Mesh(std::vector<Vertex> vertices,
		std::vector<unsigned int> indices,
		 Material* material);
	void Draw();

	unsigned int GetVAO() {
		return VAO;
	}

private:
	unsigned int VAO, VBO, EBO;
	void setupMesh();
};

class Model : public BaseObject {
public:
	Model(std::string path) {
		position = glm::vec3(0.0f);
		rotation = glm::vec3(0.0f);
		loadModel(path);
		localCenter = CalculateLocalCenter();
		name = "model" + std::to_string(count++);
	}
	Model(std::string path,std::shared_ptr<Shader> shader) {
		position = glm::vec3(0.0f);
		rotation = glm::vec3(0.0f);
		m_shader = shader;
		loadModel(path);
		localCenter = CalculateLocalCenter();
		name = "model" + std::to_string(count++);
	}
	Model(std::string path, glm::mat4 matrix) {
		position = glm::vec3(0.0f);
		rotation = glm::vec3(0.0f);
		loadModel(path);
		localCenter = CalculateLocalCenter();
		setModelMatrix(matrix);
		name = "model" + std::to_string(count++);
	}
	Model(std::vector<Mesh> inputMeshes) {
		position = glm::vec3(0.0f);
		rotation = glm::vec3(0.0f);
		meshes = inputMeshes;
		localCenter = CalculateLocalCenter();
		name = "model" + std::to_string(count++);
	}
	Model(std::vector<Mesh> inputMeshes, glm::mat4 matrix) {
		position = glm::vec3(0.0f);
		rotation = glm::vec3(0.0f);
		meshes = inputMeshes;
		localCenter = CalculateLocalCenter();
		setModelMatrix(matrix);
		name = "model" + std::to_string(count++);
	}

	Model(std::string path, Material* mat) {
		position = glm::vec3(0.0f);
		rotation = glm::vec3(0.0f);
		
		m_shader = ShaderManager::GetInstance().GetShaderByName(mat->GetShaderName());
		loadModel(path,mat);
		localCenter = CalculateLocalCenter();
		name = "model" + std::to_string(count++);
	}

	void Draw(Shader& shader, unsigned int start_tex_index = 0);
	glm::mat4 modelMatrix = glm::mat4(1.0f);

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
	std::string name;
	inline static unsigned int count = 0;
protected:
	std::vector<Mesh> meshes;
	void loadModel(std::string path,Material* mat = nullptr);
	Mesh processMesh(aiMesh* mesh, const aiScene* scene, Material* mat = nullptr);
	void processNode(aiNode* node, const aiScene* scene, Material* mat = nullptr);
	std::vector<Texture> loadMaterialTextures(aiMaterial* mat, aiTextureType type, std::string typeName);
	void prosessMaterial(aiMaterial* mat,Material* material);
	glm::vec3 CalculateLocalCenter();

	SystemProperties& properties = SystemProperties::GetInstance();
};

std::vector<Vertex> ComputeTBNVertices(std::vector<Vertex>& vertices, std::vector<unsigned int> indices);

void ComputeTBN(Vertex&, Vertex&, Vertex&);