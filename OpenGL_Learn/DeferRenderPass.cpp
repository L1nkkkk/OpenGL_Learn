#include "DeferRenderPass.h"
#include "GLStateCache.h"
#include "Profiler.h"
#include "ShaderManager.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

namespace {
	using PushDebugGroupProc = void (APIENTRY*)(
		unsigned int, unsigned int, int, const char*);
	using PopDebugGroupProc = void (APIENTRY*)();
	using PushGroupMarkerExtProc = void (APIENTRY*)(int, const char*);
	using PopGroupMarkerExtProc = void (APIENTRY*)();

	struct PointLightDebugFunctions {
		PushDebugGroupProc pushDebugGroup = nullptr;
		PopDebugGroupProc popDebugGroup = nullptr;
		PushGroupMarkerExtProc pushGroupMarkerExt = nullptr;
		PopGroupMarkerExtProc popGroupMarkerExt = nullptr;
	};

	const PointLightDebugFunctions& GetPointLightDebugFunctions()
	{
		static const PointLightDebugFunctions functions = [] {
			PointLightDebugFunctions result;
			result.pushDebugGroup = reinterpret_cast<PushDebugGroupProc>(
				glfwGetProcAddress("glPushDebugGroup"));
			result.popDebugGroup = reinterpret_cast<PopDebugGroupProc>(
				glfwGetProcAddress("glPopDebugGroup"));
			result.pushGroupMarkerExt =
				reinterpret_cast<PushGroupMarkerExtProc>(
					glfwGetProcAddress("glPushGroupMarkerEXT"));
			result.popGroupMarkerExt =
				reinterpret_cast<PopGroupMarkerExtProc>(
					glfwGetProcAddress("glPopGroupMarkerEXT"));
			return result;
		}();
		return functions;
	}

	class PointLightDebugScope {
	public:
		PointLightDebugScope(bool enabled, const char* label)
		{
			if (!enabled) return;
			const PointLightDebugFunctions& functions =
				GetPointLightDebugFunctions();
			if (functions.pushDebugGroup && functions.popDebugGroup) {
				constexpr unsigned int kDebugSourceApplication = 0x824A;
				functions.pushDebugGroup(
					kDebugSourceApplication, 0, -1, label);
				m_functions = &functions;
				m_khr = true;
			}
			else if (functions.pushGroupMarkerExt &&
				functions.popGroupMarkerExt) {
				functions.pushGroupMarkerExt(-1, label);
				m_functions = &functions;
			}
		}

		~PointLightDebugScope()
		{
			if (!m_functions) return;
			if (m_khr) m_functions->popDebugGroup();
			else m_functions->popGroupMarkerExt();
		}

		PointLightDebugScope(const PointLightDebugScope&) = delete;
		PointLightDebugScope& operator=(const PointLightDebugScope&) = delete;

	private:
		const PointLightDebugFunctions* m_functions = nullptr;
		bool m_khr = false;
	};

	struct GBufferLayout {
		int position = -1;
		int normal = 0;
		int albedo = 1;
		int material = 2;
		int emissive = 3;
		std::size_t colorAttachmentCount = 4;
	};

	bool PositionReconstructionEnabled()
	{
		return SystemProperties::GetInstance().GBUFFER_POSITION_MODE ==
			GBufferPositionProperty::ReconstructFromDepth;
	}

	GBufferLayout CurrentGBufferLayout()
	{
		GBufferLayout layout;
		if (!PositionReconstructionEnabled()) {
			layout.position = 0;
			layout.normal = 1;
			layout.albedo = 2;
			layout.material = 3;
			layout.emissive = 4;
			layout.colorAttachmentCount = 5;
		}
		return layout;
	}

	glm::mat4 CurrentProjection(const Scene* scene)
	{
		const auto& properties = SystemProperties::GetInstance();
		const float aspect = static_cast<float>(properties.SCREEN_WIDTH) /
			static_cast<float>((std::max)(1, properties.SCREEN_HEIGHT));
		return scene && scene->camera_ptr
			? scene->camera_ptr->GetProjectionMatrix(aspect)
			: glm::mat4(1.0f);
	}

	struct PointLightPixelRect {
		int x = 0;
		int y = 0;
		int width = 0;
		int height = 0;

		std::uint64_t Area() const
		{
			return static_cast<std::uint64_t>((std::max)(0, width)) *
				static_cast<std::uint64_t>((std::max)(0, height));
		}
	};

	struct PointLightScreenProxy {
		PointLight* light = nullptr;
		std::uint64_t stableLightId = 0;
		std::size_t sourceIndex = 0;
		float radius = 0.0f;
		float radiusSquared = 0.0f;
		int classification = PointLightScreenProxyProperty::Outside;
		int fallbackReason = PointLightScreenProxyProperty::None;
		PointLightPixelRect pixelRect;
		double coverageRatio = 0.0;
	};

	bool IsFiniteMatrix(const glm::mat4& matrix)
	{
		for (int column = 0; column < 4; ++column) {
			for (int row = 0; row < 4; ++row) {
				if (!std::isfinite(matrix[column][row])) return false;
			}
		}
		return true;
	}

	std::uint64_t StablePointLightId(PointLight& light, std::size_t sourceIndex)
	{
		std::uint64_t hash = 1469598103934665603ull;
		const std::string name = light.GetName();
		for (unsigned char value : name) {
			hash ^= value;
			hash *= 1099511628211ull;
		}
		for (std::size_t byte = 0; byte < sizeof(sourceIndex); ++byte) {
			hash ^= static_cast<unsigned char>(
				(sourceIndex >> (byte * 8u)) & 0xffu);
			hash *= 1099511628211ull;
		}
		return hash;
	}

