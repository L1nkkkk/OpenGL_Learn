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
#include "GLStateCache.h"
#include "Profiler.h"

struct Texture {
	unsigned int textureID;
	unsigned int textureGammaID;
	std::string type;
	aiString path;
};

class CubeTexture {
public:
	unsigned int textureID = 0;
	unsigned int textureGammaID = 0;
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
		GLState::BindTexture(GL_TEXTURE_CUBE_MAP, textureID);
		for (int i = 0; i < 6; ++i) {
			data = stbi_load((path + '/' + items[i]).c_str(), &width, &height, &nrChannels, 0);
			if (data) {
				glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_SRGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, data);
				m_estimatedBytes += static_cast<std::uint64_t>(width) *
					static_cast<std::uint64_t>(height) * 3u;
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
		textureGammaID = textureID;
		PerformanceProfiler::GetInstance().RecordMemoryAllocation(
			MemoryResourceType::Texture,
			m_estimatedBytes);
		GLState::BindTexture(GL_TEXTURE_CUBE_MAP, 0);
		stbi_set_flip_vertically_on_load(true);
	}
	~CubeTexture() {
		Release();
	}

	CubeTexture(const CubeTexture&) = delete;
	CubeTexture& operator=(const CubeTexture&) = delete;

	void Release() {
		if (textureID == 0) {
			return;
		}
		GLState::ForgetTexture(textureID);
		glDeleteTextures(1, &textureID);
		textureID = 0;
		textureGammaID = 0;
		PerformanceProfiler::GetInstance().RecordMemoryRelease(
			MemoryResourceType::Texture,
			m_estimatedBytes);
		m_estimatedBytes = 0;
	}
private:
	CubeTexture() = default;
	std::uint64_t m_estimatedBytes = 0;
};

enum class MaterialPropertyType {
	Float,
	Vec2,
	Vec3,
	Vec4,
	Color, // ��ͬ��Vec3����ImGui��ColorEdit3
	Int,
	Bool,
	Texture // ����ID
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

	std::vector<Texture> textures; // ���ڴ洢�������ԣ�����չΪ��������

	// ImGui�ؼ��õķ�Χ
	float minVal = 0.0f;
	float maxVal = 100.0f;
	float step = 0.1f;

	// ���캯������ͬ���͵ı�ݳ�ʼ��
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

// Material.h�����ʻ��࣬����ͳһ�ӿ�
enum class BlendMode { None, AlphaBlend, Additive }; // ���ģʽ��͸��/��͸����
enum class CullMode { None, Front, Back };           // �޳�ģʽ

struct RenderState {
	bool depthTest = true;    // �Ƿ�����Ȳ���
	bool depthWrite = true;   // �Ƿ�д����Ȼ���
	bool stencilTest = false; // �Ƿ���ģ�����
	BlendMode blendMode = BlendMode::None; // ���ģʽ
	CullMode cullMode = CullMode::Back;    // �޳�ģʽ
	// ����չ��ģ����Բ�������Ȳ��Ժ�����
};

class Material {
public:
	Material(const std::string& shaderName) :m_shaderName(shaderName) {}
	virtual ~Material() = default;

