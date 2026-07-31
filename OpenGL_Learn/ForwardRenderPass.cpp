#include "ForwardRenderPass.h"
#include "GLStateCache.h"
#include "Profiler.h"
#include "ShaderManager.h"

void ForwardRenderPass::Init(int width, int height)
{
	// 初始化时根据当前 SystemProperties 构建并获取合适的 FBO
	UpdateFBOFromSystemProperties();
}

FBOAttributes ForwardRenderPass::BuildAttributesFromSystemProperties()
{
	// 基于当前全局配置生成 Forward 渲染需要的 FBOAttributes
	FBOAttributes attr = FramebuffersManager::GenCurrentAttr();
	// Keep the forward target non-MSAA. The optional normal attachment is only
	// allocated when explicitly enabled for diagnostics or a future AO consumer.
	attr.aaType = AntiAliasManager::AntiAliasType::Default;
	attr.hasDepthTexture = false;

	// ForwardPass 输出：
	// color[0] = HDR 颜色
	// color[1] = Bloom BrightColor（即使 BLOOM 关闭也保留，方便 normal 固定落在 attachment2）
	// color[2] = optional world-space normal
	attr.textureAttrs.clear();
	attr.textureAttrs.push_back({ GL_TEXTURE_2D, GL_RGBA16F, GL_RGBA, GL_FLOAT }); // scene HDR
	const auto& properties = SystemProperties::GetInstance();
	if (attr.isBloom || properties.FORWARD_NORMAL_BUFFER) {
		attr.textureAttrs.push_back({ GL_TEXTURE_2D, GL_RGBA16F, GL_RGBA, GL_FLOAT }); // bloom bright
	}
	if (properties.FORWARD_NORMAL_BUFFER) {
		attr.textureAttrs.push_back({ GL_TEXTURE_2D, GL_RGB16F, GL_RGB, GL_FLOAT }); // normal
	}
	return attr;
}

