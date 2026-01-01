#pragma once
#define STB_IMAGE_IMPLEMENTATION
//#define USE_GEOMETRY_SHADER
#define USE_SCENE_SHADER
//#define USE_PLANET_SHADER
#include "Learn.h"
#include "Model.h"
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include "callbacks.h"
#include "Camera.h"
#include "Scene.h"
#include "mygui.h"
#include "ShaderManager.h"
#include "Global.h"

bool firstMouse = false;
bool lastFrameMkeyState = false;

int frameCount = 0;
float lastFrameTime = 0.0f;

Camera camera(5.0f, glm::vec3(0.0f, 0.0f, 3.0f), SCREEN_WIDTH / 2.0f, SCREEN_HEIGHT / 2.0f);
glm::mat4 view, projection;

float deltaTime = 0.0f;
float lastFrame = 0.0f;

glm::vec3 coral(1.0f, 0.5f, 0.31f);
glm::vec3 lightColor(1.0f);
glm::vec3 toyColor(1.0f, 0.5f, 0.31f);
glm::vec3 result = lightColor * toyColor;
glm::vec3 pointLightPositions[] = {
	glm::vec3(0.7f,  0.2f,  2.0f),
	glm::vec3(2.3f, -3.3f, -4.0f),
	glm::vec3(-4.0f,  2.0f, -12.0f),
	glm::vec3(0.0f,  0.0f, -3.0f)
};	



void ProcessInput(GLFWwindow* window) {
	bool currentMkeyState = glfwGetKey(window, GLFW_KEY_M) == GLFW_PRESS;
	if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
		glfwSetWindowShouldClose(window, true);
	}
	if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) {
		camera.cameraPos += deltaTime *camera.cameraSpeed * camera.cameraFront;
	}
	if(glfwGetKey(window,GLFW_KEY_S) == GLFW_PRESS){
		camera.cameraPos -= deltaTime * camera.cameraSpeed * camera.cameraFront;
	}
	if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {
		camera.cameraPos += deltaTime * camera.cameraSpeed * glm::cross(camera.cameraFront, camera.up);
	}
	if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) {
		camera.cameraPos -= deltaTime * camera.cameraSpeed * glm::cross(camera.cameraFront, camera.up);
	}

	if (currentMkeyState && !lastFrameMkeyState) {
		if (firstMouse) {
			glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
			firstMouse = false;
		}
		else {
			glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
			firstMouse = true;
		}
	}
	lastFrameMkeyState = currentMkeyState;
}

void framebuffer_size_callback(GLFWwindow* window, int width, int height) {
	glViewport(0, 0, width, height);
	SCREEN_HEIGHT = height;
	SCREEN_WIDTH = width;

	auto& fBuffersMgr = FramebuffersManager::GetInstance();
	fBuffersMgr.Resize();
}

void mouse_callback(GLFWwindow* window, double xpos, double ypos) {
	if(!firstMouse)
		camera.CameraMouseCallback(xpos, ypos);
	else {
		camera.SetLastPos(xpos, ypos);
	}
}

void scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
	camera.CameraSrollCallback(xoffset, yoffset);
}

void SetGui() {
	MyGui& mygui = MyGui::GetInstance();
	mygui.NewFrame();
	ImGui::SetNextWindowSize(ImVec2(400, 300));
}

void SetUniformBuffer() {
	view = camera.GetViewMatrix();
	projection = glm::perspective(glm::radians(camera.fov), (float)SCREEN_WIDTH / (float)SCREEN_HEIGHT, 0.1f, 100.0f);
	ShaderManager& ShaderMgr = ShaderManager::GetInstance();
	ShaderMgr.SetUBOData(ShaderManager::Matrices, 0, sizeof(glm::mat4), &view);
	ShaderMgr.SetUBOData(ShaderManager::Matrices, sizeof(glm::mat4), sizeof(glm::mat4), &projection);
}

