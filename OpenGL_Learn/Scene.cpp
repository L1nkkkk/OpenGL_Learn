#include "Scene.h"
#include "GLStateCache.h"
#include "Profiler.h"

#include <functional>

void Scene::BuildMeshDrawLists()
{
	PERF_CPU_SCOPE("Build Draw Lists");
	m_opaqueMeshList.clear();
	m_transparentMeshList.clear();
	std::uint64_t activeModelCount = 0;

	for (auto& model : modelSource.models) {
		if (!model || !model->GetAcitveStatus()) continue;
		++activeModelCount;
		model->RefreshMaterialDrivenState();
		auto shaderPtr = model->GetShader();
		Shader* shader = shaderPtr.get();
		if (!shader) continue;

		for (const auto& entry : model->GetOpaqueMeshEntries()) {
			if (!entry.mesh) continue;
			if (!entry.mesh->GetActiveStatus()) continue;
			m_opaqueMeshList.push_back({ model.get(), entry.mesh, shader });
		}
		for (const auto& entry : model->GetTransparentMeshEntries()) {
			if (!entry.mesh) continue;
			if (!entry.mesh->GetActiveStatus()) continue;
			m_transparentMeshList.push_back({ model.get(), entry.mesh, shader });
		}
	}

	std::sort(m_opaqueMeshList.begin(), m_opaqueMeshList.end(),
		[](const MeshDrawItem& a, const MeshDrawItem& b) {
			const std::less<Shader*> shaderLess;
			if (a.shader != b.shader) {
				return shaderLess(a.shader, b.shader);
			}

			const std::less<Material*> materialLess;
			Material* aMaterial = a.mesh ? a.mesh->material_ptr : nullptr;
			Material* bMaterial = b.mesh ? b.mesh->material_ptr : nullptr;
			return materialLess(aMaterial, bMaterial);
		});

	if (camera_ptr) {
		std::sort(m_transparentMeshList.begin(), m_transparentMeshList.end(),
			[this](const MeshDrawItem& a, const MeshDrawItem& b) {
				glm::vec3 aPos = a.model ? a.model->position : glm::vec3(0);
				glm::vec3 bPos = b.model ? b.model->position : glm::vec3(0);
				float da = glm::length(camera_ptr->cameraPos - aPos);
				float db = glm::length(camera_ptr->cameraPos - bPos);
				return da > db;
			});
	}

	PerformanceProfiler::GetInstance().SetSceneSubmissionStats(
		activeModelCount,
		m_opaqueMeshList.size(),
		m_transparentMeshList.size());
}

void Scene::PrepareRenderData()
{
	BuildMeshDrawLists();
}

const std::vector<Scene::MeshDrawItem>& Scene::GetOpaqueMeshes() const
{
	return m_opaqueMeshList;
}

