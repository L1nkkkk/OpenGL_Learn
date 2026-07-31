#pragma once

#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_opengl3.h>
#include <backends/imgui_impl_glfw.h>
#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <filesystem>
#include <cstring>
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <windows.h>
#include <commdlg.h>

#include "Scene.h"
#include "Global.h"
#include "ShaderManager.h"
#include "XmlMaterialManager.h"
#include "Material.h"
#include "Model.h"
#include "Profiler.h"
#include "SceneStateIO.h"
#include "EditorSceneManager.h"
#include "EditorMotionTimeline.h"

class MyGui {
public:
	static MyGui& GetInstance() {
		static MyGui instance;
		return instance;
	}
	MyGui(const MyGui&) = delete;
	MyGui& operator=(const MyGui&) = delete;

	static bool PickTextureFileWithDialog(std::string& outPath) {
		char fileBuf[MAX_PATH] = { 0 };
		OPENFILENAMEA ofn = {};
		ofn.lStructSize = sizeof(ofn);
		ofn.lpstrFile = fileBuf;
		ofn.nMaxFile = MAX_PATH;
		ofn.lpstrFilter = "Image Files\0*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.dds\0All Files\0*.*\0";
		ofn.nFilterIndex = 1;
		ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
		if (GetOpenFileNameA(&ofn) == TRUE) {
			outPath = fileBuf;
			return true;
		}
		return false;
	}

	static bool PickSceneFileWithDialog(
		std::string& outPath,
		bool saveDialog)
	{
		char fileBuffer[4096] = { 0 };
		std::error_code error;
		const std::string initialDirectory =
			std::filesystem::absolute("saved", error).string();

		OPENFILENAMEA ofn = {};
		ofn.lStructSize = sizeof(ofn);
		ofn.lpstrFile = fileBuffer;
		ofn.nMaxFile = static_cast<DWORD>(sizeof(fileBuffer));
		ofn.lpstrFilter =
			"OpenGL Learn Scene (*.json)\0*.json\0All Files (*.*)\0*.*\0";
		ofn.nFilterIndex = 1;
		ofn.lpstrDefExt = "json";
		ofn.lpstrInitialDir =
			error ? nullptr : initialDirectory.c_str();
		ofn.Flags =
			OFN_EXPLORER |
			OFN_NOCHANGEDIR |
			OFN_PATHMUSTEXIST;

		const BOOL accepted = saveDialog
			? (ofn.Flags |= OFN_OVERWRITEPROMPT, GetSaveFileNameA(&ofn))
			: (ofn.Flags |= OFN_FILEMUSTEXIST, GetOpenFileNameA(&ofn));
		if (accepted == TRUE) {
			outPath = fileBuffer;
			return true;
		}
		return false;
	}

