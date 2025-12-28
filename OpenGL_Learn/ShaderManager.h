#pragma once

#include "Shader.h"
#include <vector>
#include <unordered_map>

struct UBOInfo {
	unsigned int UBO;
	unsigned int bindingPoint;
	unsigned int size;
};

class ShaderManager {
public:
	static enum ShaderType{
		Scene = 0,
		Phong,
		Grass,
		Skybox,
		Mirror,
		Default,
		//StartGeometryShaderIndex
		Explode
	};

	static enum UniformBufferType {
		Matrices = 0
	};


	static ShaderManager& GetInstance() {
		static ShaderManager instance;
		return instance;
	}

	void Init();
	Shader* GetShader(int index);
	Shader* GetShaderByName(std::string name);
	std::vector<std::string> GetNames();
	int GetShaderIndexByShader(Shader* shaderPtr);
	void SetUBOData(UniformBufferType uboType,unsigned int offset,size_t size,const void* dataPtr);

	ShaderManager(const ShaderManager&) = delete;
	ShaderManager& operator=(const ShaderManager&) = delete;
private:
	std::unordered_map<std::string, Shader*> shaderMap;
	std::unordered_map<Shader*, int> shader2Idx;
	std::vector<UBOInfo> UBOInfos;
	std::vector<std::string> shaderNames = {
		"scene",
		"phong",
		"grass",
		"skybox",
		"mirror",
		"default"
	};
	std::vector<std::string> geometryShaderNames = {
		"explode"
	};
	ShaderManager() = default;
};