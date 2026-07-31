#include "EditorSceneManager.h"

#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <utility>

#include "../assimp/contrib/rapidjson/include/rapidjson/document.h"
#include "Camera.h"
#include "Global.h"
#include "Model.h"
#include "Scene.h"
#include "SceneStateIO.h"
#include "ShaderManager.h"

namespace {
	struct ClassicPackageMetadata {
		std::string license;
		std::string credit;
	};

	glm::vec3 ReadVec3(
		const rapidjson::Value& value,
		const glm::vec3& fallback)
	{
		if (!value.IsArray() || value.Size() < 3 ||
			!value[0].IsNumber() ||
			!value[1].IsNumber() ||
			!value[2].IsNumber()) {
			return fallback;
		}
		return glm::vec3(
			value[0].GetFloat(),
			value[1].GetFloat(),
			value[2].GetFloat());
	}

	std::filesystem::path ResolveManifestPath(const std::string& requestedPath)
	{
		const std::filesystem::path requested(requestedPath);
		std::error_code error;
		if (std::filesystem::is_regular_file(requested, error)) {
			return requested.lexically_normal();
		}

		error.clear();
		const std::filesystem::path projectRelative =
			std::filesystem::path("OpenGL_Learn") / requested;
		if (std::filesystem::is_regular_file(projectRelative, error)) {
			return projectRelative.lexically_normal();
		}
		return {};
	}

	void AddEditorDirectionalLight(Scene& scene, bool enableShadows)
	{
		DirectionLight light(
			glm::normalize(glm::vec3(-0.45f, -1.0f, -0.25f)),
			glm::vec3(0.02f),
			glm::vec3(2.0f),
			glm::vec3(1.0f));
		light.autoFitShadow = true;
		light.shadowResolution = 2048;
		light.useShadowMap = enableShadows;
		scene.lightSource.AddDirectionLight(light);
	}

	const char* RenderPresetName(ClassicSceneRenderPreset preset)
	{
		switch (preset) {
		case ClassicSceneRenderPreset::PbrForward:
			return "PBR Forward";
		case ClassicSceneRenderPreset::PhongForward:
			return "Phong Forward";
		case ClassicSceneRenderPreset::PbrDeferred:
			return "PBR Deferred";
		case ClassicSceneRenderPreset::PhongDeferred:
			return "Phong Deferred";
		default:
			return "Unknown";
		}
	}
}

bool EditorSceneManager::Initialize(
	const std::string& manifestPath,
	const std::string& sessionScenePath)
{
	m_requestedManifestPath = manifestPath;
	m_currentDocumentPath = sessionScenePath;
	m_currentSceneName = SceneStateIO::Exists(sessionScenePath)
		? "Last Session"
		: "Default Scene";
	return ReloadClassicSceneCatalog();
}

