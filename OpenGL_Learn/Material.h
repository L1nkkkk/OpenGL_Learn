#pragma once
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <string>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <vector>
#include <tuple>
#include <unordered_map>
#include "ShaderManager.h"
#include "stb_image.h"
#include "Global.h"

struct Texture {
	unsigned int textureID;
	unsigned int textureGammaID;
	std::string type;
	aiString path;
};

class CubeTexture {
public:
	unsigned int textureID;
	unsigned int textureGammaID;
	CubeTexture(std::string path) {
		int width, height, nrChannels;
		unsigned char* data;
		std::string items[6] = {
			"right.jpg",
			"left.jpg",
			"top.jpg",
			"bottom.jpg",
			"front.jpg",
			"back.jpg"
		};
		stbi_set_flip_vertically_on_load(false);
		glGenTextures(1, &textureID);
		glBindTexture(GL_TEXTURE_CUBE_MAP, textureID);
		for (int i = 0; i < 6; ++i) {
			data = stbi_load((path + '/' + items[i]).c_str(), &width, &height, &nrChannels, 0);
			if (data) {
				glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, data);
				stbi_image_free(data);
			}
			else {
				std::cout << "Cubemap texture failed to load at path: " << items[i] << std::endl;
				stbi_image_free(data);
			}
		}
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

		glGenTextures(1, &textureGammaID);
		glBindTexture(GL_TEXTURE_CUBE_MAP, textureGammaID);
		for (int i = 0; i < 6; ++i) {
			data = stbi_load((path + '/' + items[i]).c_str(), &width, &height, &nrChannels, 0);
			if (data) {
				glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_SRGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, data);
				stbi_image_free(data);
			}
			else {
				std::cout << "Cubemap texture failed to load at path: " << items[i] << std::endl;
				stbi_image_free(data);
			}
		}
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
		glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
		stbi_set_flip_vertically_on_load(true);
	}
private:
	CubeTexture() = default;
};

enum class MaterialPropertyType {
	Float,
	Vec2,
	Vec3,
	Vec4,
	Color, // 等同于Vec3，但ImGui用ColorEdit3
	Int,
	Bool,
	Texture // 纹理ID
};

struct MaterialProperty {
	MaterialPropertyType type = MaterialPropertyType::Int;
	union {
		int intValue = 0;
		float floatValue;
		bool boolValue;
	} scalarValue;
	glm::vec2 vec2Value = glm::vec2(0);
	glm::vec3 vec3Value = glm::vec3(0);
	glm::vec4 vec4Value = glm::vec4(0);

	std::vector<Texture> textures; // 用于存储纹理属性（可扩展为多纹理）

	// ImGui控件用的范围
	float minVal = 0.0f;
	float maxVal = 100.0f;
	float step = 0.1f;

	// 构造函数：不同类型的便捷初始化
	static MaterialProperty CreateTexture(const std::vector<Texture>& texs) {
		MaterialProperty p;
		p.type = MaterialPropertyType::Texture;
		p.textures = texs;
		return p;
	}

	static MaterialProperty CreateInt(int val, int min = 0, int max = 100) {
		MaterialProperty p;
		p.type = MaterialPropertyType::Int;
		p.scalarValue.intValue = val;
		p.minVal = static_cast<float>(min);
		p.maxVal = static_cast<float>(max);
		return p;
	}

	static MaterialProperty CreateFloat(float val, float min = 0.0f, float max = 100.0f, float step = 0.1f) {
		MaterialProperty p;
		p.type = MaterialPropertyType::Float;
		p.scalarValue.floatValue = val;
		p.minVal = min;
		p.maxVal = max;
		p.step = step;
		return p;
	}

	static MaterialProperty CreateVec2(glm::vec2 val, float min = 0.0f, float max = 1.0f) {
		MaterialProperty p;
		p.type = MaterialPropertyType::Vec2;
		p.vec2Value = val;
		p.minVal = min;
		p.maxVal = max;
		return p;
	}

	static MaterialProperty CreateVec3(glm::vec3 val, float min = 0.0f, float max = 1.0f) {
		MaterialProperty p;
		p.type = MaterialPropertyType::Vec3;
		p.vec3Value = val;
		p.minVal = min;
		p.maxVal = max;
		return p;
	}

	static MaterialProperty CreateColor(glm::vec3 val) {
		MaterialProperty p;
		p.type = MaterialPropertyType::Color;
		p.vec3Value = val;
		return p;
	}

	static MaterialProperty CreateBool(bool val) {
		MaterialProperty p;
		p.type = MaterialPropertyType::Bool;
		p.scalarValue.boolValue = val;
		return p;
	}
};

