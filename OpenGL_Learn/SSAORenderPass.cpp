#include "SSAORenderPass.h"
#include "GLStateCache.h"
#include "Profiler.h"
#include "ShaderManager.h"

#include <algorithm>
#include <random>

namespace {
	bool ReconstructPositionFromDepth()
	{
		return SystemProperties::GetInstance().GBUFFER_POSITION_MODE ==
			GBufferPositionProperty::ReconstructFromDepth;
	}

	int GBufferNormalAttachment()
	{
		return ReconstructPositionFromDepth() ? 0 : 1;
	}
}

FBOAttributes SSAORenderPass::BuildAttributesFromSystemProperties()
{
	const bool halfRaw =
		SystemProperties::GetInstance().SSAO_MODE ==
		SSAOProperty::HalfRaw;
	return BuildAOAttributes(halfRaw);
}

FBOAttributes SSAORenderPass::BuildAOAttributes(bool halfResolution) const
{
	FBOAttributes attr = FramebuffersManager::GenCurrentAttr();
	attr.aaType = AntiAliasManager::AntiAliasType::Default;
	attr.isBloom = false;
	attr.isHDR = false;
	attr.isGamma = false;
	attr.textureAttrs.clear();
	attr.textureAttrs.push_back({ GL_TEXTURE_2D, GL_R16F, GL_RED, GL_FLOAT });
	if (halfResolution) {
		const auto& properties = SystemProperties::GetInstance();
		attr.width = (properties.SCREEN_WIDTH + 1) / 2;
		attr.height = (properties.SCREEN_HEIGHT + 1) / 2;
	}
	return attr;
}

void SSAORenderPass::Init(int width, int height)
{
	(void)width;
	(void)height;
}

const FBO* SSAORenderPass::GetGenerationFBO() const
{
	return SystemProperties::GetInstance().SSAO_MODE ==
		SSAOProperty::HalfBilateral
		? m_halfGenerationFBO
		: m_outputFBO;
}

void SSAORenderPass::ConfigureAOTexture(
	FBO* fbo,
	std::uint64_t& configuredGeneration)
{
	if (!fbo ||
		fbo->textureIDs.empty() ||
		fbo->textureIDs.front() == 0 ||
		configuredGeneration == fbo->GetResourceGeneration()) {
		return;
	}
	GLState::BindTexture(GL_TEXTURE_2D, fbo->textureIDs.front());
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	GLState::BindTexture(GL_TEXTURE_2D, 0);
	configuredGeneration = fbo->GetResourceGeneration();
}

void SSAORenderPass::UpdateRenderTargets()
{
	auto& properties = SystemProperties::GetInstance();
	auto& framebufferManager = FramebuffersManager::GetInstance();
	const bool bilateral =
		properties.SSAO_MODE == SSAOProperty::HalfBilateral;

	if (!bilateral && m_halfGenerationFBO) {
		framebufferManager.ReleaseFBO(m_halfGenerationFBO);
		m_halfGenerationFBO = nullptr;
		m_hasHalfGenerationAttr = false;
		m_configuredHalfGeneration = 0;
		framebufferManager.TrimUnusedFBOs();
	}

	UpdateFBOFromSystemProperties();
	ConfigureAOTexture(m_outputFBO, m_configuredOutputGeneration);

	if (!bilateral) {
		return;
	}

	const FBOAttributes halfAttr = BuildAOAttributes(true);
	if (!m_hasHalfGenerationAttr ||
		!(m_lastHalfGenerationAttr == halfAttr)) {
		framebufferManager.ReleaseFBO(m_halfGenerationFBO);
		m_halfGenerationFBO = framebufferManager.GetFBO(halfAttr);
		m_lastHalfGenerationAttr = halfAttr;
		m_hasHalfGenerationAttr = true;
		m_configuredHalfGeneration = 0;
		if (m_halfGenerationFBO) {
			m_halfGenerationFBO->passName =
				"SSAORenderPass_HalfGenerate";
			framebufferManager.RegisterFBO(
				"SSAORenderPass_HalfGenerate",
				m_halfGenerationFBO);
		}
		framebufferManager.TrimUnusedFBOs();
	}
	ConfigureAOTexture(
		m_halfGenerationFBO,
		m_configuredHalfGeneration);
}

