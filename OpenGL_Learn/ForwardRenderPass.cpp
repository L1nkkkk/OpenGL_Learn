#include "ForwardRenderPass.h"

void ForwardRenderPass::Init(int width, int height)
{
	FBOAttributes attr = FramebuffersManager::GenCurrentAttr();
	attr.textureAttrs.push_back({ GL_TEXTURE_2D, GL_RGBA16F, GL_RGBA, GL_FLOAT });
	m_outputFBO = FramebuffersManager::GetInstance().GetFBO(attr);
	m_outputFBO->passName = GetPassName();

	FramebuffersManager::GetInstance().RegisterFBO(GetPassName(), m_outputFBO);
}

void ForwardRenderPass::Render(Scene* scene, const FBO* inputFBO)
{
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
	auto& transparentModels = scene->GetModelSource().GetTransparentModels();
	auto& pointLightModels = scene->GetLightSource().GetPointLights();
	for (auto& [shader, models] : opacityModels) {
		shader->use();
		scene->SetLightUniforms(*shader);
		for (auto& model : models) {
			model->Draw(*shader);
		}
	}
	for (auto& light : pointLightModels) {
		auto lightShader = light.GetShader();
		lightShader->use();
		light.Draw(*lightShader);
	}
	// 最后绘制outline（禁用深度测试，基于stencil buffer绘制）
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void ForwardRenderPass::Destroy()
{
	FramebuffersManager::GetInstance().ReleaseFBO(m_outputFBO);
}