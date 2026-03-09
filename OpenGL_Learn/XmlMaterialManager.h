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

    // 按“单文件单材质”的方式获取 / 重载材质。
    // 约定：xmlPath 是类似 "materials/Wood.xml" 这样的路径。
    // 每次调用都会读取该文件内容，如果内容与上次不同，则重新解析并更新对应的 Material。
    // 返回的 Material* 在下一次同路径重载前保持有效。
    Material* GetOrLoadMaterialByFile(const std::string& xmlPath);

    // 收集当前已加载的所有材质（包括 materials.xml 中的命名材质，
    // 以及按文件路径缓存的单文件材质）。
    // key 对于大材质表是材质名，对于单文件材质是 xml 路径。
    std::vector<std::pair<std::string, std::shared_ptr<Material>>> GetAllMaterials() const;

private:
    XmlMaterialManager() = default;

    // 基于简单字符串解析的 XML 处理函数（仅支持本项目所需的少量标签）
    bool ParseDocument(const std::string& xmlContent);
    void ParseMaterialBlock(const std::string& materialBlock);

    // 解析“单材质文件”，将结果写入指定 entry（用于每个 xml 一个材质的情况）
    struct MaterialFileEntry {
        std::shared_ptr<Material> material;
        std::string lastContent;
    };
    bool ParseSingleMaterial(const std::string& xmlContent, MaterialFileEntry& entry);

private:
    std::unordered_map<std::string, std::shared_ptr<Material>> m_materials;
    std::string m_xmlPath;
    bool m_hasLoaded = false;

    // 按文件路径缓存的材质（一个 xml 文件对应一个 entry，用于懒加载 + 热重载）
    std::unordered_map<std::string, MaterialFileEntry> m_materialFiles;
};

