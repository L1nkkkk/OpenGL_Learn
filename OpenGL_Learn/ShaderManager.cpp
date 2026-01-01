#include "ShaderManager.h"

void ShaderManager::Init() {
	//load Shaders
	for (int i = 0; i < shaderNames.size(); ++i) {
		std::string vertexPath = "shaders/" + shaderNames[i] + "Vertex.vs";
		std::string fragmentPath = "shaders/" + shaderNames[i] + "Fragment.fs";
		Shader* shader = new Shader(vertexPath.c_str(), fragmentPath.c_str());
		shaderMap[shaderNames[i]] = shader;
		shader2Idx[shader] = i;
		std::cout << "Loaded shader: " << shaderNames[i] << std::endl;
	}
	//load Geometry Shaders
	for (int i = 0; i < geometryShaderNames.size(); ++i) {
		std::string vertexPath = "shaders/" + geometryShaderNames[i] + "Vertex.vs";
		std::string geometryPath = "shaders/" + geometryShaderNames[i] + "Geometry.gs";
		std::string fragmentPath = "shaders/" + geometryShaderNames[i] + "Fragment.fs";
		Shader* shader = new GeometryShader(vertexPath.c_str(), geometryPath.c_str(), fragmentPath.c_str());
		//std::cout << shader << std::endl;
		shaderMap[geometryShaderNames[i]] = shader;
		shader2Idx[shader] = shaderNames.size() + i;
		std::cout << "Loaded shader: " << geometryShaderNames[i] << std::endl;
	}
	//bind uniform buffer objects
	unsigned int matricesUBO;
	glGenBuffers(1, &matricesUBO);
	glBindBuffer(GL_UNIFORM_BUFFER, matricesUBO);
	glBufferData(GL_UNIFORM_BUFFER, 2 * sizeof(glm::mat4), NULL, GL_STATIC_DRAW);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);
	glBindBufferRange(GL_UNIFORM_BUFFER, UniformBufferType::Matrices, matricesUBO, 0, 2 * sizeof(glm::mat4));
	UBOInfos.push_back({ matricesUBO, UniformBufferType::Matrices, 2 * sizeof(glm::mat4) });
}

void ShaderManager::SetUBOData(UniformBufferType uboType, unsigned int offset, size_t size,const void* dataPtr) {
	UBOInfo& uboInfo = UBOInfos[uboType];
	glBindBuffer(GL_UNIFORM_BUFFER, uboInfo.UBO);
	glBufferSubData(GL_UNIFORM_BUFFER, offset, size, dataPtr);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

Shader* ShaderManager::GetShader(int index) {
	assert(index < shaderMap.size() && "³¬¹ýShader·ÃÎÊ·¶Î§£¡");
	if (index < 0 || index > shaderNames.size()+geometryShaderNames.size()) return nullptr;
	if (index < shaderNames.size()) return shaderMap[shaderNames[index]];
	else return shaderMap[geometryShaderNames[index - shaderNames.size()]];
}

Shader* ShaderManager::GetShaderByName(std::string name) {
	if (shaderMap.find(name) != shaderMap.end()) return shaderMap[name];
	else return nullptr;
}

std::vector<std::string> ShaderManager::GetNames() {
	return shaderNames;
}

int ShaderManager::GetShaderIndexByShader(Shader* shaderPtr) {
	return shader2Idx[shaderPtr];
}