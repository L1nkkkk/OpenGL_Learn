#pragma once

#include <string>
#include "Scene.h"
#include "Camera.h"

class SceneStateIO {
public:
	static bool Exists(const std::string& path);
	static bool Save(const Scene& scene, const Camera& camera, const std::string& path);
	static bool Load(Scene& scene, Camera& camera, const std::string& path);
};

