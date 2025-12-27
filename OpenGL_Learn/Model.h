#pragma once

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <string>
#include <vector>


#include "Shader.h"
#include "stb_image.h"



struct Vertex {
	glm::vec3 Position;
	glm::vec3 Normal;
	glm::vec2 TexCoords;
};

struct Texture {
	unsigned int id;
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
	std::vector<Texture> bumpTextures; // map_Bump 凹凸纹理

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
		bumpTextures = {};
	}
};

struct CubeTexture {
	unsigned int id;
	CubeTexture(std::string path) {
		glGenTextures(1, &id);
		glBindTexture(GL_TEXTURE_CUBE_MAP, id);
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
		glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
		stbi_set_flip_vertically_on_load(true);
	}
};

class Mesh {
public:
	std::vector<Vertex> vertices;
	std::vector<unsigned int> indices;
	Material material;

	Mesh(std::vector<Vertex> vertices, 
		 std::vector<unsigned int> indices, 
		 Material& material);
	void Draw(Shader& shader);
private:
	unsigned int VAO, VBO, EBO;
	void setupMesh();
};

class Model {
public:
	Model(std::string path) {
		position = glm::vec3(0.0f);
		rotation = glm::vec3(0.0f);
		loadModel(path);
		localCenter = CalculateLocalCenter();
	}
	Model(std::string path,Shader) {
		position = glm::vec3(0.0f);
		rotation = glm::vec3(0.0f);
		loadModel(path);
		localCenter = CalculateLocalCenter();
	}
	Model(std::string path, glm::mat4 matrix) {
		position = glm::vec3(0.0f);
		rotation = glm::vec3(0.0f);
		loadModel(path);
		localCenter = CalculateLocalCenter();
		setModelMatrix(matrix);
	}
	Model(std::vector<Mesh> inputMeshes) {
		position = glm::vec3(0.0f);
		rotation = glm::vec3(0.0f);
		meshes = inputMeshes;
		localCenter = CalculateLocalCenter();
	}
	Model(std::vector<Mesh> inputMeshes, glm::mat4 matrix) {
		position = glm::vec3(0.0f);
		rotation = glm::vec3(0.0f);
		meshes = inputMeshes;
		localCenter = CalculateLocalCenter();
		setModelMatrix(matrix);
	}
	void Draw(Shader& shader);
	glm::mat4 modelMatrix = glm::mat4(1.0f);
	void setModelMatrix(glm::mat4 matrix) {
		modelMatrix = matrix;
	}

	glm::mat4 getModelMatrix() {
		modelMatrix = glm::mat4(1.0f);
		modelMatrix = glm::translate(modelMatrix, position);
		modelMatrix = glm::rotate(modelMatrix, glm::radians(rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
		modelMatrix = glm::rotate(modelMatrix, glm::radians(rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
		modelMatrix = glm::rotate(modelMatrix, glm::radians(rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
		modelMatrix = glm::scale(modelMatrix, scale);
		return modelMatrix;
	}

	bool hasOutline = false;
	Shader* outlineShaderPtr = nullptr;
	glm::vec3 outlineColor = glm::vec3(0.0f);
	float outlineWidth = 0.05f;

	glm::vec3 position;
	glm::vec3 rotation;
	glm::vec3 scale = glm::vec3(1.0f);

	glm::vec3 GetWorldPosition() {
		return glm::vec3(modelMatrix[3]);
	}

	glm::vec3 GetLoacalCenter() {
		return localCenter;
	}
private:
	std::vector<Texture> textures_loaded;
	std::vector<Mesh> meshes;
	std::string directory;
	glm::vec3 localCenter;

	void loadModel(std::string path);
	Mesh processMesh(aiMesh* mesh, const aiScene* scene);
	void processNode(aiNode* node, const aiScene* scene);
	std::vector<Texture> loadMaterialTextures(aiMaterial* mat, aiTextureType type, std::string typeName);
	Material prosessMaterial(aiMaterial* mat);
	glm::vec3 CalculateLocalCenter();
};

unsigned int TextureFromFile(const char* path, const std::string& directory,bool alpha = false ,bool gamma = false);