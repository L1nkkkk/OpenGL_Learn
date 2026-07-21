#include "XmlMaterialManager.h"
#include "Profiler.h"

#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

namespace {
    std::string GetAttribute(const std::string& tag, const std::string& attrName) {
        const std::string key = attrName + "=\"";
        size_t pos = tag.find(key);
        if (pos == std::string::npos) {
            return {};
        }
        pos += key.size();
        const size_t end = tag.find('"', pos);
        if (end == std::string::npos) {
            return {};
        }
        return tag.substr(pos, end - pos);
    }

    float ParseFloatAttr(const std::string& tag, const std::string& name, float defaultValue) {
        const std::string value = GetAttribute(tag, name);
        if (value.empty()) {
            return defaultValue;
        }
        try {
            return std::stof(value);
        }
        catch (...) {
            return defaultValue;
        }
    }

    bool ParseBoolAttr(const std::string& tag, const std::string& name, bool defaultValue) {
        const std::string value = GetAttribute(tag, name);
        if (value.empty()) {
            return defaultValue;
        }
        if (value == "true" || value == "1") {
            return true;
        }
        if (value == "false" || value == "0") {
            return false;
        }
        return defaultValue;
    }

    std::pair<std::string, std::string> SplitPath(const std::string& fullPath) {
        const size_t pos = fullPath.find_last_of("/\\");
        if (pos == std::string::npos) {
            return { "", fullPath };
        }
        return { fullPath.substr(0, pos), fullPath.substr(pos + 1) };
    }
}

