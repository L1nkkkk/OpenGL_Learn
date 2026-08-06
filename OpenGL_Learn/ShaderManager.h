#pragma once

#include "Shader.h"
#include "Global.h"
#include "GLStateCache.h"
#include <memory>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>

// std140 mirror of the shader SystemProperties block: 80 bytes.
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
	int shadowSamplingPattern;
	int shadowOptimizationFlags;
	int shadowAdaptiveMinSamples;
	float shadowBias2DMinTexels;
	float shadowBias2DSlopeTexels;
	float shadowBiasCubeMinTexels;
	float shadowBiasCubeSlopeTexels;
};
static_assert(sizeof(SystemUBOData) == 80);

struct UBOInfo {
	unsigned int UBO;
	unsigned int bindingPoint;
	unsigned int size;
};

class ShaderManager {
public:
	enum ShaderType {
		Scene = 0,
		DebugScene,
		Phong,
		Pbr,
		Grass,
		Skybox,
		Mirror,
		Outline,
		Default,
		Diffuse,
		Shadow,
		Bulr,
		DeferProcess,
		DeferProcessReconstruct,
		Defer,
		DeferDirLightVolume,
		LightVolume,
		LightVolumeFullscreen,
		PointLightGrid,
		SSAO,
		SSAOUpsample,
		//StartGeometryShaderIndex
		Explode,
		NormalLines,
		ShadowCube,
		ShadowCubeFace,
	};

	enum UniformBufferType {
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
	bool ReloadShader(const std::string& name, bool force = true);
	int ReloadChangedShaders();
	int ReloadAllShaders();
	std::shared_ptr<Shader> GetShader(int index);
	std::shared_ptr<Shader> GetShaderByName(std::string name);
	std::vector<std::string> GetNames();
	int GetShaderIndexByShader(std::shared_ptr<Shader> shaderPtr);
	void SetUBOData(UniformBufferType uboType, unsigned int offset, size_t size, const void* dataPtr);
	/// 从 SystemProperties 同步到 SystemProperties UBO，每帧或配置变更时调用一次即可，所有使用该 UBO 的 Shader 自动获得
	void UpdateSystemUBO();

	const std::string& GetLastReloadMessage() const { return m_lastReloadMessage; }
	bool WasLastReloadSuccessful() const { return m_lastReloadSuccessful; }
	int GetReloadCount() const { return m_reloadCount; }

	void UseShader(std::string name) {
		if (m_shaderMap.find(name) != m_shaderMap.end()) {
			m_shaderMap[name]->use();
		}
		else {
			for(int i = 0; i < shaderNames.size(); ++i){
				if(shaderNames[i] == name){
					const bool geometryShader =
						std::find(
							geometryShaderNames.begin(),
							geometryShaderNames.end(),
							name) != geometryShaderNames.end();
					if (geometryShader) {
						LoadGeometryShader(name);
					}
					else {
						LoadShader(name);
					}
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
	std::string m_lastReloadMessage;
	bool m_lastReloadSuccessful = true;
	int m_reloadCount = 0;
	std::vector<std::string> shaderNames = {
		"scene",
		"debugScene",
		"phong",
		"pbr",
		"grass",
		"skybox",
		"mirror",
		"outline",
		"default",
		"diffuse",
		"shadow",
		"bulr",
		"deferProcess",
		"deferProcessReconstruct",
		"defer",
		"deferDirLightVolume",
		"lightVolume",
		"lightVolumeFullscreen",
		"pointLightGrid",
		"ssao",
		"ssaoUpsample",
		"explode",
		"normal",
		"shadowCube",
		"shadowCubeFace"
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
			GLState::ActiveTexture(GL_TEXTURE0 + i);
			GLState::BindTexture(GL_TEXTURE_2D, 0);
			GLState::BindTexture(GL_TEXTURE_CUBE_MAP, 0);
		}
	}
};
