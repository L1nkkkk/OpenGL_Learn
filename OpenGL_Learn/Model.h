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

struct Texture {
	unsigned int textureID;
	unsigned int textureGammaID;
	std::string type;
	aiString path;
};

struct Material {
	// 基础颜色（MTL中的Ka、Kd、Ks）
	glm::vec3 ambient;    // Ka
	glm::vec3 diffuse;    // Kd
	glm::vec3 specular;   // Ks
	float shininess;      // Ns（高光指数）
	float opacity;        // d（透明度，1=不透明）

	// 纹理ID（MTL中的map_Kd等）
	std::vector<Texture> diffuseTextures;    // map_Kd 漫反射纹理
	std::vector<Texture> specularTextures; // map_Ks 高光纹理
	std::vector<Texture> normalTextures; // map_Bump 凹凸纹理

	// 构造函数：初始化默认值（匹配MTL默认规则）
	Material()
	{
		ambient = glm::vec3(0.2f);
		diffuse = glm::vec3(0.8f);
		specular = glm::vec3(0.0f);
		shininess = 1.0f;
		opacity = 1.0f;
		diffuseTextures = {};
		specularTextures = {};
		normalTextures = {};
	}
};

class CubeTexture {
public:
	unsigned int textureID;
	unsigned int textureGammaID;
	CubeTexture(std::string path) {
		int width, height, nrChannels;
		unsigned char* data;
		std::string items[6] = {
			"right.jpg",
			"left.jpg",
			"top.jpg",
			"bottom.jpg",
			"front.jpg",
			"back.jpg"
		};
		stbi_set_flip_vertically_on_load(false);
		glGenTextures(1, &textureID);
		glBindTexture(GL_TEXTURE_CUBE_MAP, textureID);
		for(int i = 0; i < 6; ++i){
			data = stbi_load((path + '/' + items[i]).c_str(), &width, &height, &nrChannels, 0);
			if (data) {
				glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, data);
				stbi_image_free(data);
			}
			else {
				std::cout << "Cubemap texture failed to load at path: " << items[i] << std::endl;
				stbi_image_free(data);
			}
		}
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

		glGenTextures(1, &textureGammaID);
		glBindTexture(GL_TEXTURE_CUBE_MAP, textureGammaID);
		for (int i = 0; i < 6; ++i) {
			data = stbi_load((path + '/' + items[i]).c_str(), &width, &height, &nrChannels, 0);
			if (data) {
				glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_SRGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, data);
				stbi_image_free(data);
			}
			else {
				std::cout << "Cubemap texture failed to load at path: " << items[i] << std::endl;
				stbi_image_free(data);
			}
		}
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
		glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
		stbi_set_flip_vertically_on_load(true);
	}
private:
	CubeTexture() = default;
};

class Mesh {
public:
	std::vector<Vertex> vertices;

	Material material;
	unsigned int start_tex_index;

	Mesh(std::vector<Vertex> vertices,
		std::vector<unsigned int> indices,
		 Material& material);
	void Draw(Shader& shader);

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
	Model(std::string path,Shader) {
		position = glm::vec3(0.0f);
		rotation = glm::vec3(0.0f);
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
	void Draw(Shader& shader, unsigned int start_tex_index = 0);
	glm::mat4 modelMatrix = glm::mat4(1.0f);

	std::unordered_map<int,bool> otherShaderUse;
	std::unordered_map<int,Shader*> otherShaderPtr;
	glm::vec3 outlineColor = glm::vec3(0.0f);
	float outlineWidth = 0.05f;

	glm::vec3 GetWorldPosition() {
		return glm::vec3(modelMatrix[3]);
	}

	glm::vec3 GetLoacalCenter() {
		return localCenter;
	}

	void AddOtherShader(OtherShaderType type, Shader* shader) {
		otherShaderUse[static_cast<int>(type)] = false;
		otherShaderPtr[static_cast<int>(type)] = shader;
	}

	Shader* GetOtherShader(OtherShaderType type) {
		return otherShaderPtr[static_cast<int>(type)];
	}

	bool IsOtherShaderUsed(OtherShaderType type) {
		return otherShaderUse[static_cast<int>(type)];
	}

	std::vector<Mesh>& GetMeshes() {
		return meshes;
	}

	unsigned int GetTextureID(int index) {
		return GAMMA_CORRECTION?textures_loaded[index].textureGammaID: textures_loaded[index].textureID;
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

protected:
	std::vector<Texture> textures_loaded;
	std::vector<Mesh> meshes;
	std::string directory;
	glm::vec3 localCenter;
	std::string name;
	inline static unsigned int count = 0;

	void loadModel(std::string path);
	Mesh processMesh(aiMesh* mesh, const aiScene* scene);
	void processNode(aiNode* node, const aiScene* scene);
	std::vector<Texture> loadMaterialTextures(aiMaterial* mat, aiTextureType type, std::string typeName);
	Material prosessMaterial(aiMaterial* mat);
	glm::vec3 CalculateLocalCenter();
};

unsigned int TextureFromFile(const char* path, const std::string& directory,bool alpha = false ,bool gamma = false);

std::vector<Vertex> ComputeTBNVertices(std::vector<Vertex>& vertices, std::vector<unsigned int> indices);

void ComputeTBN(Vertex&, Vertex&, Vertex&);