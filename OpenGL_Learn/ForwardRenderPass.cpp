#include "ForwardRenderPass.h"
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
	// ForwardPass 额外输出 AO 所需的场景信息：
	// 1) depth texture（用于后处理采样）
	// 2) normal texture（用于后处理采样）
	// 注意：depth texture 不支持 MSAA，这里强制使用非 MSAA。
	attr.aaType = AntiAliasManager::AntiAliasType::Default;
	attr.hasDepthTexture = true;

	// ForwardPass 输出：
	// color[0] = HDR 颜色
	// color[1] = Bloom BrightColor（即使 BLOOM 关闭也保留，方便 normal 固定落在 attachment2）
	// color[2] = Normal（给 AO 使用）
	attr.textureAttrs.clear();
	attr.textureAttrs.push_back({ GL_TEXTURE_2D, GL_RGBA16F, GL_RGBA, GL_FLOAT }); // scene HDR
	attr.textureAttrs.push_back({ GL_TEXTURE_2D, GL_RGBA16F, GL_RGBA, GL_FLOAT }); // bloom bright
	attr.textureAttrs.push_back({ GL_TEXTURE_2D, GL_RGB16F, GL_RGB, GL_FLOAT });   // normal
	return attr;
}

void ForwardRenderPass::Render(Scene* scene, const FBO* inputFBO)
{
	// 每帧渲染前，根据 SystemProperties 变化自动切换 / 重建 FBO
	UpdateFBOFromSystemProperties();

	scene->DrawShadowMap();
	glBindFramebuffer(GL_FRAMEBUFFER, m_outputFBO->framebufferID);
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_STENCIL_TEST);

	// Optimization:
	// Color2(normal) attachment is only needed when the user is actively viewing it in the viewport.
	// When not needed, skip the extra opaque re-draw (AOInfo pass) and skip depth blit.
	// However: if we will run postprocess that needs normals/depth (AO/GTAO/etc),
	// we must also render attachment2 during the normal forward pass.
	bool renderNormalAttachment2 = false;
	{
		auto& props = SystemProperties::GetInstance();
		// Postprocess is only executed when !DEBUG_MODE in test.cpp.
		// When we add AO/GTAO later, it will read depth+normal, so ensure Color2 is valid then too.
		if (!props.DEBUG_MODE) {
			renderNormalAttachment2 = true;
		}
		if (props.VIEWPORT_DEBUG_FBO_INDEX >= 1) {
			int fboIdx = props.VIEWPORT_DEBUG_FBO_INDEX - 1;
			// Find selected FBO and check if it's our current output FBO.
			const std::vector<FBO*> busyFBOs = FramebuffersManager::GetInstance().GetBusyFBOs();
			if (fboIdx >= 0 && fboIdx < (int)busyFBOs.size()) {
				FBO* selected = busyFBOs[fboIdx];
				if (selected == m_outputFBO && props.VIEWPORT_DEBUG_ATTACHMENT_INDEX == 2) {
					renderNormalAttachment2 = true;
				}
			}
		}
	}

	glStencilFunc(GL_ALWAYS, 0, 0xFF);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClearStencil(0);

	glStencilMask(0xFF);
	// Clear required attachments.
	// If we don't render normal attachment2, don't clear it (it's not used).
	if (renderNormalAttachment2) {
		GLenum allDrawBuffersForClear[3] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
		glDrawBuffers(3, allDrawBuffersForClear);
	}
	else {
		GLenum allDrawBuffersForClear[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
		glDrawBuffers(2, allDrawBuffersForClear);
	}
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	glStencilMask(0x00);

	// Opaque pass: optionally write attachment2(Color2/Normal) for debug/AO input.
	GLenum colorDrawBuffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
	if (renderNormalAttachment2) {
		GLenum drawBuffers3[3] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
		glDrawBuffers(3, drawBuffers3);
	}
	else {
		glDrawBuffers(2, colorDrawBuffers);
	}

	//Draw scene in the following order
	// Draw Opacity Models (先绘制所有不透明物体，记录需要outline的物体到stencil buffer)
	auto& opaqueList = scene->GetOpaqueMeshes();
	auto& transparentList = scene->GetTransparentMeshes();
	auto& pointLightModels = scene->GetLightSource().GetPointLights();
	Shader* lastShader = nullptr;
	for (const auto& item : opaqueList) {
		if (!item.shader || !item.model || !item.mesh) continue;
		if (item.shader != lastShader) {
			lastShader = item.shader;
			lastShader->use();
			scene->SetLightUniforms(*lastShader);
		}
		lastShader->setMat4("model", item.model->getModelMatrix());
		item.mesh->Draw();
	}

	// After opaque pass: don't overwrite attachment2 during lighting/skybox/transparent.
	glDrawBuffers(2, colorDrawBuffers);

	for (auto& light : pointLightModels) {
		light.Draw();
	}
	// Draw Skybox (使用深度测试优化，但不影响stencil buffer)
	scene->DrawSkybox(scene->camera_ptr->GetViewMatrix());
	// Draw Transparent Meshes
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDepthMask(GL_FALSE);

	// If the user is viewing Color2, also write normals for transparent meshes.
	// Keep attachment2 write enabled only for this transparent pass to avoid lighting/skybox overwriting it.
	if (renderNormalAttachment2) {
		GLenum drawBuffers3[3] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
		glDrawBuffers(3, drawBuffers3);
	}

	lastShader = nullptr;
	for (const auto& item : transparentList) {
		if (!item.shader || !item.model || !item.mesh) continue;
		if (item.shader != lastShader) {
			lastShader = item.shader;
			lastShader->use();
			scene->SetLightUniforms(*lastShader);
		}
		lastShader->setMat4("model", item.model->getModelMatrix());
		item.mesh->Draw();
	}
	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
	// 最后绘制outline（禁用深度测试，基于stencil buffer绘制）
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void ForwardRenderPass::Destroy()
{
	FramebuffersManager::GetInstance().ReleaseFBO(m_outputFBO);
}