#pragma once
//
//#define USE_GEOMETRY_SHADER
#define USE_SCENE_SHADER
//#define USE_PLANET_SHADER
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4005)
#endif
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include "Learn.h"
#include "Model.h"
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
#include "Profiler.h"
#include "PerformanceBenchmark.h"
#include "GLStateCache.h"
#include "ImageBasedLighting.h"
#include "SceneStateIO.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <vector>
#ifdef _MSC_VER
#pragma warning(pop)
#endif


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

struct FrameCaptureStats {
	bool valid = false;
	double meanLuminance = 0.0;
	double nonBlackRatio = 0.0;
	std::vector<unsigned char> pixels;
};

FrameCaptureStats CaptureFramebufferPpm(const FBO* fbo, const std::string& outputPath)
{
	FrameCaptureStats stats;
	if (!fbo || fbo->framebufferID == 0 || fbo->width <= 0 || fbo->height <= 0) {
		return stats;
	}

	const int width = fbo->width;
	const int height = fbo->height;
	std::vector<unsigned char> pixels(
		static_cast<size_t>(width) * static_cast<size_t>(height) * 3u);
	GLState::BindFramebuffer(GL_READ_FRAMEBUFFER, fbo->framebufferID);
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
	const GLenum readError = glGetError();
	GLState::BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	if (readError != GL_NO_ERROR) {
		std::cerr << "PBR capture glReadPixels failed with error 0x"
			<< std::hex << readError << std::dec << std::endl;
		return stats;
	}

	std::error_code directoryError;
	const std::filesystem::path path(outputPath);
	if (path.has_parent_path()) {
		std::filesystem::create_directories(path.parent_path(), directoryError);
	}
	if (directoryError) {
		return stats;
	}
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	if (!output.is_open()) {
		return stats;
	}
	output << "P6\n" << width << ' ' << height << "\n255\n";
	const size_t rowBytes = static_cast<size_t>(width) * 3u;
	for (int row = height - 1; row >= 0; --row) {
		output.write(
			reinterpret_cast<const char*>(pixels.data() + static_cast<size_t>(row) * rowBytes),
			static_cast<std::streamsize>(rowBytes));
	}
	if (!output.good()) {
		return stats;
	}

	double luminanceSum = 0.0;
	std::uint64_t nonBlackPixels = 0;
	const std::uint64_t pixelCount = static_cast<std::uint64_t>(width) * height;
	for (size_t i = 0; i < pixels.size(); i += 3) {
		const double red = pixels[i] / 255.0;
		const double green = pixels[i + 1] / 255.0;
		const double blue = pixels[i + 2] / 255.0;
		const double luminance = 0.2126 * red + 0.7152 * green + 0.0722 * blue;
		luminanceSum += luminance;
		if (luminance > 0.01) {
			++nonBlackPixels;
		}
	}
	stats.meanLuminance = pixelCount != 0 ? luminanceSum / pixelCount : 0.0;
	stats.nonBlackRatio = pixelCount != 0
		? static_cast<double>(nonBlackPixels) / pixelCount
		: 0.0;
	stats.valid = stats.meanLuminance > 0.005 && stats.nonBlackRatio > 0.01;
	stats.pixels = std::move(pixels);
	return stats;
}


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
	const float aspectRatio = static_cast<float>(properties.SCREEN_WIDTH) /
		static_cast<float>((std::max)(1, properties.SCREEN_HEIGHT));
	projection = camera.GetProjectionMatrix(aspectRatio);
	ShaderManager& ShaderMgr = ShaderManager::GetInstance();
	ShaderMgr.SetUBOData(ShaderManager::Matrices, 0, sizeof(glm::mat4), &view);
	ShaderMgr.SetUBOData(ShaderManager::Matrices, sizeof(glm::mat4), sizeof(glm::mat4), &projection);
	ShaderMgr.UpdateSystemUBO();
}

