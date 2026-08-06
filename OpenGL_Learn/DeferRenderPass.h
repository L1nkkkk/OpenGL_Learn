#pragma once
#include "RenderPass.h"
#include "SSAORenderPass.h"
#include "PointLightGridRuntime.h"

class DeferRenderPass : public RenderPass {
public:
	DeferRenderPass() : RenderPass("DeferRenderPass") {}

	void Init(int width, int height) override;
	void Render(Scene* scene, const FBO* inputFBO = nullptr) override;
	void Destroy() override;
	const FBO* GetSSAOOutputFBO() const { return m_ssao.GetOutputFBO(); }
	const FBO* GetSSAOGenerationFBO() const {
		return m_ssao.GetGenerationFBO();
	}
	const FBO* GetGBufferFBO() const { return m_gbufferFBO; }
	bool UsesPositionReconstruction() const;
	int GetPositionAttachmentIndex() const;
	int GetNormalAttachmentIndex() const;
	int GetAlbedoAttachmentIndex() const;
	int GetMaterialAttachmentIndex() const;
	int GetEmissiveAttachmentIndex() const;

protected:
	FBOAttributes BuildAttributesFromSystemProperties() override;
	FBO* m_gbufferFBO = nullptr;
	SSAORenderPass m_ssao;
	PointLightGridRuntime m_pointLightGrid;

private:
	FBOAttributes BuildGBufferAttributesFromSystemProperties() const;
	void BindGBufferTextures(
		Shader& shader,
		unsigned int& textureSlot,
		const glm::mat4& inverseProjection,
		const glm::mat4& inverseView) const;
	void ConfigurePositionSource(
		Shader& shader,
		unsigned int textureSlot,
		const glm::mat4& inverseProjection,
		const glm::mat4& inverseView) const;
	/// Stencil + 球体光体积，仅对模板内像素做点光源着色（与 Scene::DrawDefferedModels 中 LIGHT_VOLUME 分支一致）
	void DrawPointLightVolumesDeferred(
		Scene* scene,
		const glm::mat4& inverseProjection,
		const glm::mat4& inverseView);
};
