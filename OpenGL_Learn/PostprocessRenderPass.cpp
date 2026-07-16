#include "PostprocessRenderPass.h"
#include "GLStateCache.h"
#include "Global.h"
#include "Profiler.h"

// Init: Get FBO to save final postprocess color
void PostprocessRenderPass::Init(int width, int height) {
    this->UpdateFBOFromSystemProperties();
}

FBOAttributes PostprocessRenderPass::BuildAttributesFromSystemProperties() {
    // 输出为最终后处理结果：LDR color（HDR / Gamma / Bloom 都在 shader 里做）
    FBOAttributes attr = FramebuffersManager::GenCurrentAttr();
    attr.isHDR = false;
    attr.textureAttrs.clear();
    attr.textureAttrs.push_back({ GL_TEXTURE_2D, GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE });
    return attr;
}

FBOAttributes PostprocessRenderPass::BuildBloomAttributes() const {
	FBOAttributes attr;
	attr.aaType = AntiAliasManager::AntiAliasType::Default;
	attr.isHDR = true;
	attr.textureAttrs.push_back({ GL_TEXTURE_2D, GL_RGBA16F, GL_RGBA, GL_FLOAT });
	return attr;
}

bool PostprocessRenderPass::EnsureBloomTargets() {
	const FBOAttributes bloomAttr = BuildBloomAttributes();
	if (m_bloomBlurFBO &&
		m_bloomPingFBO &&
		m_bloomBlurFBO->attr == bloomAttr &&
		m_bloomPingFBO->attr == bloomAttr) {
		return true;
	}

	auto& framebufferManager = FramebuffersManager::GetInstance();
	framebufferManager.ReleaseFBO(m_bloomBlurFBO);
	framebufferManager.ReleaseFBO(m_bloomPingFBO);
	m_bloomBlurFBO = framebufferManager.GetFBO(bloomAttr);
	m_bloomPingFBO = framebufferManager.GetFBO(bloomAttr);

	if (!m_bloomBlurFBO || !m_bloomPingFBO || m_bloomBlurFBO == m_bloomPingFBO) {
		framebufferManager.ReleaseFBO(m_bloomBlurFBO);
		framebufferManager.ReleaseFBO(m_bloomPingFBO);
		m_bloomBlurFBO = nullptr;
		m_bloomPingFBO = nullptr;
		return false;
	}

	m_bloomBlurFBO->passName = "PostprocessRenderPass_BloomBlur";
	m_bloomPingFBO->passName = "PostprocessRenderPass_BloomPing";
	return true;
}

void PostprocessRenderPass::Render(Scene* scene, const FBO* inputFBO) {
    PERF_CPU_SCOPE("Postprocess Pass");
    PERF_GPU_SCOPE("Postprocess Pass");
    if (!inputFBO || inputFBO->textureIDs.empty()) return;

    this->UpdateFBOFromSystemProperties();
    if (!m_outputFBO) return;

    auto& properties = SystemProperties::GetInstance();
    bool bloomReady = false;

    // 1) Bloom blur（可选）：把 input 的 BrightColor 模糊到 m_bloomBlurFBO->textureIDs[0]
    if (properties.BLOOM &&
        properties.BLOOM_BLUR_ITERATIONS > 0 &&
        inputFBO->textureIDs.size() > 1 &&
        EnsureBloomTargets()) {
        PERF_GPU_SCOPE("Bloom Blur");
        bloomReady = Blur(properties.BLOOM_BLUR_ITERATIONS, inputFBO->textureIDs[1]);
    }

    // 2) Final composite (HDR -> tone map, gamma, bloom add) into m_outputFBO
    auto screenShader = ShaderManager::GetInstance().GetShader(ShaderManager::Scene);
    if (!screenShader) return;

    {
        PERF_GPU_SCOPE("Postprocess Composite");
        GLState::BindFramebuffer(GL_FRAMEBUFFER, m_outputFBO->framebufferID);
        GLState::Disable(GL_DEPTH_TEST);
        GLState::BindVertexArray(globalVAOs.quadVAO);

        GLState::ActiveTexture(GL_TEXTURE0);
        GLState::BindTexture(GL_TEXTURE_2D, inputFBO->textureIDs[0]);

        GLState::ActiveTexture(GL_TEXTURE1);
        if (bloomReady && m_bloomBlurFBO && !m_bloomBlurFBO->textureIDs.empty()) {
            GLState::BindTexture(GL_TEXTURE_2D, m_bloomBlurFBO->textureIDs[0]);
        } else {
            GLState::BindTexture(GL_TEXTURE_2D, 0);
        }

        screenShader->use();
        screenShader->setInt("screenTexture", 0);
        screenShader->setInt("bloomBlur", 1);
        PerformanceProfiler::GetInstance().RecordDraw(GL_TRIANGLES, 6);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    GLState::BindFramebuffer(GL_FRAMEBUFFER, 0);
}

void PostprocessRenderPass::Destroy() {
    if (m_outputFBO) {
        FramebuffersManager::GetInstance().ReleaseFBO(m_outputFBO);
        m_outputFBO = nullptr;
    }
    if (m_bloomBlurFBO) {
        FramebuffersManager::GetInstance().ReleaseFBO(m_bloomBlurFBO);
        m_bloomBlurFBO = nullptr;
    }
    if (m_bloomPingFBO) {
        FramebuffersManager::GetInstance().ReleaseFBO(m_bloomPingFBO);
        m_bloomPingFBO = nullptr;
    }
}

void PostprocessRenderPass::ProcessBloom(FBO* input) {
    if(!input) return;
    auto& properties = SystemProperties::GetInstance();
    if (!properties.BLOOM) return;
    if (properties.BLOOM_BLUR_ITERATIONS <= 0) return;
    if (input->textureIDs.size() <= 1) return;

    if (EnsureBloomTargets()) {
        Blur(properties.BLOOM_BLUR_ITERATIONS, input->textureIDs[1]);
    }
}

bool PostprocessRenderPass::Blur(int times, unsigned int textureID) {
    if (times <= 0 || !m_bloomBlurFBO || !m_bloomPingFBO) return false;
    auto& properties = SystemProperties::GetInstance();
    auto bulrShader = ShaderManager::GetInstance().GetShader(ShaderManager::Bulr);
    if (!bulrShader) return false;
    GLboolean horizontal = true;
    const GLuint amount = static_cast<GLuint>(times) * 2u;
    unsigned int sourceTexture = textureID;
    bulrShader->use();
    // Blur 是全屏 quad pass：不要受前序 Forward 的深度/模板状态影响
    GLState::Disable(GL_DEPTH_TEST);
    GLState::Disable(GL_STENCIL_TEST);
    glViewport(0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT);
    GLState::BindVertexArray(globalVAOs.quadVAO);
    for (GLuint i = 0; i < amount; i++)
    {
        FBO* target = (i % 2u == 0u) ? m_bloomPingFBO : m_bloomBlurFBO;
        GLState::BindFramebuffer(GL_FRAMEBUFFER, target->framebufferID);
        bulrShader->setBool("horizontal", horizontal);
        GLState::ActiveTexture(GL_TEXTURE0);
        GLState::BindTexture(GL_TEXTURE_2D, sourceTexture);
        bulrShader->setInt("image", 0);
        PerformanceProfiler::GetInstance().RecordDraw(GL_TRIANGLES, 6);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        sourceTexture = target->textureIDs[0];
        horizontal = !horizontal;
    }
    GLState::BindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}
