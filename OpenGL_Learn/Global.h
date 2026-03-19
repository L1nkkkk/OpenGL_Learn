#pragma once
#include "stb_image.h"
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

// Viewport 调试：从 FramebuffersManager 中当前 isBusy 的 FBO 里选一个，再选其某个 color/depth 附件展示

class SystemProperties {
public:
    SystemProperties(const SystemProperties&) = delete;
    SystemProperties& operator=(const SystemProperties&) = delete;
    SystemProperties(SystemProperties&&) = delete;
    SystemProperties& operator=(SystemProperties&&) = delete;

    static SystemProperties& GetInstance() {
        static SystemProperties instance;
        return instance;
    }

    bool DEBUG_MODE = false;

    // Viewport ???????е? FBO ?? GetBusyFBOs() ?е??±???е??????? FBO textureIDs ?е??±?
    int VIEWPORT_DEBUG_FBO_INDEX = 0;
    int VIEWPORT_DEBUG_ATTACHMENT_INDEX = 0;

    int SCREEN_WIDTH = 1440;
    int SCREEN_HEIGHT = 900;

    int USED_TEXTURE_NUM = 0;

    int SHADOW_WIDTH = 1024;
    int SHADOW_HEIGHT = 1024;
    bool SHADOW_MAP_SHOW = false;
    int SHADOW_PCF_SAMPLE_NUM = 16;
    int SHADOW_PCF_RING_NUM = 10;
    int SHADOW_TYPE = ShadowProperty::Default;

    bool GAMMA_CORRECTION = true;
    float GAMMA_VALUE = 2.2f;

    bool USE_HDR = false;
    float HDR_EXPOSURE = 1.0;

    bool BLOOM = false;
    float BLOOM_THRESHOLD = 1.0f;
    int BLOOM_BLUR_ITERATIONS = 5;

    bool DEFER_RENDERING = false;
    bool LIGHT_VOLUME = false;

    void ResetUsedTextureNum() {
        USED_TEXTURE_NUM = 0;
	}

private:
    SystemProperties() = default;
};

struct GlobalVAOs {
    unsigned int quadVAO, quadVBO;
    unsigned int cubeVAO, cubeVBO;
    unsigned int sphereVAO, sphereVBO;
};
extern GlobalVAOs globalVAOs;

inline float screenVertices[] = { // vertex attributes for a quad that fills the entire screen in Normalized Device Coordinates.
	// positions   // texCoords
	-1.0f,  1.0f,  0.0f, 1.0f,
	-1.0f, -1.0f,  0.0f, 0.0f,
	 1.0f, -1.0f,  1.0f, 0.0f,

	-1.0f,  1.0f,  0.0f, 1.0f,
	 1.0f, -1.0f,  1.0f, 0.0f,
	 1.0f,  1.0f,  1.0f, 1.0f
};

inline float cubeVertices[] = {
		-0.5f, -0.5f, -0.5f,
		 0.5f, -0.5f, -0.5f,
		 0.5f,  0.5f, -0.5f,
		 0.5f,  0.5f, -0.5f,
		-0.5f,  0.5f, -0.5f,
		-0.5f, -0.5f, -0.5f,

		-0.5f, -0.5f,  0.5f,
		 0.5f, -0.5f,  0.5f,
		 0.5f,  0.5f,  0.5f,
		 0.5f,  0.5f,  0.5f,
		-0.5f,  0.5f,  0.5f,
		-0.5f, -0.5f,  0.5f,

		-0.5f,  0.5f,  0.5f,
		-0.5f,  0.5f, -0.5f,
		-0.5f, -0.5f, -0.5f,
		-0.5f, -0.5f, -0.5f,
		-0.5f, -0.5f,  0.5f,
		-0.5f,  0.5f,  0.5f,

		 0.5f,  0.5f,  0.5f,
		 0.5f,  0.5f, -0.5f,
		 0.5f, -0.5f, -0.5f,
		 0.5f, -0.5f, -0.5f,
		 0.5f, -0.5f,  0.5f,
		 0.5f,  0.5f,  0.5f,

		-0.5f, -0.5f, -0.5f,
		 0.5f, -0.5f, -0.5f,
		 0.5f, -0.5f,  0.5f,
		 0.5f, -0.5f,  0.5f,
		-0.5f, -0.5f,  0.5f,
		-0.5f, -0.5f, -0.5f,

		-0.5f,  0.5f, -0.5f,
		 0.5f,  0.5f, -0.5f,
		 0.5f,  0.5f,  0.5f,
		 0.5f,  0.5f,  0.5f,
		-0.5f,  0.5f,  0.5f,
		-0.5f,  0.5f, -0.5f
};

