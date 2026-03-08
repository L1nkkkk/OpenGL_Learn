#include "ShaderManager.h"

void ShaderManager::Init() {
	//load Shaders
	for (int i = 0; i < shaderNames.size(); ++i) {
		LoadShader(shaderNames[i]);
		shader2Idx[m_shaderMap[shaderNames[i]]] = i;
	}
	//load Geometry Shaders
	for (int i = 0; i < geometryShaderNames.size(); ++i) {
		LoadGeometryShader(geometryShaderNames[i]);
		shader2Idx[m_shaderMap[geometryShaderNames[i]]] = shaderNames.size() + i;
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

void ShaderManager::LoadShader(std::string name) {
	m_shaderMap[name] = std::make_shared<Shader>(name);
	m_shaderMap[name]->shaderName = name;
	std::cout << "Loaded shader: " << name << std::endl;
}

void ShaderManager::LoadGeometryShader(std::string name) {
	std::string vertexPath = "shaders/" + name + "Vertex.glsl";
	std::string geometryPath = "shaders/" + name + "Geometry.glsl";
	std::string fragmentPath = "shaders/" + name + "Fragment.glsl";
	m_shaderMap[name] = std::make_shared<GeometryShader>(vertexPath.c_str(), geometryPath.c_str(), fragmentPath.c_str());
	m_shaderMap[name]->shaderName = name;
	std::cout << "Loaded shader: " << name << std::endl;
}

void ShaderManager::SetUBOData(UniformBufferType uboType, unsigned int offset, size_t size,const void* dataPtr) {
	UBOInfo& uboInfo = UBOInfos[uboType];
	glBindBuffer(GL_UNIFORM_BUFFER, uboInfo.UBO);
	glBufferSubData(GL_UNIFORM_BUFFER, offset, size, dataPtr);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

std::shared_ptr<Shader> ShaderManager::GetShader(int index) {
	assert(index < m_shaderMap.size() && "³¬¹ıShader·ÃÎÊ·¶Î§£¡");
	if (index < 0 || index > shaderNames.size()+geometryShaderNames.size()) return nullptr;
	return GetShaderByName(shaderNames[index]);
}

std::shared_ptr<Shader> ShaderManager::GetShaderByName(std::string name) {
	if (m_shaderMap.find(name) != m_shaderMap.end()) return m_shaderMap[name];
	else return nullptr;
}

std::vector<std::string> ShaderManager::GetNames() {
	return shaderNames;
}

int ShaderManager::GetShaderIndexByShader(std::shared_ptr<Shader> shaderPtr) {
	return shader2Idx[shaderPtr];
}