	PointLightScreenProxy BuildPointLightScreenProxy(
		PointLight& light,
		std::size_t sourceIndex,
		const glm::mat4& view,
		const glm::mat4& projection,
		const glm::vec3& cameraPosition,
		int viewportWidth,
		int viewportHeight,
		float rawRadius)
	{
		PointLightScreenProxy proxy;
		proxy.light = &light;
		proxy.sourceIndex = sourceIndex;
		proxy.stableLightId = StablePointLightId(light, sourceIndex);

		constexpr float kMaximumSupportedRadius = 1000000.0f;
		const bool radiusValid =
			std::isfinite(rawRadius) && rawRadius > 0.0f &&
			rawRadius <= kMaximumSupportedRadius;
		proxy.radius = radiusValid ? rawRadius : kMaximumSupportedRadius;
		proxy.radiusSquared = proxy.radius * proxy.radius;

		const int width = (std::max)(1, viewportWidth);
		const int height = (std::max)(1, viewportHeight);
		const std::uint64_t viewportArea =
			static_cast<std::uint64_t>(width) *
			static_cast<std::uint64_t>(height);
		auto makeFullscreenFallback = [&](int reason) {
			proxy.classification =
				PointLightScreenProxyProperty::FullscreenFallback;
			proxy.fallbackReason = reason;
			proxy.pixelRect = { 0, 0, width, height };
			proxy.coverageRatio = 1.0;
			return proxy;
		};

		if (!radiusValid) {
			return makeFullscreenFallback(
				PointLightScreenProxyProperty::InvalidRadius);
		}
		if (!IsFiniteMatrix(view) || !IsFiniteMatrix(projection) ||
			!std::isfinite(light.position.x) ||
			!std::isfinite(light.position.y) ||
			!std::isfinite(light.position.z)) {
			return makeFullscreenFallback(
				PointLightScreenProxyProperty::InvalidProjection);
		}

		const glm::mat4 clipFromWorld = projection * view;
		const glm::mat4 rows = glm::transpose(clipFromWorld);
		const std::array<glm::vec4, 6> planes = {
			rows[3] + rows[0],
			rows[3] - rows[0],
			rows[3] + rows[1],
			rows[3] - rows[1],
			rows[3] + rows[2],
			rows[3] - rows[2]
		};
		for (const glm::vec4& plane : planes) {
			const float normalLength = glm::length(glm::vec3(plane));
			if (!std::isfinite(normalLength) || normalLength <= 1e-7f) {
				return makeFullscreenFallback(
					PointLightScreenProxyProperty::InvalidProjection);
			}
			const float signedDistance = glm::dot(
				plane,
				glm::vec4(light.position, 1.0f));
			if (signedDistance < -proxy.radius * normalLength) {
				proxy.classification = PointLightScreenProxyProperty::Outside;
				proxy.fallbackReason = PointLightScreenProxyProperty::None;
				proxy.pixelRect = {};
				proxy.coverageRatio = 0.0;
				return proxy;
			}
		}

		const glm::vec3 cameraDelta = light.position - cameraPosition;
		if (glm::dot(cameraDelta, cameraDelta) <= proxy.radiusSquared) {
			return makeFullscreenFallback(
				PointLightScreenProxyProperty::CameraInside);
		}

		const glm::vec3 viewCenter = glm::vec3(
			view * glm::vec4(light.position, 1.0f));
		const float depth = -viewCenter.z;
		constexpr float kNearPlane = 0.1f;
		if (!std::isfinite(depth) || depth - proxy.radius <= kNearPlane) {
			return makeFullscreenFallback(
				PointLightScreenProxyProperty::NearPlaneIntersection);
		}

		const double d = static_cast<double>(depth);
		const double radius = static_cast<double>(proxy.radius);
		const double denominator = d * d - radius * radius;
		if (!std::isfinite(denominator) || denominator <= 1e-12) {
			return makeFullscreenFallback(
				PointLightScreenProxyProperty::InvalidProjection);
		}
		auto tangentBounds = [&](double center, float projectionScale,
			double& minimum, double& maximum) {
			const double rootTerm = center * center + d * d - radius * radius;
			if (!std::isfinite(rootTerm) || rootTerm < 0.0) return false;
			const double root = std::sqrt((std::max)(0.0, rootTerm));
			const double first = static_cast<double>(projectionScale) *
				(center * d - radius * root) / denominator;
			const double second = static_cast<double>(projectionScale) *
				(center * d + radius * root) / denominator;
			if (!std::isfinite(first) || !std::isfinite(second)) return false;
			minimum = (std::min)(first, second);
			maximum = (std::max)(first, second);
			return true;
		};

		double minX = 0.0;
		double maxX = 0.0;
		double minY = 0.0;
		double maxY = 0.0;
		if (!tangentBounds(viewCenter.x, projection[0][0], minX, maxX) ||
			!tangentBounds(viewCenter.y, projection[1][1], minY, maxY)) {
			return makeFullscreenFallback(
				PointLightScreenProxyProperty::InvalidProjection);
		}
		if (maxX < -1.0 || minX > 1.0 || maxY < -1.0 || minY > 1.0) {
			proxy.classification = PointLightScreenProxyProperty::Outside;
			proxy.fallbackReason = PointLightScreenProxyProperty::None;
			proxy.pixelRect = {};
			proxy.coverageRatio = 0.0;
			return proxy;
		}

		minX = (std::max)(-1.0, minX);
		maxX = (std::min)(1.0, maxX);
		minY = (std::max)(-1.0, minY);
		maxY = (std::min)(1.0, maxY);
		constexpr int kGuardPixels = 1;
		const int x0 = (std::max)(0, (std::min)(width,
			static_cast<int>(std::floor((minX * 0.5 + 0.5) * width)) -
				kGuardPixels));
		const int x1 = (std::max)(0, (std::min)(width,
			static_cast<int>(std::ceil((maxX * 0.5 + 0.5) * width)) +
				kGuardPixels));
		const int y0 = (std::max)(0, (std::min)(height,
			static_cast<int>(std::floor((minY * 0.5 + 0.5) * height)) -
				kGuardPixels));
		const int y1 = (std::max)(0, (std::min)(height,
			static_cast<int>(std::ceil((maxY * 0.5 + 0.5) * height)) +
				kGuardPixels));
		if (x1 <= x0 || y1 <= y0) {
			proxy.classification = PointLightScreenProxyProperty::Outside;
			proxy.fallbackReason = PointLightScreenProxyProperty::None;
			proxy.pixelRect = {};
			proxy.coverageRatio = 0.0;
			return proxy;
		}

		proxy.classification = PointLightScreenProxyProperty::ConservativeRect;
		proxy.fallbackReason = PointLightScreenProxyProperty::None;
		proxy.pixelRect = { x0, y0, x1 - x0, y1 - y0 };
		proxy.coverageRatio = static_cast<double>(proxy.pixelRect.Area()) /
			static_cast<double>(viewportArea);
		return proxy;
	}
}

void DeferRenderPass::Init(int width, int height)
{
	m_ssao.Init(width, height);
}

FBOAttributes DeferRenderPass::BuildAttributesFromSystemProperties()
{
	// Defer pass output is the lit HDR color buffer (plus optional bloom buffer).
	FBOAttributes attr = FramebuffersManager::GenCurrentAttr();
	attr.textureAttrs.clear();
	attr.textureAttrs.push_back({ GL_TEXTURE_2D, GL_RGBA16F, GL_RGBA, GL_FLOAT });
	if (attr.isBloom) {
		attr.textureAttrs.push_back({ GL_TEXTURE_2D, GL_RGBA16F, GL_RGBA, GL_FLOAT });
	}
	return attr;
}

FBOAttributes DeferRenderPass::BuildGBufferAttributesFromSystemProperties() const
{
	// GBuffer: position / normal / albedo / material parameters / emissive.
	FBOAttributes attr = FramebuffersManager::GenCurrentAttr();
	attr.aaType = AntiAliasManager::AntiAliasType::Default;
	attr.isDefer = true;
	attr.isBloom = false;
	attr.isGamma = false;
	attr.hasDepthTexture = PositionReconstructionEnabled();
	attr.textureAttrs.clear();
	if (!PositionReconstructionEnabled()) {
		attr.textureAttrs.push_back({ GL_TEXTURE_2D, GL_RGBA16F, GL_RGBA, GL_FLOAT });    // gPosition (rgb: world pos, a: depth)
	}
	attr.textureAttrs.push_back({ GL_TEXTURE_2D, GL_RGB16F, GL_RGB, GL_FLOAT });          // gNormal
	attr.textureAttrs.push_back({ GL_TEXTURE_2D, GL_RGB, GL_RGB, GL_UNSIGNED_BYTE });     // gAlbedoSpec
	attr.textureAttrs.push_back({ GL_TEXTURE_2D, GL_RGBA16F, GL_RGBA, GL_FLOAT });        // gMaterial
	attr.textureAttrs.push_back({ GL_TEXTURE_2D, GL_RGB16F, GL_RGB, GL_FLOAT });          // gEmissive
	return attr;
}