inline float sphereVertices[] = {
    // ???????? x, y, z
    0.0000f,  0.5000f,  0.0000f,
    0.0975f,  0.4903f,  0.0000f,
    0.0935f,  0.4903f,  0.0309f,
    0.0814f,  0.4806f,  0.0618f,
    0.0618f,  0.4806f,  0.0814f,
    0.0309f,  0.4903f,  0.0935f,
    0.0000f,  0.5000f,  0.0975f,
    -0.0309f,  0.4903f,  0.0935f,
    -0.0618f,  0.4806f,  0.0814f,
    -0.0814f,  0.4806f,  0.0618f,
    -0.0935f,  0.4903f,  0.0309f,
    -0.0975f,  0.4903f,  0.0000f,
    -0.0935f,  0.4903f,  -0.0309f,
    -0.0814f,  0.4806f,  -0.0618f,
    -0.0618f,  0.4806f,  -0.0814f,
    -0.0309f,  0.4903f,  -0.0935f,
    0.0000f,  0.5000f,  -0.0975f,
    0.0309f,  0.4903f,  -0.0935f,
    0.0618f,  0.4806f,  -0.0814f,
    0.0814f,  0.4806f,  -0.0618f,
    0.0935f,  0.4903f,  -0.0309f,
    0.1951f,  0.4619f,  0.0000f,
    0.1871f,  0.4619f,  0.0618f,
    0.1629f,  0.4438f,  0.1236f,
    0.1236f,  0.4438f,  0.1629f,
    0.0618f,  0.4619f,  0.1871f,
    0.0000f,  0.4755f,  0.1951f,
    -0.0618f,  0.4619f,  0.1871f,
    -0.1236f,  0.4438f,  0.1629f,
    -0.1629f,  0.4438f,  0.1236f,
    -0.1871f,  0.4619f,  0.0618f,
    -0.1951f,  0.4619f,  0.0000f,
    -0.1871f,  0.4619f,  -0.0618f,
    -0.1629f,  0.4438f,  -0.1236f,
    -0.1236f,  0.4438f,  -0.1629f,
    -0.0618f,  0.4619f,  -0.1871f,
    0.0000f,  0.4755f,  -0.1951f,
    0.0618f,  0.4619f,  -0.1871f,
    0.1236f,  0.4438f,  -0.1629f,
    0.1629f,  0.4438f,  -0.1236f,
    0.1871f,  0.4619f,  -0.0618f,
    0.2929f,  0.4157f,  0.0000f,
    0.2806f,  0.4157f,  0.0924f,
    0.2443f,  0.3928f,  0.1848f,
    0.1848f,  0.3928f,  0.2443f,
    0.0924f,  0.4157f,  0.2806f,
    0.0000f,  0.4330f,  0.2929f,
    -0.0924f,  0.4157f,  0.2806f,
    -0.1848f,  0.3928f,  0.2443f,
    -0.2443f,  0.3928f,  0.1848f,
    -0.2806f,  0.4157f,  0.0924f,
    -0.2929f,  0.4157f,  0.0000f,
    -0.2806f,  0.4157f,  -0.0924f,
    -0.2443f,  0.3928f,  -0.1848f,
    -0.1848f,  0.3928f,  -0.2443f,
    -0.0924f,  0.4157f,  -0.2806f,
    0.0000f,  0.4330f,  -0.2929f,
    0.0924f,  0.4157f,  -0.2806f,
    0.1848f,  0.3928f,  -0.2443f,
    0.2443f,  0.3928f,  -0.1848f,
    0.2806f,  0.4157f,  -0.0924f,
    0.3827f,  0.3536f,  0.0000f,
    0.3660f,  0.3536f,  0.1225f,
    0.3165f,  0.3268f,  0.2449f,
    0.2449f,  0.3268f,  0.3165f,
    0.1225f,  0.3536f,  0.3660f,
    0.0000f,  0.3750f,  0.3827f,
    -0.1225f,  0.3536f,  0.3660f,
    -0.2449f,  0.3268f,  0.3165f,
    -0.3165f,  0.3268f,  0.2449f,
    -0.3660f,  0.3536f,  0.1225f,
    -0.3827f,  0.3536f,  0.0000f,
    -0.3660f,  0.3536f,  -0.1225f,
    -0.3165f,  0.3268f,  -0.2449f,
    -0.2449f,  0.3268f,  -0.3165f,
    -0.1225f,  0.3536f,  -0.3660f,
    0.0000f,  0.3750f,  -0.3827f,
    0.1225f,  0.3536f,  -0.3660f,
    0.2449f,  0.3268f,  -0.3165f,
    0.3165f,  0.3268f,  -0.2449f,
    0.3660f,  0.3536f,  -0.1225f,
    0.4619f,  0.2706f,  0.0000f,
    0.4414f,  0.2706f,  0.1414f,
    0.3794f,  0.2480f,  0.2828f,
    0.2828f,  0.2480f,  0.3794f,
    0.1414f,  0.2706f,  0.4414f,
    0.0000f,  0.2903f,  0.4619f,
    -0.1414f,  0.2706f,  0.4414f,
    -0.2828f,  0.2480f,  0.3794f,
    -0.3794f,  0.2480f,  0.2828f,
    -0.4414f,  0.2706f,  0.1414f,
    -0.4619f,  0.2706f,  0.0000f,
    -0.4414f,  0.2706f,  -0.1414f,
    -0.3794f,  0.2480f,  -0.2828f,
    -0.2828f,  0.2480f,  -0.3794f,
    -0.1414f,  0.2706f,  -0.4414f,
    0.0000f,  0.2903f,  -0.4619f,
    0.1414f,  0.2706f,  -0.4414f,
    0.2828f,  0.2480f,  -0.3794f,
    0.3794f,  0.2480f,  -0.2828f,
    0.4414f,  0.2706f,  -0.1414f,
    0.5000f,  0.1768f,  0.0000f,
    0.4755f,  0.1768f,  0.1564f,
    0.4157f,  0.1587f,  0.3128f,
    0.3128f,  0.1587f,  0.4157f,
    0.1564f,  0.1768f,  0.4755f,
    0.0000f,  0.1951f,  0.5000f,
    -0.1564f,  0.1768f,  0.4755f,
    -0.3128f,  0.1587f,  0.4157f,
    -0.4157f,  0.1587f,  0.3128f,
    -0.4755f,  0.1768f,  0.1564f,
    -0.5000f,  0.1768f,  0.0000f,
    -0.4755f,  0.1768f,  -0.1564f,
    -0.4157f,  0.1587f,  -0.3128f,
    -0.3128f,  0.1587f,  -0.4157f,
    -0.1564f,  0.1768f,  -0.4755f,
    0.0000f,  0.1951f,  -0.5000f,
    0.1564f,  0.1768f,  -0.4755f,
    0.3128f,  0.1587f,  -0.4157f,
    0.4157f,  0.1587f,  -0.3128f,
    0.4755f,  0.1768f,  -0.1564f,
    0.4903f,  0.0975f,  0.0000f,
    0.4665f,  0.0975f,  0.1654f,
    0.4090f,  0.0905f,  0.3308f,
    0.3308f,  0.0905f,  0.4090f,
    0.1654f,  0.0975f,  0.4665f,
    0.0000f,  0.1082f,  0.4903f,
    -0.1654f,  0.0975f,  0.4665f,
    -0.3308f,  0.0905f,  0.4090f,
    -0.4090f,  0.0905f,  0.3308f,
    -0.4665f,  0.0975f,  0.1654f,
    -0.4903f,  0.0975f,  0.0000f,
    -0.4665f,  0.0975f,  -0.1654f,
    -0.4090f,  0.0905f,  -0.3308f,
    -0.3308f,  0.0905f,  -0.4090f,
    -0.1654f,  0.0975f,  -0.4665f,
    0.0000f,  0.1082f,  -0.4903f,
    0.1654f,  0.0975f,  -0.4665f,
    0.3308f,  0.0905f,  -0.4090f,
    0.4090f,  0.0905f,  -0.3308f,
    0.4665f,  0.0975f,  -0.1654f,
    0.4330f,  0.0000f,  0.0000f,
    0.4157f,  0.0000f,  0.1710f,
    0.3660f,  0.0000f,  0.3420f,
    0.2929f,  0.0000f,  0.4330f,
    0.1710f,  0.0000f,  0.4157f,
    0.0000f,  0.0000f,  0.4330f,
    -0.1710f,  0.0000f,  0.4157f,
    -0.2929f,  0.0000f,  0.4330f,
    -0.3660f,  0.0000f,  0.3420f,
    -0.4157f,  0.0000f,  0.1710f,
    -0.4330f,  0.0000f,  0.0000f,
    -0.4157f,  0.0000f,  -0.1710f,
    -0.3660f,  0.0000f,  -0.3420f,
    -0.2929f,  0.0000f,  -0.4330f,
    -0.1710f,  0.0000f,  -0.4157f,
    0.0000f,  0.0000f,  -0.4330f,
    0.1710f,  0.0000f,  -0.4157f,
    0.2929f,  0.0000f,  -0.4330f,
    0.3660f,  0.0000f,  -0.3420f,
    0.4157f,  0.0000f,  -0.1710f,
    0.3827f,  -0.1768f,  0.0000f,
    0.3660f,  -0.1768f,  0.1564f,
    0.3165f,  -0.1587f,  0.3128f,
    0.2449f,  -0.1587f,  0.4157f,
    0.1225f,  -0.1768f,  0.4755f,
    0.0000f,  -0.1951f,  0.5000f,
    -0.1225f,  -0.1768f,  0.4755f,
    -0.2449f,  -0.1587f,  0.4157f,
    -0.3165f,  -0.1587f,  0.3128f,
    -0.3660f,  -0.1768f,  0.1564f,
    -0.3827f,  -0.1768f,  0.0000f,
    -0.3660f,  -0.1768f,  -0.1564f,
    -0.3165f,  -0.1587f,  -0.3128f,
    -0.2449f,  -0.1587f,  -0.4157f,
    -0.1225f,  -0.1768f,  -0.4755f,
    0.0000f,  -0.1951f,  -0.5000f,
    0.1225f,  -0.1768f,  -0.4755f,
    0.2449f,  -0.1587f,  -0.4157f,
    0.3165f,  -0.1587f,  -0.3128f,
    0.3660f,  -0.1768f,  -0.1564f,
    0.2929f,  -0.2706f,  0.0000f,
    0.2806f,  -0.2706f,  0.1414f,
    0.2443f,  -0.2480f,  0.2828f,
    0.1848f,  -0.2480f,  0.3794f,
    0.0924f,  -0.2706f,  0.4414f,
    0.0000f,  -0.2903f,  0.4619f,
    -0.0924f,  -0.2706f,  0.4414f,
    -0.1848f,  -0.2480f,  0.3794f,
    -0.2443f,  -0.2480f,  0.2828f,
    -0.2806f,  -0.2706f,  0.1414f,
    -0.2929f,  -0.2706f,  0.0000f,
    -0.2806f,  -0.2706f,  -0.1414f,
    -0.2443f,  -0.2480f,  -0.2828f,
    -0.1848f,  -0.2480f,  -0.3794f,
    -0.0924f,  -0.2706f,  -0.4414f,
    0.0000f,  -0.2903f,  -0.4619f,
    0.0924f,  -0.2706f,  -0.4414f,
    0.1848f,  -0.2480f,  -0.3794f,
    0.2443f,  -0.2480f,  -0.2828f,
    0.2806f,  -0.2706f,  -0.1414f,
    0.1951f,  -0.3536f,  0.0000f,
    0.1871f,  -0.3536f,  0.1225f,
    0.1629f,  -0.3268f,  0.2449f,
    0.1236f,  -0.3268f,  0.3165f,
    0.0618f,  -0.3536f,  0.3660f,
    0.0000f,  -0.3750f,  0.3827f,
    -0.0618f,  -0.3536f,  0.3660f,
    -0.1236f,  -0.3268f,  0.3165f,
    -0.1629f,  -0.3268f,  0.2449f,
    -0.1871f,  -0.3536f,  0.1225f,
    -0.1951f,  -0.3536f,  0.0000f,
    -0.1871f,  -0.3536f,  -0.1225f,
    -0.1629f,  -0.3268f,  -0.2449f,
    -0.1236f,  -0.3268f,  -0.3165f,
    -0.0618f,  -0.3536f,  -0.3660f,
    0.0000f,  -0.3750f,  -0.3827f,
    0.0618f,  -0.3536f,  -0.3660f,
    0.1236f,  -0.3268f,  -0.3165f,
    0.1629f,  -0.3268f,  -0.2449f,
    0.1871f,  -0.3536f,  -0.1225f,
    0.0975f,  -0.4157f,  0.0000f,
    0.0935f,  -0.4157f,  0.0924f,
    0.0814f,  -0.3928f,  0.1848f,
    0.0618f,  -0.3928f,  0.2443f,
    0.0309f,  -0.4157f,  0.2806f,
    0.0000f,  -0.4330f,  0.2929f,
    -0.0309f,  -0.4157f,  0.2806f,
    -0.0618f,  -0.3928f,  0.2443f,
    -0.0814f,  -0.3928f,  0.1848f,
    -0.0935f,  -0.4157f,  0.0924f,
    -0.0975f,  -0.4157f,  0.0000f,
    -0.0935f,  -0.4157f,  -0.0924f,
    -0.0814f,  -0.3928f,  -0.1848f,
    -0.0618f,  -0.3928f,  -0.2443f,
    -0.0309f,  -0.4157f,  -0.2806f,
    0.0000f,  -0.4330f,  -0.2929f,
    0.0309f,  -0.4157f,  -0.2806f,
    0.0618f,  -0.3928f,  -0.2443f,
    0.0814f,  -0.3928f,  -0.1848f,
    0.0935f,  -0.4157f,  -0.0924f,
    0.0000f,  -0.4619f,  0.0000f,
    0.0000f,  -0.4619f,  0.0618f,
    0.0000f,  -0.4438f,  0.1236f,
    0.0000f,  -0.4438f,  0.1629f,
    0.0000f,  -0.4619f,  0.1871f,
    0.0000f,  -0.4755f,  0.1951f,
    0.0000f,  -0.4619f,  0.1871f,
    0.0000f,  -0.4438f,  0.1629f,
    0.0000f,  -0.4438f,  0.1236f,
    0.0000f,  -0.4619f,  0.0618f,
    0.0000f,  -0.4619f,  0.0000f,
    0.0000f,  -0.4619f,  -0.0618f,
    0.0000f,  -0.4438f,  -0.1236f,
    0.0000f,  -0.4438f,  -0.1629f,
    0.0000f,  -0.4619f,  -0.1871f,
    0.0000f,  -0.4755f,  -0.1951f,
    0.0000f,  -0.4619f,  -0.1871f,
    0.0000f,  -0.4438f,  -0.1629f,
    0.0000f,  -0.4438f,  -0.1236f,
    0.0000f,  -0.4619f,  -0.0618f,
    0.0000f,  -0.5000f,  0.0000f
};

