#pragma once
#include "RenderPass.h"

class DeferRenderPass : public RenderPass {
public:
	DeferRenderPass() : RenderPass("DeferRenderPass") {}

	void Init(int width, int height) override;
	void Render(Scene* scene, const FBO* inputFBO = nullptr) override;
	void Destroy() override;

protected:
	FBOAttributes BuildAttributesFromSystemProperties() override;
	FBO* m_gbufferFBO = nullptr;

private:
	FBOAttributes BuildGBufferAttributesFromSystemProperties() const;
	void BindGBufferTextures(Shader& shader, unsigned int& textureSlot) const;
};

