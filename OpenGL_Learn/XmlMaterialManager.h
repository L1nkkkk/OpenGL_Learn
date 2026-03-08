#pragma once

#include <string>
#include <unordered_map>
#include <memory>

#include "Material.h"
#include "Global.h"

// XmlMaterialManager：从 XML 文件加载并管理引擎中的材质（基于已有 Material / MaterialProperty）
class XmlMaterialManager {
public:
    static XmlMaterialManager& GetInstance() {
        static XmlMaterialManager instance;
        return instance;
    }

    XmlMaterialManager(const XmlMaterialManager&) = delete;
    XmlMaterialManager& operator=(const XmlMaterialManager&) = delete;

    // 设置 XML 路径并立刻尝试加载
    bool LoadFromFile(const std::string& xmlPath);

    // 若文件时间戳发生变化，则重新加载 XML
    void ReloadIfFileChanged();

    // 获取共享材质；若不存在返回空指针
    std::shared_ptr<Material> GetMaterial(const std::string& name);

    // 获取原始指针，方便和现有的 Mesh::material_ptr 兼容
    Material* GetMaterialRaw(const std::string& name);

    // 获取当前所有已加载材质（用于调试 / ImGui 展示）
    const std::unordered_map<std::string, std::shared_ptr<Material>>& GetMaterials() const {
        return m_materials;
    }

    const std::string& GetXmlPath() const { return m_xmlPath; }

private:
    XmlMaterialManager() = default;

    // 基于简单字符串解析的 XML 处理函数（仅支持本项目所需的少量标签）
    bool ParseDocument(const std::string& xmlContent);
    void ParseMaterialBlock(const std::string& materialBlock);

private:
    std::unordered_map<std::string, std::shared_ptr<Material>> m_materials;
    std::string m_xmlPath;
    bool m_hasLoaded = false;
};