extern unsigned int quadVAO, quadVBO;
extern unsigned int cubeVAO, cubeVBO;
extern unsigned int sphereVAO, sphereVBO;

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
//Frambuffer Begin
struct TextureAttributes {
    GLenum target;
	GLint internalFormat;
    GLenum format;
	GLenum type;

    bool operator==(const TextureAttributes& other) const {
        return std::tie(target, internalFormat, format, type) ==
            std::tie(other.target, other.internalFormat, other.format, other.type);
    }
};

template <class T>
inline void hash_combine(std::size_t& seed, const T& v) {
    std::hash<T> hasher;
    // ????? 0x9e3779b9 ??????????????????????????????????????
    seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

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
	std::vector<TextureAttributes> textureAttrs;


    bool operator==(const FBOAttributes& other) const {
        return std::tie(aaType, isHDR, isGamma, isShadowMap, shadowType, isBloom, isDefer) ==
            std::tie(other.aaType, other.isHDR, other.isGamma, other.isShadowMap, other.shadowType, other.isBloom, other.isDefer)
            && textureAttrs == other.textureAttrs;
    }
};

namespace std {
    template<> struct hash<TextureAttributes> {
        size_t operator()(const TextureAttributes& attr) const {
            size_t seed = 0;
            hash_combine(seed, static_cast<unsigned int>(attr.target));
            hash_combine(seed, static_cast<int>(attr.internalFormat));
            hash_combine(seed, static_cast<unsigned int>(attr.format));
            hash_combine(seed, static_cast<unsigned int>(attr.type));
            return seed;
        }
    };

