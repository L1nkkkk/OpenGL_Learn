#pragma once

#include <string>
#include "Scene.h"
#include "Camera.h"

class SceneStateIO {
public:
	static bool Exists(const std::string& path);
	static bool Save(const Scene& scene, const Camera& camera, const std::string& path);
	static bool Load(Scene& scene, Camera& camera, const std::string& path);
	// 分帧异步加载：LoadAsync 先恢复相机/灯光与非文件模型，把文件模型入队；UpdateAsyncLoads 每帧消费少量队列。
	static bool LoadAsync(Scene& scene, Camera& camera, const std::string& path);
	static bool ReplaceAsync(Scene& scene, Camera& camera, const std::string& path);
	static void CancelAsyncLoads();
	static void UpdateAsyncLoads(Scene& scene, int maxModelsPerFrame = 1);
	static bool HasPendingAsyncLoads();
	static int GetPendingAsyncLoadCount();
	static int GetTotalAsyncLoadCount();
};