	static RenderState GetCurrentRenderState() {
		RenderState rs;
		rs.depthTest = GLState::IsEnabled(GL_DEPTH_TEST);
		rs.depthWrite = GLState::GetDepthMask();
		rs.stencilTest = GLState::IsEnabled(GL_STENCIL_TEST);

		// ���ģʽ
		if (!GLState::IsEnabled(GL_BLEND)) {
			rs.blendMode = BlendMode::None;
		}
		else {
			GLenum srcRGB = GL_ONE;
			GLenum dstRGB = GL_ZERO;
			GLState::GetBlendFunc(srcRGB, dstRGB);

			if (srcRGB == GL_SRC_ALPHA && dstRGB == GL_ONE_MINUS_SRC_ALPHA) {
				rs.blendMode = BlendMode::AlphaBlend;
			}
			else if (srcRGB == GL_SRC_ALPHA && dstRGB == GL_ONE) {
				rs.blendMode = BlendMode::Additive;
			}
			else {
				// �����㶨����������һ��������Ĭ��
				rs.blendMode = BlendMode::AlphaBlend;
			}
		}

		// ���޳�ģʽ
		if (!GLState::IsEnabled(GL_CULL_FACE)) {
			rs.cullMode = CullMode::None;
		}
		else {
			const GLenum mode = GLState::GetCullFace();
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
		// ��Ȳ���
		if (rs.depthTest) GLState::Enable(GL_DEPTH_TEST);
		else              GLState::Disable(GL_DEPTH_TEST);

		// ���д��
		GLState::DepthMask(rs.depthWrite);

		// ģ�����
		if (rs.stencilTest) GLState::Enable(GL_STENCIL_TEST);
		else                GLState::Disable(GL_STENCIL_TEST);

		// ���ģʽ
		switch (rs.blendMode) {
		case BlendMode::None:
			GLState::Disable(GL_BLEND);
			break;
		case BlendMode::AlphaBlend:
			GLState::Enable(GL_BLEND);
			GLState::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			break;
		case BlendMode::Additive:
			GLState::Enable(GL_BLEND);
			GLState::BlendFunc(GL_SRC_ALPHA, GL_ONE);
			break;
		}

		// ���޳�
		switch (rs.cullMode) {
		case CullMode::None:
			GLState::Disable(GL_CULL_FACE);
			break;
		case CullMode::Front:
			GLState::Enable(GL_CULL_FACE);
			GLState::CullFace(GL_FRONT);
			break;
		case CullMode::Back:
			GLState::Enable(GL_CULL_FACE);
			GLState::CullFace(GL_BACK);
			break;
		}
	}
	// �����ڵ��� / GUI�������ⲿ��������ӳ��
	const std::unordered_map<std::string, MaterialProperty>& GetProperties() const {
		return m_propertiesMap;
	}
	std::unordered_map<std::string, MaterialProperty>& GetPropertiesMutable() {
		return m_propertiesMap;
	}

	// �������øò�����ʹ�õ� Shader ���ƣ����ڴ� XML ���¼���ʱ���°󶨣�
	void SetShaderName(const std::string& shaderName) {
		m_shaderName = shaderName;
	}

	// ��յ�ǰ�������ԣ����ڴ� XML ����������Դ�������
	void ClearProperties() {
		m_propertiesMap.clear();
	}

	void AddProperty(const std::string& uniformName, const MaterialProperty& param) {
		m_propertiesMap[uniformName] = param;
	}

	// ������ʣ���Shader + ������Ⱦ״̬ + ���ݲ��ʲ�����
	virtual void Activate() {
		// 1. �󶨸ò��ʵ�Shader
		ShaderManager::GetInstance().UseShader(m_shaderName);
		// 2. ������Ⱦ״̬
		ApplyRenderState();
		// 3. ���ݲ��ʲ�����������ʵ�֣�
		auto shaderPtr = ShaderManager::GetInstance().GetShaderByName(m_shaderName);
		if (shaderPtr) {
			SetMaterialParamsToShader(*shaderPtr, shaderPtr->shaderName == "deferProcess");
		}
	}

	// ������ʵ��ⲿָ�� shader������ deferred geometry pass �� override shader��
	virtual void Activate(Shader* overrideShader) {
		if (!overrideShader) {
			Activate();
			return;
		}
		overrideShader->use();
		if (overrideShader->shaderName == "shadow" || overrideShader->shaderName == "shadowCube") {
			// Transparent materials often use depthWrite=false; shadow pass must still
			// write depth, otherwise transparent meshes disappear from shadow maps.
			GLState::Enable(GL_DEPTH_TEST);
			GLState::DepthMask(true);
			GLState::Disable(GL_BLEND);
			return;
		}
		ApplyRenderState();

		// Special-case: AOInfo pass needs stable depth testing to avoid overdraw/ghosting
		// in the normal attachment (Color2).
		if (overrideShader->shaderName == "aoInfo") {
			GLState::Enable(GL_DEPTH_TEST);
			GLState::DepthMask(false); // normal-only pass: don't modify depth buffer
			GLState::Disable(GL_BLEND);
			GLState::Disable(GL_STENCIL_TEST);
			GLState::StencilFunc(GL_ALWAYS, 0, 0xFF);
			GLState::StencilMask(0x00);
			GLState::CullFace(GL_BACK); // keep default culling behavior
		}

		SetMaterialParamsToShader(*overrideShader, overrideShader->shaderName == "deferProcess");
	}

	// ��ȡ���ʶ�Ӧ��Shader����
	std::string GetShaderName() const { return m_shaderName; }
	// ����/��ȡ��Ⱦ״̬
	void SetRenderState(const RenderState& state) { m_renderState = state; }
	RenderState GetRenderState() const { return m_renderState; }

	unsigned int GetTextureCount() const {
		unsigned int count = 0;
		for (const auto& [name, prop] : m_propertiesMap) {
			if (prop.type == MaterialPropertyType::Texture) {
				count += static_cast<unsigned int>(prop.textures.size());
			}
		}
		return count;
	}

private:
	std::string m_shaderName;
	std::unordered_map<std::string, MaterialProperty> m_propertiesMap;
	RenderState m_renderState;
protected:
	// Ӧ����Ⱦ״̬���������д��
	virtual void ApplyRenderState() {
		// ��Ȳ���
		if (m_renderState.depthTest) GLState::Enable(GL_DEPTH_TEST);
		else GLState::Disable(GL_DEPTH_TEST);
		// ���д��
		GLState::DepthMask(m_renderState.depthWrite);
		// ���ģʽ
		if (m_renderState.blendMode != BlendMode::None) {
			GLState::Enable(GL_BLEND);
			if (m_renderState.blendMode == BlendMode::AlphaBlend) {
				GLState::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			}
			else if (m_renderState.blendMode == BlendMode::Additive) {
				GLState::BlendFunc(GL_SRC_ALPHA, GL_ONE);
			}
		}
		else {
			GLState::Disable(GL_BLEND);
		}
		// �޳�ģʽ
		GLState::Enable(GL_CULL_FACE);
		switch (m_renderState.cullMode) {
		case CullMode::Front: GLState::CullFace(GL_FRONT); break;
		case CullMode::Back: GLState::CullFace(GL_BACK); break;
		case CullMode::None: GLState::Disable(GL_CULL_FACE); break;
		}

	};

	virtual void SetMaterialParamsToShader(Shader& shader, bool deferProcessMode = false) {
		auto& properties = SystemProperties::GetInstance();

		// deferred geometry pass uses simplified sampler names.
		if (deferProcessMode) {
			shader.setBool("hasDiffuseMap", false);
			shader.setBool("hasSpecularMap", false);
			shader.setBool("hasNormalMap", false);
		}

		for (auto& [propertyName, property] : m_propertiesMap) {
			switch (property.type) {
			case MaterialPropertyType::Float:
				shader.setFloat("material."+propertyName, property.scalarValue.floatValue);
				break;
			case MaterialPropertyType::Vec2:
				shader.setVec2("material."+propertyName, property.vec2Value);
				break;
			case MaterialPropertyType::Vec3:
			case MaterialPropertyType::Color: // ColorҲ����Vec3����
				shader.setVec3("material." + propertyName, property.vec3Value);
				break;
			case MaterialPropertyType::Vec4:
				shader.setVec4("material." + propertyName, property.vec4Value);
				break;
			case MaterialPropertyType::Bool:
				shader.setBool("material." + propertyName, property.scalarValue.boolValue);
				break;
			case MaterialPropertyType::Int:
				shader.setInt("material." + propertyName, property.scalarValue.intValue);
				break;
			case MaterialPropertyType::Texture:
				if (!deferProcessMode) {
					shader.setBool("material.use_" + propertyName, false);
					for(int i = 0; i < property.textures.size(); ++i) {
						GLState::ActiveTexture(GL_TEXTURE0 + properties.USED_TEXTURE_NUM);
						if (properties.GAMMA_CORRECTION)
							GLState::BindTexture(GL_TEXTURE_2D, property.textures[i].textureGammaID);
						else
							GLState::BindTexture(GL_TEXTURE_2D, property.textures[i].textureID);
						shader.setInt("material." + propertyName + std::to_string(i+1), properties.USED_TEXTURE_NUM++); // ������Ԫ
						shader.setBool("material.use_" + propertyName, true);
					}
				}
				else if (!property.textures.empty()) {
					unsigned int slot = 0;
					const std::string* samplerName = nullptr;
					const std::string* hasMapName = nullptr;
					static const std::string kDiffuseSampler = "texture_diffuse1";
					static const std::string kSpecSampler = "texture_specular1";
					static const std::string kNormalSampler = "texture_normal1";
					static const std::string kHasDiffuse = "hasDiffuseMap";
					static const std::string kHasSpec = "hasSpecularMap";
					static const std::string kHasNormal = "hasNormalMap";
					if (propertyName == "texture_diffuse") {
						slot = 0; samplerName = &kDiffuseSampler; hasMapName = &kHasDiffuse;
					}
					else if (propertyName == "texture_specular") {
						slot = 1; samplerName = &kSpecSampler; hasMapName = &kHasSpec;
					}
					else if (propertyName == "texture_normal") {
						slot = 2; samplerName = &kNormalSampler; hasMapName = &kHasNormal;
					}

					if (samplerName && hasMapName) {
						GLState::ActiveTexture(GL_TEXTURE0 + slot);
						if (properties.GAMMA_CORRECTION)
							GLState::BindTexture(GL_TEXTURE_2D, property.textures[0].textureGammaID);
						else
							GLState::BindTexture(GL_TEXTURE_2D, property.textures[0].textureID);
						shader.setInt(*samplerName, slot);
						shader.setBool(*hasMapName, true);
					}
				}
				break;
			}
		}
	}
};

class MaterialBatchScope {
public:
	MaterialBatchScope()
	{
		if (s_depth++ == 0) {
			s_previousState = Material::GetCurrentRenderState();
			s_baseTextureUnit = SystemProperties::GetInstance().USED_TEXTURE_NUM;
			ResetBindingCache();
		}
	}

