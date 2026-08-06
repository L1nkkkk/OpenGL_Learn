#pragma once
#include "RenderPass.h"
#include <glm/glm.hpp>
#include <vector>

class SSAORenderPass : public RenderPass {
public:
	SSAORenderPass() : RenderPass("SSAORenderPass") {}

	void Init(int width, int height) override;
	void Render(Scene* scene, const FBO* gbufferFBO);
	void Destroy() override;
	const FBO* GetGenerationFBO() const;

private:
	FBOAttributes BuildAttributesFromSystemProperties() override;
	FBOAttributes BuildAOAttributes(bool halfResolution) const;
	void UpdateRenderTargets();
	void ConfigureAOTexture(FBO* fbo, std::uint64_t& configuredGeneration);
	void EnsureKernelAndNoise();

	bool m_kernelNoiseReady = false;
	unsigned int m_noiseTexture = 0;
	std::vector<glm::vec3> m_kernel;
	FBO* m_halfGenerationFBO = nullptr;
	FBOAttributes m_lastHalfGenerationAttr{};
	bool m_hasHalfGenerationAttr = false;
	std::uint64_t m_configuredOutputGeneration = 0;
	std::uint64_t m_configuredHalfGeneration = 0;
};
