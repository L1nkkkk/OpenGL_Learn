#include "SubmissionStressScene.h"

#include "Camera.h"
#include "Global.h"
#include "Material.h"
#include "Model.h"
#include "Scene.h"
#include "ShaderManager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace {
	std::string ToLower(std::string value)
	{
		std::transform(
			value.begin(),
			value.end(),
			value.begin(),
			[](unsigned char character) {
				return static_cast<char>(std::tolower(character));
			});
		return value;
	}

	bool ParseBoundedInt(
		const std::string& text,
		int minimum,
		int maximum,
		int& value)
	{
		try {
			std::size_t consumed = 0;
			const long parsed = std::stol(text, &consumed, 10);
			if (consumed != text.size() ||
				parsed < minimum ||
				parsed > maximum) {
				return false;
			}
			value = static_cast<int>(parsed);
			return true;
		}
		catch (...) {
			return false;
		}
	}

	bool ParseSeed(const std::string& text, std::uint32_t& value)
	{
		try {
			std::size_t consumed = 0;
			const unsigned long long parsed =
				std::stoull(text, &consumed, 0);
			if (consumed != text.size() ||
				parsed > (std::numeric_limits<std::uint32_t>::max)()) {
				return false;
			}
			value = static_cast<std::uint32_t>(parsed);
			return true;
		}
		catch (...) {
			return false;
		}
	}

	std::uint32_t HashInstance(
		std::uint32_t index,
		std::uint32_t seed)
	{
		std::uint32_t value = index + seed + 0x9e3779b9u;
		value ^= value >> 16;
		value *= 0x7feb352du;
		value ^= value >> 15;
		value *= 0x846ca68bu;
		value ^= value >> 16;
		return value;
	}

	glm::vec3 HsvToRgb(float hue, float saturation, float value)
	{
		const float wrappedHue = hue - std::floor(hue);
		const float scaled = wrappedHue * 6.0f;
		const int sector = static_cast<int>(std::floor(scaled)) % 6;
		const float fraction = scaled - std::floor(scaled);
		const float p = value * (1.0f - saturation);
		const float q = value * (1.0f - saturation * fraction);
		const float t = value * (1.0f - saturation * (1.0f - fraction));
		switch (sector) {
		case 0: return glm::vec3(value, t, p);
		case 1: return glm::vec3(q, value, p);
		case 2: return glm::vec3(p, value, t);
		case 3: return glm::vec3(p, q, value);
		case 4: return glm::vec3(t, p, value);
		default: return glm::vec3(value, p, q);
		}
	}

	std::vector<Vertex> BuildQuadVertices()
	{
		const glm::vec3 normal(0.0f, 0.0f, 1.0f);
		std::vector<Vertex> vertices;
		vertices.emplace_back(
			glm::vec3(-0.5f, -0.5f, 0.0f),
			normal,
			glm::vec2(0.0f, 0.0f));
		vertices.emplace_back(
			glm::vec3(0.5f, -0.5f, 0.0f),
			normal,
			glm::vec2(1.0f, 0.0f));
		vertices.emplace_back(
			glm::vec3(0.5f, 0.5f, 0.0f),
			normal,
			glm::vec2(1.0f, 1.0f));
		vertices.emplace_back(
			glm::vec3(-0.5f, 0.5f, 0.0f),
			normal,
			glm::vec2(0.0f, 1.0f));
		return vertices;
	}

	std::vector<Vertex> BuildTriangleVertices()
	{
		const glm::vec3 normal(0.0f, 0.0f, 1.0f);
		std::vector<Vertex> vertices;
		vertices.emplace_back(
			glm::vec3(0.0f, 0.58f, 0.0f),
			normal,
			glm::vec2(0.5f, 1.0f));
		vertices.emplace_back(
			glm::vec3(-0.55f, -0.48f, 0.0f),
			normal,
			glm::vec2(0.0f, 0.0f));
		vertices.emplace_back(
			glm::vec3(0.55f, -0.48f, 0.0f),
			normal,
			glm::vec2(1.0f, 0.0f));
		return vertices;
	}

	std::vector<Vertex> BuildOctagonVertices()
	{
		constexpr float kTwoPi = 6.28318530718f;
		const glm::vec3 normal(0.0f, 0.0f, 1.0f);
		std::vector<Vertex> vertices;
		vertices.emplace_back(
			glm::vec3(0.0f),
			normal,
			glm::vec2(0.5f));
		for (int index = 0; index < 8; ++index) {
			const float angle =
				kTwoPi * static_cast<float>(index) / 8.0f;
			const glm::vec2 unit(
				std::cos(angle),
				std::sin(angle));
			vertices.emplace_back(
				glm::vec3(unit * 0.55f, 0.0f),
				normal,
				unit * 0.5f + glm::vec2(0.5f));
		}
		return vertices;
	}

	std::vector<unsigned int> BuildOctagonIndices()
	{
		std::vector<unsigned int> indices;
		indices.reserve(24);
		for (unsigned int index = 0; index < 8; ++index) {
			indices.push_back(0);
			indices.push_back(index + 1);
			indices.push_back((index + 1) % 8 + 1);
		}
		return indices;
	}
}

