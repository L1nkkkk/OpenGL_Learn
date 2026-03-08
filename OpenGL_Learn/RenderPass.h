#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <glad/glad.h>
#include "Global.h"
#include "Scene.h"

class RenderPass {
public:
	RenderPass(const std::string& name) : m_passName(name) {}
	virtual ~RenderPass() = default;

	virtual void Init(int width, int height) = 0;

	virtual void Render(Scene* ,const FBO* inputFBO = nullptr) = 0;

	virtual void Destroy() = 0;

	virtual FBO* GetOutputFBO() const { return m_outputFBO; }

	std::string GetPassName() const { return m_passName; }

	void SetEnable(bool enable) { m_enable = enable; }

	bool IsEnabled() const { return m_enable; }

protected:
	std::string m_passName;
	bool m_enable = true;
	FBO* m_outputFBO = nullptr;
};