// Material.h：材质基类，定义统一接口
enum class BlendMode { None, AlphaBlend, Additive }; // 混合模式（透明/不透明）
enum class CullMode { None, Front, Back };           // 剔除模式

struct RenderState {
	bool depthTest = true;    // 是否开启深度测试
	bool depthWrite = true;   // 是否写入深度缓冲
	bool stencilTest = false; // 是否开启模板测试
	BlendMode blendMode = BlendMode::None; // 混合模式
	CullMode cullMode = CullMode::Back;    // 剔除模式
	// 可扩展：模板测试参数、深度测试函数等
};

class Material {
public:
	Material(const std::string& shaderName) :m_shaderName(shaderName) {}
	virtual ~Material() = default;

	static RenderState GetCurrentRenderState() {
		RenderState rs;

		// 深度测试开关
		rs.depthTest = (glIsEnabled(GL_DEPTH_TEST) == GL_TRUE);

		// 深度写入（Depth Mask）
		GLboolean depthMask = GL_TRUE;
		glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
		rs.depthWrite = (depthMask == GL_TRUE);

		// 模板测试开关
		rs.stencilTest = (glIsEnabled(GL_STENCIL_TEST) == GL_TRUE);

		// 混合模式
		if (!glIsEnabled(GL_BLEND)) {
			rs.blendMode = BlendMode::None;
		}
		else {
			GLint srcRGB = 0, dstRGB = 0;
			glGetIntegerv(GL_BLEND_SRC_RGB, &srcRGB);
			glGetIntegerv(GL_BLEND_DST_RGB, &dstRGB);

			if (srcRGB == GL_SRC_ALPHA && dstRGB == GL_ONE_MINUS_SRC_ALPHA) {
				rs.blendMode = BlendMode::AlphaBlend;
			}
			else if (srcRGB == GL_SRC_ALPHA && dstRGB == GL_ONE) {
				rs.blendMode = BlendMode::Additive;
			}
			else {
				// 不在你定义的两种里，给一个合理的默认
				rs.blendMode = BlendMode::AlphaBlend;
			}
		}

		// 面剔除模式
		if (!glIsEnabled(GL_CULL_FACE)) {
			rs.cullMode = CullMode::None;
		}
		else {
			GLint mode = 0;
			glGetIntegerv(GL_CULL_FACE_MODE, &mode);
			if (mode == GL_FRONT) {
				rs.cullMode = CullMode::Front;
			}
			else if (mode == GL_BACK) {
				rs.cullMode = CullMode::Back;
			}
			else {
				rs.cullMode = CullMode::Back;
			}
		}

		return rs;
	}

	static void RecoverRenderState(RenderState rs) {
		// 深度测试
		if (rs.depthTest) glEnable(GL_DEPTH_TEST);
		else              glDisable(GL_DEPTH_TEST);

		// 深度写入
		glDepthMask(rs.depthWrite ? GL_TRUE : GL_FALSE);

		// 模板测试
		if (rs.stencilTest) glEnable(GL_STENCIL_TEST);
		else                glDisable(GL_STENCIL_TEST);

		// 混合模式
		switch (rs.blendMode) {
		case BlendMode::None:
			glDisable(GL_BLEND);
			break;
		case BlendMode::AlphaBlend:
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			break;
		case BlendMode::Additive:
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE);
			break;
		}

		// 面剔除
		switch (rs.cullMode) {
		case CullMode::None:
			glDisable(GL_CULL_FACE);
			break;
		case CullMode::Front:
			glEnable(GL_CULL_FACE);
			glCullFace(GL_FRONT);
			break;
		case CullMode::Back:
			glEnable(GL_CULL_FACE);
			glCullFace(GL_BACK);
			break;
		}
	}
	// 仅用于调试 / GUI：允许外部访问属性映射
	const std::unordered_map<std::string, MaterialProperty>& GetProperties() const {
		return m_propertiesMap;
	}
	std::unordered_map<std::string, MaterialProperty>& GetPropertiesMutable() {
		return m_propertiesMap;
	}

	// 重新设置该材质所使用的 Shader 名称（用于从 XML 重新加载时更新绑定）
	void SetShaderName(const std::string& shaderName) {
		m_shaderName = shaderName;
	}

	// 清空当前所有属性，便于从 XML 或其他数据源重新填充
	void ClearProperties() {
		m_propertiesMap.clear();
	}

	void AddProperty(const std::string& uniformName, const MaterialProperty& param) {
		m_propertiesMap[uniformName] = param;
	}

	// 激活材质（绑定Shader + 设置渲染状态 + 传递材质参数）
	virtual void Activate() {
		// 1. 绑定该材质的Shader
		ShaderManager::GetInstance().UseShader(m_shaderName);
		// 2. 设置渲染状态
		ApplyRenderState();
		// 3. 传递材质参数（由子类实现）
		SetMaterialParams();
	}

	// 获取材质对应的Shader名称
	std::string GetShaderName() const { return m_shaderName; }
	// 设置/获取渲染状态
	void SetRenderState(const RenderState& state) { m_renderState = state; }
	RenderState GetRenderState() const { return m_renderState; }

	unsigned int GetTextureCount() const {
		unsigned int count = 0;
		for (const auto& [name, prop] : m_propertiesMap) {
			if (prop.type == MaterialPropertyType::Texture) {
				count += prop.textures.size();
			}
		}
		return count;
	}