bool EditorSceneManager::ReloadClassicSceneCatalog()
{
	m_classicScenes.clear();
	m_manifestPath.clear();

	const std::filesystem::path manifest =
		ResolveManifestPath(m_requestedManifestPath);
	if (manifest.empty()) {
		SetStatus(
			StatusKind::Error,
			"Classic scene manifest not found: " + m_requestedManifestPath);
		return false;
	}

	std::ifstream input(manifest);
	if (!input.is_open()) {
		SetStatus(
			StatusKind::Error,
			"Cannot open classic scene manifest: " + manifest.string());
		return false;
	}
	const std::string json(
		(std::istreambuf_iterator<char>(input)),
		std::istreambuf_iterator<char>());
	rapidjson::Document document;
	document.Parse(json.c_str());
	if (document.HasParseError() || !document.IsObject() ||
		!document.HasMember("scenes") ||
		!document["scenes"].IsArray()) {
		SetStatus(
			StatusKind::Error,
			"Classic scene manifest is invalid: " + manifest.string());
		return false;
	}

	std::unordered_map<std::string, ClassicPackageMetadata> packages;
	if (document.HasMember("packages") && document["packages"].IsArray()) {
		for (const auto& package : document["packages"].GetArray()) {
			if (!package.IsObject() ||
				!package.HasMember("id") ||
				!package["id"].IsString()) {
				continue;
			}
			ClassicPackageMetadata metadata;
			if (package.HasMember("license") && package["license"].IsString()) {
				metadata.license = package["license"].GetString();
			}
			if (package.HasMember("credit") && package["credit"].IsString()) {
				metadata.credit = package["credit"].GetString();
			}
			packages.emplace(package["id"].GetString(), std::move(metadata));
		}
	}

	std::filesystem::path assetRoot("classic-scenes");
	if (document.HasMember("assetRoot") && document["assetRoot"].IsString()) {
		assetRoot = document["assetRoot"].GetString();
	}
	if (assetRoot.is_relative()) {
		assetRoot = manifest.parent_path() / assetRoot;
	}
	assetRoot = assetRoot.lexically_normal();

	for (const auto& sceneValue : document["scenes"].GetArray()) {
		if (!sceneValue.IsObject() ||
			!sceneValue.HasMember("id") ||
			!sceneValue["id"].IsString() ||
			!sceneValue.HasMember("modelPath") ||
			!sceneValue["modelPath"].IsString()) {
			continue;
		}

		ClassicSceneDescriptor descriptor;
		descriptor.id = sceneValue["id"].GetString();
		descriptor.displayName =
			sceneValue.HasMember("displayName") &&
			sceneValue["displayName"].IsString()
			? sceneValue["displayName"].GetString()
			: descriptor.id;
		if (sceneValue.HasMember("category") &&
			sceneValue["category"].IsString()) {
			descriptor.category = sceneValue["category"].GetString();
		}
		if (sceneValue.HasMember("packageId") &&
			sceneValue["packageId"].IsString()) {
			descriptor.packageId = sceneValue["packageId"].GetString();
		}

		const std::filesystem::path modelPath =
			assetRoot / sceneValue["modelPath"].GetString();
		descriptor.modelPath = modelPath.lexically_normal().string();
		if (sceneValue.HasMember("camera")) {
			descriptor.cameraPosition = ReadVec3(
				sceneValue["camera"],
				descriptor.cameraPosition);
		}
		if (sceneValue.HasMember("target")) {
			descriptor.cameraTarget = ReadVec3(
				sceneValue["target"],
				descriptor.cameraTarget);
		}
		if (sceneValue.HasMember("up")) {
			descriptor.cameraUp = ReadVec3(
				sceneValue["up"],
				descriptor.cameraUp);
		}
		if (sceneValue.HasMember("normalizedRadius") &&
			sceneValue["normalizedRadius"].IsNumber()) {
			descriptor.normalizedRadius =
				sceneValue["normalizedRadius"].GetFloat();
		}
		if (sceneValue.HasMember("fov") &&
			sceneValue["fov"].IsNumber()) {
			descriptor.fov = sceneValue["fov"].GetFloat();
		}
		if (sceneValue.HasMember("expectedTriangles") &&
			sceneValue["expectedTriangles"].IsUint64()) {
			descriptor.expectedTriangles =
				sceneValue["expectedTriangles"].GetUint64();
		}

		const auto packageIt = packages.find(descriptor.packageId);
		if (packageIt != packages.end()) {
			descriptor.license = packageIt->second.license;
			descriptor.credit = packageIt->second.credit;
		}
		std::error_code error;
		descriptor.available =
			std::filesystem::is_regular_file(modelPath, error);
		m_classicScenes.push_back(std::move(descriptor));
	}

	m_manifestPath = manifest.string();
	std::size_t installedCount = 0;
	for (const auto& descriptor : m_classicScenes) {
		if (descriptor.available) {
			++installedCount;
		}
	}
	std::ostringstream status;
	status << "Classic scenes: " << installedCount << "/"
		<< m_classicScenes.size() << " installed.";
	SetStatus(
		installedCount == m_classicScenes.size()
			? StatusKind::Success
			: StatusKind::Warning,
		status.str());
	return !m_classicScenes.empty();
}

void EditorSceneManager::RefreshClassicSceneAvailability()
{
	for (auto& descriptor : m_classicScenes) {
		std::error_code error;
		descriptor.available =
			std::filesystem::is_regular_file(descriptor.modelPath, error);
	}

	std::size_t installedCount = 0;
	for (const auto& descriptor : m_classicScenes) {
		if (descriptor.available) {
			++installedCount;
		}
	}
	std::ostringstream status;
	status << "Scene availability refreshed: " << installedCount << "/"
		<< m_classicScenes.size() << " installed.";
	SetStatus(
		installedCount == m_classicScenes.size()
			? StatusKind::Success
			: StatusKind::Warning,
		status.str());
}

bool EditorSceneManager::SaveCurrent(
	const Scene& scene,
	const Camera& camera)
{
	if (m_currentDocumentPath.empty()) {
		SetStatus(StatusKind::Warning, "Choose Save As for this scene.");
		return false;
	}
	if (!SceneStateIO::Save(scene, camera, m_currentDocumentPath)) {
		SetStatus(
			StatusKind::Error,
			"Failed to save scene: " + m_currentDocumentPath);
		return false;
	}
	SetStatus(
		StatusKind::Success,
		"Saved scene: " + m_currentDocumentPath);
	return true;
}