bool DeferRenderPass::UsesPositionReconstruction() const
{
	return PositionReconstructionEnabled();
}

int DeferRenderPass::GetPositionAttachmentIndex() const
{
	return CurrentGBufferLayout().position;
}

int DeferRenderPass::GetNormalAttachmentIndex() const
{
	return CurrentGBufferLayout().normal;
}

int DeferRenderPass::GetAlbedoAttachmentIndex() const
{
	return CurrentGBufferLayout().albedo;
}

int DeferRenderPass::GetMaterialAttachmentIndex() const
{
	return CurrentGBufferLayout().material;
}

int DeferRenderPass::GetEmissiveAttachmentIndex() const
{
	return CurrentGBufferLayout().emissive;
}

void DeferRenderPass::ConfigurePositionSource(
	Shader& shader,
	unsigned int textureSlot,
	const glm::mat4& inverseProjection,
	const glm::mat4& inverseView) const
{
	const bool reconstruct = PositionReconstructionEnabled();
	shader.setBool("reconstructPosition", reconstruct);
	GLState::ActiveTexture(GL_TEXTURE0 + textureSlot);
	if (reconstruct) {
		GLState::BindTexture(GL_TEXTURE_2D, m_gbufferFBO->depthTextureID);
		shader.setInt("gDepth", textureSlot);
		shader.setMat4("inverseProjection", inverseProjection);
		shader.setMat4("inverseView", inverseView);
	}
	else {
		GLState::BindTexture(
			GL_TEXTURE_2D,
			m_gbufferFBO->textureIDs[CurrentGBufferLayout().position]);
		shader.setInt("gPosition", textureSlot);
	}
}

void DeferRenderPass::BindGBufferTextures(
	Shader& shader,
	unsigned int& textureSlot,
	const glm::mat4& inverseProjection,
	const glm::mat4& inverseView) const
{
	const GBufferLayout layout = CurrentGBufferLayout();
	if (!m_gbufferFBO ||
		m_gbufferFBO->textureIDs.size() < layout.colorAttachmentCount) return;

	ConfigurePositionSource(
		shader,
		textureSlot++,
		inverseProjection,
		inverseView);

	GLState::ActiveTexture(GL_TEXTURE0 + textureSlot);
	GLState::BindTexture(GL_TEXTURE_2D, m_gbufferFBO->textureIDs[layout.normal]);
	shader.setInt("gNormal", textureSlot++);

	GLState::ActiveTexture(GL_TEXTURE0 + textureSlot);
	GLState::BindTexture(GL_TEXTURE_2D, m_gbufferFBO->textureIDs[layout.albedo]);
	shader.setInt("gAlbedoSpec", textureSlot++);

	GLState::ActiveTexture(GL_TEXTURE0 + textureSlot);
	GLState::BindTexture(GL_TEXTURE_2D, m_gbufferFBO->textureIDs[layout.material]);
	shader.setInt("gMaterial", textureSlot++);

	GLState::ActiveTexture(GL_TEXTURE0 + textureSlot);
	GLState::BindTexture(GL_TEXTURE_2D, m_gbufferFBO->textureIDs[layout.emissive]);
	shader.setInt("gEmissive", textureSlot++);
}