	~MaterialBatchScope()
	{
		if (--s_depth == 0) {
			SystemProperties::GetInstance().USED_TEXTURE_NUM = s_baseTextureUnit;
			Material::RecoverRenderState(s_previousState);
			ResetBindingCache();
		}
	}

	MaterialBatchScope(const MaterialBatchScope&) = delete;
	MaterialBatchScope& operator=(const MaterialBatchScope&) = delete;

	static bool IsActive()
	{
		return s_depth > 0;
	}

	static bool CanReuse(
		const Material* material,
		const Shader* shader,
		bool gammaCorrection,
		int baseTextureUnit)
	{
		return IsActive() &&
			shader != nullptr &&
			s_lastMaterial == material &&
			s_lastShader == shader &&
			s_lastGammaCorrection == gammaCorrection &&
			s_lastBaseTextureUnit == baseTextureUnit;
	}

	static void MarkBound(
		const Material* material,
		const Shader* shader,
		bool gammaCorrection,
		int baseTextureUnit)
	{
		if (!IsActive()) {
			return;
		}
		s_lastMaterial = material;
		s_lastShader = shader;
		s_lastGammaCorrection = gammaCorrection;
		s_lastBaseTextureUnit = baseTextureUnit;
	}

private:
	static void ResetBindingCache()
	{
		s_lastMaterial = nullptr;
		s_lastShader = nullptr;
		s_lastGammaCorrection = false;
		s_lastBaseTextureUnit = -1;
	}