void SubmissionStressSceneState::Reset()
{
	dynamicInstances.clear();
	materials.clear();
	objectCount = 0;
	dynamicObjectCount = 0;
	materialCount = 0;
	columns = 0;
	rows = 0;
	gridSpacing = 0.0f;
	cameraDistance = 0.0f;
}

bool ParseSubmissionStressOptions(
	int argc,
	char** argv,
	SubmissionStressOptions& options,
	std::string& errorMessage)
{
	bool sawStressOption = false;
	for (int index = 1; index < argc; ++index) {
		const std::string argument = argv[index];
		if (argument == "--submission-stress-scene") {
			options.enabled = true;
			continue;
		}
		if (argument == "--stress-collection-breakdown") {
			sawStressOption = true;
			options.collectionBreakdown = true;
			continue;
		}

		auto readValue = [&](const char* optionName, std::string& value) {
			if (argument != optionName) {
				return false;
			}
			sawStressOption = true;
			if (index + 1 >= argc) {
				errorMessage =
					std::string(optionName) + " requires a value";
				return true;
			}
			value = argv[++index];
			return true;
		};

		std::string value;
		if (readValue("--stress-object-count", value)) {
			if (!errorMessage.empty()) return false;
			if (!ParseBoundedInt(value, 1, 40000, options.objectCount)) {
				errorMessage =
					"--stress-object-count must be an integer from 1 to 40000";
				return false;
			}
			continue;
		}
		if (readValue("--stress-dynamic-percent", value)) {
			if (!errorMessage.empty()) return false;
			if (!ParseBoundedInt(value, 0, 100, options.dynamicPercent)) {
				errorMessage =
					"--stress-dynamic-percent must be an integer from 0 to 100";
				return false;
			}
			continue;
		}
		if (readValue("--stress-material-count", value)) {
			if (!errorMessage.empty()) return false;
			if (!ParseBoundedInt(value, 1, 64, options.materialCount)) {
				errorMessage =
					"--stress-material-count must be an integer from 1 to 64";
				return false;
			}
			continue;
		}
		if (readValue("--stress-width", value)) {
			if (!errorMessage.empty()) return false;
			if (!ParseBoundedInt(value, 320, 7680, options.width)) {
				errorMessage =
					"--stress-width must be an integer from 320 to 7680";
				return false;
			}
			continue;
		}
		if (readValue("--stress-height", value)) {
			if (!errorMessage.empty()) return false;
			if (!ParseBoundedInt(value, 240, 4320, options.height)) {
				errorMessage =
					"--stress-height must be an integer from 240 to 4320";
				return false;
			}
			continue;
		}
		if (readValue("--stress-seed", value)) {
			if (!errorMessage.empty()) return false;
			if (!ParseSeed(value, options.seed)) {
				errorMessage =
					"--stress-seed must be an unsigned 32-bit integer";
				return false;
			}
			continue;
		}
		if (readValue("--opaque-sort-mode", value)) {
			if (!errorMessage.empty()) return false;
			value = ToLower(std::move(value));
			if (value != "legacy" &&
				value != "key-direct" &&
				value != "key-index") {
				errorMessage =
					"--opaque-sort-mode must be legacy, key-direct, or key-index";
				return false;
			}
			options.opaqueSortMode = std::move(value);
			continue;
		}
		if (readValue("--stress-render-path", value)) {
			if (!errorMessage.empty()) return false;
			value = ToLower(std::move(value));
			if (value != "forward" && value != "deferred") {
				errorMessage =
					"--stress-render-path must be forward or deferred";
				return false;
			}
			options.renderPath = std::move(value);
			continue;
		}
		if (readValue("--stress-geometry-set", value)) {
			if (!errorMessage.empty()) return false;
			value = ToLower(std::move(value));
			if (value != "quad" && value != "mixed") {
				errorMessage =
					"--stress-geometry-set must be quad or mixed";
				return false;
			}
			options.geometrySet = std::move(value);
			continue;
		}
		if (readValue("--stress-capture-path", value)) {
			if (!errorMessage.empty()) return false;
			if (value.empty()) {
				errorMessage =
					"--stress-capture-path must not be empty";
				return false;
			}
			options.capturePath = std::move(value);
			continue;
		}
	}

	if (sawStressOption && !options.enabled) {
		errorMessage =
			"submission stress options require --submission-stress-scene";
		return false;
	}
	return true;
}

