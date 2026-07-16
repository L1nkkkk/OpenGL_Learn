#include "SSAORenderPass.h"
#include "Profiler.h"
#include "ShaderManager.h"

#include <random>

FBOAttributes SSAORenderPass::BuildAttributesFromSystemProperties()
{
	FBOAttributes attr = FramebuffersManager::GenCurrentAttr();
	attr.aaType = AntiAliasManager::AntiAliasType::Default;
	attr.isBloom = false;
	attr.isHDR = false;
	attr.isGamma = false;
	attr.textureAttrs.clear();
	attr.textureAttrs.push_back({ GL_TEXTURE_2D, GL_R16F, GL_RED, GL_FLOAT });
	return attr;
}

void SSAORenderPass::Init(int width, int height)
{
	UpdateFBOFromSystemProperties();
	EnsureKernelAndNoise();
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
	glBindTexture(GL_TEXTURE_2D, m_noiseTexture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, 4, 4, 0, GL_RGB, GL_FLOAT, noise.data());
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glBindTexture(GL_TEXTURE_2D, 0);
}

void SSAORenderPass::Render(Scene* scene, const FBO* gbufferFBO)
{
	PERF_CPU_SCOPE("SSAO Pass");
	PERF_GPU_SCOPE("SSAO Pass");
	auto& properties = SystemProperties::GetInstance();
	if (!properties.SSAO || !gbufferFBO || gbufferFBO->textureIDs.size() < 2)
		return;

	UpdateFBOFromSystemProperties();
	if (!m_outputFBO || m_outputFBO->textureIDs.empty())
		return;

	EnsureKernelAndNoise();

	auto ssaoShader = ShaderManager::GetInstance().GetShader(ShaderManager::SSAO);
	if (!ssaoShader)
		return;

	glBindFramebuffer(GL_FRAMEBUFFER, m_outputFBO->framebufferID);
	glViewport(0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	ssaoShader->use();
	ssaoShader->setInt("gPosition", 0);
	ssaoShader->setInt("gNormal", 1);
	ssaoShader->setInt("texNoise", 2);
	ssaoShader->setInt("screenWidth", properties.SCREEN_WIDTH);
	ssaoShader->setInt("screenHeight", properties.SCREEN_HEIGHT);
	ssaoShader->setFloat("radius", properties.SSAO_RADIUS);
	ssaoShader->setFloat("bias", properties.SSAO_BIAS);
	ssaoShader->setInt("kernelSize", properties.SSAO_KERNEL_SIZE);

	for (int i = 0; i < 64; ++i)
		ssaoShader->setVec3("ssaoKernel[" + std::to_string(i) + "]", m_kernel[i]);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, gbufferFBO->textureIDs[0]);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, gbufferFBO->textureIDs[1]);
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, m_noiseTexture);

	glBindVertexArray(globalVAOs.quadVAO);
	PerformanceProfiler::GetInstance().RecordDraw(GL_TRIANGLES, 6);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glBindVertexArray(0);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SSAORenderPass::Destroy()
{
	if (m_noiseTexture != 0) {
		glDeleteTextures(1, &m_noiseTexture);
		m_noiseTexture = 0;
	}
	m_kernelNoiseReady = false;
	m_kernel.clear();
	FramebuffersManager::GetInstance().ReleaseFBO(m_outputFBO);
	m_outputFBO = nullptr;
}
