#include "DeferRenderPass.h"
#include "GLStateCache.h"
#include "Profiler.h"
#include "ShaderManager.h"
#include <glm/glm.hpp>

void DeferRenderPass::Init(int width, int height)
{
	m_ssao.Init(width, height);
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
	// GBuffer: position / normal / albedo / material parameters / emissive.
	FBOAttributes attr = FramebuffersManager::GenCurrentAttr();
	attr.aaType = AntiAliasManager::AntiAliasType::Default;
	attr.isDefer = true;
	attr.isBloom = false;
	attr.isGamma = false;
	attr.textureAttrs.clear();
	attr.textureAttrs.push_back({ GL_TEXTURE_2D, GL_RGBA16F, GL_RGBA, GL_FLOAT });        // gPosition (rgb: world pos, a: depth)
	attr.textureAttrs.push_back({ GL_TEXTURE_2D, GL_RGB16F, GL_RGB, GL_FLOAT });          // gNormal
	attr.textureAttrs.push_back({ GL_TEXTURE_2D, GL_RGB, GL_RGB, GL_UNSIGNED_BYTE });     // gAlbedoSpec
	attr.textureAttrs.push_back({ GL_TEXTURE_2D, GL_RGBA16F, GL_RGBA, GL_FLOAT });        // gMaterial
	attr.textureAttrs.push_back({ GL_TEXTURE_2D, GL_RGB16F, GL_RGB, GL_FLOAT });          // gEmissive
	return attr;
}

void DeferRenderPass::BindGBufferTextures(Shader& shader, unsigned int& textureSlot) const
{
	if (!m_gbufferFBO || m_gbufferFBO->textureIDs.size() < 5) return;

	GLState::ActiveTexture(GL_TEXTURE0 + textureSlot);
	GLState::BindTexture(GL_TEXTURE_2D, m_gbufferFBO->textureIDs[0]);
	shader.setInt("gPosition", textureSlot++);

	GLState::ActiveTexture(GL_TEXTURE0 + textureSlot);
	GLState::BindTexture(GL_TEXTURE_2D, m_gbufferFBO->textureIDs[1]);
	shader.setInt("gNormal", textureSlot++);

	GLState::ActiveTexture(GL_TEXTURE0 + textureSlot);
	GLState::BindTexture(GL_TEXTURE_2D, m_gbufferFBO->textureIDs[2]);
	shader.setInt("gAlbedoSpec", textureSlot++);

	GLState::ActiveTexture(GL_TEXTURE0 + textureSlot);
	GLState::BindTexture(GL_TEXTURE_2D, m_gbufferFBO->textureIDs[3]);
	shader.setInt("gMaterial", textureSlot++);

	GLState::ActiveTexture(GL_TEXTURE0 + textureSlot);
	GLState::BindTexture(GL_TEXTURE_2D, m_gbufferFBO->textureIDs[4]);
	shader.setInt("gEmissive", textureSlot++);
}

