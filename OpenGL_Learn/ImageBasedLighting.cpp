#include "ImageBasedLighting.h"

#include "GLStateCache.h"
#include "Profiler.h"
#include "Shader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>

namespace {
	constexpr unsigned int kIrradianceSize = 32;
	constexpr unsigned int kPrefilterSize = 128;
	constexpr unsigned int kPrefilterMipLevels = 5;
	constexpr unsigned int kBrdfLutSize = 256;

	std::uint64_t CubemapBytes(unsigned int baseSize, unsigned int mipLevels, unsigned int bytesPerPixel)
	{
		std::uint64_t bytes = 0;
		for (unsigned int mip = 0; mip < mipLevels; ++mip) {
			const std::uint64_t size = (std::max)(1u, baseSize >> mip);
			bytes += size * size * 6u * bytesPerPixel;
		}
		return bytes;
	}

	bool CheckFramebuffer(const char* stage)
	{
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
			return true;
		}
		std::cerr << "ImageBasedLighting: incomplete framebuffer during " << stage << std::endl;
		return false;
	}
}

ImageBasedLighting::~ImageBasedLighting()
{
	Destroy();
}

bool ImageBasedLighting::Initialize(
	unsigned int environmentCubemap,
	unsigned int cubeVAO,
	unsigned int quadVAO,
	int restoreViewportWidth,
	int restoreViewportHeight)
{
	if (m_ready) {
		return true;
	}
	if (environmentCubemap == 0 || cubeVAO == 0 || quadVAO == 0) {
		std::cerr << "ImageBasedLighting: invalid source cubemap or capture geometry" << std::endl;
		return false;
	}

	Shader irradianceShader(
		"shaders/iblCubemapVertex.glsl",
		"shaders/iblIrradianceFragment.glsl");
	Shader prefilterShader(
		"shaders/iblCubemapVertex.glsl",
		"shaders/iblPrefilterFragment.glsl");
	Shader brdfShader(
		"shaders/iblBrdfVertex.glsl",
		"shaders/iblBrdfFragment.glsl");
	if (irradianceShader.ID == 0 || prefilterShader.ID == 0 || brdfShader.ID == 0) {
		std::cerr << "ImageBasedLighting: precomputation shader compilation failed" << std::endl;
		return false;
	}

	unsigned int captureFBO = 0;
	unsigned int captureRBO = 0;
	glGenFramebuffers(1, &captureFBO);
	glGenRenderbuffers(1, &captureRBO);
	GLState::BindFramebuffer(GL_FRAMEBUFFER, captureFBO);
	glBindRenderbuffer(GL_RENDERBUFFER, captureRBO);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, captureRBO);

	glGenTextures(1, &m_irradianceMap);
	GLState::BindTexture(GL_TEXTURE_CUBE_MAP, m_irradianceMap);
	for (unsigned int face = 0; face < 6; ++face) {
		glTexImage2D(
			GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
			0,
			GL_RGB16F,
			kIrradianceSize,
			kIrradianceSize,
			0,
			GL_RGB,
			GL_FLOAT,
			nullptr);
	}
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	const glm::mat4 captureProjection = glm::perspective(
		glm::radians(90.0f), 1.0f, 0.1f, 10.0f);
	const std::array<glm::mat4, 6> captureViews = {
		glm::lookAt(glm::vec3(0.0f), glm::vec3( 1.0f,  0.0f,  0.0f), glm::vec3(0.0f, -1.0f,  0.0f)),
		glm::lookAt(glm::vec3(0.0f), glm::vec3(-1.0f,  0.0f,  0.0f), glm::vec3(0.0f, -1.0f,  0.0f)),
		glm::lookAt(glm::vec3(0.0f), glm::vec3( 0.0f,  1.0f,  0.0f), glm::vec3(0.0f,  0.0f,  1.0f)),
		glm::lookAt(glm::vec3(0.0f), glm::vec3( 0.0f, -1.0f,  0.0f), glm::vec3(0.0f,  0.0f, -1.0f)),
		glm::lookAt(glm::vec3(0.0f), glm::vec3( 0.0f,  0.0f,  1.0f), glm::vec3(0.0f, -1.0f,  0.0f)),
		glm::lookAt(glm::vec3(0.0f), glm::vec3( 0.0f,  0.0f, -1.0f), glm::vec3(0.0f, -1.0f,  0.0f))
	};

	GLState::Enable(GL_DEPTH_TEST);
	GLState::DepthFunc(GL_LEQUAL);
	GLState::Disable(GL_CULL_FACE);
	irradianceShader.use();
	irradianceShader.setInt("environmentMap", 0);
	irradianceShader.setMat4("projection", captureProjection);
	GLState::ActiveTexture(GL_TEXTURE0);
	GLState::BindTexture(GL_TEXTURE_CUBE_MAP, environmentCubemap);
	glBindRenderbuffer(GL_RENDERBUFFER, captureRBO);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, kIrradianceSize, kIrradianceSize);
	glViewport(0, 0, kIrradianceSize, kIrradianceSize);
	GLState::BindVertexArray(cubeVAO);
	bool complete = true;
	for (unsigned int face = 0; face < 6; ++face) {
		irradianceShader.setMat4("view", captureViews[face]);
		glFramebufferTexture2D(
			GL_FRAMEBUFFER,
			GL_COLOR_ATTACHMENT0,
			GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
			m_irradianceMap,
			0);
		complete = CheckFramebuffer("irradiance convolution") && complete;
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLES, 0, 36);
	}

	glGenTextures(1, &m_prefilterMap);
	GLState::BindTexture(GL_TEXTURE_CUBE_MAP, m_prefilterMap);
	for (unsigned int mip = 0; mip < kPrefilterMipLevels; ++mip) {
		const unsigned int mipSize = (std::max)(1u, kPrefilterSize >> mip);
		for (unsigned int face = 0; face < 6; ++face) {
			glTexImage2D(
				GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
				mip,
				GL_RGB16F,
				mipSize,
				mipSize,
				0,
				GL_RGB,
				GL_FLOAT,
				nullptr);
		}
	}
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, kPrefilterMipLevels - 1);

	prefilterShader.use();
	prefilterShader.setInt("environmentMap", 0);
	prefilterShader.setMat4("projection", captureProjection);
	GLState::ActiveTexture(GL_TEXTURE0);
	GLState::BindTexture(GL_TEXTURE_CUBE_MAP, environmentCubemap);
	for (unsigned int mip = 0; mip < kPrefilterMipLevels; ++mip) {
		const unsigned int mipSize = (std::max)(1u, kPrefilterSize >> mip);
		glBindRenderbuffer(GL_RENDERBUFFER, captureRBO);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, mipSize, mipSize);
		glViewport(0, 0, mipSize, mipSize);
		const float roughness = static_cast<float>(mip) /
			static_cast<float>(kPrefilterMipLevels - 1);
		prefilterShader.setFloat("roughness", roughness);
		for (unsigned int face = 0; face < 6; ++face) {
			prefilterShader.setMat4("view", captureViews[face]);
			glFramebufferTexture2D(
				GL_FRAMEBUFFER,
				GL_COLOR_ATTACHMENT0,
				GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
				m_prefilterMap,
				mip);
			complete = CheckFramebuffer("specular prefilter") && complete;
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
			glDrawArrays(GL_TRIANGLES, 0, 36);
		}
	}

	glGenTextures(1, &m_brdfLut);
	GLState::BindTexture(GL_TEXTURE_2D, m_brdfLut);
	glTexImage2D(
		GL_TEXTURE_2D,
		0,
		GL_RG16F,
		kBrdfLutSize,
		kBrdfLutSize,
		0,
		GL_RG,
		GL_FLOAT,
		nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glBindRenderbuffer(GL_RENDERBUFFER, captureRBO);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, kBrdfLutSize, kBrdfLutSize);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_brdfLut, 0);
	complete = CheckFramebuffer("BRDF integration") && complete;
	glViewport(0, 0, kBrdfLutSize, kBrdfLutSize);
	brdfShader.use();
	GLState::BindVertexArray(quadVAO);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLES, 0, 6);

	GLState::BindVertexArray(0);
	GLState::BindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);
	glDeleteRenderbuffers(1, &captureRBO);
	GLState::ForgetFramebuffer(captureFBO);
	glDeleteFramebuffers(1, &captureFBO);
	GLState::DepthFunc(GL_LESS);
	glViewport(
		0,
		0,
		(std::max)(1, restoreViewportWidth),
		(std::max)(1, restoreViewportHeight));
	GLState::ActiveTexture(GL_TEXTURE0);

	if (!complete) {
		Destroy();
		return false;
	}

	m_trackedBytes =
		CubemapBytes(kIrradianceSize, 1, 6) +
		CubemapBytes(kPrefilterSize, kPrefilterMipLevels, 6) +
		static_cast<std::uint64_t>(kBrdfLutSize) * kBrdfLutSize * 4u;
	PerformanceProfiler::GetInstance().RecordMemoryAllocation(
		MemoryResourceType::Texture,
		m_trackedBytes);
	glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &m_maxTextureUnits);
	m_ready = true;
	std::cout << "[PBR] IBL ready: irradiance=" << kIrradianceSize
		<< " prefilter=" << kPrefilterSize
		<< " mips=" << kPrefilterMipLevels
		<< " brdfLut=" << kBrdfLutSize << std::endl;
	return true;
}

