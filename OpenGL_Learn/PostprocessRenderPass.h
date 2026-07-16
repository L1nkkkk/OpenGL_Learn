#pragma once
#include "RenderPass.h"

class PostprocessRenderPass :
    public RenderPass
{
public:
	PostprocessRenderPass() : RenderPass("PostprocessRenderPass") {}

	void Init(int width, int height) override;

	void Render(Scene* scene, const FBO* inputFBO = nullptr) override;

	void ProcessBloom(FBO* input);

	void Destroy() override;
protected:
	FBO* m_bloomBlurFBO = nullptr;
	FBO* m_bloomPingFBO = nullptr;
    FBOAttributes BuildAttributesFromSystemProperties() override;
	FBOAttributes BuildBloomAttributes() const;
	bool EnsureBloomTargets();
	bool Blur(int times, unsigned int textureID);
};