void SSAORenderPass::EnsureKernelAndNoise()
{
	if (m_kernelNoiseReady)
		return;
	m_kernelNoiseReady = true;

	std::uniform_real_distribution<float> dist(0.0f, 1.0f);
	std::default_random_engine rng(1337u);

	m_kernel.resize(64);
	for (int i = 0; i < 64; ++i) {
		glm::vec3 sample(dist(rng) * 2.0f - 1.0f, dist(rng) * 2.0f - 1.0f, dist(rng));
		sample = glm::normalize(sample);
		sample *= dist(rng);
		float scale = float(i) / 64.0f;
		scale = glm::mix(0.1f, 1.0f, scale * scale);
		m_kernel[i] = sample * scale;
	}

	std::vector<glm::vec3> noise(16);
	for (int i = 0; i < 16; ++i) {
		noise[i] = glm::normalize(glm::vec3(dist(rng) * 2.0f - 1.0f, dist(rng) * 2.0f - 1.0f, 0.0f));
	}

	glGenTextures(1, &m_noiseTexture);
	GLState::BindTexture(GL_TEXTURE_2D, m_noiseTexture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, 4, 4, 0, GL_RGB, GL_FLOAT, noise.data());
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	GLState::BindTexture(GL_TEXTURE_2D, 0);
}

