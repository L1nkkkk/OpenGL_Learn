#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

class Camera;
class Material;
class Model;
class Scene;

struct SubmissionStressOptions {
	bool enabled = false;
	int objectCount = 30000;
	int dynamicPercent = 20;
	int materialCount = 16;
	int width = 1920;
	int height = 1080;
	std::uint32_t seed = 0x5eed1234u;
	std::string opaqueSortMode = "key-index";
	std::string renderPath = "forward";
	std::string geometrySet = "quad";
	bool collectionBreakdown = false;
	std::string capturePath;
};

struct SubmissionStressDynamicInstance {
	std::shared_ptr<Model> model;
	glm::vec3 basePosition = glm::vec3(0.0f);
	float phase = 0.0f;
};

struct SubmissionStressSceneState {
	std::vector<std::shared_ptr<Material>> materials;
	std::vector<SubmissionStressDynamicInstance> dynamicInstances;
	int objectCount = 0;
	int dynamicObjectCount = 0;
	int materialCount = 0;
	int columns = 0;
	int rows = 0;
	float gridSpacing = 0.0f;
	float cameraDistance = 0.0f;

	void Reset();
};

bool ParseSubmissionStressOptions(
	int argc,
	char** argv,
	SubmissionStressOptions& options,
	std::string& errorMessage);

bool BuildSubmissionStressScene(
	Scene& scene,
	Camera& camera,
	const SubmissionStressOptions& options,
	SubmissionStressSceneState& state,
	std::string& errorMessage);

void UpdateSubmissionStressScene(
	SubmissionStressSceneState& state,
	std::uint64_t frameIndex);

std::string DescribeSubmissionStressScene(
	const SubmissionStressOptions& options,
	const SubmissionStressSceneState& state);
