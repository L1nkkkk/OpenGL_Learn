#include "Global.h"
#include <unordered_map>

GlobalVAOs globalVAOs;

namespace {
    // 纹理缓存：相同文件路径 + gamma 模式复用同一个 OpenGL texture，避免重复占用显存。
    std::unordered_map<std::string, unsigned int> g_textureCache;
}

unsigned int TextureFromFile(const char* path, const std::string& directory, bool alpha , bool gamma)
{
    std::string filename = std::string(path);
    filename = directory + '/' + filename;
    const std::string cacheKey = filename + (gamma ? "|gamma=1" : "|gamma=0");
    auto it = g_textureCache.find(cacheKey);
    if (it != g_textureCache.end()) {
        return it->second;
    }

    unsigned int textureID;
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
        glBindTexture(GL_TEXTURE_2D, textureID);
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
    }
    else
    {
        std::cout << "Texture failed to load at path: " << path << std::endl;
        stbi_image_free(data);
    }

    if (textureID != 0) {
        g_textureCache[cacheKey] = textureID;
    }
    return textureID;
}