const std::vector<Scene::MeshDrawItem>& Scene::GetTransparentMeshes() const
{
	return m_transparentMeshList;
}
void Scene::Draw()
{
    PrepareRenderData();
    DrawShadowMap();
    auto attr = FramebuffersManager::GenCurrentAttr();
    fbo = FramebuffersManager::GetInstance().GetFBO(attr);
    GLState::BindFramebuffer(GL_FRAMEBUFFER, fbo->framebufferID);

    GLState::Enable(GL_DEPTH_TEST);
    GLState::Enable(GL_STENCIL_TEST);

    GLState::StencilFunc(GL_ALWAYS, 0, 0xFF);
    GLState::StencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearStencil(0);

    GLState::StencilMask(0xFF);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    GLState::StencilMask(0x00);
    //Draw scene in the following order
    if (properties.DEFER_RENDERING) {
        DrawDefferedModels();
    }
    else {
        DrawOpaqueModels();  // 先绘制所有不透明物体，记录需要outline的物体到stencil buffer
    }
    DrawNormalLines(); // 可选：绘制法线线段用于调试
    DrawPointLights();
    DrawSkybox(view);        // 绘制天空盒（使用深度测试优化，但不影响stencil buffer）
    DrawTransparentModels();  // 绘制透明物体
    DrawOutlines();
    // 最后绘制outline（禁用深度测试，基于stencil buffer绘制）
    GLState::BindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Scene::DrawDefferedModels()
{
    if (!properties.DEFER_RENDERING) return;
    FBOAttributes attr = FramebuffersManager::GenCurrentAttr();
    //反走样不支持MSAA
    attr.aaType = AntiAliasManager::AntiAliasType::Default;
    attr.isDefer = true;
    deferFBO = FramebuffersManager::GetInstance().GetFBO(attr);
    GLState::Disable(GL_BLEND);
    GLState::BindFramebuffer(GL_FRAMEBUFFER, deferFBO->framebufferID);
    GLState::StencilMask(0x00);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    GLState::StencilMask(0x00);
    deferShader->use();
    auto& list = GetOpaqueMeshes();
    {
        MaterialBatchScope materialBatch;
        for (const auto& item : list) {
            if (!item.model || !item.mesh) continue;
            deferShader->setMat4("model", item.model->getModelMatrix());
            item.mesh->Draw(deferShader.get());
        }
    }

    GLState::BindFramebuffer(GL_READ_FRAMEBUFFER, deferFBO->framebufferID);
    GLState::BindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo->framebufferID);
    glBlitFramebuffer(
        0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT, 0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT,
        GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST
    );

    GLState::BindFramebuffer(GL_FRAMEBUFFER, fbo->framebufferID);
    GLState::StencilMask(0xFF);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    if (!properties.LIGHT_VOLUME) {
        //默认光照计算
        GLState::Disable(GL_DEPTH_TEST);
        auto deferDrawShader = ShaderManager::GetInstance().GetShader(ShaderManager::Defer);
        deferDrawShader->use();
        SetLightUniforms(*deferDrawShader);
        properties.USED_TEXTURE_NUM = SetShadowMap(*deferDrawShader);
        deferDrawShader->setVec3("viewPos", camera_ptr->cameraPos);
        GLState::ActiveTexture(GL_TEXTURE0 + properties.USED_TEXTURE_NUM);
        GLState::BindTexture(GL_TEXTURE_2D, deferFBO->textureIDs[0]);
        deferDrawShader->setInt("gPosition", properties.USED_TEXTURE_NUM++);
        GLState::ActiveTexture(GL_TEXTURE0 + properties.USED_TEXTURE_NUM);
        GLState::BindTexture(GL_TEXTURE_2D, deferFBO->textureIDs[1]);
        deferDrawShader->setInt("gNormal", properties.USED_TEXTURE_NUM++);
        GLState::ActiveTexture(GL_TEXTURE0 + properties.USED_TEXTURE_NUM);
        GLState::BindTexture(GL_TEXTURE_2D, deferFBO->textureIDs[2]);
        deferDrawShader->setInt("gAlbedoSpec", properties.USED_TEXTURE_NUM++);
        GLState::ActiveTexture(GL_TEXTURE0 + properties.USED_TEXTURE_NUM);
        GLState::BindTexture(GL_TEXTURE_2D, deferFBO->textureIDs[3]);
        deferDrawShader->setInt("gMaterial", properties.USED_TEXTURE_NUM++);

        GLState::BindVertexArray(globalVAOs.quadVAO);
        PerformanceProfiler::GetInstance().RecordDraw(GL_TRIANGLES, 6);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        GLState::Enable(GL_DEPTH_TEST);
    }
    else {
        //使用延迟渲染计算平行光照
        GLState::Disable(GL_DEPTH_TEST);
        auto deferDirDrawShader = ShaderManager::GetInstance().GetShader(ShaderManager::DeferDirLightVolume);
        deferDirDrawShader->use();
        SetLightUniforms(*deferDirDrawShader);
        properties.USED_TEXTURE_NUM = SetShadowMap(*deferDirDrawShader);
        deferDirDrawShader->setVec3("viewPos", camera_ptr->cameraPos);
        GLState::ActiveTexture(GL_TEXTURE0 + properties.USED_TEXTURE_NUM);
        GLState::BindTexture(GL_TEXTURE_2D, deferFBO->textureIDs[0]);
        deferDirDrawShader->setInt("gPosition", properties.USED_TEXTURE_NUM++);
        GLState::ActiveTexture(GL_TEXTURE0 + properties.USED_TEXTURE_NUM);
        GLState::BindTexture(GL_TEXTURE_2D, deferFBO->textureIDs[1]);
        deferDirDrawShader->setInt("gNormal", properties.USED_TEXTURE_NUM++);
        GLState::ActiveTexture(GL_TEXTURE0 + properties.USED_TEXTURE_NUM);
        GLState::BindTexture(GL_TEXTURE_2D, deferFBO->textureIDs[2]);
        deferDirDrawShader->setInt("gAlbedoSpec", properties.USED_TEXTURE_NUM++);
        GLState::ActiveTexture(GL_TEXTURE0 + properties.USED_TEXTURE_NUM);
        GLState::BindTexture(GL_TEXTURE_2D, deferFBO->textureIDs[3]);
        deferDirDrawShader->setInt("gMaterial", properties.USED_TEXTURE_NUM++);
        GLState::BindVertexArray(globalVAOs.quadVAO);
        PerformanceProfiler::GetInstance().RecordDraw(GL_TRIANGLES, 6);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        //模板缓冲来计算受点光源影响的区域，减少光照计算的像素数量
        auto defaultShader = ShaderManager::GetInstance().GetShader(ShaderManager::Default);
        auto lightVolumeShader = ShaderManager::GetInstance().GetShader(ShaderManager::LightVolume);
        lightVolumeShader->use();
        lightVolumeShader->setVec3("viewPos", camera_ptr->cameraPos);
        GLState::ActiveTexture(GL_TEXTURE0);
        GLState::BindTexture(GL_TEXTURE_2D, deferFBO->textureIDs[0]);
        lightVolumeShader->setInt("gPosition", 0);
        GLState::ActiveTexture(GL_TEXTURE1);
        GLState::BindTexture(GL_TEXTURE_2D, deferFBO->textureIDs[1]);
        lightVolumeShader->setInt("gNormal", 1);
        GLState::ActiveTexture(GL_TEXTURE2);
        GLState::BindTexture(GL_TEXTURE_2D, deferFBO->textureIDs[2]);
        lightVolumeShader->setInt("gAlbedoSpec", 2);
        GLState::ActiveTexture(GL_TEXTURE3);
        GLState::BindTexture(GL_TEXTURE_2D, deferFBO->textureIDs[3]);
        lightVolumeShader->setInt("gMaterial", 3);
        GLState::Enable(GL_DEPTH_TEST);
        GLState::Enable(GL_STENCIL_TEST);
        glBlendEquation(GL_FUNC_ADD);
        GLState::Enable(GL_BLEND);
        GLState::BlendFunc(GL_ONE, GL_ONE);
        for (auto& pointLight : lightSource.pointLights) {
            if (!pointLight.GetActiveStatus()) continue;
            float radius = ComputePointLightStencilVolumeRadius(
                pointLight.constant, pointLight.linear, pointLight.quadratic, pointLight.diffuse,
                properties.LIGHT_VOLUME_CUTOFF_SCALE, properties.LIGHT_VOLUME_RADIUS_SCALE);
            const glm::vec3 savedScale = pointLight.scale;
            pointLight.SetScale(glm::vec3(radius));

            GLState::Disable(GL_BLEND);
            GLState::StencilMask(0xFF);
            glClear(GL_STENCIL_BUFFER_BIT);
            GLState::ColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            GLState::DepthMask(GL_FALSE);
            GLState::Enable(GL_DEPTH_TEST);
            GLState::DepthFunc(GL_LESS);
            GLState::Disable(GL_CULL_FACE);
            GLState::StencilFunc(GL_ALWAYS, 0, 0xFF);
            GLState::StencilOpSeparate(GL_BACK, GL_KEEP, GL_INCR_WRAP, GL_KEEP);
            GLState::StencilOpSeparate(GL_FRONT, GL_KEEP, GL_DECR_WRAP, GL_KEEP);

            defaultShader->use();
            defaultShader->setMat4("model", pointLight.getModelMatrix());
            pointLight.DrawPointLight();

            lightVolumeShader->use();
            GLState::Enable(GL_BLEND);
            GLState::BlendFunc(GL_ONE, GL_ONE);
            GLState::ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            GLState::StencilFunc(GL_NOTEQUAL, 0, 0xFF);
            GLState::StencilMask(0x00);
            GLState::Enable(GL_CULL_FACE);
            GLState::CullFace(GL_FRONT);
            GLState::DepthFunc(GL_GEQUAL);

            lightVolumeShader->setVec3("pointLight.position", pointLight.position);
            lightVolumeShader->setFloat("pointLight.constant", pointLight.constant);
            lightVolumeShader->setFloat("pointLight.linear", pointLight.linear);
            lightVolumeShader->setFloat("pointLight.quadratic", pointLight.quadratic);
            lightVolumeShader->setVec3("pointLight.ambient", pointLight.ambient);
            lightVolumeShader->setVec3("pointLight.diffuse", pointLight.diffuse);
            lightVolumeShader->setVec3("pointLight.specular", pointLight.specular);
            lightVolumeShader->setFloat("pointLight.far_plane", pointLight.far);
            GLState::ActiveTexture(GL_TEXTURE4);
            GLState::BindTexture(GL_TEXTURE_CUBE_MAP, pointLight.shadowFBO->textureIDs[0]);
            lightVolumeShader->setInt("pointLight.shadowCubeMap", 4);
            lightVolumeShader->setBool("pointLight.useShadowMap", pointLight.useShadowMap);
            lightVolumeShader->setMat4("model", pointLight.getModelMatrix());
            GLState::ActiveTexture(GL_TEXTURE0);
            GLState::BindTexture(GL_TEXTURE_2D, deferFBO->textureIDs[0]);
            GLState::ActiveTexture(GL_TEXTURE1);
            GLState::BindTexture(GL_TEXTURE_2D, deferFBO->textureIDs[1]);
            GLState::ActiveTexture(GL_TEXTURE2);
            GLState::BindTexture(GL_TEXTURE_2D, deferFBO->textureIDs[2]);
            GLState::ActiveTexture(GL_TEXTURE3);
            GLState::BindTexture(GL_TEXTURE_2D, deferFBO->textureIDs[3]);

            pointLight.DrawPointLight();

            pointLight.SetScale(savedScale);
            GLState::Disable(GL_BLEND);
            GLState::StencilMask(0xff);
            glClear(GL_STENCIL_BUFFER_BIT);
            GLState::StencilMask(0x00);
            GLState::CullFace(GL_BACK);
            GLState::DepthFunc(GL_LESS);
        }
        // 恢复默认状态
        GLState::StencilMask(0xFF);
        GLState::Disable(GL_BLEND);
        GLState::Disable(GL_STENCIL_TEST);
        GLState::DepthMask(GL_TRUE);
        GLState::Enable(GL_DEPTH_TEST);
        GLState::DepthFunc(GL_LESS);
        GLState::Disable(GL_CULL_FACE);
    }
    GLState::BindFramebuffer(GL_FRAMEBUFFER, fbo->framebufferID);
}