bool BuildSubmissionStressScene(
	Scene& scene,
	Camera& camera,
	const SubmissionStressOptions& options,
	SubmissionStressSceneState& state,
	std::string& errorMessage)
{
	state.Reset();
	auto shader =
		ShaderManager::GetInstance().GetShader(ShaderManager::Default);
	if (!shader || shader->ID == 0) {
		errorMessage =
			"submission stress scene requires the default shader";
		return false;
	}

	scene.modelSource.ClearModels();
	scene.lightSource.pointLights.clear();
	scene.lightSource.directionLights.clear();
	scene.lightSource.spotLights.clear();

	state.materials.reserve(static_cast<std::size_t>(options.materialCount));
	for (int index = 0; index < options.materialCount; ++index) {
		auto material = std::make_shared<Material>("default");
		const float hue =
			static_cast<float>(index) /
			static_cast<float>((std::max)(1, options.materialCount));
		material->AddProperty(
			"color",
			MaterialProperty::CreateColor(
				HsvToRgb(hue, 0.58f, 0.92f)));
		material->AddProperty(
			"opacity",
			MaterialProperty::CreateFloat(1.0f));
		material->AddProperty(
			"useAlphaCutoff",
			MaterialProperty::CreateBool(false));
		RenderState renderState;
		renderState.depthTest = true;
		renderState.depthWrite = true;
		renderState.blendMode = BlendMode::None;
		renderState.cullMode = CullMode::None;
		material->SetRenderState(renderState);
		state.materials.push_back(std::move(material));
	}

	std::vector<Mesh> prototypes;
	prototypes.emplace_back(
		BuildQuadVertices(),
		std::vector<unsigned int>{ 0, 1, 2, 0, 2, 3 },
		state.materials.front().get(),
		state.materials.front(),
		std::string(),
		true);
	if (options.geometrySet == "mixed") {
		prototypes.emplace_back(
			BuildTriangleVertices(),
			std::vector<unsigned int>{ 0, 1, 2 },
			state.materials.front().get(),
			state.materials.front(),
			std::string(),
			true);
		prototypes.emplace_back(
			BuildOctagonVertices(),
			BuildOctagonIndices(),
			state.materials.front().get(),
			state.materials.front(),
			std::string(),
			true);
	}

	camera.fov = 45.0f;
	const float aspectRatio =
		static_cast<float>(options.width) /
		static_cast<float>((std::max)(1, options.height));
	state.columns = (std::max)(
		1,
		static_cast<int>(
			std::ceil(
				std::sqrt(
					static_cast<float>(options.objectCount) *
					aspectRatio))));
	state.rows =
		(options.objectCount + state.columns - 1) / state.columns;
	state.gridSpacing = 0.45f;

	auto calculateCameraDistance = [&]() {
		const float gridWidth =
			static_cast<float>((std::max)(0, state.columns - 1)) *
			state.gridSpacing;
		const float gridHeight =
			static_cast<float>((std::max)(0, state.rows - 1)) *
			state.gridSpacing;
		const float halfViewHeight = (std::max)(
			gridHeight * 0.5f + state.gridSpacing,
			(gridWidth * 0.5f + state.gridSpacing) / aspectRatio);
		const float tangent =
			std::tan(glm::radians(camera.fov * 0.5f));
		return halfViewHeight / (std::max)(0.01f, tangent) * 1.08f +
			1.0f;
	};

	state.cameraDistance = calculateCameraDistance();
	constexpr float kMaximumCameraDistance = 92.0f;
	if (state.cameraDistance > kMaximumCameraDistance) {
		state.gridSpacing *=
			kMaximumCameraDistance / state.cameraDistance;
		state.cameraDistance = calculateCameraDistance();
	}

	const float gridWidth =
		static_cast<float>((std::max)(0, state.columns - 1)) *
		state.gridSpacing;
	const float gridHeight =
		static_cast<float>((std::max)(0, state.rows - 1)) *
		state.gridSpacing;
	const float objectScale = state.gridSpacing * 0.42f;
	const float left = -gridWidth * 0.5f;
	const float bottom = -gridHeight * 0.5f;

	state.dynamicInstances.reserve(
		static_cast<std::size_t>(
			options.objectCount * options.dynamicPercent / 100 + 1));
	for (int index = 0; index < options.objectCount; ++index) {
		const std::uint32_t hash = HashInstance(
			static_cast<std::uint32_t>(index),
			options.seed);
		const int column = index % state.columns;
		const int row = index / state.columns;
		const glm::vec3 position(
			left + static_cast<float>(column) * state.gridSpacing,
			bottom + static_cast<float>(row) * state.gridSpacing,
			-static_cast<float>((hash >> 16) % 7u) * 0.002f);
		const std::size_t materialIndex =
			static_cast<std::size_t>(
				hash % static_cast<std::uint32_t>(options.materialCount));

		const std::size_t prototypeIndex =
			static_cast<std::size_t>(
				(hash >> 24) %
				static_cast<std::uint32_t>(
					prototypes.size()));
		Mesh instanceMesh = prototypes[prototypeIndex];
		instanceMesh.material_owner = state.materials[materialIndex];
		instanceMesh.material_ptr = state.materials[materialIndex].get();
		std::vector<Mesh> meshes;
		meshes.push_back(std::move(instanceMesh));
		auto model = std::make_shared<Model>(std::move(meshes));
		model->SetDataSourceGenerated(
			options.geometrySet == "mixed"
				? "submission_stress_mixed"
				: "submission_stress_quad");
		model->SetShader(shader);
		model->SetName("Submission Stress " + std::to_string(index));
		model->SetScale(objectScale);
		model->SetPosition(position);
		scene.modelSource.AddModel(model);

		if (static_cast<int>((hash >> 8) % 100u) <
			options.dynamicPercent) {
			const float phase =
				static_cast<float>(hash & 0xffffu) /
				65535.0f * 6.28318530718f;
			state.dynamicInstances.push_back({
				std::move(model),
				position,
				phase });
		}
	}

	state.objectCount = options.objectCount;
	state.dynamicObjectCount =
		static_cast<int>(state.dynamicInstances.size());
	state.materialCount = options.materialCount;

	camera.cameraPos = glm::vec3(0.0f, 0.0f, state.cameraDistance);
	camera.cameraFront = glm::vec3(0.0f, 0.0f, -1.0f);
	camera.up = glm::vec3(0.0f, 1.0f, 0.0f);

	auto& properties = SystemProperties::GetInstance();
	properties.DEFER_RENDERING = options.renderPath == "deferred";
	properties.SSAO = false;
	properties.LIGHT_VOLUME = false;
	properties.BLOOM = false;
	properties.FORWARD_NORMAL_BUFFER = false;
	properties.GAMMA_CORRECTION = true;
	properties.DEBUG_MODE = false;
	properties.AUTO_RELOAD_SHADERS = false;
	properties.AUTO_RELOAD_MATERIALS = false;
	properties.FRUSTUM_CULLING = true;
	// No light in this fixture casts a shadow. Keep shadow signature work out
	// of the CPU submission baseline so Build Draw Lists measures the intended
	// traversal, item construction and sorting path.
	properties.SHADOW_CACHE_USE_LEGACY_SIGNATURE = true;
	if (options.opaqueSortMode == "legacy") {
		scene.SetOpaqueSortMode(
			Scene::OpaqueSortMode::LegacyMapComparator);
	}
	else if (options.opaqueSortMode == "key-direct") {
		scene.SetOpaqueSortMode(Scene::OpaqueSortMode::KeyDirect);
	}
	else {
		scene.SetOpaqueSortMode(Scene::OpaqueSortMode::KeyIndex);
	}
	return true;
}

