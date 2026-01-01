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
#include <imgui/imgui.h>
#include <imgui/backends/imgui_impl_opengl3.h>
#include <imgui/backends/imgui_impl_glfw.h>

struct LightSource {
	unsigned int pointLightVAO;
	std::vector<PointLight> pointLights;
	std::vector<DirectionLight> directionLights;
	std::vector<SpotLight> spotLights;

	Shader pointLightShader;
	LightSource()
		: pointLightShader("LightVertexShader.vs", "LightFragmentShader.fs") {
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
};

struct ModelSource {
	std::unordered_map<Shader*, std::vector<std::shared_ptr<Model>>> opaqueModelsMap;
	std::vector<std::pair<std::shared_ptr<Model>,Shader*>> transparentModels;
	ModelSource(){
	}

	void AddOpaqueModel(Shader* shader,std::shared_ptr<Model> model) {
		assert(shader != nullptr && "AddTransparentModel: shader 不能为空指针！");
		assert(model != nullptr && "AddTransparentModel: model 不能为空智能指针！");
		opaqueModelsMap[shader].push_back(model);
		std::cout << "Added opaque model. Total models for this shader: " << opaqueModelsMap[shader].size() << std::endl;
	}

	void DeleteOpaqueModel(std::shared_ptr<Model> model) {
		assert(model != nullptr && "AddTransparentModel: model 不能为空智能指针！");
		for (auto& pair : opaqueModelsMap) {
			auto& models = pair.second;
			models.erase(std::remove(models.begin(), models.end(), model), models.end());
		}
	}

	void AddTransparentModel(Shader* shader, std::shared_ptr<Model> model) {
		assert(shader != nullptr && "AddTransparentModel: shader 不能为空指针！");
		assert(model != nullptr && "AddTransparentModel: model 不能为空智能指针！");
		transparentModels.emplace_back(model, shader);
	}
};

class SkyboxSource {
public:
	CubeTexture* textureCubeMap;
	unsigned int cubeMapVAO;
	Shader* skyboxShader_ptr;
	SkyboxSource(const SkyboxSource& other) {
		textureCubeMap = other.textureCubeMap;
		cubeMapVAO = other.cubeMapVAO;
		skyboxShader_ptr = other.skyboxShader_ptr;
	}
	SkyboxSource(CubeTexture& textureid,unsigned int cubeMapVao,Shader* skyboxShader) {
		textureCubeMap = &textureid;
		cubeMapVAO = cubeMapVao;
		skyboxShader_ptr = skyboxShader;
	}
	SkyboxSource() = default;
};

class Scene {
public:
	LightSource lightSource;
	ModelSource modelSource;
	SkyboxSource skyboxSource;
	Camera* camera_ptr = nullptr;

	Scene(Camera* camera,const unsigned int& width,const unsigned int& height) {
		camera_ptr = camera;
		lightSource.pointLightVAO = GetPointLightVAO();
	}
	void DrawPointLights();
	void DrawOpaqueModels();
	void DrawTransparentModels();
	void Draw();
	void SetLightUniforms(Shader& shader);
	void DrawSkybox();
	void DrawOutlines();
	void DrawNormalLines();

	void SetSceneGui();

	unsigned int GetPointLightVAO() {
		float vertices[] = {
		-0.5f, -0.5f, -0.5f,
		 0.5f, -0.5f, -0.5f,
		 0.5f,  0.5f, -0.5f,
		 0.5f,  0.5f, -0.5f,
		-0.5f,  0.5f, -0.5f,
		-0.5f, -0.5f, -0.5f,

		-0.5f, -0.5f,  0.5f,
		 0.5f, -0.5f,  0.5f,
		 0.5f,  0.5f,  0.5f,
		 0.5f,  0.5f,  0.5f,
		-0.5f,  0.5f,  0.5f,
		-0.5f, -0.5f,  0.5f,

		-0.5f,  0.5f,  0.5f,
		-0.5f,  0.5f, -0.5f,
		-0.5f, -0.5f, -0.5f,
		-0.5f, -0.5f, -0.5f,
		-0.5f, -0.5f,  0.5f,
		-0.5f,  0.5f,  0.5f,

		 0.5f,  0.5f,  0.5f,
		 0.5f,  0.5f, -0.5f,
		 0.5f, -0.5f, -0.5f,
		 0.5f, -0.5f, -0.5f,
		 0.5f, -0.5f,  0.5f,
		 0.5f,  0.5f,  0.5f,

		-0.5f, -0.5f, -0.5f,
		 0.5f, -0.5f, -0.5f,
		 0.5f, -0.5f,  0.5f,
		 0.5f, -0.5f,  0.5f,
		-0.5f, -0.5f,  0.5f,
		-0.5f, -0.5f, -0.5f,

		-0.5f,  0.5f, -0.5f,
		 0.5f,  0.5f, -0.5f,
		 0.5f,  0.5f,  0.5f,
		 0.5f,  0.5f,  0.5f,
		-0.5f,  0.5f,  0.5f,
		-0.5f,  0.5f, -0.5f,
		};
		unsigned int VBO, VAO;
		glGenVertexArrays(1, &VAO);
		glGenBuffers(1, &VBO);
		glBindVertexArray(VAO);
		glBindBuffer(GL_ARRAY_BUFFER, VBO);
		glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
		glEnableVertexAttribArray(0);
		return VAO;
	}
private:
	glm::mat4 view;
	glm::mat4 projection;
};