void Scene::RenderScene(Shader& shader) {
	MaterialBatchScope materialBatch;
	for (auto& model : modelSource.models) {
		if (!model || !model->GetAcitveStatus()) continue;
		shader.setMat4("model", model->getModelMatrix());
		model->Draw(&shader);
	}
}

void Scene::DrawPointLights()
{
    GLState::StencilMask(0x00); // 禁用stencil写入，不影响后续的stencil记录
    glm::vec3 lightColor(1.0f);

    lightSource.pointLightShader.use();
    lightSource.pointLightShader.setVec3("lightColor", lightColor);
    for (unsigned int i = 0; i < lightSource.pointLights.size(); ++i) {
        if (!lightSource.pointLights[i].GetActiveStatus()) continue;
        glm::mat4 model = lightSource.pointLights[i].getModelMatrix();
        // Assume lightShader is a valid Shader object already in use
        lightSource.pointLightShader.setMat4("model", model);
        lightSource.pointLights[i].DrawPointLight();
    }
}

void Scene::DrawOpaqueModels()
{
	auto& list = GetOpaqueMeshes();
	Shader* lastShader = nullptr;
	int usedTexes = properties.USED_TEXTURE_NUM;
	MaterialBatchScope materialBatch;
	for (const auto& item : list) {
		if (!item.shader || !item.model || !item.mesh) continue;

		if (item.shader != lastShader) {
			lastShader = item.shader;
			lastShader->use();
			properties.USED_TEXTURE_NUM = SetShadowMap(*lastShader);
			GLState::ActiveTexture(GL_TEXTURE0 + properties.USED_TEXTURE_NUM++);
			usedTexes = properties.USED_TEXTURE_NUM;
			if (properties.GAMMA_CORRECTION) {
				GLState::BindTexture(GL_TEXTURE_CUBE_MAP, skyboxSource.textureCubeMap->textureGammaID);
			}
			else {
				GLState::BindTexture(GL_TEXTURE_CUBE_MAP, skyboxSource.textureCubeMap->textureID);
			}
			GLState::ActiveTexture(GL_TEXTURE0);
			lastShader->setFloat("time", static_cast<float>(glfwGetTime()));
			lastShader->setVec3("viewPos", camera_ptr->cameraPos);
			lastShader->setVec3("color", glm::vec3(0.2f));
			SetLightUniforms(*lastShader);
		}

		properties.USED_TEXTURE_NUM = usedTexes;
		if (!item.model->IsOtherShaderUsed(OtherShaderType::outline)) {
			GLState::StencilMask(0x00);
			GLState::StencilFunc(GL_ALWAYS, 0, 0xFF);
		}
		else {
			GLState::StencilMask(0xFF);
			GLState::StencilFunc(GL_ALWAYS, 1, 0xFF);
			GLState::StencilOp(GL_KEEP, GL_REPLACE, GL_REPLACE);
		}

		lastShader->setMat4("model", item.model->getModelMatrix());
		item.mesh->Draw(lastShader);
	}
}