unsigned int ImageBasedLighting::Bind(Shader& shader, unsigned int firstTextureUnit) const
{
	const bool consumesIbl =
		shader.shaderName == "pbr" ||
		shader.shaderName == "defer" ||
		shader.shaderName == "deferDirLightVolume";
	if (!consumesIbl) {
		return firstTextureUnit;
	}
	shader.setBool("useIBL", m_ready);
	if (!m_ready) {
		return firstTextureUnit;
	}

	if (firstTextureUnit + 3u > static_cast<unsigned int>((std::max)(0, m_maxTextureUnits))) {
		shader.setBool("useIBL", false);
		if (!m_textureUnitWarningPrinted) {
			std::cerr << "ImageBasedLighting: not enough fragment texture units" << std::endl;
			m_textureUnitWarningPrinted = true;
		}
		return firstTextureUnit;
	}

	GLState::ActiveTexture(GL_TEXTURE0 + firstTextureUnit);
	GLState::BindTexture(GL_TEXTURE_CUBE_MAP, m_irradianceMap);
	shader.setInt("irradianceMap", firstTextureUnit++);
	GLState::ActiveTexture(GL_TEXTURE0 + firstTextureUnit);
	GLState::BindTexture(GL_TEXTURE_CUBE_MAP, m_prefilterMap);
	shader.setInt("prefilterMap", firstTextureUnit++);
	GLState::ActiveTexture(GL_TEXTURE0 + firstTextureUnit);
	GLState::BindTexture(GL_TEXTURE_2D, m_brdfLut);
	shader.setInt("brdfLUT", firstTextureUnit++);
	return firstTextureUnit;
}

void ImageBasedLighting::Destroy()
{
	unsigned int textures[] = { m_irradianceMap, m_prefilterMap, m_brdfLut };
	for (unsigned int texture : textures) {
		if (texture != 0) {
			GLState::ForgetTexture(texture);
			glDeleteTextures(1, &texture);
		}
	}
	m_irradianceMap = 0;
	m_prefilterMap = 0;
	m_brdfLut = 0;
	m_ready = false;
	m_maxTextureUnits = 0;
	m_textureUnitWarningPrinted = false;
	if (m_trackedBytes != 0) {
		PerformanceProfiler::GetInstance().RecordMemoryRelease(
			MemoryResourceType::Texture,
			m_trackedBytes);
		m_trackedBytes = 0;
	}
}
