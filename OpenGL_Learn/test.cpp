#pragma once
//
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
#include "ModelsLoader.h"
#include "Timer.h"
#include "ForwardRenderPass.h"
#include "DeferRenderPass.h"
#include "PostprocessRenderPass.h"
#include "SceneStateIO.h"


bool firstMouse = false;
bool lastFrameMkeyState = false;


auto& properties = SystemProperties::GetInstance();
auto& xmlMaterialManager = XmlMaterialManager::GetInstance();

Camera camera(5.0f, glm::vec3(0.0f, 1.0f, -3.0f), properties.SCREEN_WIDTH / 2.0f, properties.SCREEN_HEIGHT / 2.0f);
glm::mat4 view, projection;


glm::vec3 coral(1.0f, 0.5f, 0.31f);
glm::vec3 lightColor(1.0f);
glm::vec3 toyColor(1.0f, 0.5f, 0.31f);
glm::vec3 result = lightColor * toyColor;

Timer& timer = Timer::GetInstance();


void ProcessInput(GLFWwindow* window) {
	bool currentMkeyState = glfwGetKey(window, GLFW_KEY_M) == GLFW_PRESS;
	if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
		glfwSetWindowShouldClose(window, true);
	}
	if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) {
		camera.UpdatePositionByDelta(timer.GetDeltaTime() *camera.cameraSpeed * camera.cameraFront);
	}
	if(glfwGetKey(window,GLFW_KEY_S) == GLFW_PRESS){
		camera.UpdatePositionByDelta(-timer.GetDeltaTime() * camera.cameraSpeed * camera.cameraFront);
	}
	if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {
		camera.UpdatePositionByDelta(timer.GetDeltaTime() * camera.cameraSpeed * glm::cross(camera.cameraFront, camera.up));
	}
	if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) {
		camera.UpdatePositionByDelta(-timer.GetDeltaTime() * camera.cameraSpeed * glm::cross(camera.cameraFront, camera.up));
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
	properties.SCREEN_HEIGHT = height;
	properties.SCREEN_WIDTH = width;

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
	projection = glm::perspective(glm::radians(camera.fov), (float)properties.SCREEN_WIDTH / (float)properties.SCREEN_HEIGHT, 0.1f, 100.0f);
	ShaderManager& ShaderMgr = ShaderManager::GetInstance();
	ShaderMgr.SetUBOData(ShaderManager::Matrices, 0, sizeof(glm::mat4), &view);
	ShaderMgr.SetUBOData(ShaderManager::Matrices, sizeof(glm::mat4), sizeof(glm::mat4), &projection);
	ShaderMgr.UpdateSystemUBO();
}