void Scene::DrawTransparentModels()
{
	GLState::Enable(GL_BLEND);
	GLState::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	GLState::DepthMask(GL_FALSE);

	auto& list = GetTransparentMeshes();
	Shader* lastShader = nullptr;
	int usedTexes = properties.USED_TEXTURE_NUM;
	{
		MaterialBatchScope materialBatch;
		for (const auto& item : list) {
			if (!item.shader || !item.model || !item.mesh) continue;

			if (item.shader != lastShader) {
				lastShader = item.shader;
				lastShader->use();
				properties.USED_TEXTURE_NUM = SetShadowMap(*lastShader);
				GLState::ActiveTexture(GL_TEXTURE0 + properties.USED_TEXTURE_NUM++);
				usedTexes = properties.USED_TEXTURE_NUM;
				if (skyboxSource.textureCubeMap) {
					if (properties.GAMMA_CORRECTION) {
						GLState::BindTexture(GL_TEXTURE_CUBE_MAP, skyboxSource.textureCubeMap->textureGammaID);
					}
					else {
						GLState::BindTexture(GL_TEXTURE_CUBE_MAP, skyboxSource.textureCubeMap->textureID);
					}
				}
				GLState::ActiveTexture(GL_TEXTURE0);
				lastShader->setFloat("time", static_cast<float>(glfwGetTime()));
				if (camera_ptr) {
					lastShader->setVec3("viewPos", camera_ptr->cameraPos);
				}
				SetLightUniforms(*lastShader);
			}
			properties.USED_TEXTURE_NUM = usedTexes;

			if (item.model->IsOtherShaderUsed(OtherShaderType::outline)) {
				GLState::StencilMask(0xFF);
				GLState::StencilFunc(GL_ALWAYS, 1, 0xFF);
				GLState::StencilOp(GL_KEEP, GL_REPLACE, GL_REPLACE);
			}
			else {
				GLState::StencilMask(0x00);
				GLState::StencilFunc(GL_ALWAYS, 0, 0xFF);
			}

			lastShader->setMat4("model", item.model->getModelMatrix());
			item.mesh->Draw(lastShader);
		}
	}

	GLState::DepthMask(GL_TRUE);
	GLState::Disable(GL_BLEND);
}

