#pragma once
#include "Model.h"
#include "Scene.h"
#include "Global.h"

glm::vec3 pointLightPositions[] = {
	glm::vec3(0.7f,  0.2f,  2.0f),
	glm::vec3(2.3f, -3.3f, -4.0f),
	glm::vec3(-4.0f,  2.0f, -12.0f),
	glm::vec3(0.0f,  1.5f, -3.0f)
};

void LoadModels(Scene& scene) {
	auto& shaderManager = ShaderManager::GetInstance();

	//Load Charactor
	auto object = std::make_shared<Model>("models/saki/saki.obj");
	object->scale = glm::vec3(0.1f);
	object->AddOtherShader(OtherShaderType::outline, shaderManager.GetShader(ShaderManager::Outline));
	object->AddOtherShader(OtherShaderType::normalLines, shaderManager.GetShader(ShaderManager::NormalLines));
	scene.modelSource.AddOpaqueModel(shaderManager.GetShader(ShaderManager::Phong), object);
	//Load PointLight
	
	//scene.lightSource.AddPointLight(PointLight(pointLightPositions[0], glm::vec3(0.05f), glm::vec3(0.8f), glm::vec3(1.0f)));
	//scene.lightSource.AddPointLight(PointLight(pointLightPositions[1], glm::vec3(0.05f), glm::vec3(0.8f), glm::vec3(1.0f)));
	//scene.lightSource.AddPointLight(PointLight(pointLightPositions[2], glm::vec3(0.05f), glm::vec3(0.8f), glm::vec3(1.0f)));
	scene.lightSource.AddPointLight(PointLight(pointLightPositions[3], glm::vec3(0.05f), glm::vec3(0.8f), glm::vec3(1.0f)));
	scene.lightSource.AddDirectionLight(DirectionLight(glm::vec3(-0.2f, -1.0f, -0.3f), glm::vec3(1.f), glm::vec3(0.4f), glm::vec3(0.5f)));
	
	//Load Glass
	std::vector<glm::vec3> vegetation;
	vegetation.emplace_back(-1.5f, 0.0f, -0.48f);
	vegetation.emplace_back(1.5f, 0.0f, 0.51f);
	vegetation.emplace_back(0.0f, 0.0f, 0.7f);
	vegetation.emplace_back(-0.3f, 0.0f, -2.3f);
	vegetation.emplace_back(0.5f, 0.0f, -0.6f);

	std::vector<Vertex> grassVertices;
	grassVertices.emplace_back(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f,1.0f,0.0f), glm::vec2(0.0f,0.0f));
	grassVertices.emplace_back(glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f,1.0f,0.0f), glm::vec2(1.0f,0.0f));
	grassVertices.emplace_back(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f,1.0f,0.0f), glm::vec2(0.0f,1.0f));
	grassVertices.emplace_back(glm::vec3(1.0f, 1.0f, 0.0f), glm::vec3(0.0f,1.0f,0.0f), glm::vec2(1.0f,1.0f));
	std::vector<unsigned int> grassIndices = {
		0,1,2,
		1,3,2
	};
	std::vector<Texture> grassTextures;
	Texture grassTexture;
	grassTexture.textureID = TextureFromFile("blending_transparent_window.png", "models/blending_transparent_window", true);
	grassTexture.textureGammaID = TextureFromFile("blending_transparent_window.png", "models/blending_transparent_window", true, true);
	grassTexture.type = "texture_diffuse";
	grassTextures.push_back(grassTexture);

	Material grassMaterial;
	grassMaterial.diffuseTextures = grassTextures;

	std::vector<Mesh> grassMeshes;
	grassMeshes.emplace_back(grassVertices, grassIndices, grassMaterial);

	for (auto& pos : vegetation) {
		glm::mat4 model = glm::mat4(1.0f);
		auto vegi = std::make_shared<Model>(grassMeshes);
		vegi->position = pos;
		vegi->AddOtherShader(OtherShaderType::outline, shaderManager.GetShader(ShaderManager::Outline));
		scene.modelSource.AddTransparentModel(shaderManager.GetShader(ShaderManager::Grass), vegi);
	}

	//Load Plane
	std::vector<Vertex> planeVertices;
	planeVertices.emplace_back(glm::vec3(-5.0f, 0.0f, -5.0f), glm::vec3(0.0f,1.0f,0.0f), glm::vec2(0.0f,0.0f));
	planeVertices.emplace_back(glm::vec3(5.0f, 0.0f, -5.0f), glm::vec3(0.0f,1.0f,0.0f), glm::vec2(1.0f,0.0f));
	planeVertices.emplace_back(glm::vec3(-5.0f, 0.0f, 5.0f), glm::vec3(0.0f,1.0f,0.0f), glm::vec2(0.0f,1.0f));
	planeVertices.emplace_back(glm::vec3(5.0f, 0.0f, 5.0f), glm::vec3(0.0f,1.0f,0.0f), glm::vec2(1.0f,1.0f));
	std::vector<unsigned int> planeIndices = {
		0,1,2,
		1,3,2
	};
	std::vector<Texture> planeTextures;
	Texture planeTexture;
	planeTexture.textureID = TextureFromFile("wood.png", "materials/wood", true);
	planeTexture.textureGammaID = TextureFromFile("wood.png", "materials/wood", true, true);
	planeTexture.type = "texture_diffuse";
	planeTextures.push_back(planeTexture);
	Material planeMaterial;
	planeMaterial.diffuseTextures = planeTextures;

	std::vector<Mesh> planeMeshes;
	planeMeshes.emplace_back(planeVertices, planeIndices, planeMaterial);

	auto plane = std::make_shared<Model>(planeMeshes);
	scene.modelSource.AddOpaqueModel(shaderManager.GetShader(ShaderManager::Phong), plane);
}