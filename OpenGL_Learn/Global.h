#pragma once
#include <iostream>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <string>
#include <unordered_map>
#include <vector>

extern int SCREEN_WIDTH;
extern int SCREEN_HEIGHT;

extern float GAMMA_VALUE;
extern bool GAMMA_CORRECTION;

class FBO {
public:
	static enum FramebufferType{
		Framebuffer = 0,
		Multisample,
	};
	FramebufferType type;
	unsigned int framebufferID;
	unsigned int textureID;
	unsigned int rboID;
	bool init = false;

	FBO(FramebufferType type) {
		this->type = type;
	}

	void Delete() {
		glDeleteFramebuffers(1, &framebufferID);
		glDeleteTextures(1, &textureID);
		glDeleteRenderbuffers(1, &rboID);
	}
};

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

class FramebuffersManager {
public:
	static FramebuffersManager& GetInstance() {
		static FramebuffersManager instance;
		return instance;
	}
	std::unordered_map<FBO::FramebufferType, std::vector<FBO*>> FBOmap;

	void GenFBO(FBO*);
	
	void AddFBO(FBO*);

	size_t GetFramebuffersSize(FBO::FramebufferType type);

	void Resize();
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