int main() {
	glfwInit();
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
	glfwWindowHint(GLFW_SAMPLES, 4);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	

	GLFWwindow* window = glfwCreateWindow(SCREEN_WIDTH, SCREEN_HEIGHT, "Learn OpenGL", NULL, NULL);
	if (!window) {
		std::cout << "Fail to create a window" << std::endl;
		glfwTerminate();		
		return -1;
	}
	glfwMakeContextCurrent(window);
	glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
	//register function after initializing window and before renderering
	glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
	glfwSetCursorPosCallback(window, mouse_callback);
	glfwSetScrollCallback(window, scroll_callback);
	if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
		std::cout << "Fail to initialize GLAD" << std::endl;
		return -1;
	}
	glfwGetFramebufferSize(window, &SCREEN_WIDTH, &SCREEN_HEIGHT);
	glViewport(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);


	MyGui& mygui = MyGui::GetInstance();
	mygui.Init(window);

	ShaderManager& shaderManager = ShaderManager::GetInstance();
	shaderManager.Init();
	Shader& screenShader = *(shaderManager.GetShader(ShaderManager::Scene));

#ifdef USE_GEOMETRY_SHADER
	GeometryShader geometryShader("shaders/geometryVertex.vs", "shaders/geometryGeometry.gs", "shaders/geometryFragment.fs");
	float points[] = {
	-0.5f,  0.5f, 1.0f, 0.0f, 0.0f, // 左上
	 0.5f,  0.5f, 0.0f, 1.0f, 0.0f, // 右上
	 0.5f, -0.5f, 0.0f, 0.0f, 1.0f, // 右下
	-0.5f, -0.5f, 1.0f, 1.0f, 0.0f  // 左下
	};
	unsigned int VAO, VBO;
	glGenVertexArrays(1, &VAO);
	glGenBuffers(1, &VBO);
	glBindVertexArray(VAO);
	glBindBuffer(GL_ARRAY_BUFFER, VBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(points), points, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(2 * sizeof(float)));
	glEnableVertexAttribArray(1);
#elif defined(USE_PLANET_SHADER)
	Planet planet;
	planet.Init();