void Scene::SetLightUniforms(Shader& shader)
{
    // 全局阴影采样配置（所有使用阴影的 shader 都共用这一套）
    shader.setInt("shadowSampleNum", properties.SHADOW_PCF_SAMPLE_NUM);
    shader.setInt("shadowSampleRings", properties.SHADOW_PCF_RING_NUM);
    shader.setInt("shadowType", properties.SHADOW_TYPE);

    // 具体灯光参数与阴影贴图统一交给 LightSource/Light 对象来设置
    // 保证所有 shader 里关于 pointLights/dirLights/spotLights 的布局是一致的
    lightSource.SetLightUniforms(shader);
}

unsigned int Scene::SetShadowMap(Shader& shader) {
    int shadowMapCount = 0;
    shader.setInt("shadowSampleNum", properties.SHADOW_PCF_SAMPLE_NUM);
    shader.setInt("shadowSampleRings", properties.SHADOW_PCF_RING_NUM);
    shader.setInt("shadowType", properties.SHADOW_TYPE);
    for (int i = 0; i < lightSource.pointLights.size(); ++i) {
        auto& pointLight = lightSource.pointLights[i];
        //if (!lightSource.pointLights[i].GetActiveStatus()) continue;
        shader.setBool("pointLights[" + std::to_string(i) + "].useShadowMap", pointLight.useShadowMap);
        GLState::ActiveTexture(GL_TEXTURE0 + shadowMapCount);
        GLState::BindTexture(GL_TEXTURE_2D, 0);
        GLState::BindTexture(GL_TEXTURE_CUBE_MAP, pointLight.shadowFBO->textureIDs[0]);
        shader.setInt("pointLights[" + std::to_string(i) + "].shadowCubeMap", shadowMapCount);
        shader.setFloat("pointLights[" + std::to_string(i) + "].far_plane", pointLight.far);
        shadowMapCount++;
    }
    shadowMapCount = static_cast<int>(lightSource.pointLights.size());
    for (size_t i = 0; i < lightSource.directionLights.size(); ++i) {
        auto& dirLight = lightSource.directionLights[i];
        //if (!dirLight.GetActiveStatus()) continue;
        shader.setBool("dirLights[" + std::to_string(i) + "].useShadowMap", dirLight.useShadowMap);
        GLState::ActiveTexture(GL_TEXTURE0 + shadowMapCount);
        GLState::BindTexture(GL_TEXTURE_2D, dirLight.shadowFBO->textureIDs[0]);
        GLState::BindTexture(GL_TEXTURE_CUBE_MAP, 0);
        shader.setInt("dirLights[" + std::to_string(i) + "].shadowMap", shadowMapCount);
        shader.setMat4("dirLights[" + std::to_string(i) + "].lightSpaceMatrix", dirLight.GetLightSpaceMatrix());
        shadowMapCount++;
    }
    //TODO

    GLState::ActiveTexture(GL_TEXTURE0);
    return shadowMapCount;
}

void Scene::DrawSkybox(glm::mat4 view)
{
    GLState::DepthFunc(GL_LEQUAL);
    // Skybox 只影响颜色，不参与深度信息（否则会污染 SSAO 这类基于 depth 的后处理结果）
    GLState::DepthMask(GL_FALSE);
    GLState::StencilMask(0x00); // Disable writing to stencil buffer for skybox
    skyboxSource.skyboxShader_ptr->use();
    GLState::BindVertexArray(skyboxSource.cubeMapVAO);
    GLState::ActiveTexture(GL_TEXTURE0 + properties.USED_TEXTURE_NUM);
    GLState::BindTexture(GL_TEXTURE_2D, 0);
    if (properties.GAMMA_CORRECTION)
        GLState::BindTexture(GL_TEXTURE_CUBE_MAP, skyboxSource.textureCubeMap->textureGammaID);
    else
        GLState::BindTexture(GL_TEXTURE_CUBE_MAP, skyboxSource.textureCubeMap->textureID);
    skyboxSource.skyboxShader_ptr->setInt("skybox", properties.USED_TEXTURE_NUM++);
    skyboxSource.skyboxShader_ptr->setMat4("skyboxView", glm::mat4(glm::mat3(view))); // Remove translation from the view matrix
    PerformanceProfiler::GetInstance().RecordDraw(GL_TRIANGLES, 36);
    glDrawArrays(GL_TRIANGLES, 0, 36);

    GLState::DepthFunc(GL_LESS);
    GLState::DepthMask(GL_TRUE);
    GLState::StencilMask(0xFF); // Re-enable stencil mask
}