void DeferRenderPass::DrawPointLightVolumesDeferred(Scene* scene)
{
	if (!scene || !m_gbufferFBO || m_gbufferFBO->textureIDs.size() < 5) return;

	auto defaultShader = ShaderManager::GetInstance().GetShader(ShaderManager::Default);
	auto lightVolumeShader = ShaderManager::GetInstance().GetShader(ShaderManager::LightVolume);
	if (!defaultShader || !lightVolumeShader) return;

	auto& properties = SystemProperties::GetInstance();
	const FBO* ssaoFBO = properties.SSAO ? m_ssao.GetOutputFBO() : nullptr;
	const bool useSSAOInLighting = ssaoFBO && !ssaoFBO->textureIDs.empty();
	lightVolumeShader->use();
	if (scene->camera_ptr) {
		lightVolumeShader->setVec3("viewPos", scene->camera_ptr->cameraPos);
	}
	GLState::ActiveTexture(GL_TEXTURE0);
	GLState::BindTexture(GL_TEXTURE_2D, m_gbufferFBO->textureIDs[0]);
	lightVolumeShader->setInt("gPosition", 0);
	GLState::ActiveTexture(GL_TEXTURE1);
	GLState::BindTexture(GL_TEXTURE_2D, m_gbufferFBO->textureIDs[1]);
	lightVolumeShader->setInt("gNormal", 1);
	GLState::ActiveTexture(GL_TEXTURE2);
	GLState::BindTexture(GL_TEXTURE_2D, m_gbufferFBO->textureIDs[2]);
	lightVolumeShader->setInt("gAlbedoSpec", 2);
	GLState::ActiveTexture(GL_TEXTURE3);
	GLState::BindTexture(GL_TEXTURE_2D, m_gbufferFBO->textureIDs[3]);
	lightVolumeShader->setInt("gMaterial", 3);
	lightVolumeShader->setBool("useSSAO", useSSAOInLighting);
	if (useSSAOInLighting) {
		GLState::ActiveTexture(GL_TEXTURE5);
		GLState::BindTexture(GL_TEXTURE_2D, ssaoFBO->textureIDs[0]);
		lightVolumeShader->setInt("ssaoMap", 5);
	}

	GLState::Enable(GL_STENCIL_TEST);
	glBlendEquation(GL_FUNC_ADD);
	GLState::Enable(GL_BLEND);
	GLState::BlendFunc(GL_ONE, GL_ONE);

	for (auto& pointLight : scene->GetLightSource().pointLights) {
		if (!pointLight.GetActiveStatus()) continue;

		float radius = ComputePointLightStencilVolumeRadius(
			pointLight.constant, pointLight.linear, pointLight.quadratic, pointLight.diffuse,
			properties.LIGHT_VOLUME_CUTOFF_SCALE, properties.LIGHT_VOLUME_RADIUS_SCALE);
		const glm::vec3 savedScale = pointLight.scale;
		pointLight.SetScale(glm::vec3(radius));

		// 1) 光体积写入模板：INCR_WRAP/DECR_WRAP 单遍，无面剔除（与重构前一致）
		GLState::Disable(GL_BLEND);
		GLState::StencilMask(0xFF);
		glClear(GL_STENCIL_BUFFER_BIT);
		GLState::ColorMask(false, false, false, false);
		GLState::DepthMask(false);
		GLState::Enable(GL_DEPTH_TEST);
		GLState::DepthFunc(GL_LESS);
		GLState::Disable(GL_CULL_FACE);
		GLState::StencilFunc(GL_ALWAYS, 0, 0xFF);
		GLState::StencilOpSeparate(GL_BACK, GL_KEEP, GL_INCR_WRAP, GL_KEEP);
		GLState::StencilOpSeparate(GL_FRONT, GL_KEEP, GL_DECR_WRAP, GL_KEEP);

		defaultShader->use();
		defaultShader->setMat4("model", pointLight.getModelMatrix());
		pointLight.DrawPointLight();

		// 2) 仅对模板非 0 像素做光照计算，叠加到平行光结果（深度 GEQUAL + 剔除正面，与重构前一致）
		lightVolumeShader->use();
		GLState::Enable(GL_BLEND);
		GLState::BlendFunc(GL_ONE, GL_ONE);
		GLState::ColorMask(true, true, true, true);
		GLState::StencilFunc(GL_NOTEQUAL, 0, 0xFF);
		GLState::StencilMask(0x00);
		GLState::Enable(GL_CULL_FACE);
		GLState::CullFace(GL_FRONT);
		GLState::DepthFunc(GL_GEQUAL);

		lightVolumeShader->setVec3("pointLight.position", pointLight.position);
		lightVolumeShader->setFloat("pointLight.constant", pointLight.constant);
		lightVolumeShader->setFloat("pointLight.linear", pointLight.linear);
		lightVolumeShader->setFloat("pointLight.quadratic", pointLight.quadratic);
		lightVolumeShader->setVec3("pointLight.ambient", pointLight.ambient);
		lightVolumeShader->setVec3("pointLight.diffuse", pointLight.diffuse);
		lightVolumeShader->setVec3("pointLight.specular", pointLight.specular);
		lightVolumeShader->setFloat("pointLight.far_plane", pointLight.far);
		GLState::ActiveTexture(GL_TEXTURE4);
		FBO* pointShadowFBO = pointLight.useShadowMap ? pointLight.EnsureShadowFBO() : nullptr;
		GLState::BindTexture(
			GL_TEXTURE_CUBE_MAP,
			pointShadowFBO && !pointShadowFBO->textureIDs.empty()
				? pointShadowFBO->textureIDs[0]
				: 0);
		lightVolumeShader->setInt("pointLight.shadowCubeMap", 4);
		lightVolumeShader->setBool("pointLight.useShadowMap", pointLight.useShadowMap);
		lightVolumeShader->setMat4("model", pointLight.getModelMatrix());

		GLState::ActiveTexture(GL_TEXTURE0);
		GLState::BindTexture(GL_TEXTURE_2D, m_gbufferFBO->textureIDs[0]);
		GLState::ActiveTexture(GL_TEXTURE1);
		GLState::BindTexture(GL_TEXTURE_2D, m_gbufferFBO->textureIDs[1]);
		GLState::ActiveTexture(GL_TEXTURE2);
		GLState::BindTexture(GL_TEXTURE_2D, m_gbufferFBO->textureIDs[2]);
		GLState::ActiveTexture(GL_TEXTURE3);
		GLState::BindTexture(GL_TEXTURE_2D, m_gbufferFBO->textureIDs[3]);
		if (useSSAOInLighting) {
			GLState::ActiveTexture(GL_TEXTURE5);
			GLState::BindTexture(GL_TEXTURE_2D, ssaoFBO->textureIDs[0]);
		}

		pointLight.DrawPointLight();

		pointLight.SetScale(savedScale);
		GLState::StencilMask(0xFF);
		glClear(GL_STENCIL_BUFFER_BIT);
		GLState::StencilMask(0x00);
		GLState::CullFace(GL_BACK);
		GLState::DepthFunc(GL_LESS);
	}

	GLState::Disable(GL_BLEND);
	GLState::Disable(GL_STENCIL_TEST);
	GLState::StencilMask(0xFF);
	GLState::DepthMask(true);
	GLState::Enable(GL_DEPTH_TEST);
	GLState::DepthFunc(GL_LESS);
	GLState::Disable(GL_CULL_FACE);
}

