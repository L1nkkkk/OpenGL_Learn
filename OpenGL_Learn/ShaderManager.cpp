#include "ShaderManager.h"

static void BindUniformBlocks(Shader& shader) {
	GLuint prog = shader.ID;

	// Matrices UBO
	GLuint idx = glGetUniformBlockIndex(prog, "Matrices");
	if (idx != GL_INVALID_INDEX) {
		glUniformBlockBinding(prog, idx, ShaderManager::UniformBufferType::Matrices);
	}

	// SystemProperties UBO
	idx = glGetUniformBlockIndex(prog, "SystemProperties");
	if (idx != GL_INVALID_INDEX) {
		glUniformBlockBinding(prog, idx, ShaderManager::UniformBufferType::SystemProperties);
	}
}

void ShaderManager::Init() {
	//load Shaders
	for (int i = 0; i < shaderNames.size(); ++i) {
		LoadShader(shaderNames[i]);
		shader2Idx[m_shaderMap[shaderNames[i]]] = i;
	}
	//load Geometry Shaders
	for (int i = 0; i < geometryShaderNames.size(); ++i) {
		// 某些 geometry shader 名称也在 shaderNames 里，避免重复加载同名 shader。
		if (m_shaderMap.find(geometryShaderNames[i]) == m_shaderMap.end()) {
			LoadGeometryShader(geometryShaderNames[i]);
		}
		shader2Idx[m_shaderMap[geometryShaderNames[i]]] = static_cast<int>(shaderNames.size()) + i;
	}
	//bind uniform buffer objects
	unsigned int matricesUBO;
	glGenBuffers(1, &matricesUBO);
	glBindBuffer(GL_UNIFORM_BUFFER, matricesUBO);
	glBufferData(GL_UNIFORM_BUFFER, 2 * sizeof(glm::mat4), NULL, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);
	glBindBufferRange(GL_UNIFORM_BUFFER, UniformBufferType::Matrices, matricesUBO, 0, 2 * sizeof(glm::mat4));
	UBOInfos.push_back({ matricesUBO, UniformBufferType::Matrices, 2 * sizeof(glm::mat4) });

	// SystemProperties UBO (binding 1) - 与 shader 中 layout(std140, binding=1) uniform SystemProperties 对应
	const size_t systemUBOSize = sizeof(SystemUBOData); // 13*4=52，对齐到 16 的倍数
	unsigned int systemUBO;
	glGenBuffers(1, &systemUBO);
	glBindBuffer(GL_UNIFORM_BUFFER, systemUBO);
	glBufferData(GL_UNIFORM_BUFFER, systemUBOSize, nullptr, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);
	glBindBufferRange(GL_UNIFORM_BUFFER, UniformBufferType::SystemProperties, systemUBO, 0, systemUBOSize);
	UBOInfos.push_back({ systemUBO, UniformBufferType::SystemProperties, systemUBOSize });
}

void ShaderManager::LoadShader(std::string name) {
	auto it = m_shaderMap.find(name);
	if (it == m_shaderMap.end()) {
		m_shaderMap[name] = std::make_shared<Shader>(name);
	}
	else {
		it->second->shaderName = name;
		it->second->SetSourcePaths("shaders/" + name + "Vertex.glsl", "shaders/" + name + "Fragment.glsl");
		it->second->Reload(true);
	}
	m_shaderMap[name]->shaderName = name;
	std::cout << "Loaded shader: " << name << std::endl;

	BindUniformBlocks(*m_shaderMap[name]);
}

void ShaderManager::LoadGeometryShader(std::string name) {
	std::string vertexPath = "shaders/" + name + "Vertex.glsl";
	std::string geometryPath = "shaders/" + name + "Geometry.glsl";
	std::string fragmentPath = "shaders/" + name + "Fragment.glsl";
	auto it = m_shaderMap.find(name);
	if (it == m_shaderMap.end()) {
		m_shaderMap[name] = std::make_shared<GeometryShader>(vertexPath.c_str(), geometryPath.c_str(), fragmentPath.c_str());
	}
	else {
		it->second->shaderName = name;
		it->second->SetSourcePaths(vertexPath, fragmentPath, geometryPath);
		it->second->Reload(true);
	}
	m_shaderMap[name]->shaderName = name;
	std::cout << "Loaded shader: " << name << std::endl;

	BindUniformBlocks(*m_shaderMap[name]);
}

