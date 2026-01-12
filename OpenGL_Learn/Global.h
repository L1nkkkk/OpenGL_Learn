#pragma once
#include <iostream>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <functional>
#include <tuple> 

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

	bool operator==(const FBOAttributes& other) const {
		return std::tie(aaType, isHDR, isGamma,isShadowMap, shadowType) ==
			std::tie(other.aaType, other.isHDR, other.isGamma,other.isShadowMap, other.shadowType);
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
			offset += 3;
			hashVal ^= hash<int>()(static_cast<int>(attr.shadowType)) << offset;
			offset += 1;
			hashVal ^= hash<bool>()(static_cast<int>(attr.isGamma)) << offset;

			return hashVal;
		}
	};
}

class FBO {
public:
	static enum FrameRenderType {
		Default_FrameRenderType = 0,
		ShadowMap_FrameRenderType,
	};

	inline static const char* optionFrame[] = {
		"Default",
		"ShadowMap",
	};
	bool isBusy = false;
	unsigned int framebufferID;
	unsigned int textureID;
	unsigned int rboID;
	bool init = false;

	FBOAttributes attr;

	FBO(FBOAttributes attr) {
		Init(attr);
	}

	void Delete() {
		glDeleteFramebuffers(1, &framebufferID);
		glDeleteTextures(1, &textureID);
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

	void ReleaseFBO(FBO* fbo) {
		fbo->isBusy = false;
	}
	
	FBO* GetFBO(FBOAttributes);



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