void DeferRenderPass::Render(Scene* scene, const FBO* inputFBO)
{
	PERF_CPU_SCOPE("Deferred Pass");
	PERF_GPU_SCOPE("Deferred Pass");
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
		fbMgr.TrimUnusedFBOs();
	}
	if (!m_gbufferFBO || m_gbufferFBO->textureIDs.size() < 5) return;

	auto& properties = SystemProperties::GetInstance();
	scene->PrepareRenderData();
	scene->DrawShadowMap();

	// 1) Geometry pass: write opaque meshes into GBuffer.
	GLState::Disable(GL_BLEND);
	GLState::BindFramebuffer(GL_FRAMEBUFFER, m_gbufferFBO->framebufferID);
	GLState::Enable(GL_DEPTH_TEST);
	GLState::Enable(GL_STENCIL_TEST);
	glClearStencil(0);
	GLState::StencilMask(0x00);
	glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	// GBuffer clear values:
	// gPosition.a = 0 means invalid/background pixel (valid mask).
	const float clearGPos[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	const float clearGNormal[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	const float clearGAlbedo[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	const float clearGMaterial[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	const float clearGEmissive[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	glClearBufferfv(GL_COLOR, 0, clearGPos);
	glClearBufferfv(GL_COLOR, 1, clearGNormal);
	glClearBufferfv(GL_COLOR, 2, clearGAlbedo);
	glClearBufferfv(GL_COLOR, 3, clearGMaterial);
	glClearBufferfv(GL_COLOR, 4, clearGEmissive);

	auto deferProcessShader = ShaderManager::GetInstance().GetShader(ShaderManager::DeferProcess);
	if (!deferProcessShader) return;
	deferProcessShader->use();
	auto& opaqueList = scene->GetOpaqueMeshes();
	{
		MaterialBatchScope materialBatch;
		for (const auto& item : opaqueList) {
			if (!item.model || !item.mesh) continue;
			deferProcessShader->setMat4("model", item.modelMatrix);
			item.mesh->Draw(
				deferProcessShader.get(),
				item.shader && item.shader->shaderName == "pbr");
		}
	}

	if (properties.SSAO) {
		m_ssao.Render(scene, m_gbufferFBO);
	}
	else if (m_ssao.GetOutputFBO()) {
		m_ssao.Destroy();
		fbMgr.TrimUnusedFBOs();
	}
	const FBO* ssaoFBO = properties.SSAO ? m_ssao.GetOutputFBO() : nullptr;
	const bool useSSAOInLighting = ssaoFBO && !ssaoFBO->textureIDs.empty();

	// Copy depth to output target so skybox / transparent passes can share it.
	GLState::BindFramebuffer(GL_READ_FRAMEBUFFER, m_gbufferFBO->framebufferID);
	GLState::BindFramebuffer(GL_DRAW_FRAMEBUFFER, m_outputFBO->framebufferID);
	glBlitFramebuffer(
		0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT,
		0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT,
		GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST
	);

	// 2) Lighting pass: GBuffer -> HDR（全屏 或 平行光全屏 + 点光源光体积）
	GLState::BindFramebuffer(GL_FRAMEBUFFER, m_outputFBO->framebufferID);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	GLState::StencilMask(0xFF);
	glClearStencil(0);
	glClear(GL_STENCIL_BUFFER_BIT);
	GLState::Disable(GL_DEPTH_TEST);

	// The PBR path includes one non-additive IBL/emissive contribution. Until the
	// light-volume shaders gain a dedicated ambient pass, use the correct
	// fullscreen path whenever a PBR material is present.
	const bool useLightVolumes = properties.LIGHT_VOLUME && !scene->UsesPbrMaterials();
	if (!useLightVolumes) {
		auto deferLightShader = ShaderManager::GetInstance().GetShader(ShaderManager::Defer);
		if (!deferLightShader) return;
		deferLightShader->use();
		scene->SetLightUniforms(*deferLightShader);
		unsigned int texSlot = scene->SetShadowMap(*deferLightShader);
		if (scene->camera_ptr) {
			deferLightShader->setVec3("viewPos", scene->camera_ptr->cameraPos);
		}
		BindGBufferTextures(*deferLightShader, texSlot);
		deferLightShader->setBool("useSSAO", useSSAOInLighting);
		if (useSSAOInLighting) {
			GLState::ActiveTexture(GL_TEXTURE0 + texSlot);
			GLState::BindTexture(GL_TEXTURE_2D, ssaoFBO->textureIDs[0]);
			deferLightShader->setInt("ssaoMap", texSlot);
			++texSlot;
		}
		texSlot = scene->BindImageBasedLighting(*deferLightShader, texSlot);

		GLState::BindVertexArray(globalVAOs.quadVAO);
		PerformanceProfiler::GetInstance().RecordDraw(GL_TRIANGLES, 6);
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
		deferDirShader->setBool("useSSAO", useSSAOInLighting);
		if (useSSAOInLighting) {
			GLState::ActiveTexture(GL_TEXTURE0 + texSlot);
			GLState::BindTexture(GL_TEXTURE_2D, ssaoFBO->textureIDs[0]);
			deferDirShader->setInt("ssaoMap", texSlot);
			++texSlot;
		}

		GLState::BindVertexArray(globalVAOs.quadVAO);
		PerformanceProfiler::GetInstance().RecordDraw(GL_TRIANGLES, 6);
		glDrawArrays(GL_TRIANGLES, 0, 6);

		DrawPointLightVolumesDeferred(scene);
	}

	// 3) Forward extras on top of deferred base.
	GLState::BindFramebuffer(GL_FRAMEBUFFER, m_outputFBO->framebufferID);
	GLState::Enable(GL_DEPTH_TEST);
	// Forward stage relies on USED_TEXTURE_NUM for per-material/skybox bindings.
	// Reset here to avoid stale texture unit growth across deferred lighting paths.
	properties.USED_TEXTURE_NUM = 0;
	GLState::ActiveTexture(GL_TEXTURE0);
	scene->DrawPointLights();
	if (scene->camera_ptr) {
		scene->DrawSkybox(scene->camera_ptr->GetViewMatrix());
	}
	scene->DrawTransparentModels();
	scene->DrawOutlines();

	GLState::BindFramebuffer(GL_FRAMEBUFFER, 0);
}

void DeferRenderPass::Destroy()
{
	m_ssao.Destroy();
	FramebuffersManager::GetInstance().ReleaseFBO(m_gbufferFBO);
	m_gbufferFBO = nullptr;
	FramebuffersManager::GetInstance().ReleaseFBO(m_outputFBO);
	m_outputFBO = nullptr;
	m_hasAttr = false;
	FramebuffersManager::GetInstance().TrimUnusedFBOs();
}
