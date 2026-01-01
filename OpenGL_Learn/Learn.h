#pragma once

#include "ShaderManager.h"
#include "Shader.h"
#include "Model.h"

class Planet {
public:
	unsigned int amount = 1000;
	glm::mat4* modelMatrices;
	unsigned int rockVBO;
	Model* planetModel;
	Model* rockModel;

	Shader* planetShader;
	Shader* rockShader;

	Planet() {
	}

	void Init();
	void Draw();
};