bool EditorSceneManager::SaveAs(
	const Scene& scene,
	const Camera& camera,
	const std::string& path)
{
	if (path.empty()) {
		SetStatus(StatusKind::Warning, "Save was cancelled.");
		return false;
	}
	if (!SceneStateIO::Save(scene, camera, path)) {
		SetStatus(StatusKind::Error, "Failed to save scene: " + path);
		return false;
	}
	m_currentDocumentPath = path;
	m_currentSceneName = std::filesystem::path(path).stem().string();
	SetStatus(StatusKind::Success, "Saved scene: " + path);
	return true;
}

void EditorSceneManager::RequestNewScene()
{
	if (IsBusy()) {
		SetStatus(StatusKind::Warning, "Wait for the current scene load to finish.");
		return;
	}
	m_pendingAction = {};
	m_pendingAction.type = PendingActionType::NewScene;
	SetStatus(StatusKind::Info, "Creating a new scene...");
}

void EditorSceneManager::RequestOpenScene(const std::string& path)
{
	if (IsBusy()) {
		SetStatus(StatusKind::Warning, "Wait for the current scene load to finish.");
		return;
	}
	m_pendingAction = {};
	m_pendingAction.type = PendingActionType::OpenScene;
	m_pendingAction.path = path;
	SetStatus(StatusKind::Info, "Opening scene: " + path);
}

void EditorSceneManager::RequestLoadClassicScene(
	std::size_t sceneIndex,
	const ClassicSceneLoadOptions& options)
{
	if (IsBusy()) {
		SetStatus(StatusKind::Warning, "Wait for the current scene load to finish.");
		return;
	}
	if (sceneIndex >= m_classicScenes.size()) {
		SetStatus(StatusKind::Error, "The selected classic scene is unavailable.");
		return;
	}
	m_pendingAction = {};
	m_pendingAction.type = PendingActionType::LoadClassicScene;
	m_pendingAction.classicSceneIndex = sceneIndex;
	m_pendingAction.classicOptions = options;
	SetStatus(
		StatusKind::Info,
		"Loading " + m_classicScenes[sceneIndex].displayName + "...");
}

void EditorSceneManager::ProcessPendingAction(
	Scene& scene,
	Camera& camera)
{
	if (m_waitingForSceneStateLoad &&
		!SceneStateIO::HasPendingAsyncLoads()) {
		m_waitingForSceneStateLoad = false;
		SetStatus(
			StatusKind::Success,
			"Opened scene: " + m_currentSceneName);
	}

	if (m_pendingAction.type == PendingActionType::None) {
		return;
	}

	PendingAction action = std::move(m_pendingAction);
	m_pendingAction = {};

	switch (action.type) {
	case PendingActionType::NewScene:
		CreateNewScene(scene, camera);
		break;
	case PendingActionType::OpenScene:
		if (!SceneStateIO::ReplaceAsync(scene, camera, action.path)) {
			SetStatus(
				StatusKind::Error,
				"Failed to open scene: " + action.path);
			break;
		}
		m_currentDocumentPath = action.path;
		m_currentSceneName =
			std::filesystem::path(action.path).stem().string();
		m_waitingForSceneStateLoad =
			SceneStateIO::HasPendingAsyncLoads();
		if (!m_waitingForSceneStateLoad) {
			SetStatus(
				StatusKind::Success,
				"Opened scene: " + m_currentSceneName);
		}
		else {
			SetStatus(
				StatusKind::Info,
				"Loading models for " + m_currentSceneName + "...");
		}
		break;
	case PendingActionType::LoadClassicScene:
		if (action.classicSceneIndex >= m_classicScenes.size()) {
			SetStatus(
				StatusKind::Error,
				"The selected classic scene is unavailable.");
			break;
		}
		LoadClassicScene(
			scene,
			camera,
			m_classicScenes[action.classicSceneIndex],
			action.classicOptions);
		break;
	default:
		break;
	}
}

bool EditorSceneManager::IsBusy() const
{
	return m_pendingAction.type != PendingActionType::None ||
		m_waitingForSceneStateLoad ||
		SceneStateIO::HasPendingAsyncLoads();
}