void ForwardRenderPass::Render(Scene* scene, const FBO* inputFBO)
{
	PERF_CPU_SCOPE("Forward Pass");
	PERF_GPU_SCOPE("Forward Pass");
	if (!scene) return;

	// 每帧渲染前，根据 SystemProperties 变化自动切换 / 重建 FBO
	UpdateFBOFromSystemProperties();

	scene->PrepareRenderData();
	scene->DrawShadowMap();
	GLState::BindFramebuffer(GL_FRAMEBUFFER, m_outputFBO->framebufferID);
	GLState::Enable(GL_DEPTH_TEST);
	GLState::Enable(GL_STENCIL_TEST);

	// Avoid the third MRT write unless a consumer explicitly requests it.
	const bool renderNormalAttachment2 =
		SystemProperties::GetInstance().FORWARD_NORMAL_BUFFER &&
		m_outputFBO &&
		m_outputFBO->textureIDs.size() > 2;
	const bool renderBrightAttachment1 =
		m_outputFBO && m_outputFBO->textureIDs.size() > 1;

	GLState::StencilFunc(GL_ALWAYS, 0, 0xFF);
	GLState::StencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClearStencil(0);

	GLState::StencilMask(0xFF);
	// Clear required attachments.
	// If we don't render normal attachment2, don't clear it (it's not used).
	if (renderNormalAttachment2) {
		GLenum allDrawBuffersForClear[3] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
		glDrawBuffers(3, allDrawBuffersForClear);
	}
	else if (renderBrightAttachment1) {
		GLenum allDrawBuffersForClear[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
		glDrawBuffers(2, allDrawBuffersForClear);
	}
	else {
		glDrawBuffer(GL_COLOR_ATTACHMENT0);
	}
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	GLState::StencilMask(0x00);

	// Opaque pass: optionally write attachment2(Color2/Normal) for debug/AO input.
	GLenum colorDrawBuffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
	if (renderNormalAttachment2) {
		GLenum drawBuffers3[3] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
		glDrawBuffers(3, drawBuffers3);
	}
	else if (renderBrightAttachment1) {
		glDrawBuffers(2, colorDrawBuffers);
	}
	else {
		glDrawBuffer(GL_COLOR_ATTACHMENT0);
	}

	//Draw scene in the following order
	// Draw Opacity Models (先绘制所有不透明物体，记录需要outline的物体到stencil buffer)
	auto& opaqueList = scene->GetOpaqueMeshes();
	auto& transparentList = scene->GetTransparentMeshes();
	auto& pointLightModels = scene->GetLightSource().GetPointLights();
	Shader* lastShader = nullptr;
	{
		MaterialBatchScope materialBatch;
		for (const auto& item : opaqueList) {
			if (!item.shader || !item.model || !item.mesh) continue;
			if (item.shader != lastShader) {
				lastShader = item.shader;
				lastShader->use();
				SystemProperties::GetInstance().USED_TEXTURE_NUM = 0;
				scene->SetLightUniforms(*lastShader);
				SystemProperties::GetInstance().USED_TEXTURE_NUM =
					scene->SetShadowMap(
						*lastShader,
						Scene::ShadowLightBinding::AllLights,
						10);
				if (scene->camera_ptr) {
					lastShader->setVec3("viewPos", scene->camera_ptr->cameraPos);
				}
				SystemProperties::GetInstance().USED_TEXTURE_NUM =
					scene->BindImageBasedLighting(
						*lastShader,
						SystemProperties::GetInstance().USED_TEXTURE_NUM);
			}
			lastShader->setMat4("model", item.modelMatrix);
			item.mesh->Draw(lastShader);
		}
	}

	// After opaque pass: don't overwrite attachment2 during lighting/skybox/transparent.
	if (renderBrightAttachment1) {
		glDrawBuffers(2, colorDrawBuffers);
	}
	else {
		glDrawBuffer(GL_COLOR_ATTACHMENT0);
	}

	{
		MaterialBatchScope materialBatch;
		for (auto& light : pointLightModels) {
			light.Draw();
		}
	}
	// Draw Skybox (使用深度测试优化，但不影响stencil buffer)
	scene->DrawSkybox(scene->camera_ptr->GetViewMatrix());
	// Draw Transparent Meshes
	GLState::Enable(GL_BLEND);
	GLState::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	GLState::DepthMask(false);

	// If the user is viewing Color2, also write normals for transparent meshes.
	// Keep attachment2 write enabled only for this transparent pass to avoid lighting/skybox overwriting it.
	if (renderNormalAttachment2) {
		GLenum drawBuffers3[3] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
		glDrawBuffers(3, drawBuffers3);
	}

	lastShader = nullptr;
	{
		MaterialBatchScope materialBatch;
		for (const auto& item : transparentList) {
			if (!item.shader || !item.model || !item.mesh) continue;
			if (item.shader != lastShader) {
				lastShader = item.shader;
				lastShader->use();
				SystemProperties::GetInstance().USED_TEXTURE_NUM = 0;
				scene->SetLightUniforms(*lastShader);
				SystemProperties::GetInstance().USED_TEXTURE_NUM =
					scene->SetShadowMap(
						*lastShader,
						Scene::ShadowLightBinding::AllLights,
						10);
				if (scene->camera_ptr) {
					lastShader->setVec3("viewPos", scene->camera_ptr->cameraPos);
				}
				SystemProperties::GetInstance().USED_TEXTURE_NUM =
					scene->BindImageBasedLighting(
						*lastShader,
						SystemProperties::GetInstance().USED_TEXTURE_NUM);
			}
			lastShader->setMat4("model", item.modelMatrix);
			item.mesh->Draw(lastShader);
		}
	}
	GLState::DepthMask(true);
	GLState::Disable(GL_BLEND);
	// 最后绘制outline（禁用深度测试，基于stencil buffer绘制）
	GLState::BindFramebuffer(GL_FRAMEBUFFER, 0);
}

void ForwardRenderPass::Destroy()
{
	FramebuffersManager::GetInstance().ReleaseFBO(m_outputFBO);
	m_outputFBO = nullptr;
	m_hasAttr = false;
}
