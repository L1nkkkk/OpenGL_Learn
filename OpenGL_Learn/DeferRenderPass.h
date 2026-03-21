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
	/// Stencil + 球体光体积，仅对模板内像素做点光源着色（与 Scene::DrawDefferedModels 中 LIGHT_VOLUME 分支一致）
	void DrawPointLightVolumesDeferred(Scene* scene);
};

