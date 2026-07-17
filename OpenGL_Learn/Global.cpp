#include "Global.h"
#include "GLStateCache.h"
#include "Profiler.h"
#include <unordered_map>

GlobalVAOs globalVAOs;

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
        glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, GL_UNSIGNED_BYTE, data);
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