void Scene::DrawOutlines()
{
    GLState::StencilFunc(GL_NOTEQUAL, 1, 0xFF);
    GLState::StencilMask(0x00);
    GLState::Disable(GL_DEPTH_TEST);
    {
		MaterialBatchScope materialBatch;
		for (const auto& model : modelSource.models) {
			if (!model || !model->GetAcitveStatus()) continue;
			if (!model->IsOtherShaderUsed(OtherShaderType::outline)) continue;

			std::shared_ptr<Shader> outlineShader;
			if (!(outlineShader = model->GetOtherShader(OtherShaderType::outline))) {
				std::cout << "Outline shader is null!" << std::endl;
				continue;
			}
			outlineShader->use();
			outlineShader->setVec3("Color", model->outlineColor);
			glm::mat4 modelMatrix = model->getModelMatrix();

			glm::mat4 moveToOrigin = glm::translate(glm::mat4(1.0f), -model->GetLoacalCenter());
			glm::mat4 scale = glm::scale(glm::mat4(1.0f), glm::vec3(1.f + model->outlineWidth));
			glm::mat4 moveBack = glm::translate(glm::mat4(1.0f), model->GetLoacalCenter());

			outlineShader->setMat4("model", modelMatrix * moveBack * scale * moveToOrigin);
			model->Draw(outlineShader.get());
		}
	}

    GLState::StencilMask(0xFF);
    GLState::Enable(GL_DEPTH_TEST);
}

void Scene::DrawNormalLines()
{
    //glStencilMask(0x00); // Disable writing to stencil buffer
	MaterialBatchScope materialBatch;
	for (auto& model : modelSource.models) {
		if (!model || !model->GetAcitveStatus()) continue;
		if (!model->IsOtherShaderUsed(OtherShaderType::normalLines)) continue;
		std::shared_ptr<Shader> normalLineShader;
		if (!(normalLineShader = model->GetOtherShader(OtherShaderType::normalLines))) {
			std::cout << "Normal line shader is null!" << std::endl;
			continue;
		}
		normalLineShader->use();
		normalLineShader->setFloat("MAGNITUDE", OtherShader::normalLineMagnitude);
		normalLineShader->setMat4("model", model->getModelMatrix());
		model->Draw(normalLineShader.get());
	}
    //glStencilMask(0xFF); // Re-enable stencil mask
}

void Scene::DrawShadowMap() {
    PERF_CPU_SCOPE("Shadow Maps");
    PERF_GPU_SCOPE("Shadow Maps");
    GLState::CullFace(GL_FRONT);
    GLState::Enable(GL_DEPTH_TEST);
    GLState::Disable(GL_STENCIL_TEST);
    glClearDepth(1.0f);
    glViewport(0, 0, properties.SHADOW_WIDTH, properties.SHADOW_HEIGHT);
    GLState::ColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    GLState::StencilMask(0x00);
    auto shadowShader = ShaderManager::GetInstance().GetShader(ShaderManager::Shadow);
    shadowShader->use();
    for (auto& dirLight : lightSource.directionLights) {
        if (!dirLight.useShadowMap) continue;
        GLState::BindFramebuffer(GL_FRAMEBUFFER, dirLight.shadowFBO->framebufferID);
        glClear(GL_DEPTH_BUFFER_BIT);
        shadowShader->setMat4("lightSpaceMatrix", dirLight.GetLightSpaceMatrix());
        RenderScene(*shadowShader);
    }


    auto shadowCubeShader = ShaderManager::GetInstance().GetShader(ShaderManager::ShadowCube);
    shadowCubeShader->use();
    for (auto& pointLight : lightSource.pointLights) {
        if (!pointLight.useShadowMap) continue;
        GLState::BindFramebuffer(GL_FRAMEBUFFER, pointLight.shadowFBO->framebufferID);
        glClear(GL_DEPTH_BUFFER_BIT);
        auto& lightSpaceMatrices = pointLight.GetLightSpaceMatrices();
        for (int i = 0; i < 6; ++i) {
            shadowCubeShader->setMat4("shadowMatrices[" + std::to_string(i) + "]", lightSpaceMatrices[i]);
        }
        shadowCubeShader->setFloat("far_plane", pointLight.far);
        shadowCubeShader->setVec3("lightPos", pointLight.position);
        RenderScene(*shadowCubeShader);
    }
    GLState::ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    GLState::BindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT);
    GLState::CullFace(GL_BACK);
}

