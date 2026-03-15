#include "ForwardRenderPass.h"

void ForwardRenderPass::Init(int width, int height)
{
	// 初始化时根据当前 SystemProperties 构建并获取合适的 FBO
	UpdateFBOFromSystemProperties();
}

FBOAttributes ForwardRenderPass::BuildAttributesFromSystemProperties()
{
	// 基于当前全局配置生成 Forward 渲染需要的 FBOAttributes
	FBOAttributes attr = FramebuffersManager::GenCurrentAttr();
	// ForwardPass 输出一个 HDR 颜色附件，是否 Bloom / HDR / Gamma 等由 attr 内部标志控制
	attr.textureAttrs.clear();
	attr.textureAttrs.push_back({ GL_TEXTURE_2D, GL_RGBA16F, GL_RGBA, GL_FLOAT });
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

	glStencilFunc(GL_ALWAYS, 0, 0xFF);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClearStencil(0);

	glStencilMask(0xFF);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	glStencilMask(0x00);
	//Draw scene in the following order
	// Draw Opacity Models (先绘制所有不透明物体，记录需要outline的物体到stencil buffer)
	auto& opacityModels = scene->GetModelSource().GetOpaqueModelsMap();
	auto& transparentModels = scene->GetModelSource().GetTransparentModels(scene->camera_ptr);
	auto& pointLightModels = scene->GetLightSource().GetPointLights();
	for (auto& [shader, models] : opacityModels) {
		shader->use();
		scene->SetLightUniforms(*shader);
		for (auto& model : models) {
			model->Draw();
		}
	}
	for (auto& light : pointLightModels) {
		light.Draw();
	}
	// Draw Skybox (使用深度测试优化，但不影响stencil buffer)
	scene->DrawSkybox(scene->camera_ptr->GetViewMatrix());
	// Draw Transparent Models
	for(auto& model : transparentModels) {
		auto shader = model->GetShader();
		shader->use();
		scene->SetLightUniforms(*shader);
		model->Draw();
	}
	// 最后绘制outline（禁用深度测试，基于stencil buffer绘制）
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void ForwardRenderPass::Destroy()
{
	FramebuffersManager::GetInstance().ReleaseFBO(m_outputFBO);
}