	void Init(GLFWwindow* window) {
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO(); (void)io;
		io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
		LoadEditorFonts(io);
		ApplyEditorStyle();
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

		DrawEditorToolbar();
		ImGui::Separator();
		ImGuiID dockspace_id = ImGui::GetID("MainDockSpaceID_2026");
		BuildDefaultLayout(dockspace_id, m_requestLayoutReset);
		m_requestLayoutReset = false;
		ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f));
		ImGui::End();
	}

	void Begin() {
		m_rendererPanelVisible = ImGui::Begin("Renderer");
		if (m_rendererPanelVisible) {
			DrawPanelHeader(
				"Renderer",
				"Pipeline, shadow cache, post processing and render targets");
		}
	}

	void End() {
		ImGui::End();
	}

	void Render() {
		ImGui::Render();
		ImDrawData* drawData = ImGui::GetDrawData();
		std::uint64_t uiDrawCalls = 0;
		if (drawData) {
			for (int listIndex = 0; listIndex < drawData->CmdListsCount; ++listIndex) {
				const ImDrawList* drawList = drawData->CmdLists[listIndex];
				for (int commandIndex = 0; commandIndex < drawList->CmdBuffer.Size; ++commandIndex) {
					const ImDrawCmd& command = drawList->CmdBuffer[commandIndex];
					if (command.UserCallback == nullptr && command.ElemCount > 0) {
						++uiDrawCalls;
					}
				}
			}
			PerformanceProfiler::GetInstance().RecordUiDrawData(
				uiDrawCalls,
				static_cast<std::uint64_t>(drawData->TotalVtxCount),
				static_cast<std::uint64_t>(drawData->TotalIdxCount));
		}
		ImGui_ImplOpenGL3_RenderDrawData(drawData);
	}

	void Overview_UI() {
		if (!ImGui::Begin("Overview")) {
			ImGui::End();
			return;
		}

		auto& shaderManager = ShaderManager::GetInstance();
		auto& materialManager = XmlMaterialManager::GetInstance();

		DrawPanelHeader(
			"Workspace",
			"Project health, live reload and editor-wide actions");

		const float gap = ImGui::GetStyle().ItemSpacing.x;
		const float cardWidth =
			(std::max)(120.0f, (ImGui::GetContentRegionAvail().x - gap) * 0.5f);
		const std::string viewportValue =
			std::to_string(properties.SCREEN_WIDTH) + " x " +
			std::to_string(properties.SCREEN_HEIGHT);
		DrawMetricCard(
			"overview_resolution",
			"RENDER SIZE",
			viewportValue.c_str(),
			ImVec4(0.31f, 0.73f, 0.68f, 1.0f),
			cardWidth);
		ImGui::SameLine();
		DrawMetricCard(
			"overview_pipeline",
			"ACTIVE PIPELINE",
			properties.DEFER_RENDERING ? "Deferred" : "Forward",
			ImVec4(0.45f, 0.62f, 0.95f, 1.0f),
			cardWidth);

		ImGui::Spacing();
		ImGui::SeparatorText("Live Reload");
		ImGui::Checkbox(
			"Shaders##auto_reload_shaders",
			&properties.AUTO_RELOAD_SHADERS);
		ImGui::SameLine();
		ImGui::Checkbox(
			"Materials##auto_reload_materials",
			&properties.AUTO_RELOAD_MATERIALS);

		if (ImGui::Button("Reload shaders", ImVec2(140.0f, 0.0f))) {
			shaderManager.ReloadAllShaders();
		}
		ImGui::SameLine();
		if (ImGui::Button("Reload materials", ImVec2(140.0f, 0.0f))) {
			materialManager.ReloadAllFiles();
		}

		ImGui::Spacing();
		ImGui::SeparatorText("Status");
		DrawStatusRow(
			"Shaders",
			shaderManager.WasLastReloadSuccessful(),
			shaderManager.GetLastReloadMessage().empty()
				? "Idle"
				: shaderManager.GetLastReloadMessage().c_str(),
			shaderManager.GetReloadCount());
		DrawStatusRow(
			"Materials",
			materialManager.WasLastReloadSuccessful(),
			materialManager.GetLastReloadMessage().empty()
				? "Idle"
				: materialManager.GetLastReloadMessage().c_str(),
			materialManager.GetReloadCount());
		ImGui::TextDisabled(
			"UI font: %s",
			m_editorFontDescription.c_str());

		ImGui::End();
	}

	void Profiler_UI() {
		if (!ImGui::Begin("Profiler")) {
			ImGui::End();
			return;
		}

		DrawPanelHeader(
			"Profiler",
			"Frame pacing, render submission and memory diagnostics");
		auto& profiler = PerformanceProfiler::GetInstance();
		bool enabled = profiler.IsEnabled();
		if (ImGui::Checkbox("Capture", &enabled)) {
			profiler.SetEnabled(enabled);
		}

		ImGui::SameLine();
		bool gpuTiming = profiler.IsGpuTimingEnabled();
		if (!profiler.IsGpuTimingSupported()) {
			gpuTiming = false;
		}
		if (ImGui::Checkbox("GPU timing", &gpuTiming)) {
			profiler.SetGpuTimingEnabled(gpuTiming);
		}
		if (!profiler.IsGpuTimingSupported()) {
			ImGui::SameLine();
			ImGui::TextDisabled("(not supported)");
		}

		ImGui::SameLine();
		if (ImGui::Button("Reset")) {
			profiler.ResetStatistics();
		}

		const auto& summary = profiler.GetFrameSummary();
		const double averageFps = summary.averageCpuFrameMs > 0.0
			? 1000.0 / summary.averageCpuFrameMs
			: 0.0;

		auto drawZoneTable = [](const char* id, const std::vector<ProfilerZoneStats>& zones) {
			if (zones.empty()) {
				ImGui::TextDisabled("No samples yet.");
				return;
			}

			const ImGuiTableFlags flags =
				ImGuiTableFlags_BordersInnerH |
				ImGuiTableFlags_RowBg |
				ImGuiTableFlags_SizingStretchProp;
			if (ImGui::BeginTable(id, 4, flags)) {
				ImGui::TableSetupColumn("Zone", ImGuiTableColumnFlags_WidthStretch, 2.2f);
				ImGui::TableSetupColumn("Latest");
				ImGui::TableSetupColumn("Average");
				ImGui::TableSetupColumn("Peak");
				ImGui::TableHeadersRow();
				for (const auto& zone : zones) {
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextUnformatted(zone.name.c_str());
					ImGui::TableSetColumnIndex(1);
					ImGui::Text("%.3f ms", zone.latestMs);
					ImGui::TableSetColumnIndex(2);
					ImGui::Text("%.3f ms", zone.averageMs);
					ImGui::TableSetColumnIndex(3);
					ImGui::Text("%.3f ms", zone.peakMs);
				}
				ImGui::EndTable();
			}
		};

		char cpuValue[32] = {};
		char gpuValue[32] = {};
		char fpsValue[32] = {};
		std::snprintf(cpuValue, sizeof(cpuValue), "%.2f ms", summary.cpuFrameMs);
		if (profiler.IsGpuTimingSupported()) {
			std::snprintf(
				gpuValue,
				sizeof(gpuValue),
				"%.2f ms",
				summary.gpuFrameMs);
		}
		else {
			std::snprintf(gpuValue, sizeof(gpuValue), "N/A");
		}
		std::snprintf(fpsValue, sizeof(fpsValue), "%.1f", averageFps);
		const float metricGap = ImGui::GetStyle().ItemSpacing.x;
		const float metricWidth =
			(std::max)(
				105.0f,
				(ImGui::GetContentRegionAvail().x - metricGap * 2.0f) / 3.0f);
		ImGui::Spacing();
		DrawMetricCard(
			"profiler_cpu",
			"CPU FRAME",
			cpuValue,
			ImVec4(0.31f, 0.73f, 0.68f, 1.0f),
			metricWidth);
		ImGui::SameLine();
		DrawMetricCard(
			"profiler_gpu",
			"GPU FRAME",
			gpuValue,
			ImVec4(0.45f, 0.62f, 0.95f, 1.0f),
			metricWidth);
		ImGui::SameLine();
		DrawMetricCard(
			"profiler_fps",
			"AVERAGE FPS",
			fpsValue,
			ImVec4(0.91f, 0.72f, 0.33f, 1.0f),
			metricWidth);

		const auto& renderStats = profiler.GetRenderStats();
		if (ImGui::BeginTabBar("##profiler_tabs")) {
			if (ImGui::BeginTabItem("Frame")) {
				const auto& cpuHistory = profiler.GetCpuFrameHistory();
				if (!cpuHistory.empty()) {
					const float maxValue =
						(std::max)(16.67f, *std::max_element(
							cpuHistory.begin(), cpuHistory.end()));
					ImGui::PlotLines(
						"CPU frame",
						cpuHistory.data(),
						static_cast<int>(cpuHistory.size()),
						0,
						nullptr,
						0.0f,
						maxValue * 1.15f,
						ImVec2(-1.0f, 82.0f));
				}
				const auto& gpuHistory = profiler.GetGpuFrameHistory();
				if (!gpuHistory.empty()) {
					const float maxValue =
						(std::max)(16.67f, *std::max_element(
							gpuHistory.begin(), gpuHistory.end()));
					ImGui::PlotLines(
						"GPU frame",
						gpuHistory.data(),
						static_cast<int>(gpuHistory.size()),
						0,
						nullptr,
						0.0f,
						maxValue * 1.15f,
						ImVec2(-1.0f, 82.0f));
				}
				ImGui::TextDisabled(
					"CPU average %.3f ms  |  P95 %.3f ms  |  P99 %.3f ms",
					summary.averageCpuFrameMs,
					summary.cpuP95Ms,
					summary.cpuP99Ms);
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Zones")) {
				ImGui::SeparatorText("CPU");
				drawZoneTable("cpu_profile_zones", profiler.GetCpuZoneStats());
				ImGui::SeparatorText("GPU");
				drawZoneTable("gpu_profile_zones", profiler.GetGpuZoneStats());
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Render")) {
				if (ImGui::BeginTable(
					"render_stats",
					2,
					ImGuiTableFlags_RowBg |
						ImGuiTableFlags_BordersInnerH |
						ImGuiTableFlags_SizingStretchProp)) {
					auto row = [](const char* label, const char* value) {
						ImGui::TableNextRow();
						ImGui::TableSetColumnIndex(0);
						ImGui::TextDisabled("%s", label);
						ImGui::TableSetColumnIndex(1);
						ImGui::TextUnformatted(value);
					};
					char value[128] = {};
					std::snprintf(value, sizeof(value), "%llu",
						static_cast<unsigned long long>(renderStats.drawCalls));
					row("Scene draw calls", value);
					std::snprintf(value, sizeof(value), "%llu",
						static_cast<unsigned long long>(renderStats.submittedTriangles));
					row("Submitted triangles", value);
					std::snprintf(value, sizeof(value), "%llu / %llu / %llu",
						static_cast<unsigned long long>(renderStats.activeModels),
						static_cast<unsigned long long>(renderStats.visibleModels),
						static_cast<unsigned long long>(renderStats.culledModels));
					row("Models active / visible / culled", value);
					std::snprintf(value, sizeof(value), "%llu / %llu",
						static_cast<unsigned long long>(renderStats.opaqueMeshes),
						static_cast<unsigned long long>(renderStats.transparentMeshes));
					row("Meshes opaque / transparent", value);
					std::snprintf(value, sizeof(value), "%llu / %llu",
						static_cast<unsigned long long>(renderStats.materialBinds),
						static_cast<unsigned long long>(renderStats.materialBindCacheHits));
					row("Material binds / cache hits", value);
					std::snprintf(value, sizeof(value), "%llu / %llu",
						static_cast<unsigned long long>(renderStats.renderStateChanges),
						static_cast<unsigned long long>(renderStats.renderStateCacheHits));
					row("Render state changes / cache hits", value);
					std::snprintf(value, sizeof(value), "%llu / %llu",
						static_cast<unsigned long long>(renderStats.textureStateChanges),
						static_cast<unsigned long long>(renderStats.textureStateCacheHits));
					row("Texture changes / cache hits", value);
					std::snprintf(value, sizeof(value), "%llu / %llu",
						static_cast<unsigned long long>(renderStats.framebufferBinds),
						static_cast<unsigned long long>(renderStats.framebufferBindCacheHits));
					row("Framebuffer binds / cache hits", value);
					std::snprintf(value, sizeof(value), "%llu",
						static_cast<unsigned long long>(renderStats.uiDrawCalls));
					row("ImGui draw calls", value);
					ImGui::EndTable();
				}
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Memory")) {
				const auto& memoryStats = profiler.GetMemoryStats();
				constexpr double kBytesPerMiB = 1024.0 * 1024.0;
				ImGui::Text(
					"Working set  %.2f MiB",
					static_cast<double>(
						memoryStats.processWorkingSetBytes) / kBytesPerMiB);
				ImGui::SameLine();
				ImGui::TextDisabled(
					"Private  %.2f MiB",
					static_cast<double>(
						memoryStats.processPrivateBytes) / kBytesPerMiB);
				static const char* categoryNames[] = {
					"Textures",
					"Mesh CPU",
					"Mesh GPU",
					"Render targets"
				};
				if (ImGui::BeginTable(
					"memory_categories",
					4,
					ImGuiTableFlags_RowBg |
						ImGuiTableFlags_BordersInnerH |
						ImGuiTableFlags_SizingStretchProp)) {
					ImGui::TableSetupColumn("Category");
					ImGui::TableSetupColumn("Current");
					ImGui::TableSetupColumn("Peak");
					ImGui::TableSetupColumn("Resources");
					ImGui::TableHeadersRow();
					for (std::size_t i = 0;
						i < memoryStats.categories.size();
						++i) {
						const auto& category = memoryStats.categories[i];
						ImGui::TableNextRow();
						ImGui::TableSetColumnIndex(0);
						ImGui::TextUnformatted(categoryNames[i]);
						ImGui::TableSetColumnIndex(1);
						ImGui::Text(
							"%.2f MiB",
							static_cast<double>(
								category.currentBytes) / kBytesPerMiB);
						ImGui::TableSetColumnIndex(2);
						ImGui::Text(
							"%.2f MiB",
							static_cast<double>(
								category.peakBytes) / kBytesPerMiB);
						ImGui::TableSetColumnIndex(3);
						ImGui::Text(
							"%llu",
							static_cast<unsigned long long>(
								category.resourceCount));
					}
					ImGui::EndTable();
				}
				ImGui::TextDisabled(
					"Texture cache hits / misses: %llu / %llu",
					static_cast<unsigned long long>(
						memoryStats.textureCacheHits),
					static_cast<unsigned long long>(
						memoryStats.textureCacheMisses));
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}

		ImGui::End();
	}

	// Assets 面板：浏览项目中的 models / materials 等资源（类似 Unity Project 窗口）
	void AssetsBrowser_UI() {
		if (!ImGui::Begin("Assets")) {
			ImGui::End();
			return;
		}

		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
			ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
			ResetAssetBrowserCache();
		}
		const bool cacheHit = m_assetBrowserCacheInitialized;
		InitializeAssetBrowserCache();
		PerformanceProfiler::GetInstance().RecordAssetBrowserCacheLookup(cacheHit);

		// 左：资源树  右：选中资源详情
		static std::string selectedPath;
		static std::string selectedCategory;

		ImGui::Columns(2, nullptr, true);

		ImGui::TextUnformatted("Project Assets");
		ImGui::Separator();

		for (auto& category : m_assetBrowserCategories) {
			DrawAssetBrowserCategory(category, selectedPath, selectedCategory);
		}

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
						mgr.ReloadMaterialFile(selectedPath);
					}
				}
			}
		}

		ImGui::Columns(1);

		ImGui::End();
	}

	void Anti_Aliasing_UI() {
		if (!m_rendererPanelVisible ||
			!ImGui::CollapsingHeader("Anti-aliasing")) {
			return;
		}
		AntiAliasManager& AnitAliasMgr = AntiAliasManager::GetInstance();
		static int selectedOptionAA = 0;
		int optionAACount = sizeof(AntiAliasManager::optionsAA) / sizeof(AntiAliasManager::optionsAA[0]);
		ImGui::TextDisabled(
			"Window-level multisampling. Restart may be required for format changes.");
		if (ImGui::Combo("Mode##anti_aliasing", &selectedOptionAA, AntiAliasManager::optionsAA, optionAACount)) {
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
		if (!m_rendererPanelVisible) {
			return;
		}
		if (SceneStateIO::HasPendingAsyncLoads()) {
			const int pending = SceneStateIO::GetPendingAsyncLoadCount();
			const int total = SceneStateIO::GetTotalAsyncLoadCount();
			const int done = (total >= pending) ? (total - pending) : 0;
			ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "Loading models... %d/%d", done, total);
			ImGui::ProgressBar(total > 0 ? (float)done / (float)total : 0.0f, ImVec2(-1.0f, 0.0f));
			ImGui::Separator();
		}
		if (ImGui::CollapsingHeader(
			"Render Pipeline",
			ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::TextDisabled(
				"Choose the lighting path and submission behavior.");
			const float buttonGap = ImGui::GetStyle().ItemSpacing.x;
			const float buttonWidth =
				(ImGui::GetContentRegionAvail().x - buttonGap) * 0.5f;
			DrawSelectionButton(
				"Forward",
				!properties.DEFER_RENDERING,
				ImVec2(buttonWidth, 32.0f));
			if (ImGui::IsItemClicked()) {
				properties.DEFER_RENDERING = false;
			}
			ImGui::SameLine();
			DrawSelectionButton(
				"Deferred",
				properties.DEFER_RENDERING,
				ImVec2(buttonWidth, 32.0f));
			if (ImGui::IsItemClicked()) {
				properties.DEFER_RENDERING = true;
			}

			ImGui::Checkbox("Frustum culling", &properties.FRUSTUM_CULLING);
			if (!properties.DEFER_RENDERING) {
				ImGui::Checkbox(
					"Forward normal buffer",
					&properties.FORWARD_NORMAL_BUFFER);
			}
			else {
				ImGui::Checkbox("SSAO", &properties.SSAO);
				if (properties.SSAO) {
					ImGui::Indent();
					ImGui::DragFloat(
						"Radius##ssao",
						&properties.SSAO_RADIUS,
						0.01f,
						0.05f,
						2.0f,
						"%.3f");
					ImGui::DragFloat(
						"Bias##ssao",
						&properties.SSAO_BIAS,
						0.001f,
						0.0f,
						0.2f,
						"%.4f");
					ImGui::DragInt(
						"Kernel##ssao",
						&properties.SSAO_KERNEL_SIZE,
						1.0f,
						8,
						64);
					ImGui::Unindent();
				}
				ImGui::Checkbox("Light volumes", &properties.LIGHT_VOLUME);
				if (properties.LIGHT_VOLUME) {
					ImGui::Indent();
					ImGui::DragFloat(
						"Radius scale##light_volume",
						&properties.LIGHT_VOLUME_RADIUS_SCALE,
						0.02f,
						0.1f,
						8.0f,
						"%.2f");
					ImGui::DragFloat(
						"Cutoff scale##light_volume",
						&properties.LIGHT_VOLUME_CUTOFF_SCALE,
						0.02f,
						0.05f,
						20.0f,
						"%.2f");
					ImGui::Unindent();
				}
			}
		}

		if (ImGui::CollapsingHeader("Editor & Hot Reload")) {
			ImGui::Checkbox(
				"Auto reload shaders",
				&properties.AUTO_RELOAD_SHADERS);
			ImGui::Checkbox(
				"Auto reload materials",
				&properties.AUTO_RELOAD_MATERIALS);
			ImGui::DragFloat(
				"Poll interval",
				&properties.HOT_RELOAD_POLL_INTERVAL,
				0.05f,
				0.05f,
				2.0f,
				"%.2f s");
			ImGui::Checkbox("Debug mode", &properties.DEBUG_MODE);
		}
	}

	void Gamma_UI() {
		if (!m_rendererPanelVisible ||
			!ImGui::CollapsingHeader(
				"Post Processing",
				ImGuiTreeNodeFlags_DefaultOpen)) {
			return;
		}
		ImGui::Checkbox("HDR output", &properties.USE_HDR);
		if (properties.USE_HDR) {
			ImGui::DragFloat(
				"Exposure",
				&properties.HDR_EXPOSURE,
				0.01f,
				0.01f,
				100.0f,
				"%.2f");
		}
		ImGui::Checkbox(
			"Gamma correction",
			&properties.GAMMA_CORRECTION);
		if (properties.GAMMA_CORRECTION) {
			ImGui::DragFloat(
				"Gamma",
				&properties.GAMMA_VALUE,
				0.01f,
				1.0f,
				2.6f,
				"%.2f");
		}
		ImGui::Checkbox("Bloom", &properties.BLOOM);
		if (properties.BLOOM) {
			ImGui::Indent();
			ImGui::DragFloat(
				"Threshold##bloom",
				&properties.BLOOM_THRESHOLD,
				0.01f,
				0.0f,
				10.0f,
				"%.2f");
			ImGui::DragInt(
				"Blur iterations##bloom",
				&properties.BLOOM_BLUR_ITERATIONS,
				1.0f,
				1,
				20);
			ImGui::Unindent();
		}
	}

	// Scene 面板：可单独停靠到 DockSpace 中
	void Scene_UI(
		Scene& scene,
		Camera& camera,
		EditorSceneManager& sceneManager)
	{
		if (ImGui::Begin("Scene")) {
			DrawSceneBrowserContent(scene, camera, sceneManager);
			ImGui::Separator();
			scene.SetSceneGui();
			DrawSceneReplacementPopup(scene, camera, sceneManager);
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

			// Click-to-pick: sample RGBA/depth from currently displayed framebuffer attachment.
			if (m_viewportReadFBO != 0 &&
				ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
				ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			{
				const ImVec2 imgMin = ImGui::GetItemRectMin();
				const ImVec2 imgMax = ImGui::GetItemRectMax();
				const ImVec2 mouse = ImGui::GetMousePos();
				const float w = imgMax.x - imgMin.x;
				const float h = imgMax.y - imgMin.y;
				if (w > 0.0f && h > 0.0f && m_viewportReadWidth > 0 && m_viewportReadHeight > 0) {
					float u = (mouse.x - imgMin.x) / w;
					float v = (mouse.y - imgMin.y) / h;
					u = (u < 0.0f) ? 0.0f : (u > 1.0f ? 1.0f : u);
					v = (v < 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v);
					const int px = (int)(u * (float)m_viewportReadWidth);
					const int py = (int)(v * (float)m_viewportReadHeight);
					// OpenGL readback origin is bottom-left.
					ReadViewportPixel(px, m_viewportReadHeight - 1 - py);
				}
			}

			if (m_hasPickedPixel) {
				ImGui::Separator();
				ImGui::Text("Picked Pixel: (%d, %d)", m_pickedX, m_pickedY);
				if (m_viewportReadIsDepth) {
					ImGui::Text("Depth: %.6f", m_pickedRGBA[0]);
				}
				else {
					ImGui::Text("RGBA: %.6f, %.6f, %.6f, %.6f",
						m_pickedRGBA[0], m_pickedRGBA[1], m_pickedRGBA[2], m_pickedRGBA[3]);
				}
			}
		}
		else {
			ImGui::TextUnformatted("No viewport texture.");
		}
	}

	void SetViewportReadSource(unsigned int fboID, int attachmentIndex, bool isDepth, int width, int height) {
		m_viewportReadFBO = fboID;
		m_viewportReadAttachment = attachmentIndex;
		m_viewportReadIsDepth = isDepth;
		m_viewportReadWidth = width;
		m_viewportReadHeight = height;
	}

	void Framebuffers_UI() {
		if (!m_rendererPanelVisible ||
			!ImGui::CollapsingHeader("Render Target Debug")) {
			return;
		}
		auto& framebuffersMgr = FramebuffersManager::GetInstance();
		static int selectedOption = FBO::Default_FrameRenderType;
		int optionCount = sizeof(FBO::optionFrame) / sizeof(FBO::optionFrame[0]);
		ImGui::TextDisabled(
			"Inspect intermediate color and depth attachments in the viewport.");
		if (ImGui::Combo("Display type", &selectedOption, FBO::optionFrame, optionCount)) {
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
		// Viewport：默认显示最终渲染结果；下拉先选 FBO（首项为“最终图”），再选该 FBO 的 color/depth 附件
		std::vector<FBO*> busyFBOs = framebuffersMgr.GetBusyFBOs();
		static std::vector<std::string> fboNames;
		static std::vector<const char*> fboNamesPtrs;
		fboNames.clear();
		fboNamesPtrs.clear();
		fboNames.push_back("Final (default)");  // index 0 = 延迟+正向+后处理后的最终图

		// 用 FBO 的 passName 做显示名；若重复则追加数字去重（default1/default2）
		std::unordered_map<std::string, int> totalCountByBaseName;
		for (FBO* fbo : busyFBOs) {
			std::string base = (fbo && !fbo->passName.empty()) ? fbo->passName : "default";
			++totalCountByBaseName[base];
		}
		std::unordered_map<std::string, int> seenCountByBaseName;
		for (size_t i = 0; i < busyFBOs.size(); ++i) {
			FBO* fbo = busyFBOs[i];
			std::string base = (fbo && !fbo->passName.empty()) ? fbo->passName : "default";
			int idx = ++seenCountByBaseName[base];
			if (totalCountByBaseName[base] > 1) {
				fboNames.push_back(base + std::to_string(idx));
			} else {
				fboNames.push_back(base);
			}
		}
		for (const auto& s : fboNames)
			fboNamesPtrs.push_back(s.c_str());
		int nFbo = static_cast<int>(fboNames.size());
		if (properties.VIEWPORT_DEBUG_FBO_INDEX >= nFbo) properties.VIEWPORT_DEBUG_FBO_INDEX = nFbo - 1;
		if (properties.VIEWPORT_DEBUG_FBO_INDEX < 0) properties.VIEWPORT_DEBUG_FBO_INDEX = 0;
		ImGui::Combo("Viewport FBO", &properties.VIEWPORT_DEBUG_FBO_INDEX, fboNamesPtrs.data(), nFbo);
		// 只有选了具体 FBO（非“Final”）时才显示附件下拉
		if (properties.VIEWPORT_DEBUG_FBO_INDEX >= 1 && (size_t)(properties.VIEWPORT_DEBUG_FBO_INDEX - 1) < busyFBOs.size()) {
			FBO* selectedFBO = busyFBOs[properties.VIEWPORT_DEBUG_FBO_INDEX - 1];
			int nAtt = static_cast<int>(selectedFBO->textureIDs.size());
			if (nAtt > 0) {
				static std::vector<std::string> attNames;
				static std::vector<const char*> attNamesPtrs;
				attNames.clear();
				attNamesPtrs.clear();
				for (int i = 0; i < nAtt; ++i)
					attNames.push_back(selectedFBO->attr.isShadowMap && i == 0 ? "Depth" : "Color " + std::to_string(i));
				for (const auto& s : attNames)
					attNamesPtrs.push_back(s.c_str());
				if (properties.VIEWPORT_DEBUG_ATTACHMENT_INDEX >= nAtt) properties.VIEWPORT_DEBUG_ATTACHMENT_INDEX = nAtt - 1;
				if (properties.VIEWPORT_DEBUG_ATTACHMENT_INDEX < 0) properties.VIEWPORT_DEBUG_ATTACHMENT_INDEX = 0;
				ImGui::Combo("Viewport Attachment", &properties.VIEWPORT_DEBUG_ATTACHMENT_INDEX, attNamesPtrs.data(), nAtt);
			}
		}
	}

	void Shadow_UI() {
		if (!m_rendererPanelVisible ||
			!ImGui::CollapsingHeader(
				"Shadows & Cache",
				ImGuiTreeNodeFlags_DefaultOpen)) {
			return;
		}
		int selectedOption = static_cast<int>(properties.SHADOW_TYPE);
		int optionCount = sizeof(ShadowProperty::ShadowTypeStrs) / sizeof(ShadowProperty::ShadowTypeStrs[0]);
		if (ImGui::Combo(
			"Filter",
			&selectedOption,
			ShadowProperty::ShadowTypeStrs,
			optionCount)) {
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
		ImGui::DragInt(
			"Filter samples",
			&properties.SHADOW_PCF_SAMPLE_NUM,
			1.0f,
			1,
			64);

		bool cacheEnabled = !properties.SHADOW_CACHE_DISABLED;
		if (ImGui::Checkbox("Shadow cache enabled", &cacheEnabled)) {
			properties.SHADOW_CACHE_DISABLED = !cacheEnabled;
		}
		ImGui::BeginDisabled(!cacheEnabled);
		ImGui::Checkbox(
			"Per-light dirty cache",
			&properties.SHADOW_PER_LIGHT_CACHE);
		ImGui::BeginDisabled(!properties.SHADOW_PER_LIGHT_CACHE);
		ImGui::Checkbox(
			"Spatial caster cache",
			&properties.SHADOW_SPATIAL_CASTER_CACHE);
		ImGui::Checkbox(
			"Point per-face cache",
			&properties.POINT_SHADOW_PER_FACE_CACHE);
		ImGui::BeginDisabled(
			!properties.POINT_SHADOW_PER_FACE_CACHE);
		ImGui::Checkbox(
			"Force all Point faces (audit)",
			&properties.POINT_SHADOW_FORCE_ALL_FACES_REQUIRED);
		ImGui::EndDisabled();
		ImGui::EndDisabled();
		ImGui::EndDisabled();
		const ImVec4 cacheColor =
			cacheEnabled && properties.SHADOW_PER_LIGHT_CACHE
				? ImVec4(0.31f, 0.78f, 0.59f, 1.0f)
				: ImVec4(0.91f, 0.60f, 0.30f, 1.0f);
		ImGui::TextColored(
			cacheColor,
			"%s",
			!cacheEnabled
				? "CACHE BYPASSED"
				: (properties.SHADOW_PER_LIGHT_CACHE
					? (properties.POINT_SHADOW_PER_FACE_CACHE
						? "PER-LIGHT + POINT FACE CACHE"
						: "PER-LIGHT REVISION CACHE")
					: "GLOBAL REVISION CACHE"));
		ImGui::TextDisabled(
			"Sampling: stable Vogel disk. Timeline panel shows hit/update decisions.");
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
			{
				ImGui::Text("%s (Texture x%d)", propName.c_str(), static_cast<int>(prop.textures.size()));
				for (size_t texIdx = 0; texIdx < prop.textures.size(); ++texIdx) {
					Texture& tex = prop.textures[texIdx];

					ImGui::PushID(static_cast<int>(texIdx));
					ImGui::Text("Current: %s", tex.path.C_Str());
					if (ImGui::Button("Browse...")) {
						std::string selectedPath;
						if (PickTextureFileWithDialog(selectedPath)) {
							std::filesystem::path p(selectedPath);
						std::string file = p.filename().string();
						std::string dir = p.parent_path().string();
						if (dir.empty()) dir = ".";

						if (!file.empty()) {
							Texture newTex{};
							newTex.type = tex.type;
							newTex.path = file.c_str();
							const bool srgb = tex.type == "texture_diffuse" || tex.type == "albedo" ||
								tex.type == "baseColor" || tex.type == "texture_emissive";
							newTex.textureID = TextureFromFile(file.c_str(), dir, false, srgb);
							newTex.textureGammaID = newTex.textureID;
							tex = newTex;
						}
						}
					}
					ImGui::PopID();
				}
				break;
			}
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
				ImGui::PushID(static_cast<int>(i));
				bool active = mesh.GetActiveStatus();
				if (ImGui::Checkbox("Active", &active)) {
					mesh.SetActiveStatus(active);
				}
				ImGui::Separator();
				if (mat) {
					DrawSingleMaterialContent(mat);
				} else {
					ImGui::TextUnformatted("No material.");
				}
				ImGui::PopID();
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
						mgr.ReloadMaterialFile(currentPath);
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
	void UpdateMotionTimelinePreview(
		Scene& scene,
		Camera& camera,
		float deltaSeconds)
	{
		m_motionTimeline.Update(scene, camera, deltaSeconds);
	}

	void RecordMotionTimelineTelemetry(const Scene& scene)
	{
		m_motionTimeline.RecordAfterRender(scene);
	}

	bool IsMotionTimelinePlaying() const
	{
		return m_motionTimeline.IsPlaying();
	}

	void RestoreTemporaryEditorState(Scene& scene, Camera& camera)
	{
		if (m_motionTimeline.IsThreeLightTestRigActive()) {
			m_motionTimeline.RestoreThreeLightTestRig(scene, camera);
		}
		else if (m_motionTimeline.IsCaptured()) {
			m_motionTimeline.StopAndRestore(scene, camera);
		}
	}

	void MotionTimeline_UI(Scene& scene, Camera& camera)
	{
		if (!ImGui::Begin("Motion Timeline")) {
			ImGui::End();
			return;
		}

		ImGui::TextColored(
			ImVec4(0.88f, 0.91f, 0.95f, 1.0f),
			"Motion Timeline");
		ImGui::SameLine();
		ImGui::TextDisabled(
			"Deterministic preview for per-light shadow-cache workloads");
		ImGui::Separator();

		const BenchmarkMotionProfile profile =
			m_motionTimeline.GetProfile();
		const std::uint32_t trackMask =
			BenchmarkMotionTimeline::TrackMask(profile);
		DrawTrackBadge(
			"POINT LIGHT",
			BenchmarkMotionTimeline::HasTrack(
				trackMask,
				BenchmarkMotionTrack::Point),
			ImVec4(0.93f, 0.68f, 0.30f, 1.0f));
		ImGui::SameLine();
		DrawTrackBadge(
			"CASTER",
			BenchmarkMotionTimeline::HasTrack(
				trackMask,
				BenchmarkMotionTrack::Caster),
			ImVec4(0.38f, 0.78f, 0.61f, 1.0f));
		ImGui::SameLine();
		DrawTrackBadge(
			"CAMERA",
			BenchmarkMotionTimeline::HasTrack(
				trackMask,
				BenchmarkMotionTrack::Camera),
			ImVec4(0.48f, 0.64f, 0.96f, 1.0f));

		ImGui::SameLine();
		const bool captured = m_motionTimeline.IsCaptured();
		const bool playing = m_motionTimeline.IsPlaying();
		ImGui::TextColored(
			playing
				? ImVec4(0.38f, 0.82f, 0.62f, 1.0f)
				: (captured
					? ImVec4(0.93f, 0.72f, 0.34f, 1.0f)
					: ImVec4(0.52f, 0.56f, 0.62f, 1.0f)),
			"  %s",
			playing ? "PLAYING" : (captured ? "READY" : "NOT CAPTURED"));

		ImGui::Spacing();
		if (ImGui::BeginTable(
			"motion_timeline_layout",
			2,
			ImGuiTableFlags_Resizable |
				ImGuiTableFlags_BordersInnerV |
				ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn(
				"Controls",
				ImGuiTableColumnFlags_WidthStretch,
				0.85f);
			ImGui::TableSetupColumn(
				"Telemetry",
				ImGuiTableColumnFlags_WidthStretch,
				1.15f);
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);

			ImGui::SeparatorText("Quick A/B/C Reproduction");
			bool testRigActive =
				m_motionTimeline.IsThreeLightTestRigActive();
			bool testRigReady =
				m_motionTimeline.IsThreeLightTestRigReady(scene);
			if (ImGui::Button(
				testRigActive
					? "Restore original lighting"
					: "Prepare 3-light test",
				ImVec2(-1.0f, 30.0f))) {
				if (testRigActive) {
					m_motionTimeline.RestoreThreeLightTestRig(
						scene,
						camera);
				}
				else {
					m_motionTimeline.PrepareThreeLightTestRig(
						scene,
						camera);
				}
				testRigActive =
					m_motionTimeline.IsThreeLightTestRigActive();
				testRigReady =
					m_motionTimeline.IsThreeLightTestRigReady(scene);
			}

			const float comparisonGap =
				ImGui::GetStyle().ItemSpacing.x;
			const float comparisonButtonWidth =
				(ImGui::GetContentRegionAvail().x -
					comparisonGap * 2.0f) /
				3.0f;
			const bool noCacheMode =
				properties.SHADOW_CACHE_DISABLED;
			const bool perLightMode =
				!properties.SHADOW_CACHE_DISABLED &&
				properties.SHADOW_PER_LIGHT_CACHE &&
				!properties.POINT_SHADOW_PER_FACE_CACHE;
			const bool perFaceMode =
				!properties.SHADOW_CACHE_DISABLED &&
				properties.SHADOW_PER_LIGHT_CACHE &&
				properties.POINT_SHADOW_PER_FACE_CACHE;
			DrawSelectionButton(
				"A  Cache off",
				noCacheMode,
				ImVec2(comparisonButtonWidth, 30.0f));
			if (ImGui::IsItemClicked()) {
				m_motionTimeline.SetShadowComparisonMode(
					scene,
					EditorMotionTimelineController::
						ShadowComparisonMode::GlobalDirty);
			}
			ImGui::SameLine();
			DrawSelectionButton(
				"B  Per-light cache",
				perLightMode,
				ImVec2(comparisonButtonWidth, 30.0f));
			if (ImGui::IsItemClicked()) {
				m_motionTimeline.SetShadowComparisonMode(
					scene,
					EditorMotionTimelineController::
						ShadowComparisonMode::PerLight);
			}
			ImGui::SameLine();
			DrawSelectionButton(
				"C  Per-face cache",
				perFaceMode,
				ImVec2(comparisonButtonWidth, 30.0f));
			if (ImGui::IsItemClicked()) {
				m_motionTimeline.SetShadowComparisonMode(
					scene,
					EditorMotionTimelineController::
						ShadowComparisonMode::PerFace);
			}

			const auto countShadowLights = [](const auto& lights) {
				return static_cast<std::size_t>(std::count_if(
					lights.begin(),
					lights.end(),
					[](const auto& light) {
						return light.m_active && light.useShadowMap;
					}));
			};
			const std::size_t directionalShadowCount =
				countShadowLights(scene.lightSource.directionLights);
			const std::size_t pointShadowCount =
				countShadowLights(scene.lightSource.pointLights);
			const std::size_t spotShadowCount =
				countShadowLights(scene.lightSource.spotLights);
			ImGui::TextColored(
				testRigReady
					? ImVec4(0.35f, 0.80f, 0.59f, 1.0f)
					: ImVec4(0.94f, 0.69f, 0.31f, 1.0f),
				testRigReady ? "READY" : "SCENE NOT READY");
			ImGui::SameLine();
			ImGui::TextDisabled(
				"  shadow lights  D:%llu  P:%llu  S:%llu",
				static_cast<unsigned long long>(
					directionalShadowCount),
				static_cast<unsigned long long>(pointShadowCount),
				static_cast<unsigned long long>(spotShadowCount));
			if (testRigReady) {
				ImGui::TextWrapped(
					"Select Cache 3-way phases for the formal trajectory: "
					"Point + Camera, Local Caster + Camera, then Camera-only. "
					"A redraws globally; B isolates lights; C also reuses "
					"unchanged Point faces.");
			}
			else {
				ImGui::TextWrapped(
					"Use Prepare so the preview has exactly one shadow-casting "
					"Directional, Point, and Spot light.");
			}
			ImGui::PushStyleColor(
				ImGuiCol_Text,
				ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::TextWrapped(
				"%s",
				m_motionTimeline.GetTestRigStatusText().c_str());
			ImGui::PopStyleColor();
			ImGui::Spacing();

			ImGui::SeparatorText("Workload");
			static const char* profileNames[] = {
				"Point light",
				"Shadow caster",
				"Camera control",
				"Point + camera",
				"Mixed tracks",
				"Cache 3-way phases"
			};
			int profileIndex =
				(std::max)(
					0,
					static_cast<int>(profile) -
						static_cast<int>(BenchmarkMotionProfile::Point));
			if (ImGui::Combo(
				"Profile",
				&profileIndex,
				profileNames,
				IM_ARRAYSIZE(profileNames))) {
				m_motionTimeline.ChangeProfile(
					scene,
					camera,
					static_cast<BenchmarkMotionProfile>(
						static_cast<int>(BenchmarkMotionProfile::Point) +
						profileIndex));
			}

			const bool pointTrackActive =
				BenchmarkMotionTimeline::HasTrack(
					trackMask,
					BenchmarkMotionTrack::Point);
			if (pointTrackActive &&
				!scene.lightSource.pointLights.empty()) {
				const int selectedPoint =
					(std::min)(
						m_motionTimeline.GetPointLightIndex(),
						static_cast<int>(
							scene.lightSource.pointLights.size()) - 1);
				const std::string preview =
					"Point Light " + std::to_string(selectedPoint);
				if (ImGui::BeginCombo(
					"Point target",
					preview.c_str())) {
					for (std::size_t index = 0;
						index < scene.lightSource.pointLights.size();
						++index) {
						const bool selected =
							static_cast<int>(index) == selectedPoint;
						const std::string label =
							"Point Light " + std::to_string(index);
						if (ImGui::Selectable(label.c_str(), selected)) {
							m_motionTimeline.ChangePointLightIndex(
								scene,
								camera,
								static_cast<int>(index));
						}
						if (selected) {
							ImGui::SetItemDefaultFocus();
						}
					}
					ImGui::EndCombo();
				}
				PointLight& selectedLight =
					scene.lightSource.pointLights[
						static_cast<std::size_t>(selectedPoint)];
				if (!selectedLight.useShadowMap) {
					ImGui::TextColored(
						ImVec4(0.94f, 0.69f, 0.31f, 1.0f),
						"Target shadow is disabled.");
					ImGui::SameLine();
					if (ImGui::SmallButton("Enable##timeline_point_shadow")) {
						selectedLight.useShadowMap = true;
					}
				}
			}
			else if (pointTrackActive) {
				ImGui::TextColored(
					ImVec4(0.94f, 0.48f, 0.40f, 1.0f),
					"No point light is available.");
			}

			const auto& models = scene.modelSource.GetModels();
			const bool casterTrackActive =
				BenchmarkMotionTimeline::HasTrack(
					trackMask,
					BenchmarkMotionTrack::Caster);
			if (casterTrackActive && !models.empty()) {
				const int selectedCaster =
					(std::min)(
						m_motionTimeline.GetCasterIndex(),
						static_cast<int>(models.size()) - 1);
				const std::string casterName =
					models[static_cast<std::size_t>(selectedCaster)]
						? models[static_cast<std::size_t>(selectedCaster)]
							->GetName()
						: std::string("Missing model");
				if (ImGui::BeginCombo(
					"Caster target",
					casterName.c_str())) {
					for (std::size_t index = 0;
						index < models.size();
						++index) {
						const bool selected =
							static_cast<int>(index) == selectedCaster;
						const std::string label =
							models[index]
								? models[index]->GetName()
								: ("Model " + std::to_string(index));
						if (ImGui::Selectable(label.c_str(), selected)) {
							m_motionTimeline.ChangeCasterIndex(
								scene,
								camera,
								static_cast<int>(index));
						}
						if (selected) {
							ImGui::SetItemDefaultFocus();
						}
					}
					ImGui::EndCombo();
				}
			}
			else if (casterTrackActive) {
				ImGui::TextDisabled("No caster model is available.");
			}

			BenchmarkMotionTimelineConfig config =
				m_motionTimeline.GetConfig();
			if (ImGui::CollapsingHeader("Timing & Scale")) {
				int fixedFps = config.fixedFramesPerSecond;
				if (ImGui::DragInt(
					"Fixed rate",
					&fixedFps,
					1.0f,
					1,
					240,
					"%d FPS")) {
					m_motionTimeline.SetFixedFramesPerSecond(
						scene,
						camera,
						fixedFps);
				}
				int cycleFrames = config.cycleFrames;
				if (ImGui::DragInt(
					"Cycle",
					&cycleFrames,
					1.0f,
					2,
					3600,
					"%d frames")) {
					m_motionTimeline.SetCycleFrames(
						scene,
						camera,
						cycleFrames);
				}
				float sceneRadius = config.sceneRadius;
				if (ImGui::DragFloat(
					"Scene radius",
					&sceneRadius,
					0.05f,
					0.01f,
					100000.0f,
					"%.2f")) {
					m_motionTimeline.SetSceneRadius(
						scene,
						camera,
						sceneRadius);
				}
				ImGui::SameLine();
				if (ImGui::SmallButton("Estimate")) {
					m_motionTimeline.UseEstimatedSceneRadius(scene, camera);
				}
				float playbackSpeed = m_motionTimeline.GetPlaybackSpeed();
				if (ImGui::SliderFloat(
					"Playback",
					&playbackSpeed,
					0.1f,
					4.0f,
					"%.1fx")) {
					m_motionTimeline.SetPlaybackSpeed(playbackSpeed);
				}
				bool looping = m_motionTimeline.IsLooping();
				if (ImGui::Checkbox("Loop cycle", &looping)) {
					m_motionTimeline.SetLooping(looping);
				}
			}
			else {
				ImGui::TextDisabled(
					"%d FPS  /  %d frames  /  radius %.2f",
					config.fixedFramesPerSecond,
					config.cycleFrames,
					config.sceneRadius);
			}

			ImGui::SeparatorText("Transport");
			if (ImGui::Button("Capture base", ImVec2(112.0f, 30.0f))) {
				m_motionTimeline.CaptureBaseState(scene, camera);
			}
			ImGui::SameLine();
			if (ImGui::Button(
				playing ? "Pause" : "Play",
				ImVec2(72.0f, 30.0f))) {
				if (playing) {
					m_motionTimeline.Pause();
				}
				else {
					m_motionTimeline.Play(scene, camera);
				}
			}
			ImGui::SameLine();
			if (ImGui::Button("Restore", ImVec2(72.0f, 30.0f))) {
				m_motionTimeline.StopAndRestore(scene, camera);
			}

			if (ImGui::Button("|<##timeline_start")) {
				m_motionTimeline.ResetToStart(scene, camera);
			}
			ImGui::SameLine();
			if (ImGui::Button("<##timeline_previous")) {
				m_motionTimeline.SetFrame(
					scene,
					camera,
					m_motionTimeline.GetFrame() - 1);
			}
			ImGui::SameLine();
			if (ImGui::Button(">##timeline_next")) {
				m_motionTimeline.SetFrame(
					scene,
					camera,
					m_motionTimeline.GetFrame() + 1);
			}

			int timelineFrame = m_motionTimeline.GetFrame();
			if (ImGui::SliderInt(
				"Frame",
				&timelineFrame,
				0,
				(std::max)(
					1,
					m_motionTimeline.GetConfig().cycleFrames - 1))) {
				m_motionTimeline.SetFrame(
					scene,
					camera,
					timelineFrame);
			}
			const double durationSeconds =
				static_cast<double>(
					m_motionTimeline.GetConfig().cycleFrames) /
				static_cast<double>(
					m_motionTimeline.GetConfig().fixedFramesPerSecond);
			ImGui::TextDisabled(
				"Frame %d / %d  |  %.2f s cycle",
				m_motionTimeline.GetFrame(),
				m_motionTimeline.GetConfig().cycleFrames - 1,
				durationSeconds);
			DrawTimelineTrajectoryPreview(m_motionTimeline);

			ImGui::TableSetColumnIndex(1);
			ImGui::SeparatorText("Shadow Cache Account");
			const EditorMotionFrameTelemetry& currentTelemetry =
				m_motionTimeline.GetLatestTelemetry();
			const EditorMotionFrameTelemetry& telemetry =
				m_motionTimeline.GetLatestMotionStepTelemetry();
			const bool hasMotionStep = telemetry.motionSampleApplied;
			const char* decision = hasMotionStep
				? "NO SHADOW WORK"
				: "WAITING FOR MOTION STEP";
			ImVec4 decisionColor(0.55f, 0.58f, 0.64f, 1.0f);
			if (hasMotionStep &&
				(telemetry.shadowResourceFailureCount > 0 ||
				telemetry.conservativeShadowFallbackCount > 0)) {
				decision = "CONSERVATIVE FALLBACK";
				decisionColor = ImVec4(0.95f, 0.40f, 0.36f, 1.0f);
			}
			else if (hasMotionStep &&
				telemetry.updatedLightCount > 0) {
				decision = "CACHE MISS  /  SHADOW REDRAW";
				decisionColor = ImVec4(0.94f, 0.69f, 0.31f, 1.0f);
			}
			else if (hasMotionStep &&
				(telemetry.lightCacheHitCount > 0 ||
				telemetry.cacheHitCount > 0)) {
				decision = "CACHE HIT  /  SHADOW MAP REUSED";
				decisionColor = ImVec4(0.35f, 0.80f, 0.59f, 1.0f);
			}
			ImGui::TextColored(decisionColor, "%s", decision);
			ImGui::SameLine();
			ImGui::TextDisabled(
				"  %s",
				properties.SHADOW_CACHE_DISABLED
					? "cache disabled"
					: (properties.SHADOW_PER_LIGHT_CACHE
						? (properties.POINT_SHADOW_PER_FACE_CACHE
							? "per-light + point per-face cache"
							: "per-light cache")
						: "global cache"));

			char updatedLightsValue[32] = {};
			char cacheHitsValue[32] = {};
			char submissionsValue[32] = {};
			char shadowCpuValue[32] = {};
			std::snprintf(
				updatedLightsValue,
				sizeof(updatedLightsValue),
				"%llu",
				static_cast<unsigned long long>(
					telemetry.updatedLightCount));
			std::snprintf(
				cacheHitsValue,
				sizeof(cacheHitsValue),
				"%llu",
				static_cast<unsigned long long>(
					telemetry.lightCacheHitCount));
			std::snprintf(
				submissionsValue,
				sizeof(submissionsValue),
				"%llu",
				static_cast<unsigned long long>(
					telemetry.pointShadowSubmissionPassCount));
			std::snprintf(
				shadowCpuValue,
				sizeof(shadowCpuValue),
				"%.3f ms",
				telemetry.shadowUpdateCpuMilliseconds);
			const float telemetryGap = ImGui::GetStyle().ItemSpacing.x;
			const float telemetryCardWidth =
				(std::max)(
					104.0f,
					(ImGui::GetContentRegionAvail().x - telemetryGap) *
						0.5f);
			DrawMetricCard(
				"timeline_updated_lights",
				"UPDATED LIGHTS",
				updatedLightsValue,
				ImVec4(0.94f, 0.69f, 0.31f, 1.0f),
				telemetryCardWidth);
			ImGui::SameLine();
			DrawMetricCard(
				"timeline_cache_hits",
				"CACHE HITS",
				cacheHitsValue,
				ImVec4(0.35f, 0.80f, 0.59f, 1.0f),
				telemetryCardWidth);
			DrawMetricCard(
				"timeline_point_submissions",
				"POINT SUBMITS",
				submissionsValue,
				ImVec4(0.48f, 0.64f, 0.96f, 1.0f),
				telemetryCardWidth);
			ImGui::SameLine();
			DrawMetricCard(
				"timeline_shadow_cpu",
				"SHADOW CPU",
				shadowCpuValue,
				ImVec4(0.83f, 0.50f, 0.91f, 1.0f),
				telemetryCardWidth);

			if (ImGui::BeginTable(
				"timeline_light_breakdown",
				4,
				ImGuiTableFlags_RowBg |
					ImGuiTableFlags_BordersInnerH |
					ImGuiTableFlags_SizingStretchSame)) {
				ImGui::TableSetupColumn("Directional");
				ImGui::TableSetupColumn("Point");
				ImGui::TableSetupColumn("Spot");
				ImGui::TableSetupColumn("Submits");
				ImGui::TableHeadersRow();
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::Text(
					"%llu",
					static_cast<unsigned long long>(
						telemetry.directionalLightUpdateCount));
				ImGui::TableSetColumnIndex(1);
				ImGui::Text(
					"%llu",
					static_cast<unsigned long long>(
						telemetry.pointLightUpdateCount));
				ImGui::TableSetColumnIndex(2);
				ImGui::Text(
					"%llu",
					static_cast<unsigned long long>(
						telemetry.spotLightUpdateCount));
				ImGui::TableSetColumnIndex(3);
				ImGui::Text(
					"%llu",
					static_cast<unsigned long long>(
						telemetry.pointShadowSubmissionPassCount));
				ImGui::EndTable();
			}
			if (ImGui::BeginTable(
				"timeline_point_face_breakdown",
				4,
				ImGuiTableFlags_RowBg |
					ImGuiTableFlags_BordersInnerH |
					ImGuiTableFlags_SizingStretchSame)) {
				ImGui::TableSetupColumn("Required");
				ImGui::TableSetupColumn("Rendered");
				ImGui::TableSetupColumn("Face hits");
				ImGui::TableSetupColumn("Deferred");
				ImGui::TableHeadersRow();
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::Text(
					"%llu",
					static_cast<unsigned long long>(
						telemetry.pointShadowRequiredFaceCount));
				ImGui::TableSetColumnIndex(1);
				ImGui::Text(
					"%llu",
					static_cast<unsigned long long>(
						telemetry.pointShadowRenderedFaceCount));
				ImGui::TableSetColumnIndex(2);
				ImGui::Text(
					"%llu",
					static_cast<unsigned long long>(
						telemetry.pointShadowFaceCacheHitCount));
				ImGui::TableSetColumnIndex(3);
				ImGui::Text(
					"%llu",
					static_cast<unsigned long long>(
						telemetry.pointShadowDeferredFaceCount));
				ImGui::EndTable();
			}
			ImGui::TextDisabled(
				"Current render: %llu updated light(s), %llu light cache hit(s)",
				static_cast<unsigned long long>(
					currentTelemetry.updatedLightCount),
				static_cast<unsigned long long>(
					currentTelemetry.lightCacheHitCount));

			DrawTimelineHistoryPlot(
				"Shadow update CPU (ms)",
				m_motionTimeline.GetShadowCpuHistory(),
				ImVec4(0.83f, 0.50f, 0.91f, 1.0f),
				0.25f);
			DrawTimelineHistoryPlot(
				"Updated shadow lights",
				m_motionTimeline.GetUpdatedLightHistory(),
				ImVec4(0.94f, 0.69f, 0.31f, 1.0f),
				3.0f);

			ImGui::Spacing();
			ImGui::TextWrapped(
				"%s",
				m_motionTimeline.GetStatusText().c_str());
			ImGui::TextDisabled(
				"Preview is diagnostic only. Use "
				"tools/Test-PointShadowCache3Way.ps1 for isolated "
				"1920x1080 A/B/C measurements.");
			ImGui::EndTable();
		}

		ImGui::End();
	}

	void Viewport_UI(unsigned int textureID) {
		ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_MenuBar);
		ViewportContent(textureID);
		ImGui::End();
	}
private:
	enum class SceneUiReplacementAction {
		None,
		NewScene,
		OpenScene,
		LoadClassicScene
	};

	void QueueSceneReplacement(
		SceneUiReplacementAction action,
		const std::string& path = std::string(),
		std::size_t classicSceneIndex = 0,
		const ClassicSceneLoadOptions& options = {})
	{
		m_sceneReplacementAction = action;
		m_sceneReplacementPath = path;
		m_sceneReplacementClassicIndex = classicSceneIndex;
		m_sceneReplacementOptions = options;
		ImGui::OpenPopup("Replace Current Scene##scene_browser_replace");
	}

	void ExecuteSceneReplacement(EditorSceneManager& sceneManager)
	{
		switch (m_sceneReplacementAction) {
		case SceneUiReplacementAction::NewScene:
			sceneManager.RequestNewScene();
			break;
		case SceneUiReplacementAction::OpenScene:
			sceneManager.RequestOpenScene(m_sceneReplacementPath);
			break;
		case SceneUiReplacementAction::LoadClassicScene:
			sceneManager.RequestLoadClassicScene(
				m_sceneReplacementClassicIndex,
				m_sceneReplacementOptions);
			break;
		default:
			break;
		}
		m_sceneReplacementAction = SceneUiReplacementAction::None;
		m_sceneReplacementPath.clear();
		ImGui::CloseCurrentPopup();
	}

	void DrawSceneBrowserContent(
		Scene& scene,
		Camera& camera,
		EditorSceneManager& sceneManager)
	{
		if (!ImGui::CollapsingHeader(
			"Scene Browser",
			ImGuiTreeNodeFlags_DefaultOpen)) {
			return;
		}

		ImGui::TextUnformatted("Current");
		ImGui::SameLine();
		ImGui::TextColored(
			ImVec4(0.35f, 0.85f, 0.78f, 1.0f),
			"%s",
			sceneManager.GetCurrentSceneName().c_str());
		if (!sceneManager.GetCurrentDocumentPath().empty()) {
			ImGui::TextDisabled(
				"%s",
				sceneManager.GetCurrentDocumentPath().c_str());
		}
		ImGui::TextDisabled(
			"%llu model(s), %llu light(s)",
			static_cast<unsigned long long>(
				scene.modelSource.GetModels().size()),
			static_cast<unsigned long long>(
				scene.lightSource.pointLights.size() +
				scene.lightSource.directionLights.size() +
				scene.lightSource.spotLights.size()));

		const bool busy = sceneManager.IsBusy();
		const bool temporaryTestRigActive =
			m_motionTimeline.IsThreeLightTestRigActive();
		ImGui::BeginDisabled(busy);
		if (ImGui::Button("New")) {
			QueueSceneReplacement(SceneUiReplacementAction::NewScene);
		}
		ImGui::SameLine();
		if (ImGui::Button("Open...")) {
			std::string path;
			if (PickSceneFileWithDialog(path, false)) {
				QueueSceneReplacement(
					SceneUiReplacementAction::OpenScene,
					path);
			}
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(temporaryTestRigActive);
		if (ImGui::Button("Save")) {
			if (sceneManager.GetCurrentDocumentPath().empty()) {
				std::string path;
				if (PickSceneFileWithDialog(path, true)) {
					sceneManager.SaveAs(scene, camera, path);
				}
			}
			else {
				sceneManager.SaveCurrent(scene, camera);
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Save As...")) {
			std::string path;
			if (PickSceneFileWithDialog(path, true)) {
				sceneManager.SaveAs(scene, camera, path);
			}
		}
		ImGui::EndDisabled();
		ImGui::EndDisabled();
		if (temporaryTestRigActive) {
			ImGui::TextColored(
				ImVec4(0.94f, 0.69f, 0.31f, 1.0f),
				"Restore the temporary three-light test before saving.");
		}

		if (SceneStateIO::HasPendingAsyncLoads()) {
			const int pending = SceneStateIO::GetPendingAsyncLoadCount();
			const int total = SceneStateIO::GetTotalAsyncLoadCount();
			const int completed = total >= pending ? total - pending : 0;
			const float progress = total > 0
				? static_cast<float>(completed) / static_cast<float>(total)
				: 0.0f;
			ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f));
		}
		if (!sceneManager.GetStatusText().empty()) {
			ImVec4 statusColor(0.75f, 0.78f, 0.82f, 1.0f);
			switch (sceneManager.GetStatusKind()) {
			case EditorSceneManager::StatusKind::Success:
				statusColor = ImVec4(0.35f, 0.85f, 0.48f, 1.0f);
				break;
			case EditorSceneManager::StatusKind::Warning:
				statusColor = ImVec4(1.0f, 0.78f, 0.25f, 1.0f);
				break;
			case EditorSceneManager::StatusKind::Error:
				statusColor = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
				break;
			default:
				break;
			}
			ImGui::PushStyleColor(ImGuiCol_Text, statusColor);
			ImGui::TextWrapped(
				"%s",
				sceneManager.GetStatusText().c_str());
			ImGui::PopStyleColor();
			if (ImGui::IsItemHovered()) {
				ImGui::BeginTooltip();
				ImGui::TextColored(
					statusColor,
					"%s",
					sceneManager.GetStatusText().c_str());
				ImGui::EndTooltip();
			}
		}

		ImGui::SeparatorText("Classic Test Scenes");
		const auto& classicScenes = sceneManager.GetClassicScenes();
		if (classicScenes.empty()) {
			ImGui::TextDisabled("No classic scenes found.");
			if (ImGui::Button("Reload Catalog")) {
				sceneManager.ReloadClassicSceneCatalog();
			}
			return;
		}

		if (m_selectedClassicScene >= classicScenes.size()) {
			m_selectedClassicScene = 0;
		}
		const ClassicSceneDescriptor& selectedScene =
			classicScenes[m_selectedClassicScene];
		if (ImGui::BeginCombo(
			"Scene",
			selectedScene.displayName.c_str())) {
			for (std::size_t index = 0;
				index < classicScenes.size();
				++index) {
				const bool selected = index == m_selectedClassicScene;
				std::string label = classicScenes[index].displayName;
				if (!classicScenes[index].available) {
					label += " (missing)";
				}
				if (ImGui::Selectable(label.c_str(), selected)) {
					m_selectedClassicScene = index;
				}
				if (selected) {
					ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}

		const ClassicSceneDescriptor& details =
			classicScenes[m_selectedClassicScene];
		ImGui::TextWrapped("%s", details.category.c_str());
		ImGui::Text(
			"Expected triangles: %llu",
			static_cast<unsigned long long>(details.expectedTriangles));
		ImGui::TextColored(
			details.available
				? ImVec4(0.35f, 0.85f, 0.48f, 1.0f)
				: ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
			details.available ? "Installed" : "Asset missing");
		if (ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			ImGui::TextWrapped("%s", details.modelPath.c_str());
			ImGui::EndTooltip();
		}

		const char* renderPresets[] = {
			"PBR Forward",
			"Phong Forward",
			"PBR Deferred",
			"Phong Deferred"
		};
		ImGui::Combo(
			"Render preset",
			&m_sceneRenderPreset,
			renderPresets,
			IM_ARRAYSIZE(renderPresets));
		ImGui::Checkbox(
			"Directional shadows",
			&m_sceneDirectionalShadows);

		ImGui::BeginDisabled(busy || !details.available);
		if (ImGui::Button("Load Selected Scene", ImVec2(-1.0f, 0.0f))) {
			ClassicSceneLoadOptions options;
			options.renderPreset =
				static_cast<ClassicSceneRenderPreset>(m_sceneRenderPreset);
			options.enableDirectionalShadows =
				m_sceneDirectionalShadows;
			QueueSceneReplacement(
				SceneUiReplacementAction::LoadClassicScene,
				std::string(),
				m_selectedClassicScene,
				options);
		}
		ImGui::EndDisabled();

		if (ImGui::SmallButton("Refresh installed scenes")) {
			sceneManager.RefreshClassicSceneAvailability();
		}
		if (ImGui::TreeNode("Credits and license")) {
			ImGui::TextWrapped("License: %s", details.license.c_str());
			ImGui::TextWrapped("Credit: %s", details.credit.c_str());
			ImGui::TreePop();
		}
	}

	void DrawSceneReplacementPopup(
		Scene& scene,
		Camera& camera,
		EditorSceneManager& sceneManager)
	{
		if (!ImGui::BeginPopupModal(
			"Replace Current Scene##scene_browser_replace",
			nullptr,
			ImGuiWindowFlags_AlwaysAutoResize)) {
			return;
		}

		ImGui::TextUnformatted(
			"This replaces the current models, lights, and camera.");
		ImGui::TextUnformatted("Save the current scene first?");
		ImGui::Separator();

		if (ImGui::Button("Save and Continue")) {
			if (m_motionTimeline.IsThreeLightTestRigActive()) {
				m_motionTimeline.RestoreThreeLightTestRig(scene, camera);
			}
			bool saved = false;
			if (sceneManager.GetCurrentDocumentPath().empty()) {
				std::string path;
				if (PickSceneFileWithDialog(path, true)) {
					saved = sceneManager.SaveAs(scene, camera, path);
				}
			}
			else {
				saved = sceneManager.SaveCurrent(scene, camera);
			}
			if (saved) {
				ExecuteSceneReplacement(sceneManager);
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Continue Without Saving")) {
			if (m_motionTimeline.IsThreeLightTestRigActive()) {
				m_motionTimeline.RestoreThreeLightTestRig(scene, camera);
			}
			ExecuteSceneReplacement(sceneManager);
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) {
			m_sceneReplacementAction = SceneUiReplacementAction::None;
			m_sceneReplacementPath.clear();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	struct AssetBrowserNode {
		std::string path;
		std::string name;
		bool isDirectory = false;
		bool childrenLoaded = false;
		std::vector<AssetBrowserNode> children;
	};

	struct AssetBrowserCategory {
		std::string label;
		std::string rootPath;
		std::string category;
		std::vector<std::string> extensions;
		bool rootChecked = false;
		bool rootExists = false;
		AssetBrowserNode root;
	};

	void InitializeAssetBrowserCache() {
		if (m_assetBrowserCacheInitialized) {
			return;
		}

		m_assetBrowserCategories = {{
			{ "Models", "models", "Model", { ".obj", ".fbx", ".gltf", ".glb" } },
			{ "Materials", "materials", "Material", { ".xml", ".jpg", ".png", ".hdr" } },
			{ "Shaders", "shaders", "Shader", { ".vs", ".fs", ".gs", ".vert", ".frag" } }
		}};
		for (auto& category : m_assetBrowserCategories) {
			category.root.path = category.rootPath;
			category.root.name = category.label;
			category.root.isDirectory = true;
		}
		m_assetBrowserCacheInitialized = true;
	}

	void ResetAssetBrowserCache() {
		m_assetBrowserCategories = {};
		m_assetBrowserCacheInitialized = false;
	}

	bool EnsureAssetBrowserRoot(AssetBrowserCategory& category) {
		if (category.rootChecked) {
			return category.rootExists;
		}

		PerformanceProfiler::GetInstance().RecordFileSystemCheck();
		std::error_code error;
		category.rootExists = std::filesystem::is_directory(category.rootPath, error);
		category.rootChecked = true;
		return category.rootExists;
	}

	bool MatchesAssetExtension(
		const std::filesystem::path& path,
		const std::vector<std::string>& extensions) const {
		if (extensions.empty()) {
			return true;
		}

		const std::string extension = path.extension().string();
		for (const auto& candidate : extensions) {
			if (_stricmp(extension.c_str(), candidate.c_str()) == 0) {
				return true;
			}
		}
		return false;
	}

	void LoadAssetBrowserChildren(
		AssetBrowserNode& node,
		const std::vector<std::string>& extensions) {
		if (node.childrenLoaded) {
			return;
		}

		node.childrenLoaded = true;
		PerformanceProfiler::GetInstance().RecordFileSystemCheck();
		std::error_code error;
		std::filesystem::directory_iterator iterator(node.path, error);
		const std::filesystem::directory_iterator end;
		while (!error && iterator != end) {
			const auto& entry = *iterator;
			PerformanceProfiler::GetInstance().RecordFileSystemCheck();
			std::error_code entryError;
			const bool isDirectory = entry.is_directory(entryError);
			const auto path = entry.path();
			if (!entryError && (isDirectory || MatchesAssetExtension(path, extensions))) {
				AssetBrowserNode child;
				child.path = path.string();
				child.name = path.filename().string();
				child.isDirectory = isDirectory;
				node.children.push_back(std::move(child));
			}
			iterator.increment(error);
		}
	}

	void DrawAssetBrowserChildren(
		AssetBrowserNode& node,
		const AssetBrowserCategory& category,
		std::string& selectedPath,
		std::string& selectedCategory) {
		LoadAssetBrowserChildren(node, category.extensions);
		for (auto& child : node.children) {
			ImGui::PushID(child.path.c_str());
			if (child.isDirectory) {
				if (ImGui::TreeNode(child.name.c_str())) {
					DrawAssetBrowserChildren(child, category, selectedPath, selectedCategory);
					ImGui::TreePop();
				}
			}
			else {
				const bool isSelected = child.path == selectedPath;
				if (ImGui::Selectable(child.name.c_str(), isSelected)) {
					selectedPath = child.path;
					selectedCategory = category.category;
				}
			}
			ImGui::PopID();
		}
	}

	void DrawAssetBrowserCategory(
		AssetBrowserCategory& category,
		std::string& selectedPath,
		std::string& selectedCategory) {
		if (!EnsureAssetBrowserRoot(category)) {
			return;
		}

		ImGui::PushID(category.rootPath.c_str());
		if (ImGui::TreeNode(category.label.c_str())) {
			DrawAssetBrowserChildren(category.root, category, selectedPath, selectedCategory);
			ImGui::TreePop();
		}
		ImGui::PopID();
	}

	void DrawEditorToolbar() {
		ImGui::PushStyleColor(
			ImGuiCol_ChildBg,
			ImVec4(0.065f, 0.075f, 0.095f, 1.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 7.0f));
		ImGui::BeginChild(
			"##editor_toolbar",
			ImVec2(0.0f, 44.0f),
			false,
			ImGuiWindowFlags_NoScrollbar |
				ImGuiWindowFlags_NoScrollWithMouse);
		ImGui::PopStyleVar();
		ImGui::PopStyleColor();

		ImGui::TextColored(
			ImVec4(0.34f, 0.82f, 0.75f, 1.0f),
			"OPENGL LEARN");
		ImGui::SameLine();
		ImGui::TextDisabled("/ RENDER LAB");

		const auto& profiler = PerformanceProfiler::GetInstance();
		const auto& summary = profiler.GetFrameSummary();
		const double averageFps = summary.averageCpuFrameMs > 0.0
			? 1000.0 / summary.averageCpuFrameMs
			: 0.0;
		const float rightSectionWidth = 575.0f;
		if (ImGui::GetContentRegionAvail().x > rightSectionWidth) {
			ImGui::SameLine(
				ImGui::GetWindowContentRegionMax().x -
				rightSectionWidth);
		}
		else {
			ImGui::SameLine();
		}

		DrawTrackBadge(
			properties.DEFER_RENDERING ? "DEFERRED" : "FORWARD",
			true,
			ImVec4(0.40f, 0.62f, 0.96f, 1.0f));
		ImGui::SameLine();
		ImGui::TextDisabled("M: camera look");
		ImGui::SameLine();
		ImGui::TextDisabled(
			"CPU %.2f ms",
			summary.cpuFrameMs);
		ImGui::SameLine();
		if (profiler.IsGpuTimingSupported()) {
			ImGui::TextDisabled(
				"GPU %.2f ms",
				summary.gpuFrameMs);
			ImGui::SameLine();
		}
		ImGui::TextColored(
			averageFps >= 55.0
				? ImVec4(0.39f, 0.82f, 0.62f, 1.0f)
				: ImVec4(0.94f, 0.69f, 0.31f, 1.0f),
			"%.0f FPS",
			averageFps);
		ImGui::SameLine();
		if (ImGui::Button("Reset layout")) {
			m_requestLayoutReset = true;
		}
		ImGui::EndChild();
	}

	static void DrawPanelHeader(
		const char* title,
		const char* subtitle)
	{
		ImGui::TextColored(
			ImVec4(0.88f, 0.91f, 0.95f, 1.0f),
			"%s",
			title);
		if (subtitle && subtitle[0] != '\0') {
			ImGui::TextDisabled("%s", subtitle);
		}
		ImGui::Separator();
		ImGui::Spacing();
	}

	static void DrawMetricCard(
		const char* id,
		const char* label,
		const char* value,
		const ImVec4& accent,
		float requestedWidth)
	{
		const float width =
			(std::max)(
				1.0f,
				(std::min)(
					requestedWidth,
					ImGui::GetContentRegionAvail().x));
		ImGui::PushStyleColor(
			ImGuiCol_ChildBg,
			ImVec4(0.105f, 0.12f, 0.15f, 1.0f));
		ImGui::PushStyleColor(
			ImGuiCol_Border,
			ImVec4(accent.x, accent.y, accent.z, 0.32f));
		ImGui::BeginChild(
			id,
			ImVec2(width, 62.0f),
			true,
			ImGuiWindowFlags_NoScrollbar);
		ImGui::TextDisabled("%s", label);
		ImGui::TextColored(accent, "%s", value);
		ImGui::EndChild();
		ImGui::PopStyleColor(2);
	}

	static void DrawStatusRow(
		const char* label,
		bool healthy,
		const char* message,
		int count)
	{
		const ImVec4 color = healthy
			? ImVec4(0.36f, 0.80f, 0.59f, 1.0f)
			: ImVec4(0.94f, 0.43f, 0.39f, 1.0f);
		ImGui::Bullet();
		ImGui::SameLine();
		ImGui::TextColored(color, "%s", label);
		ImGui::SameLine();
		ImGui::TextDisabled("x%d", count);
		ImGui::SameLine();
		ImGui::TextWrapped("%s", message);
	}

	static void DrawSelectionButton(
		const char* label,
		bool selected,
		const ImVec2& size)
	{
		if (selected) {
			ImGui::PushStyleColor(
				ImGuiCol_Button,
				ImVec4(0.18f, 0.43f, 0.46f, 1.0f));
			ImGui::PushStyleColor(
				ImGuiCol_ButtonHovered,
				ImVec4(0.22f, 0.51f, 0.54f, 1.0f));
		}
		else {
			ImGui::PushStyleColor(
				ImGuiCol_Button,
				ImVec4(0.12f, 0.14f, 0.17f, 1.0f));
			ImGui::PushStyleColor(
				ImGuiCol_ButtonHovered,
				ImVec4(0.17f, 0.20f, 0.24f, 1.0f));
		}
		ImGui::Button(label, size);
		ImGui::PopStyleColor(2);
	}

	static void DrawTrackBadge(
		const char* label,
		bool active,
		const ImVec4& color)
	{
		const ImVec2 textSize = ImGui::CalcTextSize(label);
		const ImVec2 size(textSize.x + 14.0f, textSize.y + 8.0f);
		const ImVec2 minimum = ImGui::GetCursorScreenPos();
		ImGui::InvisibleButton(label, size);
		const ImVec2 maximum(minimum.x + size.x, minimum.y + size.y);
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const ImU32 background = active
			? ImGui::ColorConvertFloat4ToU32(
				ImVec4(color.x, color.y, color.z, 0.22f))
			: IM_COL32(57, 62, 72, 120);
		const ImU32 border = active
			? ImGui::ColorConvertFloat4ToU32(
				ImVec4(color.x, color.y, color.z, 0.72f))
			: IM_COL32(84, 90, 102, 150);
		const ImU32 text = active
			? ImGui::ColorConvertFloat4ToU32(color)
			: IM_COL32(135, 141, 153, 220);
		drawList->AddRectFilled(minimum, maximum, background, 4.0f);
		drawList->AddRect(minimum, maximum, border, 4.0f);
		drawList->AddText(
			ImVec2(minimum.x + 7.0f, minimum.y + 4.0f),
			text,
			label);
	}

	static void DrawTimelineTrajectoryPreview(
		const EditorMotionTimelineController& timeline)
	{
		const float width =
			(std::max)(1.0f, ImGui::GetContentRegionAvail().x);
		const ImVec2 size(width, 92.0f);
		ImGui::InvisibleButton("##timeline_trajectory", size);
		const ImVec2 minimum = ImGui::GetItemRectMin();
		const ImVec2 maximum = ImGui::GetItemRectMax();
		const ImVec2 center(
			(minimum.x + maximum.x) * 0.5f,
			(minimum.y + maximum.y) * 0.5f);
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddRectFilled(
			minimum,
			maximum,
			IM_COL32(20, 24, 31, 255),
			5.0f);
		drawList->AddRect(
			minimum,
			maximum,
			IM_COL32(55, 66, 79, 220),
			5.0f);
		for (int tick = 1; tick < 4; ++tick) {
			const float x =
				minimum.x +
				(maximum.x - minimum.x) *
					(static_cast<float>(tick) / 4.0f);
			drawList->AddLine(
				ImVec2(x, minimum.y + 7.0f),
				ImVec2(x, maximum.y - 7.0f),
				IM_COL32(51, 58, 70, 115));
		}
		drawList->AddLine(
			ImVec2(minimum.x + 8.0f, center.y),
			ImVec2(maximum.x - 8.0f, center.y),
			IM_COL32(51, 58, 70, 135));

		const std::uint32_t mask = BenchmarkMotionTimeline::TrackMask(
			timeline.GetProfile());
		const double phase =
			timeline.GetConfig().cycleFrames > 0
				? static_cast<double>(timeline.GetFrame()) /
					static_cast<double>(timeline.GetConfig().cycleFrames)
				: 0.0;
		const double angle = phase * 6.28318530717958647692;
		auto drawOrbit = [&](
			BenchmarkMotionTrack track,
			const ImVec4& color,
			float radiusX,
			float radiusY,
			double phaseOffset) {
			if (!BenchmarkMotionTimeline::HasTrack(mask, track)) {
				return;
			}
			std::array<ImVec2, 65> points{};
			for (std::size_t index = 0; index < points.size(); ++index) {
				const double pathAngle =
					static_cast<double>(index) /
						static_cast<double>(points.size() - 1) *
						6.28318530717958647692 +
					phaseOffset;
				points[index] = ImVec2(
					center.x +
						radiusX *
							static_cast<float>(std::cos(pathAngle)),
					center.y +
						radiusY *
							static_cast<float>(std::sin(pathAngle)));
			}
			const ImU32 packed =
				ImGui::ColorConvertFloat4ToU32(color);
			drawList->AddPolyline(
				points.data(),
				static_cast<int>(points.size()),
				packed,
				false,
				1.5f);
			const ImVec2 marker(
				center.x +
					radiusX *
						static_cast<float>(
							std::cos(angle + phaseOffset)),
				center.y +
					radiusY *
						static_cast<float>(
							std::sin(angle + phaseOffset)));
			drawList->AddCircleFilled(marker, 4.0f, packed);
			drawList->AddCircle(
				marker,
				6.0f,
				ImGui::ColorConvertFloat4ToU32(
					ImVec4(color.x, color.y, color.z, 0.38f)));
		};

		const float availableRadiusX =
			(std::max)(18.0f, (maximum.x - minimum.x) * 0.35f);
		drawOrbit(
			BenchmarkMotionTrack::Point,
			ImVec4(0.93f, 0.68f, 0.30f, 1.0f),
			availableRadiusX,
			26.0f,
			0.0);
		drawOrbit(
			BenchmarkMotionTrack::Caster,
			ImVec4(0.38f, 0.78f, 0.61f, 1.0f),
			availableRadiusX * 0.62f,
			17.0f,
			1.57079632679489661923);
		drawOrbit(
			BenchmarkMotionTrack::Camera,
			ImVec4(0.48f, 0.64f, 0.96f, 1.0f),
			availableRadiusX * 0.82f,
			22.0f,
			3.14159265358979323846);
		drawList->AddText(
			ImVec2(minimum.x + 8.0f, minimum.y + 7.0f),
			IM_COL32(139, 149, 164, 220),
			"NORMALIZED TRACK PREVIEW");
	}

	static void DrawTimelineHistoryPlot(
		const char* label,
		const std::vector<float>& values,
		const ImVec4& color,
		float minimumMaximum)
	{
		if (values.empty()) {
			ImGui::TextDisabled("%s: waiting for samples", label);
			return;
		}
		const float maximum =
			(std::max)(
				minimumMaximum,
				*std::max_element(values.begin(), values.end()) * 1.15f);
		ImGui::PushStyleColor(ImGuiCol_PlotLines, color);
		ImGui::PlotLines(
			label,
			values.data(),
			static_cast<int>(values.size()),
			0,
			nullptr,
			0.0f,
			maximum,
			ImVec2(-1.0f, 54.0f));
		ImGui::PopStyleColor();
	}

	void LoadEditorFonts(ImGuiIO& io)
	{
		constexpr float kEditorFontSize = 16.0f;
		const std::filesystem::path latinFont =
			"C:\\Windows\\Fonts\\segoeui.ttf";
		const std::filesystem::path chineseFont =
			"C:\\Windows\\Fonts\\msyh.ttc";
		std::error_code error;

		ImFont* editorFont = nullptr;
		if (std::filesystem::is_regular_file(latinFont, error)) {
			ImFontConfig latinConfig;
			latinConfig.OversampleH = 3;
			latinConfig.OversampleV = 2;
			latinConfig.PixelSnapH = false;
			latinConfig.RasterizerMultiply = 1.05f;
			std::snprintf(
				latinConfig.Name,
				IM_ARRAYSIZE(latinConfig.Name),
				"Segoe UI %.0fpx",
				kEditorFontSize);
			editorFont = io.Fonts->AddFontFromFileTTF(
				latinFont.string().c_str(),
				kEditorFontSize,
				&latinConfig);
		}

		error.clear();
		if (editorFont &&
			std::filesystem::is_regular_file(chineseFont, error)) {
			ImFontConfig chineseConfig;
			chineseConfig.MergeMode = true;
			chineseConfig.OversampleH = 2;
			chineseConfig.OversampleV = 1;
			chineseConfig.PixelSnapH = false;
			chineseConfig.RasterizerMultiply = 1.05f;
			std::snprintf(
				chineseConfig.Name,
				IM_ARRAYSIZE(chineseConfig.Name),
				"Microsoft YaHei fallback %.0fpx",
				kEditorFontSize);
			io.Fonts->AddFontFromFileTTF(
				chineseFont.string().c_str(),
				kEditorFontSize,
				&chineseConfig,
				io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
		}

		if (editorFont) {
			io.FontDefault = editorFont;
			m_editorFontDescription =
				"Segoe UI 16 px + Microsoft YaHei Chinese fallback";
		}
		else {
			io.Fonts->AddFontDefault();
			m_editorFontDescription =
				"Dear ImGui default (system editor font unavailable)";
		}
	}

	void ApplyEditorStyle() {
		ImGui::StyleColorsDark();
		ImGuiStyle& style = ImGui::GetStyle();
		style.WindowRounding = 5.0f;
		style.ChildRounding = 5.0f;
		style.FrameRounding = 4.0f;
		style.GrabRounding = 4.0f;
		style.ScrollbarRounding = 6.0f;
		style.TabRounding = 4.0f;
		style.PopupRounding = 5.0f;
		style.WindowPadding = ImVec2(12.0f, 11.0f);
		style.FramePadding = ImVec2(9.0f, 6.0f);
		style.CellPadding = ImVec2(8.0f, 6.0f);
		style.ItemSpacing = ImVec2(8.0f, 7.0f);
		style.ItemInnerSpacing = ImVec2(6.0f, 5.0f);
		style.IndentSpacing = 16.0f;
		style.WindowBorderSize = 1.0f;
		style.ChildBorderSize = 1.0f;

		ImVec4* colors = style.Colors;
		colors[ImGuiCol_Text] = ImVec4(0.88f, 0.91f, 0.95f, 1.0f);
		colors[ImGuiCol_TextDisabled] = ImVec4(0.49f, 0.54f, 0.62f, 1.0f);
		colors[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.065f, 0.082f, 1.0f);
		colors[ImGuiCol_ChildBg] = ImVec4(0.075f, 0.088f, 0.11f, 1.0f);
		colors[ImGuiCol_PopupBg] = ImVec4(0.07f, 0.082f, 0.105f, 0.99f);
		colors[ImGuiCol_Border] = ImVec4(0.18f, 0.22f, 0.28f, 0.78f);
		colors[ImGuiCol_Separator] = ImVec4(0.18f, 0.23f, 0.29f, 0.85f);
		colors[ImGuiCol_Header] = ImVec4(0.12f, 0.24f, 0.27f, 1.0f);
		colors[ImGuiCol_HeaderHovered] = ImVec4(0.16f, 0.34f, 0.37f, 1.0f);
		colors[ImGuiCol_HeaderActive] = ImVec4(0.20f, 0.42f, 0.44f, 1.0f);
		colors[ImGuiCol_Button] = ImVec4(0.12f, 0.28f, 0.31f, 1.0f);
		colors[ImGuiCol_ButtonHovered] = ImVec4(0.17f, 0.40f, 0.42f, 1.0f);
		colors[ImGuiCol_ButtonActive] = ImVec4(0.22f, 0.49f, 0.50f, 1.0f);
		colors[ImGuiCol_CheckMark] = ImVec4(0.34f, 0.82f, 0.75f, 1.0f);
		colors[ImGuiCol_SliderGrab] = ImVec4(0.31f, 0.72f, 0.68f, 1.0f);
		colors[ImGuiCol_SliderGrabActive] = ImVec4(0.41f, 0.87f, 0.80f, 1.0f);
		colors[ImGuiCol_FrameBg] = ImVec4(0.095f, 0.115f, 0.145f, 1.0f);
		colors[ImGuiCol_FrameBgHovered] = ImVec4(0.14f, 0.17f, 0.21f, 1.0f);
		colors[ImGuiCol_FrameBgActive] = ImVec4(0.17f, 0.21f, 0.26f, 1.0f);
		colors[ImGuiCol_TitleBg] = ImVec4(0.065f, 0.078f, 0.098f, 1.0f);
		colors[ImGuiCol_TitleBgActive] = ImVec4(0.09f, 0.115f, 0.145f, 1.0f);
		colors[ImGuiCol_Tab] = ImVec4(0.08f, 0.12f, 0.15f, 1.0f);
		colors[ImGuiCol_TabHovered] = ImVec4(0.15f, 0.30f, 0.33f, 1.0f);
		colors[ImGuiCol_TabActive] = ImVec4(0.12f, 0.24f, 0.27f, 1.0f);
		colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.10f, 0.12f, 0.15f, 0.55f);
		colors[ImGuiCol_DockingPreview] = ImVec4(0.34f, 0.82f, 0.75f, 0.42f);
	}

	void BuildDefaultLayout(ImGuiID dockspaceId, bool forceReset) {
		if (m_layoutInitialized && !forceReset) {
			return;
		}

		m_layoutInitialized = true;
		if (!forceReset &&
			ImGui::DockBuilderGetNode(dockspaceId) != nullptr) {
			return;
		}
		ImGui::DockBuilderRemoveNode(dockspaceId);
		ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
		ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

		ImGuiID left = 0;
		ImGuiID right = 0;
		ImGuiID bottom = 0;
		ImGuiID overview = 0;
		ImGuiID timeline = 0;
		ImGuiID diagnostics = 0;
		ImGuiID center = dockspaceId;

		ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, &left, &center);
		ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.27f, &right, &center);
		ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.42f, &bottom, &center);
		ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.34f, &overview, &left);
		ImGui::DockBuilderSplitNode(
			bottom,
			ImGuiDir_Right,
			0.38f,
			&diagnostics,
			&timeline);

		ImGui::DockBuilderDockWindow("Scene", left);
		ImGui::DockBuilderDockWindow("Overview", overview);
		ImGui::DockBuilderDockWindow("Viewport", center);
		ImGui::DockBuilderDockWindow("Motion Timeline", timeline);
		ImGui::DockBuilderDockWindow("Assets", diagnostics);
		ImGui::DockBuilderDockWindow("Profiler", diagnostics);
		ImGui::DockBuilderDockWindow("Renderer", right);
		ImGui::DockBuilderDockWindow("Materials Inspector", right);
		ImGui::DockBuilderDockWindow("Model Materials", right);
		ImGui::DockBuilderDockWindow("Materials XML Editor", right);
		ImGui::DockBuilderFinish(dockspaceId);
	}

	void ReadViewportPixel(int x, int y) {
		GLint prevReadFBO = 0;
		GLint prevReadBuffer = 0;
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFBO);
		glGetIntegerv(GL_READ_BUFFER, &prevReadBuffer);

		GLState::BindFramebuffer(GL_READ_FRAMEBUFFER, m_viewportReadFBO);
		if (m_viewportReadIsDepth) {
			float d = 0.0f;
			glReadPixels(x, y, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &d);
			m_pickedRGBA = { d, d, d, 1.0f };
		}
		else {
			glReadBuffer(GL_COLOR_ATTACHMENT0 + m_viewportReadAttachment);
			float rgba[4] = { 0, 0, 0, 0 };
			glReadPixels(x, y, 1, 1, GL_RGBA, GL_FLOAT, rgba);
			m_pickedRGBA = { rgba[0], rgba[1], rgba[2], rgba[3] };
		}

		m_pickedX = x;
		m_pickedY = y;
		m_hasPickedPixel = true;

		GLState::BindFramebuffer(GL_READ_FRAMEBUFFER, (unsigned int)prevReadFBO);
		glReadBuffer((unsigned int)prevReadBuffer);
	}

	MyGui() = default;
	SystemProperties& properties = SystemProperties::GetInstance();
	unsigned int m_viewportReadFBO = 0;
	int m_viewportReadAttachment = 0;
	bool m_viewportReadIsDepth = false;
	int m_viewportReadWidth = 0;
	int m_viewportReadHeight = 0;
	bool m_layoutInitialized = false;
	bool m_requestLayoutReset = false;
	bool m_rendererPanelVisible = true;
	bool m_assetBrowserCacheInitialized = false;
	std::string m_editorFontDescription;
	EditorMotionTimelineController m_motionTimeline;
	std::array<AssetBrowserCategory, 3> m_assetBrowserCategories{};
	SceneUiReplacementAction m_sceneReplacementAction =
		SceneUiReplacementAction::None;
	std::string m_sceneReplacementPath;
	std::size_t m_sceneReplacementClassicIndex = 0;
	ClassicSceneLoadOptions m_sceneReplacementOptions;
	std::size_t m_selectedClassicScene = 0;
	int m_sceneRenderPreset = 0;
	bool m_sceneDirectionalShadows = true;
	bool m_hasPickedPixel = false;
	int m_pickedX = 0;
	int m_pickedY = 0;
	std::array<float, 4> m_pickedRGBA = { 0.0f, 0.0f, 0.0f, 0.0f };
};
