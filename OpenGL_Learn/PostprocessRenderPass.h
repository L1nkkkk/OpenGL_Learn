#pragma once
#include "RenderPass.h"

class PostprocessRenderPass :
    public RenderPass
{
public:
	PostprocessRenderPass() : RenderPass("PostprocessRenderPass") {}

	void Init(int width, int height) override;

	void Render(Scene* scene, const FBO* inputFBO = nullptr) override;

	void Destroy() override;
protected:
	FBO* m_inputFBO = nullptr;
	FBO* m_outputFBO = nullptr;
}; 