#pragma once

#include "Light.h"
#include "Shader.h"
#include "Model.h"
#include "Camera.h"
#include <unordered_map>
#include <vector>
#include <memory>
#include <utility>
#include <algorithm>
#include "shaderManager.h"
#include "Global.h"
#include "XmlMaterialManager.h"
#include <imgui.h>
#include <backends/imgui_impl_opengl3.h>
#include <backends/imgui_impl_glfw.h>

struct LightSource {
	unsigned int pointLightVAO;
	unsigned int vertexCount;
	std::vector<PointLight> pointLights;
	std::vector<DirectionLight> directionLights;
	std::vector<SpotLight> spotLights;

	Shader pointLightShader;
	LightSource()
		: pointLightShader("LightVertexShader.glsl", "LightFragmentShader.glsl") {
	}
	std::vector<PointLight>& GetPointLights() {
		return pointLights;
	}

	std::vector<DirectionLight>& GetDirectionLights() {
		return directionLights;
	}

	std::vector<SpotLight>& GetSpotLights() {
		return spotLights;
	}

	void AddPointLight(const PointLight& pointLight) {
		pointLights.push_back(pointLight);
	}

	void AddDirectionLight(const DirectionLight& directionLight) {
		directionLights.push_back(directionLight);
	}

	void AddSpotLight(const SpotLight& spotLight) {
		spotLights.push_back(spotLight);
	}

	void SetLightUniforms(Shader& shader) {
		for (size_t i = 0; i < pointLights.size(); ++i) {
			pointLights[i].SetLightUniforms(shader, static_cast<int>(i));
		}
		shader.setInt("NR_POINT_LIGHTS", static_cast<int>(pointLights.size()));
		for (size_t i = 0; i < directionLights.size(); ++i) {
			directionLights[i].SetLightUniforms(shader, static_cast<int>(i));
		}
		shader.setInt("NR_DIR_LIGHTS", static_cast<int>(directionLights.size()));
		for (size_t i = 0; i < spotLights.size(); ++i) {
			spotLights[i].SetLightUniforms(shader, static_cast<int>(i));
		}
		shader.setInt("NR_SPOT_LIGHTS", static_cast<int>(spotLights.size()));
	}

};

struct ModelSource {
	std::vector<std::shared_ptr<Model>> models;
	ModelSource() {}

	std::vector<std::shared_ptr<Model>>& GetModels() { return models; }
	const std::vector<std::shared_ptr<Model>>& GetModels() const { return models; }

	void AddModel(const std::shared_ptr<Model>& model) {
		assert(model != nullptr && "AddModel: model is null");
		models.push_back(model);
	}

	void DeleteModel(const std::shared_ptr<Model>& model) {
		assert(model != nullptr && "DeleteModel: model is null");
		models.erase(std::remove(models.begin(), models.end(), model), models.end());
	}
};

class SkyboxSource {
public:
	CubeTexture* textureCubeMap;
	unsigned int cubeMapVAO;
	std::shared_ptr<Shader> skyboxShader_ptr;
	SkyboxSource(const SkyboxSource& other) {
		textureCubeMap = other.textureCubeMap;
		cubeMapVAO = other.cubeMapVAO;
		skyboxShader_ptr = other.skyboxShader_ptr;
	}
	SkyboxSource(CubeTexture& textureid,unsigned int cubeMapVao,std::shared_ptr<Shader> skyboxShader) {
		textureCubeMap = &textureid;
		cubeMapVAO = cubeMapVao;
		skyboxShader_ptr = skyboxShader;
	}
	SkyboxSource() = default;
};

class Scene {
public:
	struct MeshDrawItem {
		Model* model = nullptr;
		Mesh* mesh = nullptr;
		Shader* shader = nullptr;
	};

	LightSource lightSource;
	ModelSource modelSource;
	SkyboxSource skyboxSource;
	Camera* camera_ptr = nullptr;

	FBO* fbo = nullptr;
	FBO* fboTemp = nullptr;
	FBO* deferFBO = nullptr;
	std::shared_ptr<Shader> deferShader;

	// 当前在 UI 中选中用于查看/编辑材质的模型（可为空）
	Model* selectedModelForMaterials = nullptr;
	void SetSelectedModelForMaterials(Model* model) { selectedModelForMaterials = model; }
	Model* GetSelectedModelForMaterials() const { return selectedModelForMaterials; }

	Scene(Camera* camera,const unsigned int& width,const unsigned int& height) {
		camera_ptr = camera;
		lightSource.pointLightVAO = globalVAOs.sphereVAO;
		lightSource.vertexCount = 262;
		FBOAttributes attr;
		fboTemp = FramebuffersManager::GetInstance().GetFBO(attr);
		deferShader = ShaderManager::GetInstance().GetShader(ShaderManager::DeferProcess);

		
	}
	void RenderScene(Shader&);
	//new api
	ModelSource& GetModelSource() {
		return modelSource;
	}

	LightSource& GetLightSource() {
		return lightSource;
	}

	SkyboxSource& GetSkyboxSource() {
		return skyboxSource;
	}
	unsigned int SetShadowMap(Shader&);

	//old api
	void DrawPointLights();
	void DrawOpaqueModels();
	void DrawTransparentModels();
	void Draw();

	void DrawDefferedModels();

	void SetLightUniforms(Shader& shader);

	void DrawSkybox(glm::mat4 view);
	void DrawOutlines();
	void DrawNormalLines();
	
	void DrawShadowMap();

	void SetSceneGui();

	void Blur(int,FBO*);
	void ClearFBO();

	FBO* GetNeedShowFramebuffer();

	FBO* GetDebugFramebuffer() {
		return deferFBO;
	}

	// Build render submission once after scene/editor updates and before executing passes.
	void PrepareRenderData();
	const std::vector<MeshDrawItem>& GetOpaqueMeshes() const;
	const std::vector<MeshDrawItem>& GetTransparentMeshes() const;

private:
	void BuildMeshDrawLists();
	std::vector<MeshDrawItem> m_opaqueMeshList;
	std::vector<MeshDrawItem> m_transparentMeshList;
	glm::mat4 view;
	glm::mat4 projection;
	SystemProperties& properties = SystemProperties::GetInstance();
};
