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

private:
	FBOAttributes BuildAttributesFromSystemProperties() override;
	void EnsureKernelAndNoise();

	bool m_kernelNoiseReady = false;
	unsigned int m_noiseTexture = 0;
	std::vector<glm::vec3> m_kernel;
};