void SSAORenderPass::Render(Scene* scene, const FBO* gbufferFBO)
{
	PERF_CPU_SCOPE("SSAO Pass");
	PERF_GPU_SCOPE("SSAO Pass");
	auto& properties = SystemProperties::GetInstance();
	const bool reconstructPosition = ReconstructPositionFromDepth();
	const int normalAttachment = GBufferNormalAttachment();
	const std::size_t requiredColorAttachments =
		reconstructPosition ? std::size_t{ 1 } : std::size_t{ 2 };
	if (!properties.SSAO ||
		!scene ||
		!scene->camera_ptr ||
		!gbufferFBO ||
		gbufferFBO->textureIDs.size() < requiredColorAttachments ||
		(reconstructPosition && gbufferFBO->depthTextureID == 0))
		return;
	const float aspect = static_cast<float>(properties.SCREEN_WIDTH) /
		static_cast<float>((std::max)(1, properties.SCREEN_HEIGHT));
	const glm::mat4 inverseProjection = reconstructPosition
		? glm::inverse(scene->camera_ptr->GetProjectionMatrix(aspect))
		: glm::mat4(1.0f);

	UpdateRenderTargets();
	FBO* generationFBO =
		properties.SSAO_MODE == SSAOProperty::HalfBilateral
			? m_halfGenerationFBO
			: m_outputFBO;
	if (!m_outputFBO ||
		!m_outputFBO->IsComplete() ||
		m_outputFBO->textureIDs.empty() ||
		!generationFBO ||
		!generationFBO->IsComplete() ||
		generationFBO->textureIDs.empty())
		return;

	EnsureKernelAndNoise();

	auto ssaoShader = ShaderManager::GetInstance().GetShader(ShaderManager::SSAO);
	const bool bilateral =
		properties.SSAO_MODE == SSAOProperty::HalfBilateral;
	auto upsampleShader = bilateral
		? ShaderManager::GetInstance().GetShader(ShaderManager::SSAOUpsample)
		: nullptr;
	if (!ssaoShader || (bilateral && !upsampleShader))
		return;

	{
		PERF_CPU_SCOPE("SSAO Generate");
		PERF_GPU_SCOPE("SSAO Generate");
		GLState::BindFramebuffer(
			GL_FRAMEBUFFER,
			generationFBO->framebufferID);
		glViewport(0, 0, generationFBO->width, generationFBO->height);
		GLState::Disable(GL_DEPTH_TEST);
		GLState::Disable(GL_STENCIL_TEST);
		glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);

		ssaoShader->use();
		ssaoShader->setBool("reconstructPosition", reconstructPosition);
		if (reconstructPosition) {
			ssaoShader->setInt("gDepth", 0);
			ssaoShader->setMat4("inverseProjection", inverseProjection);
		}
		else {
			ssaoShader->setInt("gPosition", 0);
		}
		ssaoShader->setInt("gNormal", 1);
		ssaoShader->setInt("texNoise", 2);
		// Keep the original four-full-resolution-pixel noise period in all
		// modes so the 64-sample A/B changes the raster grid, not the noise.
		ssaoShader->setInt("screenWidth", properties.SCREEN_WIDTH);
		ssaoShader->setInt("screenHeight", properties.SCREEN_HEIGHT);
		ssaoShader->setFloat("radius", properties.SSAO_RADIUS);
		ssaoShader->setFloat("bias", properties.SSAO_BIAS);
		ssaoShader->setInt("kernelSize", properties.SSAO_KERNEL_SIZE);

		for (int i = 0; i < 64; ++i) {
			ssaoShader->setVec3(
				"ssaoKernel[" + std::to_string(i) + "]",
				m_kernel[i]);
		}

		GLState::ActiveTexture(GL_TEXTURE0);
		GLState::BindTexture(
			GL_TEXTURE_2D,
			reconstructPosition
				? gbufferFBO->depthTextureID
				: gbufferFBO->textureIDs[0]);
		GLState::ActiveTexture(GL_TEXTURE1);
		GLState::BindTexture(
			GL_TEXTURE_2D,
			gbufferFBO->textureIDs[normalAttachment]);
		GLState::ActiveTexture(GL_TEXTURE2);
		GLState::BindTexture(GL_TEXTURE_2D, m_noiseTexture);

		GLState::BindVertexArray(globalVAOs.quadVAO);
		PerformanceProfiler::GetInstance().RecordDraw(GL_TRIANGLES, 6);
		glDrawArrays(GL_TRIANGLES, 0, 6);
	}

	if (bilateral) {
		PERF_CPU_SCOPE("SSAO Upsample");
		PERF_GPU_SCOPE("SSAO Upsample");
		GLState::BindFramebuffer(
			GL_FRAMEBUFFER,
			m_outputFBO->framebufferID);
		glViewport(0, 0, m_outputFBO->width, m_outputFBO->height);
		GLState::Disable(GL_DEPTH_TEST);
		GLState::Disable(GL_STENCIL_TEST);

		upsampleShader->use();
		upsampleShader->setInt("halfAO", 0);
		upsampleShader->setBool(
			"reconstructPosition",
			reconstructPosition);
		if (reconstructPosition) {
			upsampleShader->setInt("gDepth", 1);
			upsampleShader->setMat4(
				"inverseProjection",
				inverseProjection);
		}
		else {
			upsampleShader->setInt("gPosition", 1);
		}
		upsampleShader->setInt("gNormal", 2);
		upsampleShader->setFloat(
			"depthSigma",
			properties.SSAO_BILATERAL_DEPTH_SIGMA);
		upsampleShader->setFloat(
			"normalPower",
			properties.SSAO_BILATERAL_NORMAL_POWER);

		GLState::ActiveTexture(GL_TEXTURE0);
		GLState::BindTexture(
			GL_TEXTURE_2D,
			generationFBO->textureIDs[0]);
		GLState::ActiveTexture(GL_TEXTURE1);
		GLState::BindTexture(
			GL_TEXTURE_2D,
			reconstructPosition
				? gbufferFBO->depthTextureID
				: gbufferFBO->textureIDs[0]);
		GLState::ActiveTexture(GL_TEXTURE2);
		GLState::BindTexture(
			GL_TEXTURE_2D,
			gbufferFBO->textureIDs[normalAttachment]);

		GLState::BindVertexArray(globalVAOs.quadVAO);
		PerformanceProfiler::GetInstance().RecordDraw(GL_TRIANGLES, 6);
		glDrawArrays(GL_TRIANGLES, 0, 6);
	}

	GLState::BindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT);
}

void SSAORenderPass::Destroy()
{
	auto& framebufferManager = FramebuffersManager::GetInstance();
	framebufferManager.ReleaseFBO(m_halfGenerationFBO);
	m_halfGenerationFBO = nullptr;
	m_hasHalfGenerationAttr = false;
	m_configuredHalfGeneration = 0;
	if (m_noiseTexture != 0) {
		GLState::ForgetTexture(m_noiseTexture);
		glDeleteTextures(1, &m_noiseTexture);
		m_noiseTexture = 0;
	}
	m_kernelNoiseReady = false;
	m_kernel.clear();
	framebufferManager.ReleaseFBO(m_outputFBO);
	m_outputFBO = nullptr;
	m_hasAttr = false;
	m_configuredOutputGeneration = 0;
}
