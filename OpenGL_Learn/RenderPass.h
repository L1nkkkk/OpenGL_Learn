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

	// 记录当前 Pass 使用的 FBO 属性，用于检测 SystemProperties 变化后是否需要重新获取 FBO
	FBOAttributes m_lastAttr{};
	bool m_hasAttr = false;

	// 子类可重写该函数，根据当前 SystemProperties 生成自己需要的 FBOAttributes
	virtual FBOAttributes BuildAttributesFromSystemProperties() {
		return FramebuffersManager::GenCurrentAttr();
	}

	// 基类统一封装：根据 SystemProperties 变化，自动从 FramebuffersManager 获取 / 切换 FBO
	void UpdateFBOFromSystemProperties() {
		FBOAttributes attr = BuildAttributesFromSystemProperties();
		if (!m_hasAttr || !(attr == m_lastAttr)) {
			auto& framebufferManager = FramebuffersManager::GetInstance();
			framebufferManager.ReleaseFBO(m_outputFBO);
			m_lastAttr = attr;
			m_hasAttr = true;
			m_outputFBO = framebufferManager.GetFBO(attr);
			if (m_outputFBO) {
				m_outputFBO->passName = GetPassName();
				framebufferManager.RegisterFBO(GetPassName(), m_outputFBO);
			}
			// Configuration switches are infrequent; release the old backing storage
			// instead of retaining every historical render-target variant forever.
			framebufferManager.TrimUnusedFBOs();
		}
	}
};

