#pragma once

#include "Shader.h"
#include "Global.h"
#include <memory>
#include <utility>
#include <vector>
#include <unordered_map>

// std140 布局与 shader 中 SystemProperties 块一致：4*bool + 3*float + 6*int，共 52 字节，对齐到 64
struct alignas(16) SystemUBOData {
	int useBloom;
	int useShadowMap;
	int useGamma;
	int useHDR;
	float bloomThreshold;
	float gamma;
	float exposure;
	int bloomBlurIterations;
	int shadowSampleNum;
	int shadowSampleRings;
	int shadowType;
	int screenWidth;
	int screenHeight;
	int padding[3];
};

struct UBOInfo {
	unsigned int UBO;
	unsigned int bindingPoint;
	unsigned int size;
};

class ShaderManager {
public:
	static enum ShaderType {
		Scene = 0,
		DebugScene,
		Phong,
		Grass,
		Skybox,
		Mirror,
		Outline,
		Default,
		Diffuse,
		Shadow,
		Bulr,
		DeferProcess,
		Defer,
		DeferDirLightVolume,
		LightVolume,
		LightVolumeFullscreen,
		//StartGeometryShaderIndex
		Explode,
		NormalLines,
		ShadowCube,
	};

	static enum UniformBufferType {
		Matrices = 0,
		SystemProperties = 1
	};

	static ShaderManager& GetInstance() {
		static ShaderManager instance;
		return instance;
	}

	void Init();

	void LoadShader(std::string name);
	void LoadGeometryShader(std::string name);
	std::shared_ptr<Shader> GetShader(int index);
	std::shared_ptr<Shader> GetShaderByName(std::string name);
	std::vector<std::string> GetNames();
	int GetShaderIndexByShader(std::shared_ptr<Shader> shaderPtr);
	void SetUBOData(UniformBufferType uboType, unsigned int offset, size_t size, const void* dataPtr);
	/// 从 SystemProperties 同步到 SystemProperties UBO，每帧或配置变更时调用一次即可，所有使用该 UBO 的 Shader 自动获得
	void UpdateSystemUBO();

	void UseShader(std::string name) {
		if (m_shaderMap.find(name) != m_shaderMap.end()) {
			m_shaderMap[name]->use();
		}
		else {
			for(int i = 0; i < shaderNames.size(); ++i){
				if(shaderNames[i] == name){
					LoadShader(name);
					m_shaderMap[name]->use();
					return;
				}
			}
			for(int i = 0; i < geometryShaderNames.size(); ++i){
				if(geometryShaderNames[i] == name){
					LoadGeometryShader(name);
					m_shaderMap[name]->use();
					return;
				}
			}
			std::cout << "Shader not found: " << name << std::endl;
		}
	}
	ShaderManager(const ShaderManager&) = delete;
	ShaderManager& operator=(const ShaderManager&) = delete;

private:
	std::unordered_map<std::string, std::shared_ptr<Shader>> m_shaderMap;
	std::unordered_map<std::shared_ptr<Shader>, int> shader2Idx;
	std::vector<UBOInfo> UBOInfos;
	std::vector<std::string> shaderNames = {
		"scene",
		"debugScene",
		"phong",
		"grass",
		"skybox",
		"mirror",
		"outline",
		"default",
		"diffuse",
		"shadow",
		"bulr",
		"deferProcess",
		"defer",
		"deferDirLightVolume",
		"lightVolume",
		"lightVolumeFullscreen",
		"explode",
		"normal",
		"shadowCube"
	};
	std::vector<std::string> geometryShaderNames = {
		"explode",
		"normal",
		"shadowCube",
	};
	ShaderManager() = default;
};

class ShaderGaurd {
public:
	ShaderGaurd(std::string shaderName) {
		ShaderManager::GetInstance().UseShader(shaderName);
	}
	~ShaderGaurd() {
		//�����������������Ժ�����Ⱦ��ɸ���
		int TextureUsedNum = SystemProperties::GetInstance().USED_TEXTURE_NUM;
		for(int i = 0; i < TextureUsedNum; ++i) {
			glActiveTexture(GL_TEXTURE0 + i);
			glBindTexture(GL_TEXTURE_2D, 0);
			glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
		}
	}
};