bool XmlMaterialManager::TryGetWriteTime(const std::string& path, fs::file_time_type& outTime) {
    PerformanceProfiler::GetInstance().RecordFileSystemCheck();
    try {
        if (!fs::exists(path)) {
            return false;
        }
        outTime = fs::last_write_time(path);
        return true;
    }
    catch (...) {
        return false;
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
    const std::string content = buffer.str();

    if (!ParseDocument(content)) {
        std::cout << "XmlMaterialManager: parse failed for '" << xmlPath << "'" << std::endl;
        m_lastReloadSuccessful = false;
        m_lastReloadMessage = "Failed to parse materials table: " + xmlPath;
        return false;
    }

    m_hasLoaded = true;
    TryGetWriteTime(xmlPath, m_xmlWriteTime);
    m_lastReloadSuccessful = true;
    m_lastReloadMessage = "Reloaded materials table: " + xmlPath;
    ++m_reloadCount;
    ++m_materialRevision;
    std::cout << "XmlMaterialManager: loaded materials from '" << xmlPath << "'" << std::endl;
    return true;
}

void XmlMaterialManager::ReloadIfFileChanged() {
    ReloadChangedFiles();
}

int XmlMaterialManager::ReloadChangedFiles() {
    int reloads = 0;

    if (!m_xmlPath.empty()) {
        fs::file_time_type currentWriteTime;
        if (TryGetWriteTime(m_xmlPath, currentWriteTime) && currentWriteTime != m_xmlWriteTime && LoadFromFile(m_xmlPath)) {
            ++reloads;
        }
    }

    for (auto& [path, entry] : m_materialFiles) {
        fs::file_time_type currentWriteTime;
        if (!TryGetWriteTime(path, currentWriteTime) || currentWriteTime == entry.lastWriteTime) {
            continue;
        }

        if (LoadMaterialFile(path)) {
            m_lastReloadSuccessful = true;
            m_lastReloadMessage = "Reloaded material file: " + path;
            ++m_reloadCount;
            ++reloads;
        }
        else {
            m_lastReloadSuccessful = false;
            m_lastReloadMessage = "Failed to reload material file: " + path;
        }
    }

    return reloads;
}

int XmlMaterialManager::ReloadAllFiles() {
    int reloads = 0;

    if (!m_xmlPath.empty() && LoadFromFile(m_xmlPath)) {
        ++reloads;
    }

    for (auto& [path, entry] : m_materialFiles) {
        entry.lastContent.clear();
        if (LoadMaterialFile(path)) {
            m_lastReloadSuccessful = true;
            m_lastReloadMessage = "Reloaded material file: " + path;
            ++m_reloadCount;
            ++reloads;
        }
    }

    return reloads;
}

std::shared_ptr<Material> XmlMaterialManager::GetMaterial(const std::string& name) {
    const auto it = m_materials.find(name);
    return it != m_materials.end() ? it->second : nullptr;
}

Material* XmlMaterialManager::GetMaterialRaw(const std::string& name) {
    auto material = GetMaterial(name);
    return material ? material.get() : nullptr;
}

Material* XmlMaterialManager::GetOrLoadMaterialByFile(const std::string& xmlPath) {
    auto entryIt = m_materialFiles.find(xmlPath);
    if (entryIt != m_materialFiles.end() && entryIt->second.material) {
        return entryIt->second.material.get();
    }

    return LoadMaterialFile(xmlPath);
}

bool XmlMaterialManager::ReloadMaterialFile(const std::string& xmlPath) {
    if (LoadMaterialFile(xmlPath)) {
        m_lastReloadSuccessful = true;
        m_lastReloadMessage = "Reloaded material file: " + xmlPath;
        ++m_reloadCount;
        return true;
    }

    m_lastReloadSuccessful = false;
    m_lastReloadMessage = "Failed to reload material file: " + xmlPath;
    return false;
}

Material* XmlMaterialManager::LoadMaterialFile(const std::string& xmlPath) {
    auto entryIt = m_materialFiles.find(xmlPath);
    fs::file_time_type currentWriteTime;

    std::ifstream file(xmlPath);
    if (!file.is_open()) {
        std::cout << "XmlMaterialManager: failed to open material file '" << xmlPath << "'" << std::endl;
        return entryIt != m_materialFiles.end() ? entryIt->second.material.get() : nullptr;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string content = buffer.str();
    auto& entry = m_materialFiles[xmlPath];

    if (entry.material && entry.lastContent == content) {
        if (currentWriteTime == fs::file_time_type{}) {
            TryGetWriteTime(xmlPath, currentWriteTime);
        }
        entry.lastWriteTime = currentWriteTime;
        return entry.material.get();
    }

    if (!ParseSingleMaterial(content, entry)) {
        std::cout << "XmlMaterialManager: parse failed for '" << xmlPath << "'" << std::endl;
        m_lastReloadSuccessful = false;
        m_lastReloadMessage = "Failed to parse material file: " + xmlPath;
        return entry.material.get();
    }

    entry.lastContent = content;
    if (currentWriteTime == fs::file_time_type{}) {
        TryGetWriteTime(xmlPath, currentWriteTime);
    }
    entry.lastWriteTime = currentWriteTime;
    ++m_materialRevision;
    return entry.material.get();
}

std::vector<std::pair<std::string, std::shared_ptr<Material>>> XmlMaterialManager::GetAllMaterials() const {
    std::vector<std::pair<std::string, std::shared_ptr<Material>>> result;
    result.reserve(m_materials.size() + m_materialFiles.size());

    for (const auto& kv : m_materials) {
        result.emplace_back(kv.first, kv.second);
    }
    for (const auto& kv : m_materialFiles) {
        if (kv.second.material) {
            result.emplace_back(kv.first, kv.second.material);
        }
    }
    return result;
}

bool XmlMaterialManager::ParseDocument(const std::string& xmlContent) {
    size_t rootStart = xmlContent.find("<Materials");
    if (rootStart == std::string::npos) {
        std::cout << "XmlMaterialManager: root <Materials> not found" << std::endl;
        return false;
    }

    rootStart = xmlContent.find('>', rootStart);
    if (rootStart == std::string::npos) {
        return false;
    }
    ++rootStart;

    const size_t rootEnd = xmlContent.find("</Materials>", rootStart);
    if (rootEnd == std::string::npos) {
        return false;
    }

    const std::string body = xmlContent.substr(rootStart, rootEnd - rootStart);
    size_t pos = 0;
    while (true) {
        const size_t matOpen = body.find("<Material", pos);
        if (matOpen == std::string::npos) {
            break;
        }
        const size_t matOpenEnd = body.find('>', matOpen);
        if (matOpenEnd == std::string::npos) {
            break;
        }
        size_t matClose = body.find("</Material>", matOpenEnd);
        if (matClose == std::string::npos) {
            break;
        }
        matClose += std::string("</Material>").length();
        ParseMaterialBlock(body.substr(matOpen, matClose - matOpen));
        pos = matClose;
    }

    return true;
}

void XmlMaterialManager::ParseMaterialBlock(const std::string& materialBlock) {
    const size_t openEnd = materialBlock.find('>');
    if (openEnd == std::string::npos) {
        return;
    }

    const std::string header = materialBlock.substr(0, openEnd + 1);
    const std::string matName = GetAttribute(header, "name");
    const std::string shaderName = GetAttribute(header, "shader");
    if (matName.empty() || shaderName.empty()) {
        std::cout << "XmlMaterialManager: <Material> missing 'name' or 'shader' attribute" << std::endl;
        return;
    }

    std::shared_ptr<Material> material;
    const auto it = m_materials.find(matName);
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
    std::string inner = materialBlock.substr(openEnd + 1);
    const size_t closePos = inner.rfind("</Material>");
    if (closePos != std::string::npos) {
        inner = inner.substr(0, closePos);
    }

    size_t pos = 0;
    while (true) {
        const size_t tagStart = inner.find('<', pos);
        if (tagStart == std::string::npos) {
            break;
        }
        const size_t tagEnd = inner.find('>', tagStart);
        if (tagEnd == std::string::npos) {
            break;
        }

        const std::string tag = inner.substr(tagStart, tagEnd - tagStart + 1);
        if (tag.size() >= 2 && tag[1] == '/') {
            pos = tagEnd + 1;
            continue;
        }

        if (tag.find("RenderState") != std::string::npos) {
            renderState.depthTest = ParseBoolAttr(tag, "depthTest", renderState.depthTest);
            renderState.depthWrite = ParseBoolAttr(tag, "depthWrite", renderState.depthWrite);
            renderState.stencilTest = ParseBoolAttr(tag, "stencilTest", renderState.stencilTest);

            const std::string blendMode = GetAttribute(tag, "blendMode");
            if (!blendMode.empty()) {
                if (blendMode == "AlphaBlend") renderState.blendMode = BlendMode::AlphaBlend;
                else if (blendMode == "Additive") renderState.blendMode = BlendMode::Additive;
                else renderState.blendMode = BlendMode::None;
            }

            const std::string cullMode = GetAttribute(tag, "cullMode");
            if (!cullMode.empty()) {
                if (cullMode == "Front") renderState.cullMode = CullMode::Front;
                else if (cullMode == "Back") renderState.cullMode = CullMode::Back;
                else renderState.cullMode = CullMode::None;
            }
        }
        else if (tag.find("Float") != std::string::npos) {
            const std::string propName = GetAttribute(tag, "name");
            if (!propName.empty()) {
                material->AddProperty(propName, MaterialProperty::CreateFloat(
                    ParseFloatAttr(tag, "value", 0.0f),
                    ParseFloatAttr(tag, "min", 0.0f),
                    ParseFloatAttr(tag, "max", 100.0f),
                    ParseFloatAttr(tag, "step", 0.1f)));
            }
        }
        else if (tag.find("Vec3") != std::string::npos) {
            const std::string propName = GetAttribute(tag, "name");
            if (!propName.empty()) {
                material->AddProperty(propName, MaterialProperty::CreateVec3(
                    glm::vec3(
                        ParseFloatAttr(tag, "x", 0.0f),
                        ParseFloatAttr(tag, "y", 0.0f),
                        ParseFloatAttr(tag, "z", 0.0f)),
                    ParseFloatAttr(tag, "min", 0.0f),
                    ParseFloatAttr(tag, "max", 1.0f)));
            }
        }
        else if (tag.find("Color") != std::string::npos) {
            const std::string propName = GetAttribute(tag, "name");
            if (!propName.empty()) {
                material->AddProperty(propName, MaterialProperty::CreateColor(glm::vec3(
                    ParseFloatAttr(tag, "r", 1.0f),
                    ParseFloatAttr(tag, "g", 1.0f),
                    ParseFloatAttr(tag, "b", 1.0f))));
            }
        }
        else if (tag.find("Texture") != std::string::npos) {
            const std::string propName = GetAttribute(tag, "name");
            const std::string path = GetAttribute(tag, "path");
            if (!propName.empty() && !path.empty()) {
                const auto split = SplitPath(path);
                Texture tex{};
                tex.type = propName;
                tex.path = split.second.c_str();
                const bool srgb = propName == "texture_diffuse" || propName == "albedo" ||
                    propName == "baseColor" || propName == "texture_emissive";
                tex.textureID = TextureFromFile(split.second.c_str(), split.first, false, srgb);
                tex.textureGammaID = tex.textureID;
                material->AddProperty(propName, MaterialProperty::CreateTexture({ tex }));
            }
        }
        else if (tag.find("Bool") != std::string::npos) {
            const std::string propName = GetAttribute(tag, "name");
            if (!propName.empty()) {
                material->AddProperty(propName, MaterialProperty::CreateBool(ParseBoolAttr(tag, "value", false)));
            }
        }

        pos = tagEnd + 1;
    }

    material->SetRenderState(renderState);
}

bool XmlMaterialManager::ParseSingleMaterial(const std::string& xmlContent, MaterialFileEntry& entry) {
    const size_t matOpen = xmlContent.find("<Material");
    if (matOpen == std::string::npos) {
        std::cout << "XmlMaterialManager: <Material> tag not found in single material file" << std::endl;
        return false;
    }
    const size_t matOpenEnd = xmlContent.find('>', matOpen);
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

    const std::string materialBlock = xmlContent.substr(matOpen, matClose - matOpen);
    const size_t openEnd = materialBlock.find('>');
    if (openEnd == std::string::npos) {
        return false;
    }

    const std::string header = materialBlock.substr(0, openEnd + 1);
    std::string shaderName = GetAttribute(header, "shader");
    if (shaderName.empty()) {
        shaderName = "phong";
    }

    if (!entry.material) {
        entry.material = std::make_shared<Material>(shaderName);
    }
    else {
        entry.material->SetShaderName(shaderName);
        entry.material->ClearProperties();
    }

    Material* material = entry.material.get();
    RenderState renderState = material->GetRenderState();

    std::string inner = materialBlock.substr(openEnd + 1);
    const size_t closePos = inner.rfind("</Material>");
    if (closePos != std::string::npos) {
        inner = inner.substr(0, closePos);
    }

    size_t pos = 0;
    while (true) {
        const size_t tagStart = inner.find('<', pos);
        if (tagStart == std::string::npos) {
            break;
        }
        const size_t tagEnd = inner.find('>', tagStart);
        if (tagEnd == std::string::npos) {
            break;
        }

        const std::string tag = inner.substr(tagStart, tagEnd - tagStart + 1);
        if (tag.size() >= 2 && tag[1] == '/') {
            pos = tagEnd + 1;
            continue;
        }

        if (tag.find("RenderState") != std::string::npos) {
            renderState.depthTest = ParseBoolAttr(tag, "depthTest", renderState.depthTest);
            renderState.depthWrite = ParseBoolAttr(tag, "depthWrite", renderState.depthWrite);
            renderState.stencilTest = ParseBoolAttr(tag, "stencilTest", renderState.stencilTest);

            const std::string blendMode = GetAttribute(tag, "blendMode");
            if (!blendMode.empty()) {
                if (blendMode == "AlphaBlend") renderState.blendMode = BlendMode::AlphaBlend;
                else if (blendMode == "Additive") renderState.blendMode = BlendMode::Additive;
                else renderState.blendMode = BlendMode::None;
            }

            const std::string cullMode = GetAttribute(tag, "cullMode");
            if (!cullMode.empty()) {
                if (cullMode == "Front") renderState.cullMode = CullMode::Front;
                else if (cullMode == "Back") renderState.cullMode = CullMode::Back;
                else renderState.cullMode = CullMode::None;
            }
        }
        else if (tag.find("Float") != std::string::npos) {
            const std::string propName = GetAttribute(tag, "name");
            if (!propName.empty()) {
                material->AddProperty(propName, MaterialProperty::CreateFloat(
                    ParseFloatAttr(tag, "value", 0.0f),
                    ParseFloatAttr(tag, "min", 0.0f),
                    ParseFloatAttr(tag, "max", 100.0f),
                    ParseFloatAttr(tag, "step", 0.1f)));
            }
        }
        else if (tag.find("Vec3") != std::string::npos) {
            const std::string propName = GetAttribute(tag, "name");
            if (!propName.empty()) {
                material->AddProperty(propName, MaterialProperty::CreateVec3(
                    glm::vec3(
                        ParseFloatAttr(tag, "x", 0.0f),
                        ParseFloatAttr(tag, "y", 0.0f),
                        ParseFloatAttr(tag, "z", 0.0f)),
                    ParseFloatAttr(tag, "min", 0.0f),
                    ParseFloatAttr(tag, "max", 1.0f)));
            }
        }
        else if (tag.find("Color") != std::string::npos) {
            const std::string propName = GetAttribute(tag, "name");
            if (!propName.empty()) {
                material->AddProperty(propName, MaterialProperty::CreateColor(glm::vec3(
                    ParseFloatAttr(tag, "r", 1.0f),
                    ParseFloatAttr(tag, "g", 1.0f),
                    ParseFloatAttr(tag, "b", 1.0f))));
            }
        }
        else if (tag.find("Texture") != std::string::npos) {
            const std::string propName = GetAttribute(tag, "name");
            const std::string path = GetAttribute(tag, "path");
            if (!propName.empty() && !path.empty()) {
                const auto split = SplitPath(path);
                Texture tex{};
                tex.type = propName;
                tex.path = split.second.c_str();
                const bool srgb = propName == "texture_diffuse" || propName == "albedo" ||
                    propName == "baseColor" || propName == "texture_emissive";
                tex.textureID = TextureFromFile(split.second.c_str(), split.first, false, srgb);
                tex.textureGammaID = tex.textureID;
                material->AddProperty(propName, MaterialProperty::CreateTexture({ tex }));
            }
        }
        else if (tag.find("Bool") != std::string::npos) {
            const std::string propName = GetAttribute(tag, "name");
            if (!propName.empty()) {
                material->AddProperty(propName, MaterialProperty::CreateBool(ParseBoolAttr(tag, "value", false)));
            }
        }

        pos = tagEnd + 1;
    }

    material->SetRenderState(renderState);
    return true;
}