int main() {
	glfwInit();
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
	glfwWindowHint(GLFW_SAMPLES, 4);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	

	GLFWwindow* window = glfwCreateWindow(properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT, "Learn OpenGL", NULL, NULL);
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
	glfwGetFramebufferSize(window, &properties.SCREEN_WIDTH, &properties.SCREEN_HEIGHT);
	glViewport(0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT);

	InitVAOs();

	MyGui& mygui = MyGui::GetInstance();
	mygui.Init(window);

	ShaderManager& shaderManager = ShaderManager::GetInstance();
	shaderManager.Init();
	Shader& debugShader = *(shaderManager.GetShader(ShaderManager::DebugScene));

#ifdef USE_GEOMETRY_SHADER
	GeometryShader geometryShader("shaders/geometryVertex.vs", "shaders/geometryGeometry.gs", "shaders/geometryFragment.fs");
	float points[] = {
	-0.5f,  0.5f, 1.0f, 0.0f, 0.0f, // ????
	 0.5f,  0.5f, 0.0f, 1.0f, 0.0f, // ????
	 0.5f, -0.5f, 0.0f, 0.0f, 1.0f, // ????
	-0.5f, -0.5f, 1.0f, 1.0f, 0.0f  // ????
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
	xmlMaterialManager.LoadFromFile("materials.xml");
	Scene scene(&camera, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT);
	LoadModels(scene);
	const std::string sceneStatePath = "saved/last_scene.json";
	if (SceneStateIO::Exists(sceneStatePath)) {
		SceneStateIO::Load(scene, camera, sceneStatePath);
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

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	//glEnable(GL_CULL_FACE);
	//glCullFace(GL_BACK);

	glEnable(GL_PROGRAM_POINT_SIZE);
	
	auto forwardRenderPass = new ForwardRenderPass();
	forwardRenderPass->Init(properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT);
	auto deferRenderPass = new DeferRenderPass();
	deferRenderPass->Init(properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT);
	// PostprocessRenderPass 当前主循环未使用（最终图直接画到 postProcessFBO），若 Init 会多占一个同类型 FBO，导致列表里多一个 Forward+Gamma
	auto postprocessRenderPass = new PostprocessRenderPass();
	postprocessRenderPass->Init(properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT);

	while (!glfwWindowShouldClose(window)) {
		//calculate FPS
		timer.Tick();
		
		std::stringstream windowTitle;
		windowTitle << "OpenGL_Learn FPS:" << timer.GetFPS();
		glfwSetWindowTitle(window, windowTitle.str().c_str());
		// NewFrame
		SetGui();

		// ?? DockSpace??? Unity ?????
		mygui.MainDockSpace();

		// Settings / Scene / Materials / XML / Assets ??? Dock ? DockSpace ?
		mygui.Begin();              // Settings ??
		mygui.System_UI();
		mygui.Shadow_UI();
		mygui.Gamma_UI();
		mygui.Framebuffers_UI();
		mygui.Anti_Aliasing_UI();
		mygui.End();

		mygui.Scene_UI(scene);          // Scene：Lights + Models
		mygui.ModelMaterialsInspector_UI(scene);  // 选中模型的材质查看/编辑
		mygui.MaterialsInspector_UI();  // 全局材质 Inspector
		mygui.MaterialsEditor_UI();     // XML 编辑器

		//process input
		ProcessInput(window);
		//reset used texture num
		properties.ResetUsedTextureNum();
		//before pass: set uniform buffer
		SetUniformBuffer();
#ifdef USE_SCENE_SHADER
		//first pass: render scene to framebuffer (HDR)
		FBO* sceneFBO = nullptr;
		if (properties.DEFER_RENDERING) {
			deferRenderPass->Render(&scene);
			sceneFBO = deferRenderPass->GetOutputFBO();
		}
		else {
			forwardRenderPass->Render(&scene);
			sceneFBO = forwardRenderPass->GetOutputFBO();
		}
		//second pass: postprocess (HDR + gamma + bloom) -> LDR texture (inside postprocessRenderPass)
		
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
		if (!properties.DEBUG_MODE) {
			if (!sceneFBO || sceneFBO->textureIDs.empty()) {
				std::cout << "no valid color attachment, skip this frame" << std::endl;
				continue;
			}
			postprocessRenderPass->Render(&scene, sceneFBO);
		}
		else {
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			FBO* debugFBO = scene.GetDebugFramebuffer();
			int size = debugFBO->textureIDs.size();
			int len = 1;
			while(len*len<size){
				len++;
			}
			debugShader.use();
			for(int i = 0;i<size;i++){
				glActiveTexture(GL_TEXTURE0 + i);
				glBindTexture(GL_TEXTURE_2D, debugFBO->textureIDs[i]);
				std::string uniformName = "screenTexture[" + std::to_string(i) + "]";
				debugShader.setInt(uniformName, i);
			}
			debugShader.setFloat("div", (float)len);
			glBindVertexArray(globalVAOs.quadVAO);
			glDisable(GL_DEPTH_TEST);
			glDrawArrays(GL_TRIANGLES, 0, 6);
		}

		// ??????? FBO??????????????? ImGui ?????
		glBindFramebuffer(GL_FRAMEBUFFER, 0);

		// Viewport：默认(INDEX==0)显示最终渲染结果；否则显示所选 FBO 的指定 color/depth 附件
		unsigned int viewportTextureID = 0;
		if (properties.VIEWPORT_DEBUG_FBO_INDEX == 0) {
			// 最终图（延迟+正向+后处理后的结果）
			FBO* finalFBO = postprocessRenderPass->GetOutputFBO();
			if (finalFBO && !finalFBO->textureIDs.empty())
				viewportTextureID = finalFBO->textureIDs[0];
		} else {
			std::vector<FBO*> busyFBOs = FramebuffersManager::GetInstance().GetBusyFBOs();
			int fboIdx = properties.VIEWPORT_DEBUG_FBO_INDEX - 1;
			if (fboIdx >= 0 && fboIdx < (int)busyFBOs.size()) {
				FBO* fbo = busyFBOs[fboIdx];
				if (!fbo->textureIDs.empty()
					&& properties.VIEWPORT_DEBUG_ATTACHMENT_INDEX >= 0
					&& properties.VIEWPORT_DEBUG_ATTACHMENT_INDEX < (int)fbo->textureIDs.size())
					viewportTextureID = fbo->textureIDs[properties.VIEWPORT_DEBUG_ATTACHMENT_INDEX];
			}
		}
		mygui.Viewport_UI(viewportTextureID);

		// Assets ?????? models / materials / shaders ??
		mygui.AssetsBrowser_UI();

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
		//scene.ClearFBO();
		// glfw: swap buffers and poll IO events (keys pressed/released, mouse moved etc.)
		glfwSwapBuffers(window);
		glfwPollEvents();
		
	}

	forwardRenderPass->Destroy();
	delete forwardRenderPass;
	deferRenderPass->Destroy();
	delete deferRenderPass;
	postprocessRenderPass->Destroy();
	delete postprocessRenderPass;
	SceneStateIO::Save(scene, camera, sceneStatePath);

	glfwTerminate();
	return 0;
}