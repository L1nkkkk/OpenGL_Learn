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

void PostprocessRenderPass::Render(Scene* scene, const FBO* inputFBO) {
    PERF_CPU_SCOPE("Postprocess Pass");
    PERF_GPU_SCOPE("Postprocess Pass");
    if (!inputFBO || inputFBO->textureIDs.empty()) return;

    this->UpdateFBOFromSystemProperties();
    if (!m_outputFBO) return;

    auto& properties = SystemProperties::GetInstance();

    // 1) Bloom blur（可选）：把 input 的 BrightColor 模糊到 m_bloomBlurFBO->textureIDs[0]
    if (properties.BLOOM && inputFBO->textureIDs.size() > 1) {
        // 申请/更新 Bloom blur 目标 FBO（单附件 HDR 纹理即可）
        FBOAttributes bloomAttr = FramebuffersManager::GenCurrentAttr();
        bloomAttr.isHDR = properties.USE_HDR;
        bloomAttr.textureAttrs.clear();
        bloomAttr.textureAttrs.push_back({ GL_TEXTURE_2D, GL_RGBA16F, GL_RGBA, GL_FLOAT });
        if (!m_bloomBlurFBO || !(m_bloomBlurFBO->attr == bloomAttr)) {
            FramebuffersManager::GetInstance().ReleaseFBO(m_bloomBlurFBO);
            m_bloomBlurFBO = FramebuffersManager::GetInstance().GetFBO(bloomAttr);
            if (m_bloomBlurFBO) {
                m_bloomBlurFBO->passName = "PostprocessRenderPass_BloomBlur";
            }
        }

        if (m_bloomBlurFBO) {
            // Blur input bright texture into m_bloomBlurFBO->textureIDs[0]
            Blur(properties.BLOOM_BLUR_ITERATIONS, const_cast<unsigned int&>(inputFBO->textureIDs[1]));
        }
    }

    // 2) Final composite (HDR -> tone map, gamma, bloom add) into m_outputFBO
    auto screenShader = ShaderManager::GetInstance().GetShader(ShaderManager::Scene);
    if (!screenShader) return;

    GLState::BindFramebuffer(GL_FRAMEBUFFER, m_outputFBO->framebufferID);
    GLState::Disable(GL_DEPTH_TEST);
    GLState::BindVertexArray(globalVAOs.quadVAO);

    GLState::ActiveTexture(GL_TEXTURE0);
    GLState::BindTexture(GL_TEXTURE_2D, inputFBO->textureIDs[0]);

    GLState::ActiveTexture(GL_TEXTURE1);
    if (properties.BLOOM && m_bloomBlurFBO && !m_bloomBlurFBO->textureIDs.empty()) {
        GLState::BindTexture(GL_TEXTURE_2D, m_bloomBlurFBO->textureIDs[0]);
    } else {
        GLState::BindTexture(GL_TEXTURE_2D, 0);
    }

    screenShader->use();
    screenShader->setInt("screenTexture", 0);
    screenShader->setInt("bloomBlur", 1);
    PerformanceProfiler::GetInstance().RecordDraw(GL_TRIANGLES, 6);
    glDrawArrays(GL_TRIANGLES, 0, 6);

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
}

void PostprocessRenderPass::ProcessBloom(FBO* input) {
    if(!input) return;
    auto& properties = SystemProperties::GetInstance();
    if (!properties.BLOOM) return;
    if (input->textureIDs.size() <= 1) return;

    // Ensure bloom blur target exists, then blur input bright texture into it
    FBOAttributes bloomAttr = FramebuffersManager::GenCurrentAttr();
    bloomAttr.isHDR = properties.USE_HDR;
    bloomAttr.textureAttrs.clear();
    bloomAttr.textureAttrs.push_back({ GL_TEXTURE_2D, GL_RGBA16F, GL_RGBA, GL_FLOAT });
    if (!m_bloomBlurFBO || !(m_bloomBlurFBO->attr == bloomAttr)) {
        FramebuffersManager::GetInstance().ReleaseFBO(m_bloomBlurFBO);
        m_bloomBlurFBO = FramebuffersManager::GetInstance().GetFBO(bloomAttr);
        if (m_bloomBlurFBO) {
            m_bloomBlurFBO->passName = "PostprocessRenderPass_BloomBlur";
        }
    }

    if (m_bloomBlurFBO) {
        Blur(properties.BLOOM_BLUR_ITERATIONS, input->textureIDs[1]);
    }
}

void PostprocessRenderPass::Blur(int times, unsigned int& textureID) {
    if (times <= 0 || !m_bloomBlurFBO) return;
    auto& properties = SystemProperties::GetInstance();
    auto bulrShader = ShaderManager::GetInstance().GetShader(ShaderManager::Bulr);
    if (!bulrShader) return;
    FBO* fbos[2];
    FBOAttributes attr;
    attr.isHDR = properties.USE_HDR;
    attr.textureAttrs.clear();
    attr.textureAttrs.push_back({ GL_TEXTURE_2D, GL_RGBA16F, GL_RGBA, GL_FLOAT });
    fbos[0] = FramebuffersManager::GetInstance().GetFBO(attr);
    fbos[1] = FramebuffersManager::GetInstance().GetFBO(attr);
    GLboolean horizontal = true, first_iteration = true;
    GLuint amount = times << 1;
    bulrShader->use();
    // Blur 是全屏 quad pass：不要受前序 Forward 的深度/模板状态影响
    GLState::Disable(GL_DEPTH_TEST);
    GLState::Disable(GL_STENCIL_TEST);
    glViewport(0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT);
    GLState::BindVertexArray(globalVAOs.quadVAO);
    for (GLuint i = 0; i < amount; i++)
    {
        GLState::BindFramebuffer(GL_FRAMEBUFFER, fbos[i % 2]->framebufferID);
        bulrShader->setBool("horizontal", horizontal);
        GLState::ActiveTexture(GL_TEXTURE0);
        GLState::BindTexture(
            GL_TEXTURE_2D, first_iteration ? textureID : fbos[(i + 1) % 2]->textureIDs[0]
        );
        bulrShader->setInt("image", 0);
        PerformanceProfiler::GetInstance().RecordDraw(GL_TRIANGLES, 6);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        horizontal = !horizontal;
        if (first_iteration)
            first_iteration = false;
    }
    GLState::BindFramebuffer(GL_READ_FRAMEBUFFER, fbos[1]->framebufferID);
    GLState::BindFramebuffer(GL_DRAW_FRAMEBUFFER, m_bloomBlurFBO->framebufferID);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glBlitFramebuffer(0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT, 0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    GLState::BindFramebuffer(GL_FRAMEBUFFER, 0);
    FramebuffersManager::GetInstance().ReleaseFBO(fbos[0]);
    FramebuffersManager::GetInstance().ReleaseFBO(fbos[1]);
}
