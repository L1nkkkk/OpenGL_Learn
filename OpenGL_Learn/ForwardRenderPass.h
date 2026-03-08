#pragma once
#include "RenderPass.h"

class ForwardRenderPass :
    public RenderPass
{
public:
	ForwardRenderPass() : RenderPass("ForwardRenderPass") {}

	void Init(int width, int height) override;

	void Render(Scene* scene, const FBO* inputFBO = nullptr) override;
	
	void Destroy() override;
private:
};

