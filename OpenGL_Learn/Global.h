#pragma once
#include <iostream>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <functional>
#include <tuple> 
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

extern bool DEBUG_MODE;

extern int SCREEN_WIDTH;
extern int SCREEN_HEIGHT;

extern int USED_TEXTURE_NUM;

extern int SHADOW_WIDTH;
extern int SHADOW_HEIGHT;
extern bool SHADOW_MAP_SHOW;
extern int SHADOW_PCF_SAMPLE_NUM;
extern int SHADOW_PCF_RING_NUM;
extern int SHADOW_TYPE;

extern float GAMMA_VALUE;
extern bool GAMMA_CORRECTION;

extern bool USE_HDR;
extern float HDR_EXPOSURE;

extern bool BLOOM;
extern float BLOOM_THRESHOLD;
extern int BLOOM_BLUR_ITERATIONS;

extern bool DEFER_RENDERING;

class AntiAliasManager {
public:
	static AntiAliasManager& GetInstance() {
		static AntiAliasManager instance;
		return instance;
	}
	AntiAliasManager(const AntiAliasManager&) = delete;
	AntiAliasManager& operator=(const AntiAliasManager&) = delete;

	AntiAliasManager() = default;

	static enum AntiAliasType {
		Default = 0,
		MSAA = 1,
	};

	inline static const char* optionsAA[] = {
		"DEFAULT",
		"MSAA"
	};

	inline static std::vector<unsigned int> frameBuffers;

	void AntiAliasByType(AntiAliasType);
	AntiAliasType antiAliasType;
private:

};

struct FBOAttributes {
	static enum FramebufferType {
		Framebuffer = 0,
		Multisample,
		ShadowMap,
		ShadowBox,
		HDR,
	};

	AntiAliasManager::AntiAliasType aaType = AntiAliasManager::AntiAliasType::Default;
	bool isHDR = false; 
	bool isShadowMap = false;
	bool isGamma = false;
	FramebufferType shadowType = FramebufferType::ShadowMap;
	bool isBloom = false;
	bool isDefer = false;

	bool operator==(const FBOAttributes& other) const {
		return std::tie(aaType, isHDR, isGamma,isShadowMap, shadowType,isBloom,isDefer) ==
			std::tie(other.aaType, other.isHDR, other.isGamma,other.isShadowMap, other.shadowType,other.isBloom,other.isDefer);
	}
};

namespace std {
	template<> struct hash<FBOAttributes> {
		size_t operator()(const FBOAttributes& attr) const {
			size_t hashVal = 0;
			unsigned int offset = 0;
			hashVal ^= hash<int>()(static_cast<int>(attr.aaType)) << offset;
			offset += 3;
			hashVal ^= hash<bool>()((attr.isHDR)) << offset;
			offset += 1;
			hashVal ^= hash<bool>()(attr.isShadowMap) << offset;
			offset += 1;
			hashVal ^= hash<int>()(static_cast<int>(attr.shadowType)) << offset;
			offset += 3;
			hashVal ^= hash<bool>()(static_cast<int>(attr.isGamma)) << offset;
			offset += 1;
			hashVal ^= hash<bool>()(attr.isBloom) << offset;
			offset += 1;
			hashVal ^= hash<bool>()(attr.isDefer) << offset;
			offset += 1;
			return hashVal;
		}
	};
}

class FBO {
public:
	static enum FrameRenderType {
		Default_FrameRenderType = 0,
		ShadowMap_FrameRenderType,
		BrightColor_FrameRenderType,
	};

	inline static const char* optionFrame[] = {
		"Default",
		"ShadowMap",
		"BrightColor",
	};
	bool isBusy = false;
	unsigned int framebufferID;
	std::vector<unsigned int> textureIDs;
	unsigned int rboID;
	bool init = false;

	FBOAttributes attr;

	FBO(FBOAttributes attr) {
		Init(attr);
	}

	void Delete() {
		glDeleteFramebuffers(1, &framebufferID);
		glDeleteTextures(textureIDs.size(), textureIDs.data());
		glDeleteRenderbuffers(1, &rboID);
	}
	void Init(FBOAttributes attr);

	void Resize() {
		Delete();
		Init(attr);
	}
};



class FramebuffersManager {
public:
	static unsigned int renderFBO;
	inline static FBO::FrameRenderType useType = FBO::Default_FrameRenderType;

	static FramebuffersManager& GetInstance() {
		static FramebuffersManager instance;
		return instance;
	}

	static FBOAttributes GenCurrentAttr() {
		FBOAttributes attr;
		attr.aaType = AntiAliasManager::GetInstance().antiAliasType;
		attr.isGamma = GAMMA_CORRECTION;
		attr.isHDR = USE_HDR;
		attr.isBloom = BLOOM;
		attr.isDefer = false;
		return attr;
	}

	void ReleaseFBO(FBO* fbo) {
		//ClearFBOBuffers(fbo);
		if (fbo == nullptr) return;
		fbo->isBusy = false;
	}

	void ClearFBOBuffers(FBO* fbo) {
		glBindFramebuffer(GL_FRAMEBUFFER, fbo->framebufferID);
		if (fbo->attr.isShadowMap) {
			glClear(GL_DEPTH_BUFFER_BIT);
		}
		else {
			for(auto& texID : fbo->textureIDs) {
				glClearBufferfv(GL_COLOR, 0, glm::value_ptr(glm::vec4(0.0f)));
			}
		}
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}

	FBO* GetFBO(FBOAttributes);

	void GenGBuffers(FBO* fbo,FBOAttributes attr);

	void Resize();
private:
	inline static FBOAttributes::FramebufferType FBOResizeableTpye[] = {
		FBOAttributes::FramebufferType::Framebuffer,
		FBOAttributes::FramebufferType::Multisample
	};

	std::unordered_map<FBOAttributes, std::vector<FBO*>> m_hashMapFBO;
};

enum class OtherShaderType {
	outline = 0,
	normalLines
};

class OtherShader {
public:
	static std::string OtherShaderTypeToString(OtherShaderType type) {
		switch (type) {
		case OtherShaderType::outline:
			return "outline";
		case OtherShaderType::normalLines:
			return "normalLines";
		default:
			return "unknown";
		}
	}

	inline static float normalLineMagnitude = 0.01;
};

namespace ShadowProperty {
	enum ShadowType {
		Default = 0,
		PCF,
		PCSS,
	};
	inline const char* ShadowTypeStrs[] = {
		"Default",
		"PCF",
		"PCSS",
	};
}

class BaseObject {
public:
	glm::vec3 position = glm::vec3(0);
	glm::vec3 rotation = glm::vec3(0);
	glm::vec3 scale = glm::vec3(1);
	bool m_active = true;
	glm::mat4 getModelMatrix();
	void setModelMatrix(glm::mat4);

	bool GetActiveStatus() {
		return m_active;
	}

	void SetActiveStatus(bool val) {
		m_active = val;
	}
protected:
	glm::mat4 modelMatrix;
};