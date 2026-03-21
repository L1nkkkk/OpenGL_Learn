#include "DeferRenderPass.h"
#include "ShaderManager.h"
#include <glm/glm.hpp>

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

void DeferRenderPass::DrawPointLightVolumesDeferred(Scene* scene)
{
	if (!scene || !m_gbufferFBO || m_gbufferFBO->textureIDs.size() < 4) return;

	auto defaultShader = ShaderManager::GetInstance().GetShader(ShaderManager::Default);
	auto lightVolumeShader = ShaderManager::GetInstance().GetShader(ShaderManager::LightVolume);
	if (!defaultShader || !lightVolumeShader) return;

	auto& properties = SystemProperties::GetInstance();
	lightVolumeShader->use();
	if (scene->camera_ptr) {
		lightVolumeShader->setVec3("viewPos", scene->camera_ptr->cameraPos);
	}
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, m_gbufferFBO->textureIDs[0]);
	lightVolumeShader->setInt("gPosition", 0);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, m_gbufferFBO->textureIDs[1]);
	lightVolumeShader->setInt("gNormal", 1);
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, m_gbufferFBO->textureIDs[2]);
	lightVolumeShader->setInt("gAlbedoSpec", 2);
	glActiveTexture(GL_TEXTURE3);
	glBindTexture(GL_TEXTURE_2D, m_gbufferFBO->textureIDs[3]);
	lightVolumeShader->setInt("gMaterial", 3);

	glEnable(GL_STENCIL_TEST);
	glBlendEquation(GL_FUNC_ADD);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE);

	for (auto& pointLight : scene->GetLightSource().pointLights) {
		if (!pointLight.GetActiveStatus()) continue;

		float radius = ComputePointLightStencilVolumeRadius(
			pointLight.constant, pointLight.linear, pointLight.quadratic, pointLight.diffuse,
			properties.LIGHT_VOLUME_CUTOFF_SCALE, properties.LIGHT_VOLUME_RADIUS_SCALE);
		const glm::vec3 savedScale = pointLight.scale;
		pointLight.SetScale(glm::vec3(radius));

		// 1) 光体积写入模板：INCR_WRAP/DECR_WRAP 单遍，无面剔除（与重构前一致）
		glDisable(GL_BLEND);
		glStencilMask(0xFF);
		glClear(GL_STENCIL_BUFFER_BIT);
		glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
		glDepthMask(GL_FALSE);
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LESS);
		glDisable(GL_CULL_FACE);
		glStencilFunc(GL_ALWAYS, 0, 0xFF);
		glStencilOpSeparate(GL_BACK, GL_KEEP, GL_INCR_WRAP, GL_KEEP);
		glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_DECR_WRAP, GL_KEEP);

		defaultShader->use();
		defaultShader->setMat4("model", pointLight.getModelMatrix());
		pointLight.DrawPointLight();

		// 2) 仅对模板非 0 像素做光照计算，叠加到平行光结果（深度 GEQUAL + 剔除正面，与重构前一致）
		lightVolumeShader->use();
		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glStencilFunc(GL_NOTEQUAL, 0, 0xFF);
		glStencilMask(0x00);
		glEnable(GL_CULL_FACE);
		glCullFace(GL_FRONT);
		glDepthFunc(GL_GEQUAL);

		lightVolumeShader->setVec3("pointLight.position", pointLight.position);
		lightVolumeShader->setFloat("pointLight.constant", pointLight.constant);
		lightVolumeShader->setFloat("pointLight.linear", pointLight.linear);
		lightVolumeShader->setFloat("pointLight.quadratic", pointLight.quadratic);
		lightVolumeShader->setVec3("pointLight.ambient", pointLight.ambient);
		lightVolumeShader->setVec3("pointLight.diffuse", pointLight.diffuse);
		lightVolumeShader->setVec3("pointLight.specular", pointLight.specular);
		lightVolumeShader->setFloat("pointLight.far_plane", pointLight.far);
		glActiveTexture(GL_TEXTURE4);
		glBindTexture(GL_TEXTURE_CUBE_MAP, pointLight.shadowFBO->textureIDs[0]);
		lightVolumeShader->setInt("pointLight.shadowCubeMap", 4);
		lightVolumeShader->setBool("pointLight.useShadowMap", pointLight.useShadowMap);
		lightVolumeShader->setMat4("model", pointLight.getModelMatrix());

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, m_gbufferFBO->textureIDs[0]);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, m_gbufferFBO->textureIDs[1]);
		glActiveTexture(GL_TEXTURE2);
		glBindTexture(GL_TEXTURE_2D, m_gbufferFBO->textureIDs[2]);
		glActiveTexture(GL_TEXTURE3);
		glBindTexture(GL_TEXTURE_2D, m_gbufferFBO->textureIDs[3]);

		pointLight.DrawPointLight();

		pointLight.SetScale(savedScale);
		glStencilMask(0xFF);
		glClear(GL_STENCIL_BUFFER_BIT);
		glStencilMask(0x00);
		glCullFace(GL_BACK);
		glDepthFunc(GL_LESS);
	}

	glBindVertexArray(0);
	glDisable(GL_BLEND);
	glDisable(GL_STENCIL_TEST);
	glStencilMask(0xFF);
	glDepthMask(GL_TRUE);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glDisable(GL_CULL_FACE);
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
		GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST
	);

	// 2) Lighting pass: GBuffer -> HDR（全屏 或 平行光全屏 + 点光源光体积）
	glBindFramebuffer(GL_FRAMEBUFFER, m_outputFBO->framebufferID);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glStencilMask(0xFF);
	glClearStencil(0);
	glClear(GL_STENCIL_BUFFER_BIT);
	glDisable(GL_DEPTH_TEST);

	if (!properties.LIGHT_VOLUME) {
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
	} else {
		// 与 Scene::DrawDefferedModels 一致：平行光全屏；点光源用模板 + 球体裁剪像素（聚光灯此模式下不单独绘制）
		auto deferDirShader = ShaderManager::GetInstance().GetShader(ShaderManager::DeferDirLightVolume);
		if (!deferDirShader) return;
		deferDirShader->use();
		scene->SetLightUniforms(*deferDirShader);
		unsigned int texSlot = scene->SetShadowMap(*deferDirShader);
		if (scene->camera_ptr) {
			deferDirShader->setVec3("viewPos", scene->camera_ptr->cameraPos);
		}
		BindGBufferTextures(*deferDirShader, texSlot);

		glBindVertexArray(globalVAOs.quadVAO);
		glDrawArrays(GL_TRIANGLES, 0, 6);

		DrawPointLightVolumesDeferred(scene);
	}

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