FBO* Scene::GetNeedShowFramebuffer() {
    FBO* ret = fbo;
    if (FramebuffersManager::GetInstance().useType == FBO::Default_FrameRenderType) {
        if (fbo->attr.aaType == AntiAliasManager::MSAA) {
            FramebuffersManager::GetInstance().ReleaseFBO(fboTemp);
            FBOAttributes attr;
            attr.isBloom = properties.BLOOM;
            attr.isHDR = properties.USE_HDR;
            fboTemp = FramebuffersManager::GetInstance().GetFBO(attr);
            GLState::BindFramebuffer(GL_READ_FRAMEBUFFER, fbo->framebufferID);
            GLState::BindFramebuffer(GL_DRAW_FRAMEBUFFER, fboTemp->framebufferID);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            glBlitFramebuffer(0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT, 0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            if (fbo->attr.isBloom) {
                glReadBuffer(GL_COLOR_ATTACHMENT1);
                glDrawBuffer(GL_COLOR_ATTACHMENT1);
                glBlitFramebuffer(0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT, 0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            }
            if (fbo->attr.isBloom) {
                GLuint attachments[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
                glDrawBuffers(2, attachments);
            }
            else {
                glDrawBuffer(GL_COLOR_ATTACHMENT0);
            }
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            GLState::BindFramebuffer(GL_FRAMEBUFFER, 0);
            if (properties.BLOOM) {
                Blur(properties.BLOOM_BLUR_ITERATIONS, fboTemp);
            }
            ret = fboTemp;
        }
        else if (fbo->attr.isBloom) {
            Blur(properties.BLOOM_BLUR_ITERATIONS, fbo);
            ret = fbo;
        }
        else ret = fbo;
    }
    else if (FramebuffersManager::GetInstance().useType == FBO::ShadowMap_FrameRenderType) {
        return lightSource.directionLights[0].shadowFBO;
    }
    else if (FramebuffersManager::GetInstance().useType == FBO::BrightColor_FrameRenderType) {
        if (properties.BLOOM) {
            ret = fbo;
        }
        else {
            std::cout << "NO BLOOM USED" << std::endl;
            ret = fbo;
        }
    }
    return ret;
}

void Scene::ClearFBO() {
    FramebuffersManager::GetInstance().ReleaseFBO(fbo);
    FramebuffersManager::GetInstance().ReleaseFBO(fboTemp);
    FramebuffersManager::GetInstance().ReleaseFBO(deferFBO);
}

void Scene::Blur(int times, FBO* fbo) {
    if (times <= 0) return;
    auto bulrShader = ShaderManager::GetInstance().GetShader(ShaderManager::Bulr);
    FBO* fbos[2];
    FBOAttributes attr;
    attr.isHDR = properties.USE_HDR;
    fbos[0] = FramebuffersManager::GetInstance().GetFBO(attr);
    fbos[1] = FramebuffersManager::GetInstance().GetFBO(attr);
    GLboolean horizontal = true, first_iteration = true;
    GLuint amount = times << 1;
    bulrShader->use();
    for (GLuint i = 0; i < amount; i++)
    {
        GLState::BindFramebuffer(GL_FRAMEBUFFER, fbos[i % 2]->framebufferID);
        bulrShader->setBool("horizontal", horizontal);
        GLState::ActiveTexture(GL_TEXTURE0);
        GLState::BindTexture(
            GL_TEXTURE_2D, first_iteration ? fbo->textureIDs[1] : fbos[(i + 1) % 2]->textureIDs[0]
        );
        bulrShader->setInt("image", 0);
        PerformanceProfiler::GetInstance().RecordDraw(GL_TRIANGLES, 6);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        horizontal = !horizontal;
        if (first_iteration)
            first_iteration = false;
    }
    GLState::BindFramebuffer(GL_READ_FRAMEBUFFER, fbos[1]->framebufferID);
    GLState::BindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo->framebufferID);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glDrawBuffer(GL_COLOR_ATTACHMENT1);
    glBlitFramebuffer(0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT, 0, 0, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    GLuint attachments[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    glDrawBuffers(2, attachments);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    GLState::BindFramebuffer(GL_FRAMEBUFFER, 0);
    FramebuffersManager::GetInstance().ReleaseFBO(fbos[0]);
    FramebuffersManager::GetInstance().ReleaseFBO(fbos[1]);
}

void Scene::SetSceneGui()
{
    if (ImGui::CollapsingHeader("Light Settings")) {
        if (ImGui::TreeNode("Direction Lights")) {
            for (size_t i = 0; i < lightSource.directionLights.size(); ++i) {
                std::string label = "Direction Light " + std::to_string(i);
                if (ImGui::TreeNode(label.c_str())) {
                    ImGui::Checkbox("Active", &lightSource.directionLights[i].m_active);
                    ImGui::ColorEdit3("Ambient", &lightSource.directionLights[i].ambient[0]);
                    ImGui::ColorEdit3("Diffuse", &lightSource.directionLights[i].diffuse[0]);
                    ImGui::ColorEdit3("Specular", &lightSource.directionLights[i].specular[0]);
                    ImGui::DragFloat3("Direction", &lightSource.directionLights[i].direction[0], 0.1f);
                    ImGui::Checkbox("useShadow", &lightSource.directionLights[i].useShadowMap);
                    ImGui::DragFloat("distance", &lightSource.directionLights[i].distance, 0.1f, 1.0f, 100.0f);
                    ImGui::DragFloat("nearPlane", &lightSource.directionLights[i].near_plane, 0.1f, 0.1f, 5.0f);
                    ImGui::DragFloat("farPlane", &lightSource.directionLights[i].far_plane, 0.1f, 5.0f, 100.0f);
                    ImGui::DragFloat("shadowWidth", &lightSource.directionLights[i].width, 0.1f, 10.0f, 100.f);
                    ImGui::TreePop();
                }
            }
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Point Lights")) {
            for (size_t i = 0; i < lightSource.pointLights.size(); ++i) {
                std::string label = "Point Light " + std::to_string(i);
                if (ImGui::TreeNode(label.c_str())) {
                    ImGui::Checkbox("Active", &lightSource.pointLights[i].m_active);
                    ImGui::ColorEdit3("Ambient", &lightSource.pointLights[i].ambient[0]);
                    ImGui::ColorEdit3("Diffuse", &lightSource.pointLights[i].diffuse[0]);
                    ImGui::ColorEdit3("Specular", &lightSource.pointLights[i].specular[0]);
                    ImGui::DragFloat3("Position", &lightSource.pointLights[i].position[0], 0.1f);
                    ImGui::DragFloat3("Scale", &lightSource.pointLights[i].scale[0], 0.01f, 0.01f, 100.0f);
                    ImGui::DragFloat("Constant", &lightSource.pointLights[i].constant, 0.01f, 0.0f, 10.0f);
                    ImGui::DragFloat("Linear", &lightSource.pointLights[i].linear, 0.001f, 0.0f, 1.0f);
                    ImGui::DragFloat("Quadratic", &lightSource.pointLights[i].quadratic, 0.0001f, 0.0f, 1.0f);
                    ImGui::Checkbox("useShadow", &lightSource.pointLights[i].useShadowMap);
                    ImGui::DragFloat("nearPlane", &lightSource.pointLights[i].near, 0.1f, 0.1f, 5.0f);
                    ImGui::DragFloat("farPlane", &lightSource.pointLights[i].far, 0.1f, 5.0f, 100.0f);
                    ImGui::TreePop();
                }
            }
            ImGui::TreePop();
        }
    }

    static int selectedOption = 0;
    const char* options[] = { "Phong","Mirror","Explode" };
    int optionCount = sizeof(options) / sizeof(options[0]);

    if (ImGui::CollapsingHeader("Model Settings")) {
        if (ImGui::TreeNode("Models")) {
            for (auto& model : modelSource.models) {
                std::string label = "Model: " + model->GetName();
                if (ImGui::TreeNode(label.c_str())) {
                    if (ImGui::Button("View Materials")) {
                        SetSelectedModelForMaterials(model.get());
                    }
                    ImGui::SameLine();
                    ImGui::Checkbox("Active", &model->m_active);
                    ImGui::DragFloat3("Position", &model->position[0], 0.1f);
                    ImGui::DragFloat3("Rotation", &model->rotation[0], 0.5f);
                    ImGui::DragFloat3("Scale", &model->scale[0], 0.01f, 0.01f, 10.0f);
                    if (ImGui::TreeNode("Other Shader Use")) {
                        for (auto& [key, value] : model->otherShaderUse)
                        {
                            ImGui::Checkbox(OtherShader::OtherShaderTypeToString(static_cast<OtherShaderType>(key)).c_str(), &value);
                        }
                        ImGui::TreePop();
                    }
                    ImGui::DragFloat("Outline Width", &model->outlineWidth, 0.01f, 0.0f, 0.5f);
                    ImGui::ColorEdit3("Outline Color", &model->outlineColor[0]);
                    ImGui::DragFloat("NormalLine Width", &OtherShader::normalLineMagnitude, 0.01f, 0.0f, 0.4f);

                    auto shaderPtr = model->GetShader();
                    int curShaderIdx = shaderPtr ? ShaderManager::GetInstance().GetShaderIndexByShader(shaderPtr) : -1;
                    if (ImGui::Combo("Shader Type", &selectedOption, options, optionCount)) {
                        switch (selectedOption) {
                        case 0:
                            if (curShaderIdx != ShaderManager::Phong) model->SetShader(ShaderManager::GetInstance().GetShader(ShaderManager::Phong));
                            break;
                        case 1:
                            if (curShaderIdx != ShaderManager::Mirror) model->SetShader(ShaderManager::GetInstance().GetShader(ShaderManager::Mirror));
                            break;
                        case 2:
                            if (curShaderIdx != ShaderManager::Explode) model->SetShader(ShaderManager::GetInstance().GetShader(ShaderManager::Explode));
                            break;
                        }
                    }
                    ImGui::TreePop();
                }
            }
            ImGui::TreePop();
        }

    }
}
