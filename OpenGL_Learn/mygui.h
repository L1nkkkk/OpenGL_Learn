#pragma once

#include <imgui.h>
#include <backends/imgui_impl_opengl3.h>
#include <backends/imgui_impl_glfw.h>
#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <filesystem>

#include "Scene.h"
#include "Global.h"
#include "XmlMaterialManager.h"
#include "Material.h"
#include "Model.h"

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
		io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
		ImGui::StyleColorsDark();
		ImGui_ImplGlfw_InitForOpenGL(window, true);
		ImGui_ImplOpenGL3_Init("#version 330 core");
	}

	void NewFrame() {
		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();
	}

	// 全屏 DockSpace（类似 Unity 主编辑区），供各个面板停靠
	void MainDockSpace() {
		ImGuiWindowFlags window_flags =
			ImGuiWindowFlags_NoDocking |
			ImGuiWindowFlags_NoTitleBar |
			ImGuiWindowFlags_NoCollapse |
			ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoBringToFrontOnFocus |
			ImGuiWindowFlags_NoNavFocus;

		ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(viewport->Pos);
		ImGui::SetNextWindowSize(viewport->Size);
		ImGui::SetNextWindowViewport(viewport->ID);

		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
		ImGui::Begin("MainDockSpace", nullptr, window_flags);
		ImGui::PopStyleVar(3);

		ImGuiID dockspace_id = ImGui::GetID("MainDockSpaceID");
		ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f));
		ImGui::End();
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

	// Assets 面板：浏览项目中的 models / materials 等资源（类似 Unity Project 窗口）
	void AssetsBrowser_UI() {
		namespace fs = std::filesystem;
		if (!ImGui::Begin("Assets")) {
			ImGui::End();
			return;
		}

		// 左：资源树  右：选中资源详情
		static std::string selectedPath;
		static std::string selectedCategory;

		ImGui::Columns(2, nullptr, true);

		ImGui::TextUnformatted("Project Assets");
		ImGui::Separator();

		auto drawDirTree = [&](const char* label,
			const char* rootPath,
			const char* extsCsv,
			const char* category) {
				if (!fs::exists(rootPath))
					return;
				if (!ImGui::TreeNode(label))
					return;

				// 解析扩展名列表
				std::vector<std::string> exts;
				if (extsCsv && *extsCsv) {
					std::string csv = extsCsv;
					size_t start = 0;
					while (start < csv.size()) {
						size_t comma = csv.find(',', start);
						if (comma == std::string::npos) comma = csv.size();
						std::string e = csv.substr(start, comma - start);
						if (!e.empty()) exts.push_back(e);
						start = comma + 1;
					}
				}

				std::function<void(const fs::path&)> drawNode = [&](const fs::path& dir) {
					for (auto& entry : fs::directory_iterator(dir)) {
						const auto& p = entry.path();
						std::string name = p.filename().string();
						if (entry.is_directory()) {
							if (ImGui::TreeNode(name.c_str())) {
								drawNode(p);
								ImGui::TreePop();
							}
						}
						else {
							if (!exts.empty()) {
								std::string ext = p.extension().string();
								bool match = false;
								for (auto& e : exts) {
									if (_stricmp(ext.c_str(), e.c_str()) == 0) {
										match = true;
										break;
									}
								}
								if (!match) continue;
							}
							bool isSelected = (p.string() == selectedPath);
							if (ImGui::Selectable(name.c_str(), isSelected)) {
								selectedPath = p.string();
								selectedCategory = category;
							}
						}
					}
				};

				drawNode(fs::path(rootPath));

				ImGui::TreePop();
		};

		// 常见资源类别（根节点）
		drawDirTree("Models", "models", ".obj,.fbx,.gltf,.glb", "Model");
		drawDirTree("Materials", "materials", ".xml,.jpg,.png,.hdr", "Material");
		drawDirTree("Shaders", "shaders", ".vs,.fs,.gs,.vert,.frag", "Shader");

		ImGui::NextColumn();

		// 右侧：选中资源详情
		ImGui::TextUnformatted("Details");
		ImGui::Separator();
		if (selectedPath.empty()) {
			ImGui::TextUnformatted("No asset selected.");
		}
		else {
			ImGui::Text("Type: %s", selectedCategory.empty() ? "Unknown" : selectedCategory.c_str());
			ImGui::TextWrapped("Path: %s", selectedPath.c_str());

			std::string ext = std::filesystem::path(selectedPath).extension().string();
			if (_stricmp(ext.c_str(), ".xml") == 0) {
				if (ImGui::Button("Reload as Material XML")) {
					auto& mgr = XmlMaterialManager::GetInstance();
					if (selectedPath.find("materials.xml") != std::string::npos) {
						mgr.LoadFromFile(selectedPath);
					}
					else {
						mgr.GetOrLoadMaterialByFile(selectedPath);
					}
				}
			}
		}

		ImGui::Columns(1);

		ImGui::End();
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

	void System_UI() {
		ImGui::Checkbox("Defer Rendering", &properties.DEFER_RENDERING);
		if (properties.DEFER_RENDERING) {
			ImGui::Checkbox("Light Volume", &properties.LIGHT_VOLUME);
		}
		ImGui::Checkbox("Debug Mode", &properties.DEBUG_MODE);
	}

	void Gamma_UI() {
		ImGui::Checkbox("HDR", &properties.USE_HDR);
		if (properties.HDR_EXPOSURE) {
			ImGui::DragFloat("hdr exposure", &properties.HDR_EXPOSURE, 0.00f, 0.01f, 100.0f, "%.2f");
		}
		ImGui::Checkbox("gammaCorrection", &properties.GAMMA_CORRECTION);
		if (properties.GAMMA_CORRECTION) {
			ImGui::DragFloat("gamma Value", &properties.GAMMA_VALUE, 0.01f, 1.0f, 2.6f, "%.2f");
		}
		ImGui::Checkbox("Bloom", &properties.BLOOM);
		ImGui::DragFloat("bloom threshold", &properties.BLOOM_THRESHOLD, 0.01f, 0.0f, 10.0f, "%.2f");
		ImGui::DragInt("bloom blur iterations", &properties.BLOOM_BLUR_ITERATIONS, 1.0f, 1, 20);
	}

	// Scene 面板：可单独停靠到 DockSpace 中
	void Scene_UI(Scene& scene) {
		if (ImGui::Begin("Scene")) {
			scene.SetSceneGui();
		}
		ImGui::End();
	}

public:
	// 竖向分隔条：拖动改变某个面板宽度
	void SplitterV(const char* id, float thickness, float* size, float minSize, float maxSize) {
		ImVec2 cursor = ImGui::GetCursorPos();
		ImGui::InvisibleButton(id, ImVec2(thickness, -1));
		if (ImGui::IsItemActive()) {
			float delta = ImGui::GetIO().MouseDelta.x;
			*size += delta;
			if (*size < minSize) *size = minSize;
			if (*size > maxSize) *size = maxSize;
		}
		ImGui::SetCursorPos(cursor);
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 p0 = ImGui::GetCursorScreenPos();
		ImVec2 p1 = ImVec2(p0.x + thickness, p0.y + ImGui::GetContentRegionAvail().y);
		dl->AddRectFilled(p0, p1, IM_COL32(80, 80, 80, 255));
		ImGui::SetCursorPos(ImVec2(cursor.x + thickness, cursor.y));
	}

	// Viewport 内容（放在 BeginChild/MenuBar 内），不自己 Begin/End
	void ViewportContent(unsigned int textureID) {
		static int aspectMode = 0; // 0: Free, 1:16:9, 2:4:3, 3:1:1, 4:Custom
		static int customWidth = 1280;
		static int customHeight = 720;
		static bool initializedResolution = false;

		if (!initializedResolution) {
			customWidth = properties.SCREEN_WIDTH;
			customHeight = properties.SCREEN_HEIGHT;
			initializedResolution = true;
		}

		// 菜单栏
		if (ImGui::BeginMenuBar()) {
			const char* aspectItems[] = { "Free", "16:9", "4:3", "1:1", "Custom" };
			ImGui::TextUnformatted("Viewport");
			ImGui::SameLine();
			ImGui::TextDisabled("(%dx%d)", properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT);
			ImGui::SameLine();
			ImGui::Separator();
			ImGui::SameLine();
			ImGui::TextUnformatted("Aspect");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(90.0f);
			ImGui::Combo("##aspect_mode", &aspectMode, aspectItems, IM_ARRAYSIZE(aspectItems));

			if (aspectMode == 4) {
				ImGui::SameLine();
				ImGui::SetNextItemWidth(70.0f);
				ImGui::InputInt("W", &customWidth);
				ImGui::SameLine();
				ImGui::SetNextItemWidth(70.0f);
				ImGui::InputInt("H", &customHeight);
				if (customWidth < 1) customWidth = 1;
				if (customHeight < 1) customHeight = 1;
				ImGui::SameLine();
				if (ImGui::Button("Apply")) {
					properties.SCREEN_WIDTH = customWidth;
					properties.SCREEN_HEIGHT = customHeight;
					FramebuffersManager::GetInstance().Resize();
					glViewport(0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT);
				}
			}

			ImGui::EndMenuBar();
		}

		ImVec2 avail = ImGui::GetContentRegionAvail();
		ImVec2 imageSize = avail;

		float targetAspect = 0.0f;
		switch (aspectMode) {
		case 1: targetAspect = 16.0f / 9.0f; break;
		case 2: targetAspect = 4.0f / 3.0f; break;
		case 3: targetAspect = 1.0f; break;
		case 4: targetAspect = (customHeight > 0) ? (customWidth / (float)customHeight) : 0.0f; break;
		default: break;
		}
		if (targetAspect > 0.0f && avail.x > 0.0f && avail.y > 0.0f) {
			float availAspect = avail.x / avail.y;
			if (availAspect > targetAspect) {
				imageSize.y = avail.y;
				imageSize.x = imageSize.y * targetAspect;
			}
			else {
				imageSize.x = avail.x;
				imageSize.y = imageSize.x / targetAspect;
			}
		}

		if (textureID != 0) {
			ImGui::Image(
				(ImTextureID)(intptr_t)textureID,
				imageSize,
				ImVec2(0.0f, 1.0f),
				ImVec2(1.0f, 0.0f)
			);
		}
		else {
			ImGui::TextUnformatted("No viewport texture.");
		}
	}

	void Framebuffers_UI() {
		auto& framebuffersMgr = FramebuffersManager::GetInstance();
		static int selectedOption = FBO::Default_FrameRenderType;
		int optionCount = sizeof(FBO::optionFrame) / sizeof(FBO::optionFrame[0]);
		if (ImGui::Combo("FrameType", &selectedOption, FBO::optionFrame, optionCount)) {
			switch (selectedOption) {
			case FBO::FrameRenderType::Default_FrameRenderType:
				framebuffersMgr.useType = FBO::FrameRenderType::Default_FrameRenderType;
				properties.SHADOW_MAP_SHOW = false;
				break;
			case FBO::FrameRenderType::ShadowMap_FrameRenderType:
				framebuffersMgr.useType = FBO::FrameRenderType::ShadowMap_FrameRenderType;
				properties.SHADOW_MAP_SHOW = true;
				break;
			case FBO::FrameRenderType::BrightColor_FrameRenderType:
				framebuffersMgr.useType = FBO::FrameRenderType::BrightColor_FrameRenderType;
				properties.SHADOW_MAP_SHOW = false;
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
					properties.SHADOW_TYPE = ShadowProperty::Default;
					break;
				case ShadowProperty::PCF:
					properties.SHADOW_TYPE = ShadowProperty::PCF;
					break;
				case ShadowProperty::PCSS:
					properties.SHADOW_TYPE = ShadowProperty::PCSS;
					break;
				}
			}
		}
		ImGui::DragInt("shadow samples", &properties.SHADOW_PCF_SAMPLE_NUM, 1.0, 16, 512);
		ImGui::DragInt("shadow rings", &properties.SHADOW_PCF_RING_NUM, 1.0, 5, 20);
	}

	// 绘制单个材质的 Shader / Render State / Properties（供 Materials Inspector 与 Model Materials 共用）
	static void DrawSingleMaterialContent(Material* mat) {
		if (!mat) return;
		auto& props = mat->GetPropertiesMutable();

		ImGui::Separator();
		ImGui::Text("Shader: %s", mat->GetShaderName().c_str());

		RenderState rs = mat->GetRenderState();
		if (ImGui::CollapsingHeader("Render State")) {
			ImGui::Checkbox("Depth Test", &rs.depthTest);
			ImGui::Checkbox("Depth Write", &rs.depthWrite);
			ImGui::Checkbox("Stencil Test", &rs.stencilTest);
			int blend = static_cast<int>(rs.blendMode);
			const char* blendNames[] = { "None", "AlphaBlend", "Additive" };
			if (ImGui::Combo("Blend Mode", &blend, blendNames, IM_ARRAYSIZE(blendNames))) {
				rs.blendMode = static_cast<BlendMode>(blend);
			}
			int cull = static_cast<int>(rs.cullMode);
			const char* cullNames[] = { "None", "Front", "Back" };
			if (ImGui::Combo("Cull Mode", &cull, cullNames, IM_ARRAYSIZE(cullNames))) {
				rs.cullMode = static_cast<CullMode>(cull);
			}
			mat->SetRenderState(rs);
		}

		ImGui::Separator();
		ImGui::TextUnformatted("Properties");

		for (auto& kv : props) {
			const std::string& propName = kv.first;
			MaterialProperty& prop = kv.second;
			ImGui::PushID(propName.c_str());
			switch (prop.type) {
			case MaterialPropertyType::Float:
				ImGui::DragFloat(propName.c_str(), &prop.scalarValue.floatValue, prop.step, prop.minVal, prop.maxVal);
				break;
			case MaterialPropertyType::Int:
			{
				int v = prop.scalarValue.intValue;
				if (ImGui::DragInt(propName.c_str(), &v, 1.0f, static_cast<int>(prop.minVal), static_cast<int>(prop.maxVal))) {
					prop.scalarValue.intValue = v;
				}
				break;
			}
			case MaterialPropertyType::Bool:
				ImGui::Checkbox(propName.c_str(), &prop.scalarValue.boolValue);
				break;
			case MaterialPropertyType::Vec2:
				ImGui::DragFloat2(propName.c_str(), &prop.vec2Value.x, 0.01f, prop.minVal, prop.maxVal);
				break;
			case MaterialPropertyType::Vec3:
				ImGui::DragFloat3(propName.c_str(), &prop.vec3Value.x, 0.01f, prop.minVal, prop.maxVal);
				break;
			case MaterialPropertyType::Vec4:
				ImGui::DragFloat4(propName.c_str(), &prop.vec4Value.x, 0.01f, prop.minVal, prop.maxVal);
				break;
			case MaterialPropertyType::Color:
				ImGui::ColorEdit3(propName.c_str(), &prop.vec3Value.x);
				break;
			case MaterialPropertyType::Texture:
				ImGui::Text("%s (Texture x%d)", propName.c_str(), static_cast<int>(prop.textures.size()));
				break;
			}
			ImGui::PopID();
		}
	}

	// 材质参数调试面板：可单独停靠到 DockSpace 中
	void MaterialsInspector_UI() {
		auto& matMgr = XmlMaterialManager::GetInstance();
		auto allMats = matMgr.GetAllMaterials();

		if (!ImGui::Begin("Materials Inspector")) {
			ImGui::End();
			return;
		}

		if (allMats.empty()) {
			ImGui::TextUnformatted("No materials loaded.");
			ImGui::End();
			return;
		}

		static int currentIndex = 0;
		static std::vector<std::string> names;
		names.clear();
		names.reserve(allMats.size());
		for (const auto& kv : allMats) {
			names.push_back(kv.first);
		}
		if (currentIndex >= static_cast<int>(names.size())) currentIndex = 0;

		// 左侧下拉选择当前材质
		ImGui::TextUnformatted("Materials Inspector");
		if (!names.empty()) {
			if (ImGui::BeginCombo("Material", names[currentIndex].c_str())) {
				for (int i = 0; i < static_cast<int>(names.size()); ++i) {
					bool selected = (i == currentIndex);
					if (ImGui::Selectable(names[i].c_str(), selected)) {
						currentIndex = i;
					}
					if (selected) {
						ImGui::SetItemDefaultFocus();
					}
				}
				ImGui::EndCombo();
			}
		}

		if (names.empty()) {
			ImGui::End();
			return;
		}

		if (currentIndex < 0 || currentIndex >= static_cast<int>(allMats.size()) || !allMats[currentIndex].second) {
			ImGui::TextUnformatted("Invalid material.");
			ImGui::End();
			return;
		}

		Material* mat = allMats[currentIndex].second.get();
		DrawSingleMaterialContent(mat);

		ImGui::End();
	}

	// 选中模型材质面板：在 Scene 中选中某个模型后，在此窗口查看/编辑该模型下每个 Mesh 的材质
	void ModelMaterialsInspector_UI(Scene& scene) {
		if (!ImGui::Begin("Model Materials")) {
			ImGui::End();
			return;
		}

		Model* model = scene.GetSelectedModelForMaterials();
		if (!model) {
			ImGui::TextUnformatted("Select a model in Scene panel:");
			ImGui::TextUnformatted("Open 'Model Settings' -> expand a model -> click 'View Materials'.");
			ImGui::End();
			return;
		}

		ImGui::Text("Model: %s", model->GetName().c_str());
		if (ImGui::Button("Clear Selection")) {
			scene.SetSelectedModelForMaterials(nullptr);
		}
		ImGui::Separator();

		auto& meshes = model->GetMeshes();
		if (meshes.empty()) {
			ImGui::TextUnformatted("No meshes.");
			ImGui::End();
			return;
		}

		for (size_t i = 0; i < meshes.size(); ++i) {
			Mesh& mesh = meshes[i];
			Material* mat = mesh.material_ptr;
			char headerLabel[64];
			snprintf(headerLabel, sizeof(headerLabel), "Mesh %zu", i);
			if (ImGui::CollapsingHeader(headerLabel)) {
				if (mat) {
					ImGui::PushID(static_cast<int>(i));
					DrawSingleMaterialContent(mat);
					ImGui::PopID();
				} else {
					ImGui::TextUnformatted("No material.");
				}
			}
		}

		ImGui::End();
	}

	// Materials XML Editor: 查看并实时编辑所有材质相关的 xml 文件
	void MaterialsEditor_UI() {
		static bool initialized = false;
		static std::vector<std::string> files;
		static int currentIndex = -1;
		static std::string currentPath;
		static std::string currentContent;
		static bool contentDirty = false;
		static std::string errorMsg;

		auto buildFileList = [&]() {
			files.clear();
			errorMsg.clear();

			// 默认候选文件列表，可按需扩展
			const char* candidates[] = {
				"materials.xml",
				"materials/brickwall/brickwall.xml"
			};

			for (const char* path : candidates) {
				std::ifstream ifs(path);
				if (ifs) {
					files.emplace_back(path);
				}
			}

			if (currentIndex >= static_cast<int>(files.size())) {
				currentIndex = -1;
				currentPath.clear();
				currentContent.clear();
				contentDirty = false;
			}
		};

		if (!initialized) {
			buildFileList();
			initialized = true;
		}

		if (!ImGui::Begin("Materials XML Editor")) {
			ImGui::End();
			return;
		}

		ImGui::Columns(2, nullptr, true);

		// 左侧：文件列表
		if (ImGui::Button("Refresh")) {
			buildFileList();
		}
		ImGui::SameLine();
		static char customPath[260] = "";
		ImGui::SetNextItemWidth(150.0f);
		ImGui::InputText("##custom_material_path", customPath, sizeof(customPath));
		ImGui::SameLine();
		if (ImGui::Button("Add")) {
			if (customPath[0] != '\0') {
				std::ifstream ifs(customPath);
				if (ifs) {
					std::string pathStr = customPath;
					bool existsInList = false;
					for (const auto& f : files) {
						if (f == pathStr) {
							existsInList = true;
							break;
						}
					}
					if (!existsInList) {
						files.push_back(pathStr);
					}
					errorMsg.clear();
				}
				else {
					errorMsg = std::string("Failed to open: ") + customPath;
				}
			}
		}

		ImGui::Separator();
		for (int i = 0; i < static_cast<int>(files.size()); ++i) {
			bool selected = (i == currentIndex);
			const char* label = files[i].c_str();
			if (ImGui::Selectable(label, selected)) {
				std::ifstream ifs(files[i]);
				if (ifs) {
					std::stringstream buf;
					buf << ifs.rdbuf();
					currentContent = buf.str();
					currentPath = files[i];
					currentIndex = i;
					contentDirty = false;
					errorMsg.clear();
				}
				else {
					errorMsg = std::string("Failed to open: ") + files[i];
				}
			}
		}

		ImGui::NextColumn();

		// 右侧：文本编辑器
		if (currentIndex >= 0) {
			ImGui::TextUnformatted(currentPath.c_str());
			ImGui::Separator();

			static std::vector<char> textBuffer;
			// 当首次载入或内容非脏时，同步 buffer
			if (!contentDirty || textBuffer.empty()) {
				textBuffer.assign(currentContent.begin(), currentContent.end());
				textBuffer.push_back('\0');
			}

			if (ImGui::InputTextMultiline(
				"##xml_editor",
				textBuffer.data(),
				textBuffer.size(),
				ImVec2(-1.0f, ImGui::GetTextLineHeight() * 20),
				ImGuiInputTextFlags_AllowTabInput)) {
				currentContent = textBuffer.data();
				contentDirty = true;
			}

			if (ImGui::Button("Reload")) {
				std::ifstream ifs(currentPath);
				if (ifs) {
					std::stringstream buf;
					buf << ifs.rdbuf();
					currentContent = buf.str();
					contentDirty = false;
					errorMsg.clear();
				}
				else {
					errorMsg = std::string("Failed to reload: ") + currentPath;
				}
			}
			ImGui::SameLine();
			if (ImGui::Button("Save")) {
				std::ofstream ofs(currentPath);
				if (ofs) {
					ofs << currentContent;
					ofs.close();
					contentDirty = false;
					errorMsg.clear();

					// 保存后通知 XmlMaterialManager 重新加载，使修改实时生效
					auto& mgr = XmlMaterialManager::GetInstance();
					if (currentPath == "materials.xml") {
						mgr.LoadFromFile(currentPath);
					}
					else {
						mgr.GetOrLoadMaterialByFile(currentPath);
					}
				}
				else {
					errorMsg = std::string("Failed to save: ") + currentPath;
				}
			}
			if (contentDirty) {
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "* modified");
			}
		}
		else {
			ImGui::TextUnformatted("No material xml file selected.");
		}

		if (!errorMsg.empty()) {
			ImGui::Separator();
			ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", errorMsg.c_str());
		}

		ImGui::Columns(1);
		ImGui::End();
	}

	// 旧的独立 Viewport 窗口保留为兼容（UnityLayout 会用内嵌 ViewportContent）
	void Viewport_UI(unsigned int textureID) {
		ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_MenuBar);
		ViewportContent(textureID);
		ImGui::End();
	}
private:
	MyGui() = default;
	SystemProperties& properties = SystemProperties::GetInstance();
};