void DeferRenderPass::DrawPointLightVolumesDeferred(
	Scene* scene,
	const glm::mat4& inverseProjection,
	const glm::mat4& inverseView)
{
	PERF_CPU_SCOPE("Deferred Point Lights");
	PERF_GPU_SCOPE("Deferred Point Lights");
	const GBufferLayout layout = CurrentGBufferLayout();
	if (!scene ||
		!m_gbufferFBO ||
		m_gbufferFBO->textureIDs.size() < layout.colorAttachmentCount) return;

	auto& properties = SystemProperties::GetInstance();
	const int renderMode = properties.POINT_LIGHT_RENDER_MODE;
	const bool gridPath = PointLightRenderProperty::UsesGrid(renderMode);
	const bool screenPath =
		PointLightRenderProperty::UsesScreenDraw(renderMode);
	auto defaultShader = ShaderManager::GetInstance().GetShader(
		ShaderManager::Default);
	auto lightVolumeShader = ShaderManager::GetInstance().GetShader(
		ShaderManager::LightVolume);
	auto lightScreenShader = ShaderManager::GetInstance().GetShader(
		ShaderManager::LightVolumeFullscreen);
	auto lightGridShader = ShaderManager::GetInstance().GetShader(
		ShaderManager::PointLightGrid);
	if ((!screenPath && (!defaultShader || !lightVolumeShader)) ||
		(screenPath && !gridPath && !lightScreenShader) ||
		(gridPath && !lightGridShader)) return;
	const bool renderDocMarkers = scene->GetPointLightRenderDocMarkers();
	PointLightDebugScope pointLightPhaseMarker(
		renderDocMarkers,
		"PointLightStress/PointLightPhase");

	const FBO* ssaoFBO = properties.SSAO ? m_ssao.GetOutputFBO() : nullptr;
	const bool useSSAOInLighting = ssaoFBO && !ssaoFBO->textureIDs.empty();
	auto configurePointLightShader = [&](Shader& shader) {
		shader.use();
		if (scene->camera_ptr) {
			shader.setVec3("viewPos", scene->camera_ptr->cameraPos);
		}
		ConfigurePositionSource(
			shader,
			0,
			inverseProjection,
			inverseView);
		GLState::ActiveTexture(GL_TEXTURE1);
		GLState::BindTexture(
			GL_TEXTURE_2D,
			m_gbufferFBO->textureIDs[layout.normal]);
		shader.setInt("gNormal", 1);
		GLState::ActiveTexture(GL_TEXTURE2);
		GLState::BindTexture(
			GL_TEXTURE_2D,
			m_gbufferFBO->textureIDs[layout.albedo]);
		shader.setInt("gAlbedoSpec", 2);
		GLState::ActiveTexture(GL_TEXTURE3);
		GLState::BindTexture(
			GL_TEXTURE_2D,
			m_gbufferFBO->textureIDs[layout.material]);
		shader.setInt("gMaterial", 3);
		shader.setBool("useSSAO", useSSAOInLighting);
		if (useSSAOInLighting) {
			GLState::ActiveTexture(GL_TEXTURE5);
			GLState::BindTexture(GL_TEXTURE_2D, ssaoFBO->textureIDs[0]);
			shader.setInt("ssaoMap", 5);
		}
	};
	auto configurePointLight = [&](Shader& shader, PointLight& pointLight,
		float radiusSquared, bool analyticPredicate) {
		shader.setVec3("pointLight.position", pointLight.position);
		shader.setFloat("pointLight.constant", pointLight.constant);
		shader.setFloat("pointLight.linear", pointLight.linear);
		shader.setFloat("pointLight.quadratic", pointLight.quadratic);
		shader.setVec3("pointLight.ambient", pointLight.ambient);
		shader.setVec3("pointLight.diffuse", pointLight.diffuse);
		shader.setVec3("pointLight.specular", pointLight.specular);
		shader.setFloat("pointLight.far_plane", pointLight.far);
		shader.setBool("useAnalyticRadiusPredicate", analyticPredicate);
		if (analyticPredicate) {
			shader.setFloat("pointLightRadiusSquared", radiusSquared);
		}
		FBO* pointShadowFBO = pointLight.shadowFBO;
		const bool pointShadowSampleable =
			pointLight.useShadowMap &&
			pointLight.shadowCache.IsSampleable(pointShadowFBO);
		GLState::ActiveTexture(GL_TEXTURE4);
		GLState::BindTexture(
			GL_TEXTURE_CUBE_MAP,
			pointShadowSampleable
				? pointShadowFBO->textureIDs[0]
				: 0);
		shader.setInt("pointLight.shadowCubeMap", 4);
		shader.setBool(
			"pointLight.useShadowMap",
			pointShadowSampleable);

		// Preserve the established per-light binding sequence. This keeps the
		// candidate-path comparison about pixel generation, not binding caches.
		ConfigurePositionSource(
			shader,
			0,
			inverseProjection,
			inverseView);
		GLState::ActiveTexture(GL_TEXTURE1);
		GLState::BindTexture(
			GL_TEXTURE_2D,
			m_gbufferFBO->textureIDs[layout.normal]);
		GLState::ActiveTexture(GL_TEXTURE2);
		GLState::BindTexture(
			GL_TEXTURE_2D,
			m_gbufferFBO->textureIDs[layout.albedo]);
		GLState::ActiveTexture(GL_TEXTURE3);
		GLState::BindTexture(
			GL_TEXTURE_2D,
			m_gbufferFBO->textureIDs[layout.material]);
		if (useSSAOInLighting) {
			GLState::ActiveTexture(GL_TEXTURE5);
			GLState::BindTexture(GL_TEXTURE_2D, ssaoFBO->textureIDs[0]);
		}
	};

	const std::uint64_t totalLights =
		static_cast<std::uint64_t>(scene->GetLightSource().pointLights.size());
	const std::uint64_t activeLights = static_cast<std::uint64_t>(
		std::count_if(
			scene->GetLightSource().pointLights.begin(),
			scene->GetLightSource().pointLights.end(),
			[](const PointLight& light) { return light.m_active; }));
	const int viewportWidth = (std::max)(1, properties.SCREEN_WIDTH);
	const int viewportHeight = (std::max)(1, properties.SCREEN_HEIGHT);
	const std::uint64_t viewportArea =
		static_cast<std::uint64_t>(viewportWidth) *
		static_cast<std::uint64_t>(viewportHeight);
	std::vector<PointLightScreenProxy> proxies;
	std::vector<PointLightScreenProxy*> submittedProxies;
	std::uint64_t boundsRect = 0;
	std::uint64_t boundsOutside = 0;
	std::uint64_t boundsFullscreenFallback = 0;
	std::uint64_t fallbackCameraInside = 0;
	std::uint64_t fallbackNearPlane = 0;
	std::uint64_t fallbackInvalid = 0;
	std::uint64_t rectPixelArea = 0;
	if (PointLightRenderProperty::RequiresBounds(renderMode)) {
		PERF_CPU_SCOPE("Point Light Bounds");
		const glm::mat4 view = scene->camera_ptr
			? scene->camera_ptr->GetViewMatrix()
			: glm::mat4(1.0f);
		const glm::mat4 projection = CurrentProjection(scene);
		const glm::vec3 cameraPosition = scene->camera_ptr
			? scene->camera_ptr->cameraPos
			: glm::vec3(0.0f);
		proxies.reserve(static_cast<std::size_t>(activeLights));
		std::size_t sourceIndex = 0;
		for (PointLight& pointLight : scene->GetLightSource().pointLights) {
			if (!pointLight.GetActiveStatus()) {
				++sourceIndex;
				continue;
			}
			const float radius = ComputePointLightStencilVolumeRadius(
				pointLight.constant,
				pointLight.linear,
				pointLight.quadratic,
				pointLight.diffuse,
				properties.LIGHT_VOLUME_CUTOFF_SCALE,
				properties.LIGHT_VOLUME_RADIUS_SCALE);
			proxies.push_back(BuildPointLightScreenProxy(
				pointLight,
				sourceIndex,
				view,
				projection,
				cameraPosition,
				viewportWidth,
				viewportHeight,
				radius));
			PointLightScreenProxy& proxy = proxies.back();
			rectPixelArea += proxy.pixelRect.Area();
			switch (proxy.classification) {
			case PointLightScreenProxyProperty::Outside:
				++boundsOutside;
				break;
			case PointLightScreenProxyProperty::ConservativeRect:
				++boundsRect;
				break;
			case PointLightScreenProxyProperty::FullscreenFallback:
				++boundsFullscreenFallback;
				break;
			default:
				break;
			}
			switch (proxy.fallbackReason) {
			case PointLightScreenProxyProperty::CameraInside:
				++fallbackCameraInside;
				break;
			case PointLightScreenProxyProperty::NearPlaneIntersection:
				++fallbackNearPlane;
				break;
			case PointLightScreenProxyProperty::InvalidRadius:
			case PointLightScreenProxyProperty::InvalidProjection:
				++fallbackInvalid;
				break;
			default:
				break;
			}
			++sourceIndex;
		}

		if (properties.POINT_LIGHT_BOUNDS_TELEMETRY_REQUESTED &&
			!properties.POINT_LIGHT_BOUNDS_TELEMETRY_EXECUTED) {
			auto& telemetry = properties.POINT_LIGHT_BOUNDS_TELEMETRY;
			telemetry.clear();
			telemetry.reserve(proxies.size());
			for (const PointLightScreenProxy& proxy : proxies) {
				PointLightScreenProxyTelemetry record;
				record.stableLightId = proxy.stableLightId;
				record.sourceIndex = proxy.sourceIndex;
				record.radius = proxy.radius;
				record.classification = proxy.classification;
				record.fallbackReason = proxy.fallbackReason;
				record.rectX = proxy.pixelRect.x;
				record.rectY = proxy.pixelRect.y;
				record.rectWidth = proxy.pixelRect.width;
				record.rectHeight = proxy.pixelRect.height;
				record.coverageRatio = proxy.coverageRatio;
				telemetry.push_back(record);
			}
			properties.POINT_LIGHT_BOUNDS_TELEMETRY_EXECUTED = true;
		}
	}

	if (PointLightRenderProperty::RequiresBounds(renderMode)) {
		PERF_CPU_SCOPE("Point Light Selector");
		for (PointLightScreenProxy& proxy : proxies) {
			if (properties.POINT_LIGHT_OFFSCREEN_CULLING &&
				proxy.classification == PointLightScreenProxyProperty::Outside) {
				continue;
			}
			submittedProxies.push_back(&proxy);
		}
	}
	const std::uint64_t submittedLights =
		PointLightRenderProperty::RequiresBounds(renderMode)
			? static_cast<std::uint64_t>(submittedProxies.size())
			: activeLights;
	const std::uint64_t culledLights = activeLights - submittedLights;
	PerformanceProfiler::GetInstance().SetDeferredPointLightStats(
		totalLights,
		activeLights,
		submittedLights,
		culledLights);

	const bool previousScissorEnabled =
		PointLightRenderProperty::RequiresBounds(renderMode)
			? GLState::IsEnabled(GL_SCISSOR_TEST)
			: false;
	const std::array<GLint, 4> previousScissorBox =
		PointLightRenderProperty::RequiresBounds(renderMode)
			? GLState::GetScissorBox()
			: std::array<GLint, 4>{ 0, 0, viewportWidth, viewportHeight };
	std::uint64_t pointLightClearPixelArea = 0;
	std::uint64_t volumeCount = 0;
	std::uint64_t screenCount = 0;
	std::uint64_t stencilDraws = 0;
	std::uint64_t lightingVolumeDraws = 0;
	std::uint64_t screenDraws = 0;

	const bool coalescedStencilClears =
		properties.POINT_LIGHT_STENCIL_CLEAR_MODE ==
		PointLightStencilClearProperty::CoalescedNPlusOne;
	if (!screenPath) {
		PERF_CPU_SCOPE("Point Light Volume CPU");
		PERF_GPU_SCOPE("Point Light Volume GPU");
		configurePointLightShader(*lightVolumeShader);
		const bool analyticVolume =
			PointLightRenderProperty::UsesAnalyticVolume(renderMode);
		const bool scissoredVolume =
			PointLightRenderProperty::UsesScissor(renderMode);
		// The OBJ's minimum face-plane radius is 0.99043723, requiring
		// 1.0096551x to contain the ideal unit sphere. Use 1.02x so the
		// rasterized proxy also has a small precision/raster-rule guard; the
		// fragment predicate restores the exact analytic radius.
		constexpr float kConservativeSphereProxyScale = 1.02f;
		GLState::Enable(GL_STENCIL_TEST);
		glBlendEquation(GL_FUNC_ADD);
		GLState::Enable(GL_BLEND);
		GLState::BlendFunc(GL_ONE, GL_ONE);
		bool needsInitialStencilClear = submittedLights > 0;
		std::size_t lightOrdinal = 0;
		auto renderVolumeLight = [&](PointLight& pointLight,
			float radius, float radiusSquared,
			const PointLightPixelRect* pixelRect) {
		char lightMarkerName[64];
		const char* lightMarkerLabel = nullptr;
		if (renderDocMarkers) {
			std::snprintf(
				lightMarkerName,
				sizeof(lightMarkerName),
				"PointLightStress/Light[%04zu]",
				lightOrdinal);
			lightMarkerLabel = lightMarkerName;
		}
		++lightOrdinal;
		PointLightDebugScope lightMarker(
			renderDocMarkers,
			lightMarkerLabel);
		const glm::vec3 savedScale = pointLight.scale;
		pointLight.SetScale(glm::vec3(
			analyticVolume
				? radius * kConservativeSphereProxyScale
				: radius));

		GLState::Disable(GL_BLEND);
		GLState::StencilMask(0xFF);
		if (!coalescedStencilClears || needsInitialStencilClear) {
			if (scissoredVolume && coalescedStencilClears &&
				needsInitialStencilClear) {
				GLState::Disable(GL_SCISSOR_TEST);
			}
			else if (scissoredVolume && pixelRect) {
				GLState::Scissor(
					pixelRect->x,
					pixelRect->y,
					pixelRect->width,
					pixelRect->height);
				GLState::Enable(GL_SCISSOR_TEST);
			}
			PointLightDebugScope marker(
				renderDocMarkers,
				coalescedStencilClears
					? "PointLightStress/StencilClearInitial"
					: "PointLightStress/StencilClearBefore");
			glClear(GL_STENCIL_BUFFER_BIT);
			PerformanceProfiler::GetInstance().RecordStencilClear(true);
			pointLightClearPixelArea +=
				(scissoredVolume && !needsInitialStencilClear && pixelRect)
					? pixelRect->Area()
					: viewportArea;
			needsInitialStencilClear = false;
		}
		if (scissoredVolume && pixelRect) {
			GLState::Scissor(
				pixelRect->x,
				pixelRect->y,
				pixelRect->width,
				pixelRect->height);
			GLState::Enable(GL_SCISSOR_TEST);
		}
		GLState::ColorMask(false, false, false, false);
		GLState::DepthMask(false);
		GLState::Enable(GL_DEPTH_TEST);
		GLState::DepthFunc(GL_LESS);
		GLState::Disable(GL_CULL_FACE);
		GLState::StencilFunc(GL_ALWAYS, 0, 0xFF);
		GLState::StencilOpSeparate(GL_BACK, GL_KEEP, GL_INCR_WRAP, GL_KEEP);
		GLState::StencilOpSeparate(GL_FRONT, GL_KEEP, GL_DECR_WRAP, GL_KEEP);

		defaultShader->use();
		defaultShader->setMat4("model", pointLight.getModelMatrix());
		{
			PointLightDebugScope marker(
				renderDocMarkers,
				"PointLightStress/StencilVolumeDraw");
			pointLight.DrawGeometry();
			++stencilDraws;
		}

		lightVolumeShader->use();
		GLState::Enable(GL_BLEND);
		GLState::BlendFunc(GL_ONE, GL_ONE);
		GLState::ColorMask(true, true, true, true);
		GLState::StencilFunc(GL_NOTEQUAL, 0, 0xFF);
		GLState::StencilMask(0x00);
		GLState::Enable(GL_CULL_FACE);
		GLState::CullFace(GL_FRONT);
		GLState::DepthFunc(GL_GEQUAL);

		configurePointLight(
			*lightVolumeShader,
			pointLight,
			radiusSquared,
			analyticVolume);
		lightVolumeShader->setMat4("model", pointLight.getModelMatrix());

		{
			PointLightDebugScope marker(
				renderDocMarkers,
				"PointLightStress/LightingVolumeDraw");
			pointLight.DrawGeometry();
			++lightingVolumeDraws;
		}

		pointLight.SetScale(savedScale);
		GLState::StencilMask(0xFF);
		{
			PointLightDebugScope marker(
				renderDocMarkers,
				"PointLightStress/StencilClearAfter");
			glClear(GL_STENCIL_BUFFER_BIT);
			PerformanceProfiler::GetInstance().RecordStencilClear(true);
			pointLightClearPixelArea +=
				(scissoredVolume && pixelRect)
					? pixelRect->Area()
					: viewportArea;
		}
		GLState::StencilMask(0x00);
		GLState::CullFace(GL_BACK);
		GLState::DepthFunc(GL_LESS);
		++volumeCount;
		};

		if (PointLightRenderProperty::RequiresBounds(renderMode)) {
			for (PointLightScreenProxy* proxy : submittedProxies) {
				renderVolumeLight(
					*proxy->light,
					proxy->radius,
					proxy->radiusSquared,
					scissoredVolume ? &proxy->pixelRect : nullptr);
			}
		}
		else {
			for (PointLight& pointLight : scene->GetLightSource().pointLights) {
				if (!pointLight.GetActiveStatus()) continue;
				const float radius = ComputePointLightStencilVolumeRadius(
					pointLight.constant,
					pointLight.linear,
					pointLight.quadratic,
					pointLight.diffuse,
					properties.LIGHT_VOLUME_CUTOFF_SCALE,
					properties.LIGHT_VOLUME_RADIUS_SCALE);
				renderVolumeLight(
					pointLight,
					radius,
					radius * radius,
					nullptr);
			}
		}
	}
	else if (!gridPath) {
		PERF_CPU_SCOPE("Point Light Screen CPU");
		PERF_GPU_SCOPE("Point Light Screen GPU");
		configurePointLightShader(*lightScreenShader);
		lightScreenShader->setBool("useAnalyticRadiusPredicate", true);
		glBlendEquation(GL_FUNC_ADD);
		GLState::Enable(GL_BLEND);
		GLState::BlendFunc(GL_ONE, GL_ONE);
		GLState::ColorMask(true, true, true, true);
		GLState::DepthMask(false);
		GLState::Disable(GL_DEPTH_TEST);
		GLState::Disable(GL_STENCIL_TEST);
		GLState::Disable(GL_CULL_FACE);
		const bool scissoredScreen =
			renderMode == PointLightRenderProperty::AnalyticScreen;
		std::size_t lightOrdinal = 0;
		for (PointLightScreenProxy* proxy : submittedProxies) {
			char lightMarkerName[64];
			const char* lightMarkerLabel = nullptr;
			if (renderDocMarkers) {
				std::snprintf(
					lightMarkerName,
					sizeof(lightMarkerName),
					"PointLightStress/Light[%04zu]",
					lightOrdinal);
				lightMarkerLabel = lightMarkerName;
			}
			++lightOrdinal;
			PointLightDebugScope lightMarker(
				renderDocMarkers,
				lightMarkerLabel);
			if (scissoredScreen) {
				GLState::Scissor(
					proxy->pixelRect.x,
					proxy->pixelRect.y,
					proxy->pixelRect.width,
					proxy->pixelRect.height);
				GLState::Enable(GL_SCISSOR_TEST);
			}
			else {
				GLState::Disable(GL_SCISSOR_TEST);
			}
			lightScreenShader->use();
			configurePointLight(
				*lightScreenShader,
				*proxy->light,
				proxy->radiusSquared,
				true);
			GLState::BindVertexArray(globalVAOs.quadVAO);
			{
				PointLightDebugScope marker(
					renderDocMarkers,
					"PointLightStress/LightingScreenDraw");
				PerformanceProfiler::GetInstance().RecordDraw(GL_TRIANGLES, 6);
				glDrawArrays(GL_TRIANGLES, 0, 6);
				++screenDraws;
			}
			++screenCount;
		}
	}
	else {
		const int legacySliceCount =
			renderMode == PointLightRenderProperty::Cluster16 ? 16 : 1;
		const int sliceCount = properties.POINT_LIGHT_GRID_SLICE_COUNT_EXPLICIT
			? properties.POINT_LIGHT_GRID_SLICE_COUNT
			: legacySliceCount;
		const bool forceRebuild =
			properties.POINT_LIGHT_GRID_UPDATE_MODE ==
			PointLightGridUpdateProperty::RebuildEveryFrame;
		const glm::mat4 view = scene->camera_ptr
			? scene->camera_ptr->GetViewMatrix()
			: glm::mat4(1.0f);
		const glm::mat4 projection = CurrentProjection(scene);
		constexpr float nearPlane = 0.1f;
		constexpr float farPlane = 100.0f;
		const bool prepared = m_pointLightGrid.Prepare(
			*scene,
			view,
			projection,
			viewportWidth,
			viewportHeight,
			sliceCount,
			forceRebuild,
			nearPlane,
			farPlane);
		const PointLightGridRuntimeStats& gridStats =
			m_pointLightGrid.GetStats();
		auto& telemetry = properties.POINT_LIGHT_GRID_TELEMETRY;
		telemetry.valid = gridStats.valid;
		telemetry.clustered = gridStats.clustered;
		telemetry.rebuiltThisFrame = gridStats.rebuiltThisFrame;
		telemetry.cacheHit = gridStats.cacheHit;
		telemetry.overflow = gridStats.overflow;
		telemetry.tileSize = gridStats.tileSize;
		telemetry.sliceCount = gridStats.sliceCount;
		telemetry.tilesX = gridStats.tilesX;
		telemetry.tilesY = gridStats.tilesY;
		telemetry.logicalCells = gridStats.logicalCells;
		telemetry.nonEmptyCells = gridStats.nonEmptyCells;
		telemetry.lightCount = gridStats.lightCount;
		telemetry.totalIndices = gridStats.totalIndices;
		telemetry.maximumLightsPerCell = gridStats.maximumLightsPerCell;
		telemetry.averageLightsPerCell = gridStats.averageLightsPerCell;
		telemetry.metadataBytes = gridStats.metadataBytes;
		telemetry.indexBytes = gridStats.indexBytes;
		telemetry.lightBytes = gridStats.lightBytes;
		telemetry.residentBytes = gridStats.residentBytes;
		telemetry.uploadedBytesThisFrame = gridStats.uploadedBytesThisFrame;
		telemetry.buildCount = gridStats.buildCount;
		telemetry.uploadCount = gridStats.uploadCount;
		telemetry.cacheHitCount = gridStats.cacheHitCount;
		telemetry.inputSignature = gridStats.inputSignature;
		telemetry.csrSignature = gridStats.csrSignature;
		telemetry.maxTextureBufferTexels = gridStats.maxTextureBufferTexels;
		telemetry.error = gridStats.error;
		if (prepared && m_pointLightGrid.HasLights()) {
			PERF_CPU_SCOPE("Point Light Grid Lighting CPU");
			PERF_GPU_SCOPE("Point Light Grid Lighting GPU");
			lightGridShader->use();
			if (scene->camera_ptr) {
				lightGridShader->setVec3("viewPos", scene->camera_ptr->cameraPos);
			}
			ConfigurePositionSource(
				*lightGridShader,
				0,
				inverseProjection,
				inverseView);
			GLState::ActiveTexture(GL_TEXTURE1);
			GLState::BindTexture(
				GL_TEXTURE_2D,
				m_gbufferFBO->textureIDs[layout.normal]);
			lightGridShader->setInt("gNormal", 1);
			GLState::ActiveTexture(GL_TEXTURE2);
			GLState::BindTexture(
				GL_TEXTURE_2D,
				m_gbufferFBO->textureIDs[layout.albedo]);
			lightGridShader->setInt("gAlbedoSpec", 2);
			GLState::ActiveTexture(GL_TEXTURE3);
			GLState::BindTexture(
				GL_TEXTURE_2D,
				m_gbufferFBO->textureIDs[layout.material]);
			lightGridShader->setInt("gMaterial", 3);
			lightGridShader->setBool("useSSAO", useSSAOInLighting);
			if (useSSAOInLighting) {
				GLState::ActiveTexture(GL_TEXTURE5);
				GLState::BindTexture(GL_TEXTURE_2D, ssaoFBO->textureIDs[0]);
				lightGridShader->setInt("ssaoMap", 5);
			}
			lightGridShader->setMat4("viewMatrix", view);
			lightGridShader->setFloat("gridNearPlane", nearPlane);
			lightGridShader->setFloat("gridFarPlane", farPlane);
			m_pointLightGrid.Bind(*lightGridShader, 6);

			glBlendEquation(GL_FUNC_ADD);
			GLState::Enable(GL_BLEND);
			GLState::BlendFunc(GL_ONE, GL_ONE);
			GLState::ColorMask(true, true, true, true);
			GLState::DepthMask(false);
			GLState::Disable(GL_DEPTH_TEST);
			GLState::Disable(GL_STENCIL_TEST);
			GLState::Disable(GL_CULL_FACE);
			GLState::Disable(GL_SCISSOR_TEST);
			GLState::BindVertexArray(globalVAOs.quadVAO);
			{
				char gridMarkerName[96] = {};
				std::snprintf(
					gridMarkerName,
					sizeof(gridMarkerName),
					"PointLightStress/GridS%dLightingDraw",
					sliceCount);
				PointLightDebugScope marker(renderDocMarkers, gridMarkerName);
				PerformanceProfiler::GetInstance().RecordDraw(GL_TRIANGLES, 6);
				glDrawArrays(GL_TRIANGLES, 0, 6);
				++screenDraws;
			}
			screenCount = activeLights;
		}
	}

	PerformanceProfiler::GetInstance().SetDeferredPointLightPathStats(
		boundsRect,
		boundsOutside,
		boundsFullscreenFallback,
		fallbackCameraInside,
		fallbackNearPlane,
		fallbackInvalid,
		volumeCount,
		screenCount,
		stencilDraws,
		lightingVolumeDraws,
		screenDraws,
		rectPixelArea,
		pointLightClearPixelArea);

	if (PointLightRenderProperty::RequiresBounds(renderMode)) {
		GLState::Scissor(
			previousScissorBox[0],
			previousScissorBox[1],
			previousScissorBox[2],
			previousScissorBox[3]);
		if (previousScissorEnabled) GLState::Enable(GL_SCISSOR_TEST);
		else GLState::Disable(GL_SCISSOR_TEST);
	}

	GLState::Disable(GL_BLEND);
	GLState::Disable(GL_STENCIL_TEST);
	GLState::StencilMask(0xFF);
	GLState::DepthMask(true);
	GLState::Enable(GL_DEPTH_TEST);
	GLState::DepthFunc(GL_LESS);
	GLState::Disable(GL_CULL_FACE);

	if (properties.POINT_LIGHT_STENCIL_LIFECYCLE_CHECK &&
		!properties.POINT_LIGHT_STENCIL_LIFECYCLE_CHECK_EXECUTED) {
		const int width = (std::max)(1, properties.SCREEN_WIDTH);
		const int height = (std::max)(1, properties.SCREEN_HEIGHT);
		const std::size_t pixelCount =
			static_cast<std::size_t>(width) *
			static_cast<std::size_t>(height);
		std::vector<unsigned char> stencilPixels(pixelCount);
		glReadPixels(
			0,
			0,
			width,
			height,
			GL_STENCIL_INDEX,
			GL_UNSIGNED_BYTE,
			stencilPixels.data());
		properties.POINT_LIGHT_STENCIL_NONZERO_PIXELS =
			static_cast<std::uint64_t>(std::count_if(
				stencilPixels.begin(),
				stencilPixels.end(),
				[](unsigned char value) { return value != 0; }));
		properties.POINT_LIGHT_STENCIL_LIFECYCLE_CHECK_EXECUTED = true;
	}
}

