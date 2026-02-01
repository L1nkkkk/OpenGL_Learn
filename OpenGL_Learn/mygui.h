#pragma once

#include <imgui/imgui.h>
#include <imgui/backends/imgui_impl_opengl3.h>
#include <imgui/backends/imgui_impl_glfw.h>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include "Scene.h"
#include "Global.h"

class MyGui {
public:
	static MyGui& GetInstance() {
		static MyGui instance;
		return instance;
	}
	MyGui(const MyGui&) = delete;
	MyGui& operator=(const MyGui&) = delete;

	void Init(GLFWwindow* window) {
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO(); (void)io;
		ImGui::StyleColorsDark();
		ImGui_ImplGlfw_InitForOpenGL(window, true);
		ImGui_ImplOpenGL3_Init("#version 330 core");
	}

	void NewFrame() {
		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();
	}

	void Begin() {
		ImGui::Begin("Settings");
	}

	void End() {
		ImGui::End();
	}

	void Render() {
		ImGui::Render();
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
	}

	void Anti_Aliasing_UI() {
		AntiAliasManager& AnitAliasMgr = AntiAliasManager::GetInstance();
		static int selectedOptionAA = 0;
		int optionAACount = sizeof(AntiAliasManager::optionsAA) / sizeof(AntiAliasManager::optionsAA[0]);
		if (ImGui::Combo("Anti-aliasing", &selectedOptionAA, AntiAliasManager::optionsAA, optionAACount)) {
			switch (selectedOptionAA) {
			case AntiAliasManager::Default:
				AnitAliasMgr.AntiAliasByType(AntiAliasManager::Default);
				break;
			case AntiAliasManager::MSAA:
				AnitAliasMgr.AntiAliasByType(AntiAliasManager::MSAA);
				break;
			}
		}
	}

	void Gamma_UI() {
		ImGui::Checkbox("HDR", &USE_HDR);
		if (HDR_EXPOSURE) {
			ImGui::DragFloat("hdr exposure", &HDR_EXPOSURE, 0.00f, 0.01f, 100.0f, "%.2f");
		}
		ImGui::Checkbox("gammaCorrection", &GAMMA_CORRECTION);
		if (GAMMA_CORRECTION) {
			ImGui::DragFloat("gamma Value", &GAMMA_VALUE, 0.01f, 1.0f, 2.6f, "%.2f");
		}
		ImGui::Checkbox("Bloom", &BLOOM);
		ImGui::DragFloat("bloom threshold", &BLOOM_THRESHOLD, 0.01f, 0.0f, 10.0f, "%.2f");
		ImGui::DragInt("bloom blur iterations", &BLOOM_BLUR_ITERATIONS, 1.0f, 1, 20);
	}

	void Scene_UI(Scene& scene) {
		scene.SetSceneGui();
	}

	void Framebuffers_UI() {
		auto& framebuffersMgr = FramebuffersManager::GetInstance();
		static int selectedOption = FBO::Default_FrameRenderType;
		int optionCount = sizeof(FBO::optionFrame) / sizeof(FBO::optionFrame[0]);
		if (ImGui::Combo("FrameType", &selectedOption, FBO::optionFrame, optionCount)) {
			switch (selectedOption) {
			case FBO::FrameRenderType::Default_FrameRenderType:
				framebuffersMgr.useType = FBO::FrameRenderType::Default_FrameRenderType;
				SHADOW_MAP_SHOW = false;
				break;
			case FBO::FrameRenderType::ShadowMap_FrameRenderType:
				framebuffersMgr.useType = FBO::FrameRenderType::ShadowMap_FrameRenderType;
				SHADOW_MAP_SHOW = true;
				break;
			case FBO::FrameRenderType::BrightColor_FrameRenderType:
				framebuffersMgr.useType = FBO::FrameRenderType::BrightColor_FrameRenderType;
				SHADOW_MAP_SHOW = false;
				break;
			}
		}
	}

	void Shadow_UI() {
		static int selectedOption = ShadowProperty::Default;
		int optionCount = sizeof(ShadowProperty::ShadowTypeStrs) / sizeof(ShadowProperty::ShadowTypeStrs[0]);
		if (ImGui::CollapsingHeader("Shadow Settings")) {
			if (ImGui::Combo("ShadowType", &selectedOption, ShadowProperty::ShadowTypeStrs, optionCount)) {
				switch (selectedOption) {
				case ShadowProperty::Default:
					SHADOW_TYPE = ShadowProperty::Default;
					break;
				case ShadowProperty::PCF:
					SHADOW_TYPE = ShadowProperty::PCF;				
					break;
				case ShadowProperty::PCSS:
					SHADOW_TYPE = ShadowProperty::PCSS;
					break;
				}
			}
		}
		ImGui::DragInt("shadow samples", &SHADOW_PCF_SAMPLE_NUM, 1.0, 16, 512);
		ImGui::DragInt("shadow rings", &SHADOW_PCF_RING_NUM, 1.0, 5, 20);
	}
private:
	MyGui() = default;
};