	inline static thread_local int s_depth = 0;
	inline static thread_local RenderState s_previousState{};
	inline static thread_local int s_baseTextureUnit = 0;
	inline static thread_local const Material* s_lastMaterial = nullptr;
	inline static thread_local const Shader* s_lastShader = nullptr;
	inline static thread_local bool s_lastGammaCorrection = false;
	inline static thread_local int s_lastBaseTextureUnit = -1;
};

class MaterialGaurd {
public:
	MaterialGaurd(Material& material, Shader* overrideShader = nullptr)
		: m_material(material)
	{
		auto& properties = SystemProperties::GetInstance();
		m_startTextureUnit = properties.USED_TEXTURE_NUM;
		m_inBatch = MaterialBatchScope::IsActive();

		Shader* resolvedShader = overrideShader;
		if (!resolvedShader) {
			auto shader = ShaderManager::GetInstance().GetShaderByName(m_material.GetShaderName());
			resolvedShader = shader.get();
		}

		const bool cacheHit = MaterialBatchScope::CanReuse(
			&m_material,
			resolvedShader,
			properties.GAMMA_CORRECTION,
			m_startTextureUnit);
		PerformanceProfiler::GetInstance().RecordMaterialBind(cacheHit);
		if (cacheHit) {
			return;
		}

		if (!m_inBatch) {
			m_previousState = Material::GetCurrentRenderState();
		}
		m_material.Activate(overrideShader);
		m_activated = true;
		MaterialBatchScope::MarkBound(
			&m_material,
			resolvedShader,
			properties.GAMMA_CORRECTION,
			m_startTextureUnit);
	}

	~MaterialGaurd() {
		SystemProperties::GetInstance().USED_TEXTURE_NUM = m_startTextureUnit;
		if (!m_inBatch && m_activated) {
			Material::RecoverRenderState(m_previousState);
		}
	}

private:
	Material& m_material;
	RenderState m_previousState;
	int m_startTextureUnit = 0;
	bool m_inBatch = false;
	bool m_activated = false;
};