void DeferRenderPass::Render(Scene* scene, const FBO* inputFBO)
{
	PERF_CPU_SCOPE("Deferred Pass");
	PERF_GPU_SCOPE("Deferred Pass");
	if (!scene) return;

	UpdateFBOFromSystemProperties();
	if (!m_outputFBO) return;

	auto& fbMgr = FramebuffersManager::GetInstance();
	FBOAttributes gbufferAttr = BuildGBufferAttributesFromSystemProperties();
	if (!m_gbufferFBO || !(m_gbufferFBO->attr == gbufferAttr)) {
		fbMgr.ReleaseFBO(m_gbufferFBO);
		m_gbufferFBO = fbMgr.GetFBO(gbufferAttr);
		if (m_gbufferFBO) {
			m_gbufferFBO->passName = "DeferRenderPass_GBuffer";
		}
		fbMgr.TrimUnusedFBOs();
	}
	const GBufferLayout layout = CurrentGBufferLayout();
	if (!m_gbufferFBO ||
		!m_gbufferFBO->IsComplete() ||
		m_gbufferFBO->textureIDs.size() < layout.colorAttachmentCount ||
		(PositionReconstructionEnabled() &&
			m_gbufferFBO->depthTextureID == 0)) return;

	auto& properties = SystemProperties::GetInstance();
	const glm::mat4 currentView = scene->camera_ptr
		? scene->camera_ptr->GetViewMatrix()
		: glm::mat4(1.0f);
	const bool reconstructPosition = PositionReconstructionEnabled();
	const glm::mat4 inverseProjection = reconstructPosition
		? glm::inverse(CurrentProjection(scene))
		: glm::mat4(1.0f);
	const glm::mat4 inverseView = reconstructPosition
		? glm::inverse(currentView)
		: glm::mat4(1.0f);
	scene->PrepareRenderData();
	scene->DrawShadowMap();

	// 1) Geometry pass: write opaque meshes into GBuffer.
	{
		PERF_CPU_SCOPE("G-Buffer Geometry");
		PERF_GPU_SCOPE("G-Buffer Geometry");
		GLState::Disable(GL_BLEND);
		GLState::BindFramebuffer(GL_FRAMEBUFFER, m_gbufferFBO->framebufferID);
		GLState::Enable(GL_DEPTH_TEST);
		GLState::Enable(GL_STENCIL_TEST);
		GLState::DepthMask(true);
		glClearDepth(1.0);
		glClearStencil(0);
		GLState::StencilMask(0xFF);
		glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
		PerformanceProfiler::GetInstance().RecordStencilClear();
		GLState::StencilMask(0x00);
		// Explicit mode uses gPosition.a == 0 as its valid mask. Candidate
		// mode uses the untouched clear depth of 1.0 instead.
		const float clearValue[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		for (std::size_t attachment = 0;
			attachment < layout.colorAttachmentCount;
			++attachment) {
			glClearBufferfv(
				GL_COLOR,
				static_cast<GLint>(attachment),
				clearValue);
		}

		auto deferProcessShader = ShaderManager::GetInstance().GetShader(
			PositionReconstructionEnabled()
				? ShaderManager::DeferProcessReconstruct
				: ShaderManager::DeferProcess);
		if (!deferProcessShader) return;
		deferProcessShader->use();
		auto& opaqueList = scene->GetOpaqueMeshes();
		{
			MaterialBatchScope materialBatch;
			for (const auto& item : opaqueList) {
				if (!item.model || !item.mesh) continue;
				deferProcessShader->setMat4("model", item.modelMatrix);
				item.mesh->Draw(
					deferProcessShader.get(),
					item.shader && item.shader->shaderName == "pbr");
			}
		}
	}

	if (properties.SSAO) {
		m_ssao.Render(scene, m_gbufferFBO);
	}
	else if (m_ssao.GetOutputFBO()) {
		m_ssao.Destroy();
		fbMgr.TrimUnusedFBOs();
	}
	const FBO* ssaoFBO = properties.SSAO ? m_ssao.GetOutputFBO() : nullptr;
	const bool useSSAOInLighting = ssaoFBO && !ssaoFBO->textureIDs.empty();

	// Copy depth to output target so skybox / transparent passes can share it.
	{
		PERF_CPU_SCOPE("Depth/Stencil Copy");
		PERF_GPU_SCOPE("Depth/Stencil Copy");
		GLState::BindFramebuffer(GL_READ_FRAMEBUFFER, m_gbufferFBO->framebufferID);
		GLState::BindFramebuffer(GL_DRAW_FRAMEBUFFER, m_outputFBO->framebufferID);
		glBlitFramebuffer(
			0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT,
			0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT,
			GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST
		);
	}

	// 2) Lighting pass: GBuffer -> HDR（全屏 或 平行光全屏 + 点光源光体积）
	{
		PERF_CPU_SCOPE("Deferred Lighting");
		PERF_GPU_SCOPE("Deferred Lighting");
	GLState::BindFramebuffer(GL_FRAMEBUFFER, m_outputFBO->framebufferID);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	GLState::StencilMask(0xFF);
	glClearStencil(0);
	glClear(GL_STENCIL_BUFFER_BIT);
	PerformanceProfiler::GetInstance().RecordStencilClear();
	GLState::Disable(GL_DEPTH_TEST);

	// The PBR path includes one non-additive IBL/emissive contribution. Until the
	// light-volume shaders gain a dedicated ambient pass, use the correct
	// fullscreen path whenever a PBR material is present.
	const bool useLightVolumes = properties.LIGHT_VOLUME && !scene->UsesPbrMaterials();
	if (!useLightVolumes) {
		auto deferLightShader = ShaderManager::GetInstance().GetShader(ShaderManager::Defer);
		if (!deferLightShader) return;
		deferLightShader->use();
		scene->SetLightUniforms(*deferLightShader);
		unsigned int texSlot = scene->SetShadowMap(
			*deferLightShader,
			Scene::ShadowLightBinding::AllLights,
			9);
		if (scene->camera_ptr) {
			deferLightShader->setVec3("viewPos", scene->camera_ptr->cameraPos);
		}
		BindGBufferTextures(
			*deferLightShader,
			texSlot,
			inverseProjection,
			inverseView);
		deferLightShader->setBool("useSSAO", useSSAOInLighting);
		if (useSSAOInLighting) {
			GLState::ActiveTexture(GL_TEXTURE0 + texSlot);
			GLState::BindTexture(GL_TEXTURE_2D, ssaoFBO->textureIDs[0]);
			deferLightShader->setInt("ssaoMap", texSlot);
			++texSlot;
		}
		texSlot = scene->BindImageBasedLighting(*deferLightShader, texSlot);

		GLState::BindVertexArray(globalVAOs.quadVAO);
		PerformanceProfiler::GetInstance().RecordDraw(GL_TRIANGLES, 6);
		glDrawArrays(GL_TRIANGLES, 0, 6);
	} else {
		// 与 Scene::DrawDefferedModels 一致：平行光全屏；点光源用模板 + 球体裁剪像素（聚光灯此模式下不单独绘制）
		auto deferDirShader = ShaderManager::GetInstance().GetShader(ShaderManager::DeferDirLightVolume);
		if (!deferDirShader) return;
		deferDirShader->use();
		scene->SetLightUniforms(*deferDirShader);
		unsigned int texSlot = scene->SetShadowMap(
			*deferDirShader,
			Scene::ShadowLightBinding::DirectionalOnly,
			6);
		if (scene->camera_ptr) {
			deferDirShader->setVec3("viewPos", scene->camera_ptr->cameraPos);
		}
		BindGBufferTextures(
			*deferDirShader,
			texSlot,
			inverseProjection,
			inverseView);
		deferDirShader->setBool("useSSAO", useSSAOInLighting);
		if (useSSAOInLighting) {
			GLState::ActiveTexture(GL_TEXTURE0 + texSlot);
			GLState::BindTexture(GL_TEXTURE_2D, ssaoFBO->textureIDs[0]);
			deferDirShader->setInt("ssaoMap", texSlot);
			++texSlot;
		}

		GLState::BindVertexArray(globalVAOs.quadVAO);
		PerformanceProfiler::GetInstance().RecordDraw(GL_TRIANGLES, 6);
		glDrawArrays(GL_TRIANGLES, 0, 6);

		DrawPointLightVolumesDeferred(
			scene,
			inverseProjection,
			inverseView);
	}
	}

	// 3) Forward extras on top of deferred base.
	GLState::BindFramebuffer(GL_FRAMEBUFFER, m_outputFBO->framebufferID);
	GLState::Enable(GL_DEPTH_TEST);
	// Forward stage relies on USED_TEXTURE_NUM for per-material/skybox bindings.
	// Reset here to avoid stale texture unit growth across deferred lighting paths.
	properties.USED_TEXTURE_NUM = 0;
	GLState::ActiveTexture(GL_TEXTURE0);
	scene->DrawPointLights();
	if (scene->camera_ptr) {
		scene->DrawSkybox(scene->camera_ptr->GetViewMatrix());
	}
	scene->DrawTransparentModels();
	scene->DrawOutlines();

	GLState::BindFramebuffer(GL_FRAMEBUFFER, 0);
}

void DeferRenderPass::Destroy()
{
	m_pointLightGrid.Destroy();
	m_ssao.Destroy();
	FramebuffersManager::GetInstance().ReleaseFBO(m_gbufferFBO);
	m_gbufferFBO = nullptr;
	FramebuffersManager::GetInstance().ReleaseFBO(m_outputFBO);
	m_outputFBO = nullptr;
	m_hasAttr = false;
	FramebuffersManager::GetInstance().TrimUnusedFBOs();
}
