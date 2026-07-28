#include "Global.h"
#include "GLStateCache.h"
#include "Profiler.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <unordered_map>

GlobalVAOs globalVAOs;

SystemProperties::SystemProperties()
{
    const auto parseEnvironmentFlag = [](
        std::string value,
        bool& parsedValue) {
        std::transform(
            value.begin(),
            value.end(),
            value.begin(),
            [](unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
        if (value == "1" || value == "true" ||
            value == "on" || value == "yes") {
            parsedValue = true;
            return true;
        }
        if (value == "0" || value == "false" ||
            value == "off" || value == "no") {
            parsedValue = false;
            return true;
        }
        return false;
    };
    const auto parseEnvironmentFloat = [](
        const std::string& text,
        float& parsedValue) {
        try {
            std::size_t consumed = 0;
            const float value = std::stof(text, &consumed);
            const bool trailingWhitespaceOnly = std::all_of(
                text.begin() + static_cast<std::ptrdiff_t>(consumed),
                text.end(),
                [](unsigned char character) {
                    return std::isspace(character) != 0;
                });
            if (!trailingWhitespaceOnly ||
                !std::isfinite(value) ||
                value < 0.0f) {
                return false;
            }
            parsedValue = (std::min)(value, 64.0f);
            return true;
        }
        catch (...) {
            return false;
        }
    };
#ifdef _MSC_VER
    char* cacheStrategy = nullptr;
    std::size_t cacheStrategyLength = 0;
    if (_dupenv_s(
            &cacheStrategy,
            &cacheStrategyLength,
            "OPENGL_LEARN_SHADOW_CACHE") == 0 &&
        cacheStrategy != nullptr) {
        const std::string value(cacheStrategy);
        std::free(cacheStrategy);
        SHADOW_CACHE_DISABLED =
            value == "none" || value == "NONE" ||
            value == "disabled" || value == "DISABLED" ||
            value == "off" || value == "OFF";
        SHADOW_CACHE_USE_LEGACY_SIGNATURE =
            !SHADOW_CACHE_DISABLED &&
            (value == "legacy" || value == "LEGACY" || value == "1");
    }
    char* perLightCache = nullptr;
    std::size_t perLightCacheLength = 0;
    if (_dupenv_s(
            &perLightCache,
            &perLightCacheLength,
            "OPENGL_LEARN_SHADOW_PER_LIGHT_CACHE") == 0 &&
        perLightCache != nullptr) {
        const std::string value(perLightCache);
        std::free(perLightCache);
        parseEnvironmentFlag(value, SHADOW_PER_LIGHT_CACHE);
    }
    char* casterCulling = nullptr;
    std::size_t casterCullingLength = 0;
    if (_dupenv_s(
            &casterCulling,
            &casterCullingLength,
            "OPENGL_LEARN_SHADOW_CASTER_CULLING") == 0 &&
        casterCulling != nullptr) {
        const std::string value(casterCulling);
        std::free(casterCulling);
        parseEnvironmentFlag(value, SHADOW_CASTER_CULLING);
    }
    char* directionalShadowFit = nullptr;
    std::size_t directionalShadowFitLength = 0;
    if (_dupenv_s(
            &directionalShadowFit,
            &directionalShadowFitLength,
            "OPENGL_LEARN_DIRECTIONAL_SHADOW_FIT") == 0 &&
        directionalShadowFit != nullptr) {
        const std::string value(directionalShadowFit);
        std::free(directionalShadowFit);
        DIRECTIONAL_SHADOW_LIGHT_AABB_FIT =
            value == "light-aabb" ||
            value == "LIGHT-AABB" ||
            value == "aabb" ||
            value == "AABB" ||
            value == "1";
    }
    char* directionalShadowResolution = nullptr;
    std::size_t directionalShadowResolutionLength = 0;
    if (_dupenv_s(
            &directionalShadowResolution,
            &directionalShadowResolutionLength,
            "OPENGL_LEARN_DIRECTIONAL_SHADOW_RESOLUTION") == 0 &&
        directionalShadowResolution != nullptr) {
        const std::string value(directionalShadowResolution);
        std::free(directionalShadowResolution);
        DIRECTIONAL_SHADOW_DENSITY_RESOLUTION =
            value == "density" ||
            value == "DENSITY" ||
            value == "adaptive" ||
            value == "ADAPTIVE" ||
            value == "1";
    }
    auto readEnvironmentFlag = [&](const char* name, bool& destination) {
        char* rawValue = nullptr;
        std::size_t rawValueLength = 0;
        if (_dupenv_s(
                &rawValue,
                &rawValueLength,
                name) == 0 &&
            rawValue != nullptr) {
            const std::string value(rawValue);
            std::free(rawValue);
            parseEnvironmentFlag(value, destination);
        }
    };
    auto readEnvironmentFloat = [&](const char* name, float& destination) {
        char* rawValue = nullptr;
        std::size_t rawValueLength = 0;
        if (_dupenv_s(
                &rawValue,
                &rawValueLength,
                name) == 0 &&
            rawValue != nullptr) {
            parseEnvironmentFloat(rawValue, destination);
            std::free(rawValue);
        }
    };
    readEnvironmentFlag(
        "OPENGL_LEARN_SHADOW_EXACT_EARLY_OUT",
        SHADOW_EXACT_EARLY_OUT);
    readEnvironmentFlag(
        "OPENGL_LEARN_SHADOW_PREPARED_POINT_INPUTS",
        SHADOW_PREPARED_POINT_INPUTS);
    readEnvironmentFlag(
        "OPENGL_LEARN_SHADOW_ADAPTIVE_POINT_SAMPLES",
        SHADOW_ADAPTIVE_POINT_SAMPLES);
    readEnvironmentFlag(
        "OPENGL_LEARN_SHADOW_ADAPTIVE_PCSS_FILTER",
        SHADOW_ADAPTIVE_PCSS_FILTER);
    readEnvironmentFlag(
        "OPENGL_LEARN_SHADOW_STAGED_BLOCKER",
        SHADOW_STAGED_PCSS_BLOCKER);
    readEnvironmentFlag(
        "OPENGL_LEARN_SHADOW_HARDWARE_COMPARE",
        SHADOW_HARDWARE_DEPTH_COMPARE);
    readEnvironmentFlag(
        "OPENGL_LEARN_SHADOW_HARDWARE_LINEAR",
        SHADOW_HARDWARE_LINEAR_PCF);
    readEnvironmentFlag(
        "OPENGL_LEARN_SHADOW_HARDWARE_REDUCED_PCF",
        SHADOW_HARDWARE_REDUCED_PCF);
    readEnvironmentFlag(
        "OPENGL_LEARN_SHADOW_TEXEL_BIAS",
        SHADOW_TEXEL_SCALED_BIAS);
    readEnvironmentFlag(
        "OPENGL_LEARN_SHADOW_SPOT_RADIAL_BIAS_DIRECTION",
        SHADOW_SPOT_RADIAL_BIAS_DIRECTION);
    readEnvironmentFlag(
        "OPENGL_LEARN_SHADOW_SPOT_PCSS_LINEAR_DEPTH",
        SHADOW_SPOT_PCSS_LINEAR_DEPTH);
    readEnvironmentFlag(
        "OPENGL_LEARN_SHADOW_SPOT_PCSS_REDUCED_FILTER",
        SHADOW_SPOT_PCSS_REDUCED_FILTER);
    readEnvironmentFlag(
        "OPENGL_LEARN_SHADOW_SPOT_CASTER_DEPTH_FIT",
        SHADOW_SPOT_CASTER_DEPTH_FIT);
    readEnvironmentFloat(
        "OPENGL_LEARN_SHADOW_BIAS_2D_MIN_TEXELS",
        SHADOW_BIAS_2D_MIN_TEXELS);
    readEnvironmentFloat(
        "OPENGL_LEARN_SHADOW_BIAS_2D_SLOPE_TEXELS",
        SHADOW_BIAS_2D_SLOPE_TEXELS);
    readEnvironmentFloat(
        "OPENGL_LEARN_SHADOW_BIAS_CUBE_MIN_TEXELS",
        SHADOW_BIAS_CUBE_MIN_TEXELS);
    readEnvironmentFloat(
        "OPENGL_LEARN_SHADOW_BIAS_CUBE_SLOPE_TEXELS",
        SHADOW_BIAS_CUBE_SLOPE_TEXELS);
    char* pointShadowRenderPath = nullptr;
    std::size_t pointShadowRenderPathLength = 0;
    if (_dupenv_s(
            &pointShadowRenderPath,
            &pointShadowRenderPathLength,
            "OPENGL_LEARN_POINT_SHADOW_RENDER_PATH") == 0 &&
        pointShadowRenderPath != nullptr) {
        const std::string value(pointShadowRenderPath);
        std::free(pointShadowRenderPath);
        const bool forceSixFace =
            value == "six-face" ||
            value == "SIX-FACE" ||
            value == "six_face" ||
            value == "SIX_FACE" ||
            value == "six" ||
            value == "SIX" ||
            value == "1";
        const bool forceLayered =
            value == "layered" ||
            value == "LAYERED" ||
            value == "geometry" ||
            value == "GEOMETRY" ||
            value == "0";
        POINT_SHADOW_ADAPTIVE_RENDERING =
            !forceSixFace && !forceLayered;
        POINT_SHADOW_SIX_FACE_RENDERING = forceSixFace;
    }
    readEnvironmentFlag(
        "OPENGL_LEARN_POINT_SHADOW_FACE_CULLING",
        POINT_SHADOW_FACE_CULLING);
    char* adaptiveMinSamples = nullptr;
    std::size_t adaptiveMinSamplesLength = 0;
    if (_dupenv_s(
            &adaptiveMinSamples,
            &adaptiveMinSamplesLength,
            "OPENGL_LEARN_SHADOW_ADAPTIVE_MIN_SAMPLES") == 0 &&
        adaptiveMinSamples != nullptr) {
        try {
            SHADOW_ADAPTIVE_MIN_SAMPLES = (std::max)(
                1,
                (std::min)(64, std::stoi(adaptiveMinSamples)));
        }
        catch (...) {
        }
        std::free(adaptiveMinSamples);
    }
#else
    const char* cacheStrategy = std::getenv("OPENGL_LEARN_SHADOW_CACHE");
    if (cacheStrategy != nullptr) {
        const std::string value(cacheStrategy);
        SHADOW_CACHE_DISABLED =
            value == "none" || value == "NONE" ||
            value == "disabled" || value == "DISABLED" ||
            value == "off" || value == "OFF";
        SHADOW_CACHE_USE_LEGACY_SIGNATURE =
            !SHADOW_CACHE_DISABLED &&
            (value == "legacy" || value == "LEGACY" || value == "1");
    }
    const char* perLightCache =
        std::getenv("OPENGL_LEARN_SHADOW_PER_LIGHT_CACHE");
    if (perLightCache != nullptr) {
        const std::string value(perLightCache);
        parseEnvironmentFlag(value, SHADOW_PER_LIGHT_CACHE);
    }
    const char* casterCulling =
        std::getenv("OPENGL_LEARN_SHADOW_CASTER_CULLING");
    if (casterCulling != nullptr) {
        const std::string value(casterCulling);
        parseEnvironmentFlag(value, SHADOW_CASTER_CULLING);
    }
    const char* directionalShadowFit =
        std::getenv("OPENGL_LEARN_DIRECTIONAL_SHADOW_FIT");
    if (directionalShadowFit != nullptr) {
        const std::string value(directionalShadowFit);
        DIRECTIONAL_SHADOW_LIGHT_AABB_FIT =
            value == "light-aabb" ||
            value == "LIGHT-AABB" ||
            value == "aabb" ||
            value == "AABB" ||
            value == "1";
    }
    const char* directionalShadowResolution =
        std::getenv("OPENGL_LEARN_DIRECTIONAL_SHADOW_RESOLUTION");
    if (directionalShadowResolution != nullptr) {
        const std::string value(directionalShadowResolution);
        DIRECTIONAL_SHADOW_DENSITY_RESOLUTION =
            value == "density" ||
            value == "DENSITY" ||
            value == "adaptive" ||
            value == "ADAPTIVE" ||
            value == "1";
    }
    auto readEnvironmentFlag = [&](const char* name, bool& destination) {
        const char* rawValue = std::getenv(name);
        if (rawValue != nullptr) {
            parseEnvironmentFlag(rawValue, destination);
        }
    };
    auto readEnvironmentFloat = [&](const char* name, float& destination) {
        const char* rawValue = std::getenv(name);
        if (rawValue == nullptr) {
            return;
        }
        parseEnvironmentFloat(rawValue, destination);
    };
    readEnvironmentFlag(
        "OPENGL_LEARN_SHADOW_EXACT_EARLY_OUT",
        SHADOW_EXACT_EARLY_OUT);
    readEnvironmentFlag(
        "OPENGL_LEARN_SHADOW_PREPARED_POINT_INPUTS",
        SHADOW_PREPARED_POINT_INPUTS);
    readEnvironmentFlag(
        "OPENGL_LEARN_SHADOW_ADAPTIVE_POINT_SAMPLES",
        SHADOW_ADAPTIVE_POINT_SAMPLES);
    readEnvironmentFlag(
        "OPENGL_LEARN_SHADOW_ADAPTIVE_PCSS_FILTER",
        SHADOW_ADAPTIVE_PCSS_FILTER);
    readEnvironmentFlag(
        "OPENGL_LEARN_SHADOW_STAGED_BLOCKER",
        SHADOW_STAGED_PCSS_BLOCKER);
    readEnvironmentFlag(
        "OPENGL_LEARN_SHADOW_HARDWARE_COMPARE",
        SHADOW_HARDWARE_DEPTH_COMPARE);
    readEnvironmentFlag(
        "OPENGL_LEARN_SHADOW_HARDWARE_LINEAR",
        SHADOW_HARDWARE_LINEAR_PCF);
    readEnvironmentFlag(
        "OPENGL_LEARN_SHADOW_HARDWARE_REDUCED_PCF",
        SHADOW_HARDWARE_REDUCED_PCF);
    readEnvironmentFlag(
        "OPENGL_LEARN_SHADOW_TEXEL_BIAS",
        SHADOW_TEXEL_SCALED_BIAS);
    readEnvironmentFlag(
        "OPENGL_LEARN_SHADOW_SPOT_RADIAL_BIAS_DIRECTION",
        SHADOW_SPOT_RADIAL_BIAS_DIRECTION);
    readEnvironmentFlag(
        "OPENGL_LEARN_SHADOW_SPOT_PCSS_LINEAR_DEPTH",
        SHADOW_SPOT_PCSS_LINEAR_DEPTH);
    readEnvironmentFlag(
        "OPENGL_LEARN_SHADOW_SPOT_PCSS_REDUCED_FILTER",
        SHADOW_SPOT_PCSS_REDUCED_FILTER);
    readEnvironmentFlag(
        "OPENGL_LEARN_SHADOW_SPOT_CASTER_DEPTH_FIT",
        SHADOW_SPOT_CASTER_DEPTH_FIT);
    readEnvironmentFloat(
        "OPENGL_LEARN_SHADOW_BIAS_2D_MIN_TEXELS",
        SHADOW_BIAS_2D_MIN_TEXELS);
    readEnvironmentFloat(
        "OPENGL_LEARN_SHADOW_BIAS_2D_SLOPE_TEXELS",
        SHADOW_BIAS_2D_SLOPE_TEXELS);
    readEnvironmentFloat(
        "OPENGL_LEARN_SHADOW_BIAS_CUBE_MIN_TEXELS",
        SHADOW_BIAS_CUBE_MIN_TEXELS);
    readEnvironmentFloat(
        "OPENGL_LEARN_SHADOW_BIAS_CUBE_SLOPE_TEXELS",
        SHADOW_BIAS_CUBE_SLOPE_TEXELS);
    const char* pointShadowRenderPath =
        std::getenv("OPENGL_LEARN_POINT_SHADOW_RENDER_PATH");
    if (pointShadowRenderPath != nullptr) {
        const std::string value(pointShadowRenderPath);
        const bool forceSixFace =
            value == "six-face" ||
            value == "SIX-FACE" ||
            value == "six_face" ||
            value == "SIX_FACE" ||
            value == "six" ||
            value == "SIX" ||
            value == "1";
        const bool forceLayered =
            value == "layered" ||
            value == "LAYERED" ||
            value == "geometry" ||
            value == "GEOMETRY" ||
            value == "0";
        POINT_SHADOW_ADAPTIVE_RENDERING =
            !forceSixFace && !forceLayered;
        POINT_SHADOW_SIX_FACE_RENDERING = forceSixFace;
    }
    readEnvironmentFlag(
        "OPENGL_LEARN_POINT_SHADOW_FACE_CULLING",
        POINT_SHADOW_FACE_CULLING);
    const char* adaptiveMinSamples =
        std::getenv("OPENGL_LEARN_SHADOW_ADAPTIVE_MIN_SAMPLES");
    if (adaptiveMinSamples != nullptr) {
        try {
            SHADOW_ADAPTIVE_MIN_SAMPLES = (std::max)(
                1,
                (std::min)(64, std::stoi(adaptiveMinSamples)));
        }
        catch (...) {
        }
    }
#endif
    if (SHADOW_HARDWARE_REDUCED_PCF) {
        SHADOW_HARDWARE_LINEAR_PCF = true;
    }
    if (SHADOW_HARDWARE_LINEAR_PCF) {
        SHADOW_HARDWARE_DEPTH_COMPARE = true;
    }
}

namespace {
    // 纹理缓存：相同文件路径 + gamma 模式复用同一个 OpenGL texture，避免重复占用显存。
    struct CachedTexture {
        unsigned int id = 0;
        std::uint64_t bytes = 0;
    };

    std::unordered_map<std::string, CachedTexture> g_textureCache;

    std::uint64_t CalculateMipChainBytes(int width, int height, int components)
    {
        std::uint64_t bytes = 0;
        int mipWidth = (std::max)(1, width);
        int mipHeight = (std::max)(1, height);
        while (true) {
            bytes += static_cast<std::uint64_t>(mipWidth) *
                static_cast<std::uint64_t>(mipHeight) *
                static_cast<std::uint64_t>((std::max)(1, components));
            if (mipWidth == 1 && mipHeight == 1) {
                break;
            }
            mipWidth = (std::max)(1, mipWidth / 2);
            mipHeight = (std::max)(1, mipHeight / 2);
        }
        return bytes;
    }
}

unsigned int TextureFromFile(const char* path, const std::string& directory, bool alpha , bool gamma)
{
    std::string filename = std::string(path);
    filename = directory + '/' + filename;
    const std::string cacheKey = filename +
        (alpha ? "|alpha=1" : "|alpha=0") +
        (gamma ? "|srgb=1" : "|srgb=0");
    auto it = g_textureCache.find(cacheKey);
    if (it != g_textureCache.end()) {
        PerformanceProfiler::GetInstance().RecordTextureCacheLookup(true);
        return it->second.id;
    }
    PerformanceProfiler::GetInstance().RecordTextureCacheLookup(false);

    unsigned int textureID = 0;
    glGenTextures(1, &textureID);
    stbi_set_flip_vertically_on_load(true);
    int width, height, nrComponents;
    unsigned char* data = stbi_load(filename.c_str(), &width, &height, &nrComponents, 0);
    if (data)
    {
        GLenum format;
        GLenum internalFormat;
        if (nrComponents == 1) {
            internalFormat = format = GL_RED;
        }
        else if (nrComponents == 2) {
            internalFormat = format = GL_RG;
        }
        else if (nrComponents == 3) {
            internalFormat = gamma ? GL_SRGB : GL_RGB;
            format = GL_RGB;
        }
        else if (nrComponents == 4) {
            internalFormat = gamma ? GL_SRGB_ALPHA : GL_RGBA;
            format = GL_RGBA;
        }
        GLState::BindTexture(GL_TEXTURE_2D, textureID);
        GLint previousUnpackAlignment = 4;
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &previousUnpackAlignment);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, GL_UNSIGNED_BYTE, data);
        glPixelStorei(GL_UNPACK_ALIGNMENT, previousUnpackAlignment);
        glGenerateMipmap(GL_TEXTURE_2D);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        if (alpha) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        stbi_image_free(data);

        const std::uint64_t textureBytes = CalculateMipChainBytes(width, height, nrComponents);
        g_textureCache.emplace(cacheKey, CachedTexture{ textureID, textureBytes });
        PerformanceProfiler::GetInstance().RecordMemoryAllocation(
            MemoryResourceType::Texture,
            textureBytes);
        return textureID;
    }
    std::cout << "Texture failed to load at path: " << path << std::endl;
    stbi_image_free(data);
    if (textureID != 0) {
        GLState::ForgetTexture(textureID);
        glDeleteTextures(1, &textureID);
    }
    return 0;
}

void DestroyTextureCache()
{
    auto& profiler = PerformanceProfiler::GetInstance();
    for (auto& [key, texture] : g_textureCache) {
        (void)key;
        if (texture.id != 0) {
            GLState::ForgetTexture(texture.id);
            glDeleteTextures(1, &texture.id);
            profiler.RecordMemoryRelease(MemoryResourceType::Texture, texture.bytes);
        }
    }
    g_textureCache.clear();
}