private:
	std::string m_shaderName;
	std::unordered_map<std::string, MaterialProperty> m_propertiesMap;
	RenderState m_renderState;
protected:
	// 应用渲染状态（子类可重写）
	virtual void ApplyRenderState() {
		// 深度测试
		if (m_renderState.depthTest) glEnable(GL_DEPTH_TEST);
		else glDisable(GL_DEPTH_TEST);
		// 深度写入
		glDepthMask(m_renderState.depthWrite ? GL_TRUE : GL_FALSE);
		// 混合模式
		if (m_renderState.blendMode != BlendMode::None) {
			glEnable(GL_BLEND);
			if (m_renderState.blendMode == BlendMode::AlphaBlend) {
				glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			}
			else if (m_renderState.blendMode == BlendMode::Additive) {
				glBlendFunc(GL_SRC_ALPHA, GL_ONE);
			}
		}
		else {
			glDisable(GL_BLEND);
		}
		// 剔除模式
		glEnable(GL_CULL_FACE);
		switch (m_renderState.cullMode) {
		case CullMode::Front: glCullFace(GL_FRONT); break;
		case CullMode::Back: glCullFace(GL_BACK); break;
		case CullMode::None: glDisable(GL_CULL_FACE); break;
		}

	};

	virtual void SetMaterialParams() {
		auto shaderPtr = ShaderManager::GetInstance().GetShaderByName(m_shaderName);
		auto& properties = SystemProperties::GetInstance();
		if (!shaderPtr) {
			std::cout << "Shader not found for material: " << m_shaderName << std::endl;
			return;
		}
		for (auto& [propertyName, property] : m_propertiesMap) {
			switch (property.type) {
			case MaterialPropertyType::Float:
				shaderPtr->setFloat("material."+propertyName, property.scalarValue.floatValue);
				break;
			case MaterialPropertyType::Vec2:
				shaderPtr->setVec2("material."+propertyName, property.vec2Value);
				break;
			case MaterialPropertyType::Vec3:
			case MaterialPropertyType::Color: // Color也当作Vec3处理
				shaderPtr->setVec3("material." + propertyName, property.vec3Value);
				break;
			case MaterialPropertyType::Vec4:
				shaderPtr->setVec4("material." + propertyName, property.vec4Value);
				break;
			case MaterialPropertyType::Bool:
				shaderPtr->setBool("material." + propertyName, property.scalarValue.boolValue);
				break;
			case MaterialPropertyType::Int:
				shaderPtr->setInt("material." + propertyName, property.scalarValue.intValue);
				break;
			case MaterialPropertyType::Texture:
				shaderPtr->setBool("material.use_" + propertyName, false);
				for(int i = 0; i < property.textures.size(); ++i) {
					glActiveTexture(GL_TEXTURE0 + properties.USED_TEXTURE_NUM);
					if (properties.GAMMA_CORRECTION)
						glBindTexture(GL_TEXTURE_2D, property.textures[i].textureGammaID);
					else
						glBindTexture(GL_TEXTURE_2D, property.textures[i].textureID);
					shaderPtr->setInt("material." + propertyName + std::to_string(i+1), properties.USED_TEXTURE_NUM++); // 纹理单元
					shaderPtr->setBool("material.use_" + propertyName, true);
				}
				break;
			}
		}
	}
};

class MaterialGaurd {
public:
	MaterialGaurd(Material& material) :m_material(material) {
		m_previousState = Material::GetCurrentRenderState();
		m_material.Activate();
	}
	~MaterialGaurd() {
		// 解绑纹理
		auto& property = SystemProperties::GetInstance();
		int TextureUsedNum = m_material.GetTextureCount();
		for(int i = 0; i < TextureUsedNum; ++i) {
			glActiveTexture(GL_TEXTURE0 + --property.USED_TEXTURE_NUM);
			glBindTexture(GL_TEXTURE_2D, 0);
		}
		Material::RecoverRenderState(m_previousState);
	}
private:
	Material& m_material;
	RenderState m_previousState;
};