void UpdateSubmissionStressScene(
	SubmissionStressSceneState& state,
	std::uint64_t frameIndex)
{
	if (state.dynamicInstances.empty()) {
		return;
	}
	const float time =
		static_cast<float>(frameIndex % 1000000u) / 60.0f;
	const float amplitude = state.gridSpacing * 0.10f;
	for (SubmissionStressDynamicInstance& instance :
		state.dynamicInstances) {
		if (!instance.model) {
			continue;
		}
		const float horizontal =
			std::sin(time * 0.73f + instance.phase) * amplitude;
		const float vertical =
			std::cos(time * 0.91f + instance.phase) * amplitude;
		instance.model->SetPosition(
			instance.basePosition +
			glm::vec3(horizontal, vertical, 0.0f));
		instance.model->SetRotation(glm::vec3(
			0.0f,
			0.0f,
			std::sin(time * 0.37f + instance.phase) * 8.0f));
	}
}

std::string DescribeSubmissionStressScene(
	const SubmissionStressOptions& options,
	const SubmissionStressSceneState& state)
{
	std::ostringstream stream;
	stream << "builtin/submission-stress"
		<< "?objects=" << state.objectCount
		<< "&dynamic=" << state.dynamicObjectCount
		<< "&dynamicPercent=" << options.dynamicPercent
		<< "&materials=" << state.materialCount
		<< "&seed=" << options.seed
		<< "&opaqueSort=" << options.opaqueSortMode
		<< "&renderPath=" << options.renderPath
		<< "&geometrySet=" << options.geometrySet
		<< "&collectionBreakdown="
		<< (options.collectionBreakdown ? 1 : 0)
		<< "&grid=" << state.columns << 'x' << state.rows;
	return stream.str();
}