#endif

	Scene scene(&camera, SCREEN_WIDTH, SCREEN_HEIGHT);

	auto object = std::make_shared<Model>("models/saki/saki.obj");
	object->scale = glm::vec3(0.1f);
	object->AddOtherShader(OtherShaderType::outline, shaderManager.GetShader(ShaderManager::Outline));
	object->AddOtherShader(OtherShaderType::normalLines, shaderManager.GetShader(ShaderManager::NormalLines));
	scene.modelSource.AddOpaqueModel(shaderManager.GetShader(ShaderManager::Phong), object);

	//scene.lightSource.AddPointLight(PointLight(pointLightPositions[0], glm::vec3(0.05f), glm::vec3(0.8f), glm::vec3(1.0f)));
	//scene.lightSource.AddPointLight(PointLight(pointLightPositions[1], glm::vec3(0.05f), glm::vec3(0.8f), glm::vec3(1.0f)));
	//scene.lightSource.AddPointLight(PointLight(pointLightPositions[2], glm::vec3(0.05f), glm::vec3(0.8f), glm::vec3(1.0f)));
	scene.lightSource.AddPointLight(PointLight(pointLightPositions[3], glm::vec3(0.05f), glm::vec3(0.8f), glm::vec3(1.0f)));
	scene.lightSource.AddDirectionLight(DirectionLight(glm::vec3(-0.2f, -1.0f, -0.3f), glm::vec3(1.05f), glm::vec3(0.4f), glm::vec3(0.5f)));

	std::vector<glm::vec3> vegetation;
	vegetation.push_back(glm::vec3(-1.5f, 0.0f, -0.48f));
	vegetation.push_back(glm::vec3(1.5f, 0.0f, 0.51f));
	vegetation.push_back(glm::vec3(0.0f, 0.0f, 0.7f));
	vegetation.push_back(glm::vec3(-0.3f, 0.0f, -2.3f));
	vegetation.push_back(glm::vec3(0.5f, 0.0f, -0.6f));

	std::vector<Vertex> grassVertices;
	grassVertices.push_back({ glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f,1.0f,0.0f), glm::vec2(0.0f,0.0f) });
	grassVertices.push_back({ glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f,1.0f,0.0f), glm::vec2(1.0f,0.0f) });
	grassVertices.push_back({ glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f,1.0f,0.0f), glm::vec2(0.0f,1.0f) });
	grassVertices.push_back({ glm::vec3(1.0f, 1.0f, 0.0f), glm::vec3(0.0f,1.0f,0.0f), glm::vec2(1.0f,1.0f) });
	std::vector<unsigned int> grassIndices = {
		0,1,2,
		1,3,2
	};
	std::vector<Texture> grassTextures;
	Texture grassTexture;
	grassTexture.textureID = TextureFromFile("blending_transparent_window.png", "models/blending_transparent_window",true);
	grassTexture.textureGammaID = TextureFromFile("blending_transparent_window.png", "models/blending_transparent_window", true,true);
	grassTexture.type = "texture_diffuse";
	grassTextures.push_back(grassTexture);

	Material grassMaterial;
	grassMaterial.diffuseTextures = grassTextures;

	std::vector<Mesh> grassMeshes;
	Mesh grassMesh(grassVertices, grassIndices, grassMaterial);
	grassMeshes.push_back(grassMesh);

	for(auto& pos: vegetation){
		glm::mat4 model = glm::mat4(1.0f);
		auto vegi = std::make_shared<Model>(grassMeshes);
		vegi->position = pos;
		vegi->AddOtherShader(OtherShaderType::outline,shaderManager.GetShader(ShaderManager::Default));
		scene.modelSource.AddTransparentModel(shaderManager.GetShader(ShaderManager::Grass),vegi);
	}
	
	CubeTexture skybox("materials/skybox");


	float skyboxVertices[] = {
		// positions          
		-1.0f,  1.0f, -1.0f,
		-1.0f, -1.0f, -1.0f,
		 1.0f, -1.0f, -1.0f,
		 1.0f, -1.0f, -1.0f,
		 1.0f,  1.0f, -1.0f,
		-1.0f,  1.0f, -1.0f,

		-1.0f, -1.0f,  1.0f,
		-1.0f, -1.0f, -1.0f,
		-1.0f,  1.0f, -1.0f,
		-1.0f,  1.0f, -1.0f,
		-1.0f,  1.0f,  1.0f,
		-1.0f, -1.0f,  1.0f,

		 1.0f, -1.0f, -1.0f,
		 1.0f, -1.0f,  1.0f,
		 1.0f,  1.0f,  1.0f,
		 1.0f,  1.0f,  1.0f,
		 1.0f,  1.0f, -1.0f,
		 1.0f, -1.0f, -1.0f,

		-1.0f, -1.0f,  1.0f,
		-1.0f,  1.0f,  1.0f,
		 1.0f,  1.0f,  1.0f,
		 1.0f,  1.0f,  1.0f,
		 1.0f, -1.0f,  1.0f,
		-1.0f, -1.0f,  1.0f,

		-1.0f,  1.0f, -1.0f,
		 1.0f,  1.0f, -1.0f,
		 1.0f,  1.0f,  1.0f,
		 1.0f,  1.0f,  1.0f,
		-1.0f,  1.0f,  1.0f,
		-1.0f,  1.0f, -1.0f,

		-1.0f, -1.0f, -1.0f,
		-1.0f, -1.0f,  1.0f,
		 1.0f, -1.0f, -1.0f,
		 1.0f, -1.0f, -1.0f,
		-1.0f, -1.0f,  1.0f,
		 1.0f, -1.0f,  1.0f
	};
	unsigned int skyboxVAO,skyboxVBO;
	glGenVertexArrays(1, &skyboxVAO);
	glGenBuffers(1, &skyboxVBO);
	glBindBuffer(GL_ARRAY_BUFFER, skyboxVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(skyboxVertices), &skyboxVertices, GL_STATIC_DRAW);
	glBindVertexArray(skyboxVAO);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
	glBindVertexArray(0);
	scene.skyboxSource = SkyboxSource(skybox, skyboxVAO, shaderManager.GetShader(ShaderManager::Skybox));
	
	FramebuffersManager& framebuffersMgr = FramebuffersManager::GetInstance();
	AntiAliasManager& antiAliasMgr = AntiAliasManager::GetInstance();
	//default fBuffer
	FBO defaultFBO(FBO::Framebuffer);
	framebuffersMgr.GenFBO(&defaultFBO);
	//MSAA fBuffer
	FBO multisampleFBO(FBO::Multisample);
	framebuffersMgr.GenFBO(&multisampleFBO);

	FBO intermediateFBO(FBO::Framebuffer);
	framebuffersMgr.GenFBO(&intermediateFBO);

	unsigned int quadVAO,quadVBO,quadEBO;
	glGenVertexArrays(1, &quadVAO);
	glGenBuffers(1, &quadVBO);
	float screenVertices[] = { // vertex attributes for a quad that fills the entire screen in Normalized Device Coordinates.
		// positions   // texCoords
		-1.0f,  1.0f,  0.0f, 1.0f,
		-1.0f, -1.0f,  0.0f, 0.0f,
		 1.0f, -1.0f,  1.0f, 0.0f,

		-1.0f,  1.0f,  0.0f, 1.0f,
		 1.0f, -1.0f,  1.0f, 0.0f,
		 1.0f,  1.0f,  1.0f, 1.0f
	};
	float backviewScreenVertices[] = {
		-0.2f, 1.0f, 0.0f,1.0f,
		-0.2f, 0.6f, 0.0f,0.0f,
		 0.2f, 0.6f, 1.0f,0.0f,

		-0.2f, 1.0f, 0.0f,1.0f,
		 0.2f, 0.6f, 1.0f,0.0f,
		 0.2f, 1.0f, 1.0f,1.0f
	};

	glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(screenVertices), &screenVertices, GL_STATIC_DRAW);


	glBindVertexArray(quadVAO);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
	glBindVertexArray(0);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	//glEnable(GL_CULL_FACE);
	//glCullFace(GL_BACK);

	glEnable(GL_PROGRAM_POINT_SIZE);

	glEnable(GL_BLEND);
	
	//glEnable(GL_FRAMEBUFFER_SRGB);

	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	
	while (!glfwWindowShouldClose(window)) {
		++frameCount;
		float currentTime = (float)glfwGetTime();
		deltaTime = currentTime - lastFrame;
		lastFrame = currentTime;
		//calculate FPS
		if (currentTime - lastFrameTime > .1f) {
			std::stringstream windowTitle;
			
			windowTitle << "OpenGL_Learn FPS:" << (float)frameCount/(currentTime-lastFrameTime);
			lastFrameTime = currentTime;
			frameCount = 0;
			glfwSetWindowTitle(window, windowTitle.str().c_str());
		}
		//set system configUI
		SetGui();
		mygui.Begin();
		mygui.Gamma_UI();
		mygui.Anti_Aliasing_UI();
		mygui.Scene_UI(scene);
		mygui.End();
		//process input
		ProcessInput(window);
		//before pass: set uniform buffer
		SetUniformBuffer();
#ifdef USE_SCENE_SHADER
		//first pass: render scene to framebuffer
		if (antiAliasMgr.antiAliasType == AntiAliasManager::MSAA) {
			glBindFramebuffer(GL_FRAMEBUFFER, multisampleFBO.framebufferID);
		}
		else {
			glBindFramebuffer(GL_FRAMEBUFFER, defaultFBO.framebufferID);
		}
		glEnable(GL_DEPTH_TEST);
		glEnable(GL_STENCIL_TEST);
		glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
		glClearStencil(0);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
		glStencilMask(0x00);
		scene.Draw();
		//second pass: render framebuffer texture to screen
		if (antiAliasMgr.antiAliasType == AntiAliasManager::MSAA) {
			glBindFramebuffer(GL_READ_FRAMEBUFFER, multisampleFBO.framebufferID);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, intermediateFBO.framebufferID);
			glBlitFramebuffer(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		}
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
		glDisable(GL_DEPTH_TEST);
		screenShader.use();
		glBindVertexArray(quadVAO);
		glDisable(GL_DEPTH_TEST);
		if (antiAliasMgr.antiAliasType == AntiAliasManager::MSAA) {
			glBindTexture(GL_TEXTURE_2D, intermediateFBO.textureID);
		}
		else {
			glBindTexture(GL_TEXTURE_2D, defaultFBO.textureID);
		}
		glActiveTexture(GL_TEXTURE0);
		screenShader.setFloat("gamma", GAMMA_VALUE);
		screenShader.setBool("useGamma", GAMMA_CORRECTION);
		glDrawArrays(GL_TRIANGLES, 0, 6);
		//Draw GUI
		mygui.Render();
#elif defined(USE_GEOMETRY_SHADER)
		geometryShader.use();
		glBindVertexArray(VAO);
		glDrawArrays(GL_POINTS, 0, 4);
#elif defined(USE_PLANET_SHADER)
		glEnable(GL_DEPTH_TEST);
		glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
		glClearStencil(0);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		planet.Draw();
#endif
		// glfw: swap buffers and poll IO events (keys pressed/released, mouse moved etc.)
		glfwSwapBuffers(window);
		glfwPollEvents();
		
	}
	glfwTerminate();
	return 0;
}