    template<> struct hash<FBOAttributes> {
        size_t operator()(const FBOAttributes& attr) const {
            size_t seed = 0;
            hash_combine(seed, static_cast<int>(attr.aaType));
            hash_combine(seed, attr.isHDR);
            hash_combine(seed, attr.isShadowMap);
            hash_combine(seed, attr.isGamma);
            hash_combine(seed, static_cast<int>(attr.shadowType));
            hash_combine(seed, attr.isBloom);
            hash_combine(seed, attr.isDefer);

            for (const auto& tex : attr.textureAttrs) {
                hash_combine(seed, tex);
            }

            return seed;
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
    std::string passName;
    int width;
    int height;

	FBOAttributes attr;

	FBO(FBOAttributes attr) {
		width = properties.SCREEN_WIDTH;
		height = properties.SCREEN_HEIGHT;
		passName = "Default";
		Init(attr);
	}

    FBO(int w, int h, FBOAttributes attr) {
        width = w;
        height = h;
        passName = "Default";
        Init(attr);
	}

    FBO(int w, int h, FBOAttributes attr, std::string pass) {
        width = w;
        height = h;
        passName = pass;
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
private:
	SystemProperties& properties = SystemProperties::GetInstance();
};

class FramebuffersManager {
public:
    static unsigned int renderFBO;
    inline static FBO::FrameRenderType useType = FBO::Default_FrameRenderType;

    static FramebuffersManager& GetInstance() {
        static FramebuffersManager instance;
        return instance;
    }

    void RegisterFBO(const std::string& passName, FBO* fbo) {
        m_fboMap[passName] = fbo;
    }

    FBO* GetFBOByPassName(const std::string& passName) {
        if(m_fboMap.find(passName) != m_fboMap.end()) {
            return m_fboMap[passName];
		}
		std::cout << "There is no FBO registered for pass name: " << passName << std::endl;
		return nullptr;
    }

	static FBOAttributes GenCurrentAttr() {
		FBOAttributes attr;
        auto& properties = SystemProperties::GetInstance();
		attr.aaType = AntiAliasManager::GetInstance().antiAliasType;
		attr.isGamma = properties.GAMMA_CORRECTION;
		attr.isHDR = properties.USE_HDR;
		attr.isBloom = properties.BLOOM;
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

	void Resize();

	// ?????????? isBusy ?? FBO ?????????? Viewport ??????? FBO ????????????
	std::vector<FBO*> GetBusyFBOs() const;
	// ???? FBO ?? attr ?????????????????????????? UI ??????
	static std::string GetFBODisplayName(const FBOAttributes& attr, int indexInList);
private:
	inline static FBOAttributes::FramebufferType FBOResizeableTpye[] = {
		FBOAttributes::FramebufferType::Framebuffer,
		FBOAttributes::FramebufferType::Multisample
	};
    //FBO objcet pool
	std::unordered_map<FBOAttributes, std::vector<FBO*>> m_hashMapFBO;
    //record the active FBOs by name
    std::unordered_map<std::string, FBO*> m_fboMap;

};
//Frambuffer End
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

unsigned int TextureFromFile(const char* path, const std::string& directory, bool alpha = false, bool gamma = false);

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

    void SetScale(glm::vec3 s) {
        scale = s;
	}
    
    void SetScale(float s) {
        scale = glm::vec3(s);
    }

    void SetPosition(glm::vec3 p) {
        position = p;
	}

    void SetRotation(glm::vec3 r) {
        rotation = r;
	}
protected:
	glm::mat4 modelMatrix;
};