int main(int argc, char** argv) {
	const auto applicationStart = PerformanceBenchmarkSession::Clock::now();
	bool resourceSmokeTest = false;
	bool pbrSmokeTest = false;
	bool pbrSmokeFailed = false;
	bool benchmarkPhongMaterialScene = false;
	bool benchmarkPbrMaterialScene = false;
	bool benchmarkUnsharedImportedMaterials = false;
	PerformanceBenchmarkOptions benchmarkOptions;
	std::string benchmarkOptionError;
	if (!ParsePerformanceBenchmarkOptions(argc, argv, benchmarkOptions, benchmarkOptionError)) {
		std::cerr << "Performance benchmark option error: " << benchmarkOptionError << std::endl;
		return 4;
	}
	for (int i = 1; i < argc; ++i) {
		if (std::string(argv[i]) == "--resource-smoke-test") {
			resourceSmokeTest = true;
		}
		else if (std::string(argv[i]) == "--pbr-smoke-test") {
			pbrSmokeTest = true;
		}
		else if (std::string(argv[i]) == "--benchmark-phong-material-scene") {
			benchmarkPhongMaterialScene = true;
		}
		else if (std::string(argv[i]) == "--benchmark-pbr-material-scene") {
			benchmarkPbrMaterialScene = true;
		}
		else if (std::string(argv[i]) == "--benchmark-unshared-imported-materials") {
			benchmarkUnsharedImportedMaterials = true;
		}
	}
	if ((resourceSmokeTest && benchmarkOptions.enabled) ||
		(pbrSmokeTest && (resourceSmokeTest || benchmarkOptions.enabled)) ||
		(benchmarkPhongMaterialScene && benchmarkPbrMaterialScene) ||
		((benchmarkPhongMaterialScene || benchmarkPbrMaterialScene) && !benchmarkOptions.enabled) ||
		(benchmarkUnsharedImportedMaterials &&
			(!benchmarkOptions.enabled || !benchmarkPbrMaterialScene))) {
		std::cerr << "automated smoke modes and --performance-benchmark are mutually exclusive" << std::endl;
		return 4;
	}
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
	if (benchmarkOptions.enabled) {
		// Make the benchmark request explicit. GPU timestamp zones remain the
		// authoritative metric if a driver-level frame limiter is still active.
		glfwSwapInterval(0);
	}
	else {
		glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
	}
	//register function after initializing window and before renderering
	glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
	if (!benchmarkOptions.enabled) {
		glfwSetCursorPosCallback(window, mouse_callback);
		glfwSetScrollCallback(window, scroll_callback);
	}
	if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
		std::cout << "Fail to initialize GLAD" << std::endl;
		return -1;
	}
	glfwGetFramebufferSize(window, &properties.SCREEN_WIDTH, &properties.SCREEN_HEIGHT);
	glViewport(0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT);
	PerformanceProfiler::GetInstance().Initialize();
	GLState::Initialize();

	InitVAOs();

	MyGui& mygui = MyGui::GetInstance();
	mygui.Init(window);
	if (benchmarkOptions.enabled || resourceSmokeTest || pbrSmokeTest) {
		// Automated runs must not modify the user's editor layout or depend on
		// ImGui's periodic ini writes while checks are being collected.
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard;
	}

	ShaderManager& shaderManager = ShaderManager::GetInstance();
	shaderManager.Init();
	if (benchmarkOptions.enabled || resourceSmokeTest || pbrSmokeTest) {
		bool invalidShaderFound = false;
		for (const std::string& shaderName : shaderManager.GetNames()) {
			auto shader = shaderManager.GetShaderByName(shaderName);
			if (!shader || shader->ID == 0) {
				std::cerr << "Automated validation: invalid shader '"
					<< shaderName << "'" << std::endl;
				invalidShaderFound = true;
			}
		}
		if (invalidShaderFound) {
			return 5;
		}
	}
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
	GLState::BindVertexArray(VAO);
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
	const std::string sceneStatePath = "saved/last_scene.json";
	const bool useBuiltInMaterialScene =
		pbrSmokeTest || benchmarkPhongMaterialScene || benchmarkPbrMaterialScene;
	if (useBuiltInMaterialScene) {
		Model::SetImportedMaterialSharingEnabled(!benchmarkUnsharedImportedMaterials);
		LoadDefaultLights(scene);
		const bool usePbrMaterial = pbrSmokeTest || benchmarkPbrMaterialScene;
		auto validationModel = std::make_shared<Model>(
			"models/backpack/backpack.obj",
			shaderManager.GetShader(
				usePbrMaterial ? ShaderManager::Pbr : ShaderManager::Phong));
		validationModel->SetName("pbr-backpack-validation");
		validationModel->SetPosition(glm::vec3(0.0f));
		validationModel->SetScale(1.0f);
		scene.modelSource.AddModel(validationModel);
		camera.cameraPos = glm::vec3(0.0f, 0.0f, 5.0f);
		camera.cameraFront = glm::vec3(0.0f, 0.0f, -1.0f);
		camera.up = glm::vec3(0.0f, 1.0f, 0.0f);
		properties.DEFER_RENDERING = false;
		properties.SSAO = false;
		properties.LIGHT_VOLUME = false;
		properties.BLOOM = false;
		properties.GAMMA_CORRECTION = true;

		if (pbrSmokeTest) {
			bool hasPbrMaterial = false;
			bool hasAlbedo = false;
			bool hasNormal = false;
			bool hasRoughness = false;
			bool hasAo = false;
			bool hasMetallicFactor = false;
			bool hasEmissiveFactor = false;
			for (const Mesh& mesh : validationModel->GetMeshes()) {
				if (!mesh.material_ptr || mesh.material_ptr->GetShaderName() != "pbr") {
					continue;
				}
				hasPbrMaterial = true;
				const auto& materialProperties = mesh.material_ptr->GetProperties();
				auto hasTexture = [&](const char* name) {
					const auto it = materialProperties.find(name);
					return it != materialProperties.end() &&
						it->second.type == MaterialPropertyType::Texture &&
						!it->second.textures.empty() &&
						it->second.textures.front().textureID != 0;
				};
				hasAlbedo = hasAlbedo || hasTexture("texture_diffuse");
				hasNormal = hasNormal || hasTexture("texture_normal");
				hasRoughness = hasRoughness || hasTexture("texture_roughness");
				hasAo = hasAo || hasTexture("texture_ao");
				const auto metallic = materialProperties.find("metallic");
				hasMetallicFactor = hasMetallicFactor ||
					(metallic != materialProperties.end() &&
						metallic->second.type == MaterialPropertyType::Float);
				const auto emissive = materialProperties.find("emissive");
				hasEmissiveFactor = hasEmissiveFactor ||
					(emissive != materialProperties.end() &&
						(emissive->second.type == MaterialPropertyType::Color ||
							emissive->second.type == MaterialPropertyType::Vec3));
			}
			pbrSmokeFailed = !(
				hasPbrMaterial &&
				hasAlbedo &&
				hasNormal &&
				hasRoughness &&
				hasAo &&
				hasMetallicFactor &&
				hasEmissiveFactor);
			std::cout << "[PBRSmoke] material=" << hasPbrMaterial
				<< " albedo=" << hasAlbedo
				<< " normal=" << hasNormal
				<< " roughness=" << hasRoughness
				<< " ao=" << hasAo
				<< " metallicFactor=" << hasMetallicFactor
				<< " emissiveFactor=" << hasEmissiveFactor << std::endl;
		}
	}
	else if (SceneStateIO::Exists(sceneStatePath)) {
		// 有存档：只初始化默认灯光占位，其它由 SceneStateIO 恢复，避免默认模型+存档模型双加载。
		LoadDefaultLights(scene);
		const bool loaded = SceneStateIO::LoadAsync(scene, camera, sceneStatePath);
		// 兼容兜底：若存档损坏/旧格式导致没有任何模型，则回退到默认场景，避免“模型全没了”。
		if (!loaded) {
			scene.lightSource.pointLights.clear();
			scene.lightSource.directionLights.clear();
			scene.lightSource.spotLights.clear();
			LoadModels(scene);
		}
		// 额外兜底：某些旧/异常存档可能把 lights 写成空数组，导致场景几乎全黑。
		if (scene.lightSource.pointLights.empty() &&
			scene.lightSource.directionLights.empty() &&
			scene.lightSource.spotLights.empty()) {
			LoadDefaultLights(scene);
		}
	}
	else {
		// 无存档：走默认场景。
		LoadModels(scene);
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
	GLState::BindVertexArray(skyboxVAO);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
	GLState::BindVertexArray(0);
	scene.skyboxSource = SkyboxSource(skybox, skyboxVAO, shaderManager.GetShader(ShaderManager::Skybox));
	ImageBasedLighting imageBasedLighting;
	scene.SetImageBasedLighting(&imageBasedLighting);
	bool iblInitializationAttempted = false;
	if (useBuiltInMaterialScene && scene.UsesPbrMaterials()) {
		iblInitializationAttempted = true;
		if (!imageBasedLighting.Initialize(
			skybox.textureID,
			skyboxVAO,
			globalVAOs.quadVAO,
			properties.SCREEN_WIDTH,
			properties.SCREEN_HEIGHT)) {
			std::cerr << "PBR IBL initialization failed; using direct-light fallback" << std::endl;
			if (pbrSmokeTest) pbrSmokeFailed = true;
		}
	}
	
	FramebuffersManager& framebuffersMgr = FramebuffersManager::GetInstance();
	AntiAliasManager& antiAliasMgr = AntiAliasManager::GetInstance();

	GLState::BindFramebuffer(GL_FRAMEBUFFER, 0);
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
	bool deferredPassActive = properties.DEFER_RENDERING;
	int resourceSmokeFrames = 0;
	int pbrSmokeFrames = 0;
	std::vector<unsigned char> pbrForwardPixels;
	bool resourceSmokeFailed = false;
	auto reportResourceState = [&](const char* stage, std::size_t expectedBusyFBOs) {
		const auto busyFBOs = FramebuffersManager::GetInstance().GetBusyFBOs();
		const auto& memoryStats = PerformanceProfiler::GetInstance().GetMemoryStats();
		const auto& renderTargets = memoryStats.categories[
			static_cast<std::size_t>(MemoryResourceType::RenderTarget)];
		const auto& meshCpu = memoryStats.categories[
			static_cast<std::size_t>(MemoryResourceType::MeshCpu)];
		const auto& meshGpu = memoryStats.categories[
			static_cast<std::size_t>(MemoryResourceType::MeshGpu)];
		const double renderTargetMiB =
			static_cast<double>(renderTargets.currentBytes) / (1024.0 * 1024.0);
		std::cout << "[ResourceSmoke] stage=" << stage
			<< " busyFBOs=" << busyFBOs.size()
			<< " renderTargetMiB=" << std::fixed << std::setprecision(2)
			<< renderTargetMiB
			<< " meshCpuMiB="
			<< static_cast<double>(meshCpu.currentBytes) / (1024.0 * 1024.0)
			<< " meshGpuMiB="
			<< static_cast<double>(meshGpu.currentBytes) / (1024.0 * 1024.0)
			<< std::endl;
		if (busyFBOs.size() != expectedBusyFBOs) {
			resourceSmokeFailed = true;
		}
	};
	double nextHotReloadPollTime = 0.0;
	PerformanceBenchmarkSession benchmarkSession(benchmarkOptions, applicationStart);
	if (benchmarkOptions.enabled) {
		GLint windowSampleBuffers = 0;
		GLint windowSamples = 0;
		glGetIntegerv(GL_SAMPLE_BUFFERS, &windowSampleBuffers);
		glGetIntegerv(GL_SAMPLES, &windowSamples);
		auto glString = [](GLenum name) -> std::string {
			const GLubyte* value = glGetString(name);
			return value ? reinterpret_cast<const char*>(value) : std::string();
		};

		int shadowCastingLights = 0;
		for (auto& light : scene.lightSource.pointLights) {
			if (light.GetActiveStatus() && light.useShadowMap) ++shadowCastingLights;
		}
		for (auto& light : scene.lightSource.directionLights) {
			if (light.GetActiveStatus() && light.useShadowMap) ++shadowCastingLights;
		}

		PerformanceBenchmarkMetadata metadata;
		metadata.scenePath = benchmarkPbrMaterialScene
			? (benchmarkUnsharedImportedMaterials
				? "builtin/backpack-pbr-unshared-materials"
				: "builtin/backpack-pbr")
			: (benchmarkPhongMaterialScene ? "builtin/backpack-phong" : sceneStatePath);
		metadata.glVendor = glString(GL_VENDOR);
		metadata.glRenderer = glString(GL_RENDERER);
		metadata.glVersion = glString(GL_VERSION);
#ifdef NDEBUG
		metadata.buildConfiguration = "Release";
#else
		metadata.buildConfiguration = "Debug";
#endif
#ifdef _WIN64
		metadata.architecture = "x64";
#else
		metadata.architecture = "Win32";
#endif
		metadata.width = properties.SCREEN_WIDTH;
		metadata.height = properties.SCREEN_HEIGHT;
		metadata.windowSampleBuffers = windowSampleBuffers;
		metadata.windowSamples = windowSamples;
		metadata.requestedSwapInterval = 0;
		metadata.pointLights = static_cast<int>(scene.lightSource.pointLights.size());
		metadata.directionLights = static_cast<int>(scene.lightSource.directionLights.size());
		metadata.spotLights = static_cast<int>(scene.lightSource.spotLights.size());
		metadata.shadowCastingLights = shadowCastingLights;
		metadata.bloom = properties.BLOOM;
		metadata.deferredRendering = properties.DEFER_RENDERING;
		metadata.ssao = properties.SSAO;
		metadata.forwardNormalBuffer = properties.FORWARD_NORMAL_BUFFER;
		metadata.gammaCorrection = properties.GAMMA_CORRECTION;
		metadata.autoReloadShaders = properties.AUTO_RELOAD_SHADERS;
		metadata.autoReloadMaterials = properties.AUTO_RELOAD_MATERIALS;
		metadata.inputFrozen = true;
		metadata.gpuTimingSupported = PerformanceProfiler::GetInstance().IsGpuTimingSupported();
		benchmarkSession.SetMetadata(metadata);
	}

	while (!glfwWindowShouldClose(window)) {
		if (benchmarkOptions.enabled &&
			!benchmarkSession.OnFrameBoundary(!SceneStateIO::HasPendingAsyncLoads())) {
			break;
		}
		PERF_FRAME_SCOPE();
		// 分帧异步恢复存档里的文件模型，减少单帧加载峰值。
		{
			PERF_CPU_SCOPE("Async Model Loads");
			SceneStateIO::UpdateAsyncLoads(scene, 1);
		}
		if (!iblInitializationAttempted && scene.UsesPbrMaterials()) {
			iblInitializationAttempted = true;
			if (!imageBasedLighting.Initialize(
				skybox.textureID,
				skyboxVAO,
				globalVAOs.quadVAO,
				properties.SCREEN_WIDTH,
				properties.SCREEN_HEIGHT)) {
				std::cerr << "PBR IBL initialization failed; using direct-light fallback" << std::endl;
				if (pbrSmokeTest) pbrSmokeFailed = true;
			}
		}
		if (pbrSmokeTest) {
			++pbrSmokeFrames;
		}
		if (resourceSmokeTest && !SceneStateIO::HasPendingAsyncLoads()) {
			++resourceSmokeFrames;
			if (resourceSmokeFrames == 30) {
				reportResourceState("forward-default", 2);
				properties.BLOOM = true;
			}
			else if (resourceSmokeFrames == 60) {
				reportResourceState("forward-bloom", 4);
				properties.DEFER_RENDERING = true;
				properties.SSAO = true;
			}
			else if (resourceSmokeFrames == 90) {
				reportResourceState("deferred-ssao-bloom", 6);
				for (auto& light : scene.lightSource.pointLights) {
					light.useShadowMap = true;
				}
				for (auto& light : scene.lightSource.directionLights) {
					light.useShadowMap = true;
				}
			}
			else if (resourceSmokeFrames == 120) {
				reportResourceState("all-effects", 8);
				properties.BLOOM = false;
				properties.SSAO = false;
				properties.DEFER_RENDERING = false;
				for (auto& light : scene.lightSource.pointLights) {
					light.useShadowMap = false;
				}
				for (auto& light : scene.lightSource.directionLights) {
					light.useShadowMap = false;
				}
			}
			else if (resourceSmokeFrames == 150) {
				reportResourceState("reclaimed-default", 2);
				glfwSetWindowShouldClose(window, true);
			}
		}

		//calculate FPS
		timer.Tick();
		
		std::stringstream windowTitle;
		windowTitle << "OpenGL_Learn FPS:" << timer.GetFPS();
		if (SceneStateIO::HasPendingAsyncLoads()) {
			const int pending = SceneStateIO::GetPendingAsyncLoadCount();
			const int total = SceneStateIO::GetTotalAsyncLoadCount();
			const int done = (total >= pending) ? (total - pending) : 0;
			windowTitle << " [Loading " << done << "/" << total << "]";
		}
		glfwSetWindowTitle(window, windowTitle.str().c_str());

		const double currentTime = glfwGetTime();
		if (currentTime >= nextHotReloadPollTime) {
			PERF_CPU_SCOPE("Hot Reload Polling");
			if (properties.AUTO_RELOAD_SHADERS) {
				shaderManager.ReloadChangedShaders();
			}
			if (properties.AUTO_RELOAD_MATERIALS) {
				xmlMaterialManager.ReloadChangedFiles();
			}
			const double pollInterval = (std::max)(
				0.05,
				static_cast<double>(properties.HOT_RELOAD_POLL_INTERVAL));
			nextHotReloadPollTime = currentTime + pollInterval;
		}
		// NewFrame
		{
			PERF_CPU_SCOPE("Editor UI Build");
			SetGui();

		// ?? DockSpace??? Unity ?????
		mygui.MainDockSpace();
		mygui.Overview_UI();
		mygui.Profiler_UI();

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

		}
		//process input
		{
			PERF_CPU_SCOPE("Input and Frame Uniforms");
			if (!benchmarkOptions.enabled) {
				ProcessInput(window);
			}
		//reset used texture num
		properties.ResetUsedTextureNum();
		//before pass: set uniform buffer
			SetUniformBuffer();
		}
		{
			PERF_GPU_SCOPE("GPU Frame");
#ifdef USE_SCENE_SHADER
		//first pass: render scene to framebuffer (HDR)
		FBO* sceneFBO = nullptr;
		if (properties.DEFER_RENDERING != deferredPassActive) {
			if (properties.DEFER_RENDERING) {
				forwardRenderPass->Destroy();
			}
			else {
				deferRenderPass->Destroy();
			}
			FramebuffersManager::GetInstance().TrimUnusedFBOs();
			deferredPassActive = properties.DEFER_RENDERING;
		}
		if (properties.DEFER_RENDERING) {
			deferRenderPass->Render(&scene);
			sceneFBO = deferRenderPass->GetOutputFBO();
		}
		else {
			forwardRenderPass->Render(&scene);
			sceneFBO = forwardRenderPass->GetOutputFBO();
		}
		//second pass: postprocess (HDR + gamma + bloom) -> LDR texture (inside postprocessRenderPass)
		
		GLState::BindFramebuffer(GL_FRAMEBUFFER, 0);
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
			GLState::BindFramebuffer(GL_FRAMEBUFFER, 0);
			FBO* debugFBO = scene.GetDebugFramebuffer();
			int size = static_cast<int>(debugFBO->textureIDs.size());
			int len = 1;
			while(len*len<size){
				len++;
			}
			debugShader.use();
			for(int i = 0;i<size;i++){
				GLState::ActiveTexture(GL_TEXTURE0 + i);
				GLState::BindTexture(GL_TEXTURE_2D, debugFBO->textureIDs[i]);
				std::string uniformName = "screenTexture[" + std::to_string(i) + "]";
				debugShader.setInt(uniformName, i);
			}
			debugShader.setFloat("div", (float)len);
			GLState::BindVertexArray(globalVAOs.quadVAO);
			GLState::Disable(GL_DEPTH_TEST);
			PerformanceProfiler::GetInstance().RecordDraw(GL_TRIANGLES, 6);
			glDrawArrays(GL_TRIANGLES, 0, 6);
		}

		// ??????? FBO??????????????? ImGui ?????
		GLState::BindFramebuffer(GL_FRAMEBUFFER, 0);

		// Viewport：默认(INDEX==0)显示最终渲染结果；否则显示所选 FBO 的指定 color/depth 附件
		unsigned int viewportTextureID = 0;
		unsigned int viewportReadFBO = 0;
		int viewportReadAttachment = 0;
		bool viewportReadIsDepth = false;
		int viewportReadWidth = properties.SCREEN_WIDTH;
		int viewportReadHeight = properties.SCREEN_HEIGHT;
		if (properties.VIEWPORT_DEBUG_FBO_INDEX == 0) {
			// 最终图（延迟+正向+后处理后的结果）
			FBO* finalFBO = postprocessRenderPass->GetOutputFBO();
			if (finalFBO && !finalFBO->textureIDs.empty()) {
				viewportTextureID = finalFBO->textureIDs[0];
				viewportReadFBO = finalFBO->framebufferID;
				viewportReadAttachment = 0;
				viewportReadIsDepth = false;
				viewportReadWidth = finalFBO->width;
				viewportReadHeight = finalFBO->height;
			}
		} else {
			std::vector<FBO*> busyFBOs = FramebuffersManager::GetInstance().GetBusyFBOs();
			int fboIdx = properties.VIEWPORT_DEBUG_FBO_INDEX - 1;
			if (fboIdx >= 0 && fboIdx < (int)busyFBOs.size()) {
				FBO* fbo = busyFBOs[fboIdx];
				if (!fbo->textureIDs.empty()
					&& properties.VIEWPORT_DEBUG_ATTACHMENT_INDEX >= 0
					&& properties.VIEWPORT_DEBUG_ATTACHMENT_INDEX < (int)fbo->textureIDs.size()) {
					viewportTextureID = fbo->textureIDs[properties.VIEWPORT_DEBUG_ATTACHMENT_INDEX];
					viewportReadFBO = fbo->framebufferID;
					viewportReadAttachment = properties.VIEWPORT_DEBUG_ATTACHMENT_INDEX;
					viewportReadIsDepth = (fbo->attr.isShadowMap && properties.VIEWPORT_DEBUG_ATTACHMENT_INDEX == 0);
					viewportReadWidth = fbo->width;
					viewportReadHeight = fbo->height;
				}
			}
		}
		{
			PERF_CPU_SCOPE("Viewport and Assets UI");
			{
				PERF_CPU_SCOPE("Viewport UI");
				mygui.SetViewportReadSource(
					viewportReadFBO,
					viewportReadAttachment,
					viewportReadIsDepth,
					viewportReadWidth,
					viewportReadHeight
				);
				mygui.Viewport_UI(viewportTextureID);
			}

			// Assets ?????? models / materials / shaders ??
			{
				PERF_CPU_SCOPE("Assets Browser UI");
				mygui.AssetsBrowser_UI();
			}
		}

		//Draw GUI
		{
			PERF_CPU_SCOPE("ImGui Render");
			mygui.Render();
		}
		if (pbrSmokeTest && (pbrSmokeFrames == 30 || pbrSmokeFrames == 60)) {
			const bool deferredCapture = pbrSmokeFrames == 60;
			const std::string capturePath = deferredCapture
				? "benchmark-results/pbr-ibl/pbr-deferred.ppm"
				: "benchmark-results/pbr-ibl/pbr-forward.ppm";
			const FrameCaptureStats capture = CaptureFramebufferPpm(
				postprocessRenderPass->GetOutputFBO(),
				capturePath);
			std::cout << "[PBRSmoke] mode="
				<< (deferredCapture ? "deferred" : "forward")
				<< " iblReady=" << imageBasedLighting.IsReady()
				<< " meanLuminance=" << std::fixed << std::setprecision(4)
				<< capture.meanLuminance
				<< " nonBlackRatio=" << capture.nonBlackRatio
				<< " capture=" << capturePath << std::endl;
			if (!capture.valid || !imageBasedLighting.IsReady()) {
				pbrSmokeFailed = true;
			}
			if (!deferredCapture) {
				pbrForwardPixels = capture.pixels;
				properties.DEFER_RENDERING = true;
			}
			else {
				double meanAbsoluteDifference = 1.0;
				if (!pbrForwardPixels.empty() &&
					pbrForwardPixels.size() == capture.pixels.size()) {
					double differenceSum = 0.0;
					for (size_t i = 0; i < capture.pixels.size(); ++i) {
						differenceSum += std::abs(
							static_cast<int>(pbrForwardPixels[i]) -
							static_cast<int>(capture.pixels[i]));
					}
					meanAbsoluteDifference = differenceSum /
						(static_cast<double>(capture.pixels.size()) * 255.0);
				}
				std::cout << "[PBRSmoke] forwardDeferredMae="
					<< std::fixed << std::setprecision(6)
					<< meanAbsoluteDifference << std::endl;
				if (meanAbsoluteDifference > 0.01) {
					pbrSmokeFailed = true;
				}
				glfwSetWindowShouldClose(window, true);
			}
		}
#elif defined(USE_GEOMETRY_SHADER)
		geometryShader.use();
		GLState::BindVertexArray(VAO);
		PerformanceProfiler::GetInstance().RecordDraw(GL_POINTS, 4);
		glDrawArrays(GL_POINTS, 0, 4);
#elif defined(USE_PLANET_SHADER)
		GLState::Enable(GL_DEPTH_TEST);
		glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
		glClearStencil(0);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		planet.Draw();
#endif
		}
		//scene.ClearFBO();
		// glfw: swap buffers and poll IO events (keys pressed/released, mouse moved etc.)
		{
			PERF_CPU_SCOPE("Present and Events");
			glfwSwapBuffers(window);
			glfwPollEvents();
		}
		
	}
	if (benchmarkOptions.enabled && !benchmarkSession.IsComplete()) {
		benchmarkSession.Abort();
	}

	forwardRenderPass->Destroy();
	delete forwardRenderPass;
	deferRenderPass->Destroy();
	delete deferRenderPass;
	postprocessRenderPass->Destroy();
	delete postprocessRenderPass;
	if (!resourceSmokeTest && !pbrSmokeTest && !benchmarkOptions.enabled) {
		SceneStateIO::Save(scene, camera, sceneStatePath);
	}
	scene.SetSelectedModelForMaterials(nullptr);
	scene.modelSource.models.clear();
	scene.lightSource.pointLights.clear();
	scene.lightSource.directionLights.clear();
	scene.lightSource.spotLights.clear();
	Model::DestroyMeshCache();
	scene.SetImageBasedLighting(nullptr);
	imageBasedLighting.Destroy();
	skybox.Release();
	DestroyTextureCache();
	FramebuffersManager::GetInstance().Shutdown();
	if (pbrSmokeTest) {
		const auto& memory = PerformanceProfiler::GetInstance().GetMemoryStats();
		auto currentBytes = [&](MemoryResourceType type) {
			return memory.categories[static_cast<size_t>(type)].currentBytes;
		};
		const std::uint64_t textureBytes = currentBytes(MemoryResourceType::Texture);
		const std::uint64_t meshCpuBytes = currentBytes(MemoryResourceType::MeshCpu);
		const std::uint64_t meshGpuBytes = currentBytes(MemoryResourceType::MeshGpu);
		const std::uint64_t renderTargetBytes = currentBytes(MemoryResourceType::RenderTarget);
		std::cout << "[PBRSmoke] released textureBytes=" << textureBytes
			<< " meshCpuBytes=" << meshCpuBytes
			<< " meshGpuBytes=" << meshGpuBytes
			<< " renderTargetBytes=" << renderTargetBytes << std::endl;
		if (textureBytes != 0 || meshCpuBytes != 0 || meshGpuBytes != 0 || renderTargetBytes != 0) {
			pbrSmokeFailed = true;
		}
	}
	PerformanceProfiler::GetInstance().Shutdown();

	glfwTerminate();
	if (resourceSmokeFailed) {
		return 2;
	}
	if (pbrSmokeFailed) {
		return 6;
	}
	if (benchmarkOptions.enabled && !benchmarkSession.WasSuccessful()) {
		return 3;
	}
	return 0;
}
