#pragma once
#include "Model.h"
#include "Scene.h"
#include "Global.h"
#include "XmlMaterialManager.h"

glm::vec3 pointLightPositions[] = {
	glm::vec3(0.7f,  0.2f,  2.0f),
	glm::vec3(2.3f, -3.3f, -4.0f),
	glm::vec3(-4.0f,  2.0f, -12.0f),
	glm::vec3(0.0f,  1.5f, -3.0f)
};

void InitVAOs() {
	auto& properties = SystemProperties::GetInstance();
	glGenVertexArrays(1, &globalVAOs.quadVAO);
	glGenBuffers(1, &globalVAOs.quadVBO);
	glBindBuffer(GL_ARRAY_BUFFER, globalVAOs.quadVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(screenVertices), &screenVertices, GL_STATIC_DRAW);
	GLState::BindVertexArray(globalVAOs.quadVAO);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
	GLState::BindVertexArray(0);

	glGenVertexArrays(1, &globalVAOs.cubeVAO);
	glGenBuffers(1, &globalVAOs.cubeVBO);
	GLState::BindVertexArray(globalVAOs.cubeVAO);
	glBindBuffer(GL_ARRAY_BUFFER, globalVAOs.cubeVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(cubeVertices), &cubeVertices, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);

	glGenVertexArrays(1, &globalVAOs.sphereVAO);
	glGenBuffers(1, &globalVAOs.sphereVBO);
	glBindBuffer(GL_ARRAY_BUFFER, globalVAOs.sphereVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(sphereVertices), &sphereVertices, GL_STATIC_DRAW);
	GLState::BindVertexArray(globalVAOs.sphereVAO);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
}

void LoadDefaultLights(Scene& scene) {
	// Load PointLight
	auto pointLight = PointLight(
		pointLightPositions[3],
		glm::vec3(0.05f),
		glm::vec3(0.8f),
		glm::vec3(1.0f),
		"models/sphere/sphere.obj",
		XmlMaterialManager::GetInstance().GetMaterialRaw("Light")
	);
	pointLight.SetScale(0.2f);
	scene.lightSource.AddPointLight(pointLight);

	scene.lightSource.AddDirectionLight(
		DirectionLight(glm::vec3(-0.2f, -1.0f, -0.3f), glm::vec3(10.f), glm::vec3(0.4f), glm::vec3(0.5f))
	);
}

void LoadDefaultModels(Scene& scene) {
	auto& shaderManager = ShaderManager::GetInstance();

	auto object1 = std::make_shared<Model>("models/plk/plk.obj",shaderManager.GetShader(ShaderManager::Phong));
	object1->SetScale(0.1f);
	object1->AddOtherShader(OtherShaderType::outline, shaderManager.GetShader(ShaderManager::Outline));
	object1->AddOtherShader(OtherShaderType::normalLines, shaderManager.GetShader(ShaderManager::NormalLines));
	scene.modelSource.AddModel(object1);
	object1->SetName("peilika");
	object1->SetPosition(glm::vec3(-1.5f, 0.0f, 0.0f));

	//Load Charactor
	auto object = std::make_shared<Model>("models/saki/saki.obj", shaderManager.GetShader(ShaderManager::Phong));
	object->SetScale(0.1f);
	object->AddOtherShader(OtherShaderType::outline, shaderManager.GetShader(ShaderManager::Outline));
	object->AddOtherShader(OtherShaderType::normalLines, shaderManager.GetShader(ShaderManager::NormalLines));
	scene.modelSource.AddModel(object);
	object->SetName("saki");

	
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

	Material* grassMaterial = nullptr;

	std::vector<Mesh> grassMeshes;
	grassMeshes.emplace_back(grassVertices, grassIndices, grassMaterial,"materials/transparent_window/transparent_window.xml");
	int count = 0;
	for (auto& pos : vegetation) {
		glm::mat4 model = glm::mat4(1.0f);
		auto vegi = std::make_shared<Model>(grassMeshes);
		vegi->SetDataSourceGenerated("transparent_window_quad");
		vegi->position = pos;
		vegi->AddOtherShader(OtherShaderType::outline, shaderManager.GetShader(ShaderManager::Outline));
		vegi->SetShader(shaderManager.GetShader(ShaderManager::Grass));
		vegi->SetName("window"+std::to_string(count++));
		scene.modelSource.AddModel(vegi);
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
	

	Material* planeMaterial = nullptr;
	std::vector<Mesh> planeMeshes;
	planeMeshes.emplace_back(planeVertices,planeIndices,planeMaterial,"materials/brickwall/brickwall.xml");

	auto plane = std::make_shared<Model>(planeMeshes);
	plane->SetDataSourceGenerated("plane_quad");
	plane->AddOtherShader(OtherShaderType::outline, shaderManager.GetShader(ShaderManager::Outline));
	plane->AddOtherShader(OtherShaderType::normalLines, shaderManager.GetShader(ShaderManager::NormalLines));
	plane->SetShader(shaderManager.GetShader(ShaderManager::Phong));
	scene.modelSource.AddModel(plane);
	plane->SetName("plane");

	auto ceiling = std::make_shared<Model>(planeMeshes);
	ceiling->SetDataSourceGenerated("plane_quad");
	ceiling->AddOtherShader(OtherShaderType::outline, shaderManager.GetShader(ShaderManager::Outline));
	ceiling->AddOtherShader(OtherShaderType::normalLines, shaderManager.GetShader(ShaderManager::NormalLines));
	ceiling->SetShader(shaderManager.GetShader(ShaderManager::Phong));
	scene.modelSource.AddModel(ceiling);
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
	wallMeshes.emplace_back(wallVertices, wallIndices, planeMaterial, "materials/brickwall/brickwall.xml");
	auto wall1 = std::make_shared<Model>(wallMeshes);
	wall1->SetDataSourceGenerated("wall_quad");
	wall1->AddOtherShader(OtherShaderType::outline, shaderManager.GetShader(ShaderManager::Outline));
	wall1->AddOtherShader(OtherShaderType::normalLines, shaderManager.GetShader(ShaderManager::NormalLines));
	wall1->SetShader(shaderManager.GetShader(ShaderManager::Phong));
	scene.modelSource.AddModel(wall1);
	wall1->SetName("wall1");
	wall1->rotation.z = -90;
	wall1->position = glm::vec3(-5.0, 5.0, 0);

	auto wall2 = std::make_shared<Model>(wallMeshes);
	wall2->SetDataSourceGenerated("wall_quad");
	wall2->AddOtherShader(OtherShaderType::outline, shaderManager.GetShader(ShaderManager::Outline));
	wall2->SetShader(shaderManager.GetShader(ShaderManager::Phong));
	scene.modelSource.AddModel(wall2);
	wall2->SetName("wall2");
	wall2->rotation.z = 90;
	wall2->position = glm::vec3(5.0, 0, 0);

	auto wall3 = std::make_shared<Model>(wallMeshes);
	wall3->SetDataSourceGenerated("wall_quad");
	wall3->AddOtherShader(OtherShaderType::outline, shaderManager.GetShader(ShaderManager::Outline));
	wall3->SetShader(shaderManager.GetShader(ShaderManager::Phong));
	scene.modelSource.AddModel(wall3);
	wall3->SetName("wall3");
	wall3->SetRotation(glm::vec3(0, -90, -90));
	wall3->SetPosition(glm::vec3(0, 5, -5));
}

void LoadModels(Scene& scene) {
	LoadDefaultLights(scene);
	LoadDefaultModels(scene);
}
