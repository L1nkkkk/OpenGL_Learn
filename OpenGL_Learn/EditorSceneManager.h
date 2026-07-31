#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

class Camera;
class Scene;

enum class ClassicSceneRenderPreset {
	PbrForward = 0,
	PhongForward,
	PbrDeferred,
	PhongDeferred
};

struct ClassicSceneLoadOptions {
	ClassicSceneRenderPreset renderPreset = ClassicSceneRenderPreset::PbrForward;
	bool enableDirectionalShadows = true;
};

struct ClassicSceneDescriptor {
	std::string id;
	std::string displayName;
	std::string category;
	std::string packageId;
	std::string modelPath;
	std::string license;
	std::string credit;
	glm::vec3 cameraPosition = glm::vec3(0.0f, 0.0f, 3.0f);
	glm::vec3 cameraTarget = glm::vec3(0.0f);
	glm::vec3 cameraUp = glm::vec3(0.0f, 1.0f, 0.0f);
	float normalizedRadius = 15.0f;
	float fov = 45.0f;
	std::uint64_t expectedTriangles = 0;
	bool available = false;
};

class EditorSceneManager {
public:
	enum class StatusKind {
		Info,
		Success,
		Warning,
		Error
	};

	bool Initialize(
		const std::string& manifestPath,
		const std::string& sessionScenePath);
	bool ReloadClassicSceneCatalog();
	void RefreshClassicSceneAvailability();

	const std::vector<ClassicSceneDescriptor>& GetClassicScenes() const {
		return m_classicScenes;
	}
	const std::string& GetManifestPath() const {
		return m_manifestPath;
	}
	const std::string& GetCurrentSceneName() const {
		return m_currentSceneName;
	}
	const std::string& GetCurrentDocumentPath() const {
		return m_currentDocumentPath;
	}
	const std::string& GetStatusText() const {
		return m_statusText;
	}
	StatusKind GetStatusKind() const {
		return m_statusKind;
	}

	bool SaveCurrent(const Scene& scene, const Camera& camera);
	bool SaveAs(
		const Scene& scene,
		const Camera& camera,
		const std::string& path);

	void RequestNewScene();
	void RequestOpenScene(const std::string& path);
	void RequestLoadClassicScene(
		std::size_t sceneIndex,
		const ClassicSceneLoadOptions& options);
	void ProcessPendingAction(Scene& scene, Camera& camera);

	bool IsBusy() const;

private:
	enum class PendingActionType {
		None,
		NewScene,
		OpenScene,
		LoadClassicScene
	};

	struct PendingAction {
		PendingActionType type = PendingActionType::None;
		std::string path;
		std::size_t classicSceneIndex = 0;
		ClassicSceneLoadOptions classicOptions;
	};

	bool LoadClassicScene(
		Scene& scene,
		Camera& camera,
		const ClassicSceneDescriptor& descriptor,
		const ClassicSceneLoadOptions& options);
	void CreateNewScene(Scene& scene, Camera& camera);
	void SetStatus(StatusKind kind, const std::string& text);

	std::vector<ClassicSceneDescriptor> m_classicScenes;
	std::string m_requestedManifestPath;
	std::string m_manifestPath;
	std::string m_currentSceneName = "Default Scene";
	std::string m_currentDocumentPath;
	std::string m_statusText;
	StatusKind m_statusKind = StatusKind::Info;
	PendingAction m_pendingAction;
	bool m_waitingForSceneStateLoad = false;
};
