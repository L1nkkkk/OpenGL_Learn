#include "DeferRenderPass.h"

void DeferRenderPass::Init(int width, int height)
{
	UpdateFBOFromSystemProperties();
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
	// GBuffer: position / normal / albedoSpec / material
	FBOAttributes attr = FramebuffersManager::GenCurrentAttr();
	attr.aaType = AntiAliasManager::AntiAliasType::Default;
	attr.isDefer = true;
	attr.isBloom = false;
	attr.isGamma = false;
	attr.textureAttrs.clear();
	attr.textureAttrs.push_back({ GL_TEXTURE_2D, GL_RGB16F, GL_RGB, GL_FLOAT });          // gPosition
	attr.textureAttrs.push_back({ GL_TEXTURE_2D, GL_RGB16F, GL_RGB, GL_FLOAT });          // gNormal
	attr.textureAttrs.push_back({ GL_TEXTURE_2D, GL_RGB, GL_RGB, GL_UNSIGNED_BYTE });     // gAlbedoSpec
	attr.textureAttrs.push_back({ GL_TEXTURE_2D, GL_RGBA16F, GL_RGBA, GL_FLOAT });        // gMaterial
	return attr;
}

void DeferRenderPass::BindGBufferTextures(Shader& shader, unsigned int& textureSlot) const
{
	if (!m_gbufferFBO || m_gbufferFBO->textureIDs.size() < 4) return;

	glActiveTexture(GL_TEXTURE0 + textureSlot);
	glBindTexture(GL_TEXTURE_2D, m_gbufferFBO->textureIDs[0]);
	shader.setInt("gPosition", textureSlot++);

	glActiveTexture(GL_TEXTURE0 + textureSlot);
	glBindTexture(GL_TEXTURE_2D, m_gbufferFBO->textureIDs[1]);
	shader.setInt("gNormal", textureSlot++);

	glActiveTexture(GL_TEXTURE0 + textureSlot);
	glBindTexture(GL_TEXTURE_2D, m_gbufferFBO->textureIDs[2]);
	shader.setInt("gAlbedoSpec", textureSlot++);

	glActiveTexture(GL_TEXTURE0 + textureSlot);
	glBindTexture(GL_TEXTURE_2D, m_gbufferFBO->textureIDs[3]);
	shader.setInt("gMaterial", textureSlot++);
}

void DeferRenderPass::Render(Scene* scene, const FBO* inputFBO)
{
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
	}
	if (!m_gbufferFBO || m_gbufferFBO->textureIDs.size() < 4) return;

	scene->DrawShadowMap();

	// 1) Geometry pass: write opaque meshes into GBuffer.
	glDisable(GL_BLEND);
	glBindFramebuffer(GL_FRAMEBUFFER, m_gbufferFBO->framebufferID);
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_STENCIL_TEST);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClearStencil(0);
	glStencilMask(0x00);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

	auto deferProcessShader = ShaderManager::GetInstance().GetShader(ShaderManager::DeferProcess);
	if (!deferProcessShader) return;
	deferProcessShader->use();
	auto& opaqueList = scene->GetOpaqueMeshes();
	for (const auto& item : opaqueList) {
		if (!item.model || !item.mesh) continue;
		deferProcessShader->setMat4("model", item.model->getModelMatrix());
		item.mesh->Draw(deferProcessShader.get());
	}

	// Copy depth to output target so skybox / transparent passes can share it.
	auto& properties = SystemProperties::GetInstance();
	glBindFramebuffer(GL_READ_FRAMEBUFFER, m_gbufferFBO->framebufferID);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_outputFBO->framebufferID);
	glBlitFramebuffer(
		0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT,
		0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT,
		GL_DEPTH_BUFFER_BIT, GL_NEAREST
	);

	// 2) Lighting pass: shade full-screen quad from GBuffer into output HDR buffer.
	glBindFramebuffer(GL_FRAMEBUFFER, m_outputFBO->framebufferID);
	glDisable(GL_DEPTH_TEST);

	auto deferLightShader = ShaderManager::GetInstance().GetShader(ShaderManager::Defer);
	if (!deferLightShader) return;
	deferLightShader->use();
	scene->SetLightUniforms(*deferLightShader);
	unsigned int texSlot = scene->SetShadowMap(*deferLightShader);
	if (scene->camera_ptr) {
		deferLightShader->setVec3("viewPos", scene->camera_ptr->cameraPos);
	}
	BindGBufferTextures(*deferLightShader, texSlot);

	glBindVertexArray(globalVAOs.quadVAO);
	glDrawArrays(GL_TRIANGLES, 0, 6);

	// 3) Forward extras on top of deferred base.
	glBindFramebuffer(GL_FRAMEBUFFER, m_outputFBO->framebufferID);
	glEnable(GL_DEPTH_TEST);
	scene->DrawPointLights();
	if (scene->camera_ptr) {
		scene->DrawSkybox(scene->camera_ptr->GetViewMatrix());
	}
	scene->DrawTransparentModels();
	scene->DrawOutlines();

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void DeferRenderPass::Destroy()
{
	FramebuffersManager::GetInstance().ReleaseFBO(m_gbufferFBO);
	m_gbufferFBO = nullptr;
	FramebuffersManager::GetInstance().ReleaseFBO(m_outputFBO);
	m_outputFBO = nullptr;
}