bool EditorSceneManager::LoadClassicScene(
	Scene& scene,
	Camera& camera,
	const ClassicSceneDescriptor& descriptor,
	const ClassicSceneLoadOptions& options)
{
	if (!descriptor.available) {
		SetStatus(
			StatusKind::Error,
			"Scene asset is not installed: " + descriptor.modelPath);
		return false;
	}

	const bool usePbr =
		options.renderPreset == ClassicSceneRenderPreset::PbrForward ||
		options.renderPreset == ClassicSceneRenderPreset::PbrDeferred;
	const bool useDeferred =
		options.renderPreset == ClassicSceneRenderPreset::PbrDeferred ||
		options.renderPreset == ClassicSceneRenderPreset::PhongDeferred;
	const auto loadStart = std::chrono::steady_clock::now();

	std::shared_ptr<Model> model;
	try {
		model = std::make_shared<Model>(
			descriptor.modelPath,
			ShaderManager::GetInstance().GetShader(
				usePbr ? ShaderManager::Pbr : ShaderManager::Phong));
	}
	catch (const std::exception& error) {
		SetStatus(
			StatusKind::Error,
			"Failed to load " + descriptor.displayName + ": " + error.what());
		return false;
	}
	catch (...) {
		SetStatus(
			StatusKind::Error,
			"Failed to load " + descriptor.displayName + ".");
		return false;
	}

	if (!model || model->GetMeshes().empty() ||
		model->GetLocalBoundingRadius() <= 0.0001f) {
		SetStatus(
			StatusKind::Error,
			"Scene contains no usable geometry: " + descriptor.modelPath);
		return false;
	}

	SceneStateIO::CancelAsyncLoads();
	scene.ClearContent();
	AddEditorDirectionalLight(scene, options.enableDirectionalShadows);

	const float appliedScale =
		descriptor.normalizedRadius / model->GetLocalBoundingRadius();
	model->SetName(descriptor.id);
	model->SetScale(appliedScale);
	model->SetPosition(-model->GetLoacalCenter() * appliedScale);
	model->AddOtherShader(
		OtherShaderType::outline,
		ShaderManager::GetInstance().GetShader(ShaderManager::Outline));
	model->AddOtherShader(
		OtherShaderType::normalLines,
		ShaderManager::GetInstance().GetShader(ShaderManager::NormalLines));
	scene.modelSource.AddModel(model);

	const glm::vec3 cameraDirection =
		descriptor.cameraTarget - descriptor.cameraPosition;
	camera.cameraPos = descriptor.cameraPosition;
	camera.cameraFront = glm::length(cameraDirection) > 0.0001f
		? glm::normalize(cameraDirection)
		: glm::vec3(0.0f, 0.0f, -1.0f);
	camera.up = glm::length(descriptor.cameraUp) > 0.0001f
		? glm::normalize(descriptor.cameraUp)
		: glm::vec3(0.0f, 1.0f, 0.0f);
	camera.fov = descriptor.fov;

	auto& properties = SystemProperties::GetInstance();
	properties.DEFER_RENDERING = useDeferred;
	properties.LIGHT_VOLUME = false;
	properties.SHADOW_MAP_SHOW = false;
	properties.DEBUG_MODE = false;
	scene.InvalidateShadowCache();

	std::uint64_t triangleCount = 0;
	for (const Mesh& mesh : model->GetMeshes()) {
		triangleCount += mesh.UsesIndices()
			? mesh.GetIndexCount() / 3u
			: mesh.GetVertexCount() / 3u;
	}
	const double loadMilliseconds =
		std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - loadStart).count();

	m_currentSceneName = descriptor.displayName;
	m_currentDocumentPath.clear();
	std::ostringstream status;
	status << "Loaded " << descriptor.displayName
		<< " (" << RenderPresetName(options.renderPreset)
		<< ", " << triangleCount << " triangles, "
		<< std::fixed << std::setprecision(1)
		<< loadMilliseconds << " ms).";
	SetStatus(StatusKind::Success, status.str());
	return true;
}

void EditorSceneManager::CreateNewScene(
	Scene& scene,
	Camera& camera)
{
	SceneStateIO::CancelAsyncLoads();
	scene.ClearContent();
	AddEditorDirectionalLight(scene, true);
	camera.cameraPos = glm::vec3(0.0f, 1.0f, 5.0f);
	camera.cameraFront = glm::normalize(glm::vec3(0.0f, -0.1f, -1.0f));
	camera.up = glm::vec3(0.0f, 1.0f, 0.0f);
	camera.fov = 45.0f;
	m_currentSceneName = "Untitled";
	m_currentDocumentPath.clear();
	SetStatus(StatusKind::Success, "Created a new empty scene.");
}

void EditorSceneManager::SetStatus(
	StatusKind kind,
	const std::string& text)
{
	m_statusKind = kind;
	m_statusText = text;
}
