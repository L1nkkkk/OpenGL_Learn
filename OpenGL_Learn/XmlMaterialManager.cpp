#include "XmlMaterialManager.h"

#include <fstream>
#include <iostream>
#include <sstream>

namespace {
    // 简单工具：在 tag 片段中提取属性值，例如 name="Default"
    std::string GetAttribute(const std::string& tag, const std::string& attrName) {
        std::string key = attrName + "=\"";
        size_t pos = tag.find(key);
        if (pos == std::string::npos) return {};
        pos += key.size();
        size_t end = tag.find('"', pos);
        if (end == std::string::npos) return {};
        return tag.substr(pos, end - pos);
    }

    float ParseFloatAttr(const std::string& tag, const std::string& name, float defaultValue) {
        std::string s = GetAttribute(tag, name);
        if (s.empty()) return defaultValue;
        try {
            return std::stof(s);
        }
        catch (...) {
            return defaultValue;
        }
    }

    bool ParseBoolAttr(const std::string& tag, const std::string& name, bool defaultValue) {
        std::string s = GetAttribute(tag, name);
        if (s.empty()) return defaultValue;
        if (s == "true" || s == "1") return true;
        if (s == "false" || s == "0") return false;
        return defaultValue;
    }

    // 将路径拆分为目录和文件名，便于复用现有 TextureFromFile 接口
    std::pair<std::string, std::string> SplitPath(const std::string& fullPath) {
        size_t pos = fullPath.find_last_of("/\\");
        if (pos == std::string::npos) {
            return { "", fullPath };
        }
        std::string dir = fullPath.substr(0, pos);
        std::string file = fullPath.substr(pos + 1);
        return { dir, file };
    }
}

