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

void InitVAOs() {
	glGenVertexArrays(1, &quadVAO);
	glGenBuffers(1, &quadVBO);
	glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(screenVertices), &screenVertices, GL_STATIC_DRAW);
	glBindVertexArray(quadVAO);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
	glBindVertexArray(0);

	glGenVertexArrays(1, &cubeVAO);
	glGenBuffers(1, &cubeVBO);
	glBindVertexArray(cubeVAO);
	glBindBuffer(GL_ARRAY_BUFFER, cubeVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(cubeVertices), &cubeVertices, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);

	glGenVertexArrays(1, &sphereVAO);
	glGenBuffers(1, &sphereVBO);
	glBindBuffer(GL_ARRAY_BUFFER, sphereVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(sphereVertices), &sphereVertices, GL_STATIC_DRAW);
	glBindVertexArray(sphereVAO);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
}

void LoadModels(Scene& scene) {
	auto& shaderManager = ShaderManager::GetInstance();

	//Load Charactor
	auto object = std::make_shared<Model>("models/saki/saki.obj");
	object->scale = glm::vec3(0.1f);
	object->AddOtherShader(OtherShaderType::outline, shaderManager.GetShader(ShaderManager::Outline));
	object->AddOtherShader(OtherShaderType::normalLines, shaderManager.GetShader(ShaderManager::NormalLines));
	scene.modelSource.AddOpaqueModel(shaderManager.GetShader(ShaderManager::Phong), object);
	object->SetName("saki");
	//Load PointLight
	
	//scene.lightSource.AddPointLight(PointLight(pointLightPositions[0], glm::vec3(0.05f), glm::vec3(0.8f), glm::vec3(1.0f)));
	//scene.lightSource.AddPointLight(PointLight(pointLightPositions[1], glm::vec3(0.05f), glm::vec3(0.8f), glm::vec3(1.0f)));
	//scene.lightSource.AddPointLight(PointLight(pointLightPositions[2], glm::vec3(0.05f), glm::vec3(0.8f), glm::vec3(1.0f)));
	auto pointLight = PointLight(pointLightPositions[3], glm::vec3(0.05f), glm::vec3(0.8f), glm::vec3(1.0f), "models/sphere/sphere.obj");
	pointLight.scale = glm::vec3(0.2);
	scene.lightSource.AddPointLight(pointLight);
	scene.lightSource.AddDirectionLight(DirectionLight(glm::vec3(-0.2f, -1.0f, -0.3f), glm::vec3(10.f), glm::vec3(0.4f), glm::vec3(0.5f)));
	
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
	planeVertices.emplace_back(glm::vec3(5.0f, 0.0f, -5.0f), glm::vec3(0.0f,1.0f,0.0f), glm::vec2(2.0f,0.0f));
	planeVertices.emplace_back(glm::vec3(-5.0f, 0.0f, 5.0f), glm::vec3(0.0f,1.0f,0.0f), glm::vec2(0.0f,2.0f));
	planeVertices.emplace_back(glm::vec3(5.0f, 0.0f, 5.0f), glm::vec3(0.0f,1.0f,0.0f), glm::vec2(2.0f,2.0f));
	std::vector<unsigned int> planeIndices = {
		0,1,2,
		1,3,2
	};
	std::vector<Texture> planeTextures;
	std::vector<Texture> planeNormalTextures;
	Texture planeTexture;
	Texture planeNormalTexture;
	planeTexture.textureID = TextureFromFile("brickwall.jpg", "materials/brickwall", true);
	glBindTexture(GL_TEXTURE_2D, planeTexture.textureID);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	planeTexture.textureGammaID = TextureFromFile("brickwall.jpg", "materials/brickwall", true, true);
	glBindTexture(GL_TEXTURE_2D, planeTexture.textureGammaID);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	glBindTexture(GL_TEXTURE_2D,0);
	planeTexture.type = "texture_diffuse";
	planeTextures.push_back(planeTexture);

	planeNormalTexture.textureID = TextureFromFile("brickwall_normal.jpg", "materials/brickwall", true);
	glBindTexture(GL_TEXTURE_2D, planeNormalTexture.textureID);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	glBindTexture(GL_TEXTURE_2D, 0);
	planeNormalTexture.type = "texture_normal";
	planeNormalTextures.push_back(planeNormalTexture);

	Material planeMaterial;
	planeMaterial.diffuseTextures = planeTextures;
	planeMaterial.normalTextures = planeNormalTextures;

	std::vector<Mesh> planeMeshes;
	planeMeshes.emplace_back(planeVertices,planeIndices,planeMaterial);

	auto plane = std::make_shared<Model>(planeMeshes);
	plane->AddOtherShader(OtherShaderType::outline, shaderManager.GetShader(ShaderManager::Outline));
	plane->AddOtherShader(OtherShaderType::normalLines, shaderManager.GetShader(ShaderManager::NormalLines));
	scene.modelSource.AddOpaqueModel(shaderManager.GetShader(ShaderManager::Phong), plane);
	plane->SetName("plane");

	auto ceiling = std::make_shared<Model>(planeMeshes);
	ceiling->AddOtherShader(OtherShaderType::outline, shaderManager.GetShader(ShaderManager::Outline));
	ceiling->AddOtherShader(OtherShaderType::normalLines, shaderManager.GetShader(ShaderManager::NormalLines));
	scene.modelSource.AddOpaqueModel(shaderManager.GetShader(ShaderManager::Phong), ceiling);
	ceiling->SetName("ceiling");
	ceiling->rotation = glm::vec3(0,0,180);
	ceiling->position = glm::vec3(0, 5, 0);

	//load wall
	std::vector<Vertex> wallVertices;
	wallVertices.emplace_back(glm::vec3(0.0, 0.0f, -5.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(0.0f, 0.0f));
	wallVertices.emplace_back(glm::vec3(5.0f, 0.0f, -5.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(1.0f, 0.0f));
	wallVertices.emplace_back(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(0.0f, 2.0f));
	wallVertices.emplace_back(glm::vec3(5.0f, 0.0f, 5.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(1.0f, 2.0f));
	std::vector<unsigned int> wallIndices = {
		0,1,2,
		1,3,2
	};
	std::vector<Mesh> wallMeshes;
	wallMeshes.emplace_back(wallVertices, wallIndices, planeMaterial);
	auto wall1 = std::make_shared<Model>(wallMeshes);
	wall1->AddOtherShader(OtherShaderType::outline, shaderManager.GetShader(ShaderManager::Outline));
	wall1->AddOtherShader(OtherShaderType::normalLines, shaderManager.GetShader(ShaderManager::NormalLines));
	scene.modelSource.AddOpaqueModel(shaderManager.GetShader(ShaderManager::Phong), wall1);
	wall1->SetName("wall1");
	wall1->rotation.z = -90;
	wall1->position = glm::vec3(-5.0, 5.0, 0);

	auto wall2 = std::make_shared<Model>(wallMeshes);
	wall2->AddOtherShader(OtherShaderType::outline, shaderManager.GetShader(ShaderManager::Outline));
	scene.modelSource.AddOpaqueModel(shaderManager.GetShader(ShaderManager::Phong), wall2);
	wall2->SetName("wall2");
	wall2->rotation.z = 90;
	wall2->position = glm::vec3(5.0, 0, 0);

	auto wall3 = std::make_shared<Model>(wallMeshes);
	wall3->AddOtherShader(OtherShaderType::outline, shaderManager.GetShader(ShaderManager::Outline));
	scene.modelSource.AddOpaqueModel(shaderManager.GetShader(ShaderManager::Phong), wall3);
	wall3->SetName("wall3");
	wall3->rotation = glm::vec3(0, -90, -90);
	wall3->position = glm::vec3(0, 5, -5);
}