bool ShaderManager::ReloadShader(const std::string& name, bool force)
{
	auto it = m_shaderMap.find(name);
	if (it == m_shaderMap.end() || !it->second) {
		m_lastReloadSuccessful = false;
		m_lastReloadMessage = "Shader not found: " + name;
		return false;
	}

	std::string errorMessage;
	const bool reloaded = it->second->Reload(force, &errorMessage);
	if (!errorMessage.empty()) {
		m_lastReloadSuccessful = false;
		m_lastReloadMessage = errorMessage;
		return false;
	}
	if (!reloaded) {
		return false;
	}

	BindUniformBlocks(*it->second);
	m_lastReloadSuccessful = true;
	m_lastReloadMessage = "Reloaded shader: " + name;
	++m_reloadCount;
	return true;
}

int ShaderManager::ReloadChangedShaders()
{
	int reloads = 0;
	for (auto& [name, shader] : m_shaderMap) {
		if (!shader || !shader->HasSourceChanges()) {
			continue;
		}
		if (ReloadShader(name, false)) {
			++reloads;
		}
	}
	return reloads;
}

int ShaderManager::ReloadAllShaders()
{
	int reloads = 0;
	for (auto& [name, shader] : m_shaderMap) {
		if (!shader) {
			continue;
		}
		if (ReloadShader(name, true)) {
			++reloads;
		}
	}
	return reloads;
}

void ShaderManager::SetUBOData(UniformBufferType uboType, unsigned int offset, size_t size,const void* dataPtr) {
	UBOInfo& uboInfo = UBOInfos[uboType];
	glBindBuffer(GL_UNIFORM_BUFFER, uboInfo.UBO);
	glBufferSubData(GL_UNIFORM_BUFFER, offset, size, dataPtr);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

std::shared_ptr<Shader> ShaderManager::GetShader(int index) {
	assert(index < m_shaderMap.size() && "����Shader���ʷ�Χ��");
	const int shaderCount = static_cast<int>(shaderNames.size());
	const int geometryCount = static_cast<int>(geometryShaderNames.size());
	if (index < 0 || index >= shaderCount + geometryCount) return nullptr;
	if (index < shaderCount) return GetShaderByName(shaderNames[index]);
	return GetShaderByName(geometryShaderNames[index - shaderCount]);
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



void ShaderManager::UpdateSystemUBO() {
	auto& p = SystemProperties::GetInstance();

	// 使用大括号初始化，未显式指定的字段（如 _padding）会自动清零
	SystemUBOData data = {};

	// 第一组
	data.useBloom = p.BLOOM ? 1 : 0;
	data.useShadowMap = p.SHADOW_MAP_SHOW ? 1 : 0;
	data.useGamma = p.GAMMA_CORRECTION ? 1 : 0;
	data.useHDR = p.USE_HDR ? 1 : 0;

	// 第二组
	data.bloomThreshold = p.BLOOM_THRESHOLD;
	data.gamma = p.GAMMA_VALUE;
	data.exposure = p.HDR_EXPOSURE;
	data.bloomBlurIterations = p.BLOOM_BLUR_ITERATIONS;

	// 第三组
	data.shadowSampleNum = p.SHADOW_PCF_SAMPLE_NUM;
	data.shadowSampleRings = p.SHADOW_PCF_RING_NUM;
	data.shadowType = p.SHADOW_TYPE;
	data.screenWidth = p.SCREEN_WIDTH;

	// 第四组
	data.screenHeight = p.SCREEN_HEIGHT;
	// data._padding 已经在上面 = {} 时被初始化为 0 了

	// 提交数据
	// sizeof(SystemUBOData) 现在应该是 64
	SetUBOData(UniformBufferType::SystemProperties, 0, sizeof(SystemUBOData), &data);
}