bool XmlMaterialManager::LoadFromFile(const std::string& xmlPath) {
    m_xmlPath = xmlPath;

    std::ifstream file(xmlPath);
    if (!file.is_open()) {
        std::cout << "XmlMaterialManager: failed to open '" << xmlPath << "'" << std::endl;
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    if (!ParseDocument(content)) {
        std::cout << "XmlMaterialManager: parse failed for '" << xmlPath << "'" << std::endl;
        return false;
    }

    std::cout << "XmlMaterialManager: loaded materials from '" << xmlPath << "'" << std::endl;
    m_hasLoaded = true;
    return true;
}

void XmlMaterialManager::ReloadIfFileChanged() {
    if (m_xmlPath.empty()) return;
    // 这里没有使用 std::filesystem 检查时间戳，而是在被调用时直接重新加载。
    // XmlMaterialManager 会复用已有 Material 实例，因此重复加载的主要成本是解析 XML。
    LoadFromFile(m_xmlPath);
}

std::shared_ptr<Material> XmlMaterialManager::GetMaterial(const std::string& name) {
    auto it = m_materials.find(name);
    if (it != m_materials.end()) {
        return it->second;
    }
    return nullptr;
}

Material* XmlMaterialManager::GetMaterialRaw(const std::string& name) {
    auto mat = GetMaterial(name);
    return mat ? mat.get() : nullptr;
}

Material* XmlMaterialManager::GetOrLoadMaterialByFile(const std::string& xmlPath) {
    // 读取文件内容
    std::ifstream file(xmlPath);
    if (!file.is_open()) {
        std::cout << "XmlMaterialManager: failed to open material file '" << xmlPath << "'" << std::endl;
        auto it = m_materialFiles.find(xmlPath);
        if (it != m_materialFiles.end()) {
            return it->second.material.get();
        }
        return nullptr;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    auto& entry = m_materialFiles[xmlPath]; // 若不存在则创建一个空 entry

    // 如果内容完全相同，则直接复用已有材质实例
    if (entry.material && entry.lastContent == content) {
        return entry.material.get();
    }

    // 内容变化或首次加载，重新解析
    if (!ParseSingleMaterial(content, entry)) {
        std::cout << "XmlMaterialManager: parse failed for '" << xmlPath << "'" << std::endl;
        return entry.material.get();
    }

    entry.lastContent = content;
    return entry.material.get();
}

std::vector<std::pair<std::string, std::shared_ptr<Material>>> XmlMaterialManager::GetAllMaterials() const {
    std::vector<std::pair<std::string, std::shared_ptr<Material>>> result;
    result.reserve(m_materials.size() + m_materialFiles.size());

    // 来自大材质表（materials.xml）的命名材质
    for (const auto& kv : m_materials) {
        result.emplace_back(kv.first, kv.second);
    }

    // 来自单文件 xml 的材质，用文件路径作为 key 方便区分
    for (const auto& kv : m_materialFiles) {
        if (kv.second.material) {
            result.emplace_back(kv.first, kv.second.material);
        }
    }

    return result;
}

bool XmlMaterialManager::ParseDocument(const std::string& xmlContent) {
    // 查找 <Materials> ... </Materials> 块（简单实现，假设只有一层）
    size_t rootStart = xmlContent.find("<Materials");
    if (rootStart == std::string::npos) {
        std::cout << "XmlMaterialManager: root <Materials> not found" << std::endl;
        return false;
    }
    rootStart = xmlContent.find('>', rootStart);
    if (rootStart == std::string::npos) return false;
    ++rootStart;

    size_t rootEnd = xmlContent.find("</Materials>", rootStart);
    if (rootEnd == std::string::npos) return false;

    std::string body = xmlContent.substr(rootStart, rootEnd - rootStart);

    // 依次解析每个 <Material ...> ... </Material> 块
    size_t pos = 0;
    while (true) {
        size_t matOpen = body.find("<Material", pos);
        if (matOpen == std::string::npos) break;
        size_t matOpenEnd = body.find('>', matOpen);
        if (matOpenEnd == std::string::npos) break;

        size_t matClose = body.find("</Material>", matOpenEnd);
        if (matClose == std::string::npos) break;
        matClose += std::string("</Material>").length();

        std::string materialBlock = body.substr(matOpen, matClose - matOpen);
        ParseMaterialBlock(materialBlock);

        pos = matClose;
    }

    return true;
}

void XmlMaterialManager::ParseMaterialBlock(const std::string& materialBlock) {
    // 拿到开头的 <Material ...> 这一行
    size_t openEnd = materialBlock.find('>');
    if (openEnd == std::string::npos) return;
    std::string header = materialBlock.substr(0, openEnd + 1);

    std::string matName = GetAttribute(header, "name");
    std::string shaderName = GetAttribute(header, "shader");
    if (matName.empty() || shaderName.empty()) {
        std::cout << "XmlMaterialManager: <Material> missing 'name' or 'shader' attribute" << std::endl;
        return;
    }

    std::shared_ptr<Material> material;
    auto it = m_materials.find(matName);
    if (it == m_materials.end()) {
        material = std::make_shared<Material>(shaderName);
        m_materials[matName] = material;
    }
    else {
        material = it->second;
        material->SetShaderName(shaderName);
        material->ClearProperties();
    }

    RenderState renderState = material->GetRenderState();

    // 提取内部内容（去掉外层 <Material> ... </Material>）
    std::string inner = materialBlock.substr(openEnd + 1);
    size_t closePos = inner.rfind("</Material>");
    if (closePos != std::string::npos) {
        inner = inner.substr(0, closePos);
    }

    size_t pos = 0;
    while (true) {
        size_t tagStart = inner.find('<', pos);
        if (tagStart == std::string::npos) break;
        size_t tagEnd = inner.find('>', tagStart);
        if (tagEnd == std::string::npos) break;

        std::string tag = inner.substr(tagStart, tagEnd - tagStart + 1);

        // 跳过结束标签
        if (tag.size() >= 2 && tag[1] == '/') {
            pos = tagEnd + 1;
            continue;
        }

        // 支持自闭合标签（... />），也支持简单的 <RenderState ...>（不包含子内容）
        if (tag.find("RenderState") != std::string::npos) {
            renderState.depthTest = ParseBoolAttr(tag, "depthTest", renderState.depthTest);
            renderState.depthWrite = ParseBoolAttr(tag, "depthWrite", renderState.depthWrite);
            renderState.stencilTest = ParseBoolAttr(tag, "stencilTest", renderState.stencilTest);

            std::string bm = GetAttribute(tag, "blendMode");
            if (!bm.empty()) {
                if (bm == "AlphaBlend") renderState.blendMode = BlendMode::AlphaBlend;
                else if (bm == "Additive") renderState.blendMode = BlendMode::Additive;
                else renderState.blendMode = BlendMode::None;
            }

            std::string cm = GetAttribute(tag, "cullMode");
            if (!cm.empty()) {
                if (cm == "Front") renderState.cullMode = CullMode::Front;
                else if (cm == "Back") renderState.cullMode = CullMode::Back;
                else renderState.cullMode = CullMode::None;
            }
        }
        else if (tag.find("Float") != std::string::npos) {
            std::string propName = GetAttribute(tag, "name");
            if (!propName.empty()) {
                float value = ParseFloatAttr(tag, "value", 0.0f);
                float minVal = ParseFloatAttr(tag, "min", 0.0f);
                float maxVal = ParseFloatAttr(tag, "max", 100.0f);
                float step = ParseFloatAttr(tag, "step", 0.1f);
                material->AddProperty(propName, MaterialProperty::CreateFloat(value, minVal, maxVal, step));
            }
        }
        else if (tag.find("Vec3") != std::string::npos) {
            std::string propName = GetAttribute(tag, "name");
            if (!propName.empty()) {
                glm::vec3 v(
                    ParseFloatAttr(tag, "x", 0.0f),
                    ParseFloatAttr(tag, "y", 0.0f),
                    ParseFloatAttr(tag, "z", 0.0f));
                float minVal = ParseFloatAttr(tag, "min", 0.0f);
                float maxVal = ParseFloatAttr(tag, "max", 1.0f);
                material->AddProperty(propName, MaterialProperty::CreateVec3(v, minVal, maxVal));
            }
        }
        else if (tag.find("Color") != std::string::npos) {
            std::string propName = GetAttribute(tag, "name");
            if (!propName.empty()) {
                glm::vec3 c(
                    ParseFloatAttr(tag, "r", 1.0f),
                    ParseFloatAttr(tag, "g", 1.0f),
                    ParseFloatAttr(tag, "b", 1.0f));
                material->AddProperty(propName, MaterialProperty::CreateColor(c));
            }
        }
        else if (tag.find("Texture") != std::string::npos) {
            std::string propName = GetAttribute(tag, "name");
            std::string path = GetAttribute(tag, "path");
            if (!propName.empty() && !path.empty()) {
                auto split = SplitPath(path);
                std::string dir = split.first;
                std::string file = split.second;

                Texture tex{};
                tex.type = propName;
                tex.path = file.c_str();
                tex.textureID = TextureFromFile(file.c_str(), dir, false, false);
                tex.textureGammaID = TextureFromFile(file.c_str(), dir, false, true);

                std::vector<Texture> texs;
                texs.push_back(tex);
                material->AddProperty(propName, MaterialProperty::CreateTexture(texs));
            }
        }
        else if (tag.find("Bool") != std::string::npos) {
            std::string propName = GetAttribute(tag, "name");
            if (!propName.empty()) {
                bool value = ParseBoolAttr(tag, "value", false);
                material->AddProperty(propName, MaterialProperty::CreateBool(value));
            }
		}
        pos = tagEnd + 1;
    }

    material->SetRenderState(renderState);
}

bool XmlMaterialManager::ParseSingleMaterial(const std::string& xmlContent, MaterialFileEntry& entry) {
    // 在整个文件中找到第一个 <Material ...> ... </Material> 块
    size_t matOpen = xmlContent.find("<Material");
    if (matOpen == std::string::npos) {
        std::cout << "XmlMaterialManager: <Material> tag not found in single material file" << std::endl;
        return false;
    }
    size_t matOpenEnd = xmlContent.find('>', matOpen);
    if (matOpenEnd == std::string::npos) {
        std::cout << "XmlMaterialManager: malformed <Material> tag" << std::endl;
        return false;
    }
    size_t matClose = xmlContent.find("</Material>", matOpenEnd);
    if (matClose == std::string::npos) {
        std::cout << "XmlMaterialManager: </Material> not found for single material file" << std::endl;
        return false;
    }
    matClose += std::string("</Material>").length();

    std::string materialBlock = xmlContent.substr(matOpen, matClose - matOpen);

    // 解析头部获取 shader 名称（name 主要用于调试）
    size_t openEnd = materialBlock.find('>');
    if (openEnd == std::string::npos) return false;
    std::string header = materialBlock.substr(0, openEnd + 1);

    std::string shaderName = GetAttribute(header, "shader");
    if (shaderName.empty()) {
        std::cout << "XmlMaterialManager: single material file missing 'shader' attribute, use 'phong' by default" << std::endl;
        shaderName = "phong";
    }

    if (!entry.material) {
        entry.material = std::make_shared<Material>(shaderName);
    } else {
        entry.material->SetShaderName(shaderName);
        entry.material->ClearProperties();
    }

    Material* material = entry.material.get();
    RenderState renderState = material->GetRenderState();

    // 提取内部内容（去掉外层 <Material> ... </Material>）
    std::string inner = materialBlock.substr(openEnd + 1);
    size_t closePos = inner.rfind("</Material>");
    if (closePos != std::string::npos) {
        inner = inner.substr(0, closePos);
    }

    size_t pos = 0;
    while (true) {
        size_t tagStart = inner.find('<', pos);
        if (tagStart == std::string::npos) break;
        size_t tagEnd = inner.find('>', tagStart);
        if (tagEnd == std::string::npos) break;

        std::string tag = inner.substr(tagStart, tagEnd - tagStart + 1);

        // 跳过结束标签
        if (tag.size() >= 2 && tag[1] == '/') {
            pos = tagEnd + 1;
            continue;
        }

        if (tag.find("RenderState") != std::string::npos) {
            renderState.depthTest   = ParseBoolAttr(tag, "depthTest",   renderState.depthTest);
            renderState.depthWrite  = ParseBoolAttr(tag, "depthWrite",  renderState.depthWrite);
            renderState.stencilTest = ParseBoolAttr(tag, "stencilTest", renderState.stencilTest);

            std::string bm = GetAttribute(tag, "blendMode");
            if (!bm.empty()) {
                if (bm == "AlphaBlend")      renderState.blendMode = BlendMode::AlphaBlend;
                else if (bm == "Additive")   renderState.blendMode = BlendMode::Additive;
                else                         renderState.blendMode = BlendMode::None;
            }

            std::string cm = GetAttribute(tag, "cullMode");
            if (!cm.empty()) {
                if (cm == "Front")      renderState.cullMode = CullMode::Front;
                else if (cm == "Back")  renderState.cullMode = CullMode::Back;
                else                    renderState.cullMode = CullMode::None;
            }
        }
        else if (tag.find("Float") != std::string::npos) {
            std::string propName = GetAttribute(tag, "name");
            if (!propName.empty()) {
                float value  = ParseFloatAttr(tag, "value", 0.0f);
                float minVal = ParseFloatAttr(tag, "min",   0.0f);
                float maxVal = ParseFloatAttr(tag, "max",   100.0f);
                float step   = ParseFloatAttr(tag, "step",  0.1f);
                material->AddProperty(propName, MaterialProperty::CreateFloat(value, minVal, maxVal, step));
            }
        }
        else if (tag.find("Vec3") != std::string::npos) {
            std::string propName = GetAttribute(tag, "name");
            if (!propName.empty()) {
                glm::vec3 v(
                    ParseFloatAttr(tag, "x", 0.0f),
                    ParseFloatAttr(tag, "y", 0.0f),
                    ParseFloatAttr(tag, "z", 0.0f));
                float minVal = ParseFloatAttr(tag, "min", 0.0f);
                float maxVal = ParseFloatAttr(tag, "max", 1.0f);
                material->AddProperty(propName, MaterialProperty::CreateVec3(v, minVal, maxVal));
            }
        }
        else if (tag.find("Color") != std::string::npos) {
            std::string propName = GetAttribute(tag, "name");
            if (!propName.empty()) {
                glm::vec3 c(
                    ParseFloatAttr(tag, "r", 1.0f),
                    ParseFloatAttr(tag, "g", 1.0f),
                    ParseFloatAttr(tag, "b", 1.0f));
                material->AddProperty(propName, MaterialProperty::CreateColor(c));
            }
        }
        else if (tag.find("Texture") != std::string::npos) {
            std::string propName = GetAttribute(tag, "name");
            std::string path     = GetAttribute(tag, "path");
            if (!propName.empty() && !path.empty()) {
                auto split = SplitPath(path);
                std::string dir  = split.first;
                std::string file = split.second;

                Texture tex{};
                tex.type = propName;
                tex.path = file.c_str();
                tex.textureID     = TextureFromFile(file.c_str(), dir, false, false);
                tex.textureGammaID = TextureFromFile(file.c_str(), dir, false, true);

                std::vector<Texture> texs;
                texs.push_back(tex);
                material->AddProperty(propName, MaterialProperty::CreateTexture(texs));
            }
        }
        else if (tag.find("Bool") != std::string::npos) {
            std::string propName = GetAttribute(tag, "name");
            if (!propName.empty()) {
                bool value = ParseBoolAttr(tag, "value", false);
                material->AddProperty(propName, MaterialProperty::CreateBool(value));
            }
        }

        pos = tagEnd + 1;
    }

    material->SetRenderState(renderState);
    return true;
}


