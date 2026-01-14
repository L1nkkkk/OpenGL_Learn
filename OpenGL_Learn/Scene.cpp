#include "Scene.h"
void Scene::Draw()
{
    DrawShadowMap();
    auto attr = FramebuffersManager::GenCurrentAttr();
    fbo = FramebuffersManager::GetInstance().GetFBO(attr);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo->framebufferID);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    glStencilFunc(GL_ALWAYS, 0, 0xFF); 
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glStencilMask(0x00);
    //Draw scene in the following order
    DrawPointLights();
    DrawOpaqueModels();  // 先绘制所有不透明物体，记录需要outline的物体到stencil buffer
    DrawNormalLines(); // 可选：绘制法线线段用于调试
    DrawSkybox();        // 绘制天空盒（使用深度测试优化，但不影响stencil buffer）
    DrawTransparentModels();  // 绘制透明物体
    DrawOutlines();      // 最后绘制outline（禁用深度测试，基于stencil buffer绘制）
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Scene::RenderScene(Shader& shader) {
    for (auto& [_, opaqueModels] : modelSource.opaqueModelsMap) {
        for (auto& model : opaqueModels) {
            shader.setMat4("model", model->getModelMatrix());
            model->Draw(shader);
        }
    }

    for (auto& [model, _] : modelSource.transparentModels) {
        shader.setMat4("model", model->getModelMatrix());
        model->Draw(shader);
    }
}

void Scene::DrawPointLights()
{
    glStencilMask(0x00); // 禁用stencil写入，不影响后续的stencil记录
    glm::vec3 lightColor(1.0f);
    

	lightSource.pointLightShader.use();
    lightSource.pointLightShader.setVec3("lightColor", lightColor);
    for (unsigned int i = 0; i < lightSource.pointLights.size(); ++i) {
        if (!lightSource.pointLights[i].GetActiveStatus()) continue;
        glm::mat4 model = lightSource.pointLights[i].getModelMatrix();
        // Assume lightShader is a valid Shader object already in use
		lightSource.pointLightShader.setMat4("model", model);
		
        glBindVertexArray(lightSource.pointLightVAO);
        glDrawArrays(GL_TRIANGLES, 0, 36);
    }
}

void Scene::DrawOpaqueModels()
{
    for (const auto& ourshaderPair : modelSource.opaqueModelsMap) {

        Shader& ourShader = *(ourshaderPair.first);
		//std::cout << ourshaderPair.second.size() << std::endl;
        ourShader.use();
        USED_TEXTURE_NUM = SetShadowMap(ourShader);
        glActiveTexture(GL_TEXTURE0+ USED_TEXTURE_NUM++);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (GAMMA_CORRECTION) {
            glBindTexture(GL_TEXTURE_CUBE_MAP, skyboxSource.textureCubeMap->textureGammaID);
        }
        else {
            glBindTexture(GL_TEXTURE_CUBE_MAP, skyboxSource.textureCubeMap->textureID);
        }
        glActiveTexture(GL_TEXTURE0);
        ourShader.setFloat("time", glfwGetTime());
        ourShader.setVec3("viewPos", camera_ptr->cameraPos);
        ourShader.setVec3("color", glm::vec3(0.2f));
        SetLightUniforms(ourShader);
        for (auto& model : ourshaderPair.second) {
            if (!model->GetAcitveStatus()) continue;
            if (!model->IsOtherShaderUsed(OtherShaderType::outline)) {
				glStencilMask(0x00); // Disable writing to the stencil buffer
                glStencilFunc(GL_ALWAYS, 0, 0xFF);
                ourShader.setMat4("model", model->getModelMatrix());
                model->Draw(ourShader);
            }
            else {
                // Draw outline
                glStencilMask(0xFF);
                glStencilFunc(GL_ALWAYS, 1, 0xFF);
                glStencilOp(GL_KEEP, GL_REPLACE, GL_REPLACE);
                ourShader.setMat4("model", model->getModelMatrix());
                model->Draw(ourShader);
            }
        }
    }
}

void Scene::DrawTransparentModels()
{
    view = camera_ptr->GetViewMatrix();
    projection = glm::perspective(glm::radians(camera_ptr->fov), (float)SCREEN_WIDTH / (float)SCREEN_HEIGHT, 0.1f, 100.0f);
    std::sort(modelSource.transparentModels.begin(), modelSource.transparentModels.end(), [this](const auto& a, const auto& b) {
		glm::vec3 aPos = a.first->GetWorldPosition();
		glm::vec3 bPos = b.first->GetWorldPosition();
		float aDistance = glm::length(this->camera_ptr->cameraPos - aPos);
        float bDistance = glm::length(this->camera_ptr->cameraPos - bPos);
        return aDistance > bDistance;
        });
    Shader* lastShaderPtr = nullptr;
    for (const auto& modelPair : modelSource.transparentModels) {
        if (!modelPair.first->GetAcitveStatus()) continue;
        if(modelPair.first->IsOtherShaderUsed(OtherShaderType::outline)){
            glStencilMask(0xFF);
            glStencilFunc(GL_ALWAYS, 1, 0xFF);
            glStencilOp(GL_KEEP, GL_REPLACE, GL_REPLACE);
        }
        else {
            glStencilMask(0x00); // Disable writing to the stencil buffer
            glStencilFunc(GL_ALWAYS, 0, 0xFF);
        }
        if(!lastShaderPtr||lastShaderPtr!=modelPair.second){
            lastShaderPtr = modelPair.second;
            lastShaderPtr->use();
            lastShaderPtr->setVec3("viewPos", camera_ptr->cameraPos);
            SetLightUniforms(*lastShaderPtr);
		}
        lastShaderPtr->setMat4("model", modelPair.first->getModelMatrix());
		modelPair.first->Draw(*lastShaderPtr);
    }
}

void Scene::SetLightUniforms(Shader& shader)
{
    int count = 0;
    for(auto& pointLight : lightSource.pointLights) {
        if (!pointLight.GetActiveStatus()) continue;
        std::string baseName = "pointLights[" + std::to_string(count++) + "]";
        shader.setVec3(baseName + ".position", pointLight.position);
        shader.setVec3(baseName + ".ambient", pointLight.ambient);
        shader.setVec3(baseName + ".diffuse", pointLight.diffuse);
        shader.setVec3(baseName + ".specular", pointLight.specular);
        shader.setFloat(baseName + ".constant", pointLight.constant);
        shader.setFloat(baseName + ".linear", pointLight.linear);
        shader.setFloat(baseName + ".quadratic", pointLight.quadratic);
	}
    shader.setInt("NR_POINT_LIGHTS", count);
    count = 0;
    for(auto& dirLight : lightSource.directionLights) {
        if (!dirLight.GetActiveStatus()) continue;
        std::string baseName = "dirLights[" + std::to_string(count++) + "]";
        shader.setVec3(baseName + ".direction", dirLight.direction);
        shader.setVec3(baseName + ".ambient", dirLight.ambient);
        shader.setVec3(baseName + ".diffuse", dirLight.diffuse);
        shader.setVec3(baseName + ".specular", dirLight.specular);
	}
    shader.setInt("NR_DIR_LIGHTS", count);
    count = 0;
    
    for (auto& spotLight : lightSource.spotLights) {
        if (!spotLight.GetActiveStatus()) continue;
        std::string baseName = "spotLights[" + std::to_string(count++) + "]";
        shader.setVec3(baseName + ".position", spotLight.position);
        shader.setVec3(baseName + ".ambient", spotLight.ambient);
        shader.setVec3(baseName + ".diffuse", spotLight.diffuse);
        shader.setVec3(baseName + ".specular", spotLight.specular);
        shader.setFloat(baseName + ".constant", spotLight.constant);
        shader.setFloat(baseName + ".linear", spotLight.linear);
        shader.setFloat(baseName + ".quadratic", spotLight.quadratic);
        shader.setVec3(baseName + ".direction", spotLight.direction);
    }
    shader.setInt("NR_SPOT_LIGHTS", count);
}

unsigned int Scene::SetShadowMap(Shader& shader) {
    int shadowMapCount = 0;
    shader.setInt("shadowSampleNum", SHADOW_PCF_SAMPLE_NUM);
    shader.setInt("shadowSampleRings", SHADOW_PCF_RING_NUM);
    shader.setInt("shadowType", SHADOW_TYPE);
    for (int i = 0; i < lightSource.directionLights.size(); ++i) {
        auto& dirLight = lightSource.directionLights[i];
        if (!dirLight.GetActiveStatus()) continue;
        shader.setBool("dirLights[" + std::to_string(i) + "].useShadowMap", dirLight.useShadowMap);
        glActiveTexture(GL_TEXTURE0 + shadowMapCount);
        glBindTexture(GL_TEXTURE_2D, dirLight.shadowFBO->textureIDs[0]);
        glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
        shader.setInt("dirLights[" + std::to_string(i) + "].shadowMap", shadowMapCount);
        shader.setMat4("lightSpaceMatrix", dirLight.GetLightSpaceMatrix());
        shadowMapCount++;
    }
    //TODO
    for (int i = 0; i < lightSource.pointLights.size(); ++i) {
        auto& pointLight = lightSource.pointLights[i];
        if (!pointLight.GetActiveStatus()) continue;
        shader.setBool("pointLights[" + std::to_string(i) + "].useShadowMap", pointLight.useShadowMap);
        glActiveTexture(GL_TEXTURE0 + shadowMapCount);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindTexture(GL_TEXTURE_CUBE_MAP, pointLight.shadowFBO->textureIDs[0]);
        shader.setInt("pointLights[" + std::to_string(i) + "].shadowCubeMap", shadowMapCount);
        shader.setFloat("pointLights[" + std::to_string(i) + "].far_plane", pointLight.far);
        shadowMapCount++;
    }
    glActiveTexture(GL_TEXTURE0);
    return shadowMapCount;
}

void Scene::DrawSkybox()
{
    glDepthFunc(GL_LEQUAL);
	glStencilMask(0x00); // Disable writing to stencil buffer for skybox
	skyboxSource.skyboxShader_ptr->use();
    glBindVertexArray(skyboxSource.cubeMapVAO);
    glActiveTexture(GL_TEXTURE0+ USED_TEXTURE_NUM);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (GAMMA_CORRECTION)
        glBindTexture(GL_TEXTURE_CUBE_MAP, skyboxSource.textureCubeMap->textureGammaID);
    else
        glBindTexture(GL_TEXTURE_CUBE_MAP, skyboxSource.textureCubeMap->textureID);
	skyboxSource.skyboxShader_ptr->setInt("skybox", USED_TEXTURE_NUM++);
	skyboxSource.skyboxShader_ptr->setMat4("skyboxView", glm::mat4(glm::mat3(view))); // Remove translation from the view matrix
    glDrawArrays(GL_TRIANGLES, 0, 36);
    
	glBindVertexArray(0);
    glDepthFunc(GL_LESS);
	glStencilMask(0xFF); // Re-enable stencil mask
}

void Scene::DrawOutlines()
{
	glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
	glStencilMask(0x00);
	glDisable(GL_DEPTH_TEST);
    for (const auto& ourshaderPair : modelSource.opaqueModelsMap) {
        for (auto& model : ourshaderPair.second) {
            if (model->IsOtherShaderUsed(OtherShaderType::outline)) {
                Shader* outlineShader;
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

                outlineShader->setMat4("model", modelMatrix*moveBack*scale*moveToOrigin);
                model->Draw(*outlineShader);
            }
        }
	}

    for(const auto& modelPair : modelSource.transparentModels) {
		Model* model = modelPair.first.get();
        if (model->IsOtherShaderUsed(OtherShaderType::outline)) {
            Shader* outlineShader;
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
            model->Draw(*outlineShader);
        }
	}

	glStencilMask(0xFF);
	glEnable(GL_DEPTH_TEST);
}

void Scene::DrawNormalLines()
{
    //glStencilMask(0x00); // Disable writing to stencil buffer
    for (auto& [shader,models] : modelSource.opaqueModelsMap) {
        for (auto& model : models) {
            if (model->IsOtherShaderUsed(OtherShaderType::normalLines)) {
                Shader* normalLineShader;
                if (!(normalLineShader = model->GetOtherShader(OtherShaderType::normalLines))) {
                    std::cout << "Normal line shader is null!" << std::endl;
                    continue;
                }
                normalLineShader->use();
                normalLineShader->setFloat("MAGNITUDE", OtherShader::normalLineMagnitude);
                normalLineShader->setMat4("model", model->getModelMatrix());
				model->Draw(*normalLineShader);
            }
        }
    }
    //glStencilMask(0xFF); // Re-enable stencil mask
}



void Scene::DrawShadowMap() {
    glCullFace(GL_FRONT);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glClearDepth(1.0f);
    glViewport(0, 0, SHADOW_WIDTH, SHADOW_HEIGHT);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glStencilMask(0x00);
    auto* shadowShader = ShaderManager::GetInstance().GetShader(ShaderManager::Shadow);
    shadowShader->use();
    for (auto& dirLight : lightSource.directionLights) {
        if (!dirLight.useShadowMap) continue;
        glBindFramebuffer(GL_FRAMEBUFFER, dirLight.shadowFBO->framebufferID);
        glClear(GL_DEPTH_BUFFER_BIT);
        shadowShader->setMat4("lightSpaceMatrix", dirLight.GetLightSpaceMatrix());
        RenderScene(*shadowShader);
    }
    
    
    auto* shadowCubeShader = ShaderManager::GetInstance().GetShader(ShaderManager::ShadowCube);
    shadowCubeShader->use();
    for (auto& pointLight : lightSource.pointLights) {
        if (!pointLight.useShadowMap) continue;
        glBindFramebuffer(GL_FRAMEBUFFER, pointLight.shadowFBO->framebufferID);
        glClear(GL_DEPTH_BUFFER_BIT);
        auto& lightSpaceMatrices = pointLight.GetLightSpaceMatrices();
        for (int i = 0; i < 6; ++i) {
            shadowCubeShader->setMat4("shadowMatrices[" +std::to_string(i) + "]", lightSpaceMatrices[i]);
        }
        shadowCubeShader->setFloat("far_plane", pointLight.far);
        shadowCubeShader->setVec3("lightPos", pointLight.position);
        RenderScene(*shadowCubeShader);
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    glCullFace(GL_BACK);
}

unsigned int Scene::GetNeedShowFramebuffer() {
    unsigned int texID = 0;
    if (FramebuffersManager::GetInstance().useType == FBO::Default_FrameRenderType) {
        if (fbo->attr.aaType == AntiAliasManager::MSAA) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo->framebufferID);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fboTemp->framebufferID);
            glBlitFramebuffer(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            texID = fboTemp->textureIDs[0];
        }
        else if (fbo->attr.isBloom) {
            texID = fbo->textureIDs[0];
        }
        else texID=fbo->textureIDs[0];
    }
    else if (FramebuffersManager::GetInstance().useType == FBO::ShadowMap_FrameRenderType) {
        return lightSource.directionLights[0].shadowFBO->textureIDs[0];
    }
    else if (FramebuffersManager::GetInstance().useType == FBO::BrightColor_FrameRenderType) {
        if (BLOOM) {

            texID = fbo->textureIDs[1];
        }
        else {
            std::cout << "NO BLOOM USED" << std::endl;
            texID = fbo->textureIDs[0];
        }
    }
    return texID;
}

void Scene::ClearFBO() {
    FramebuffersManager::GetInstance().ReleaseFBO(fbo);
    FramebuffersManager::GetInstance().ReleaseFBO(fboTemp);
}

void Scene::Bulr(int times) {
    if (times <= 0) return;
    auto bulrShader = ShaderManager::GetInstance().GetShader(ShaderManager::Bulr);
    FBO* fbos[2];
    FBOAttributes attr;
    attr.isHDR = USE_HDR;
    fbos[0] = FramebuffersManager::GetInstance().GetFBO(attr);
    fbos[1] = FramebuffersManager::GetInstance().GetFBO(attr);
    GLboolean horizontal = true, first_iteration = true;
    GLuint amount = times<<1;
    bulrShader->use();
    for (GLuint i = 0; i < amount; i++)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, fbos[i%2]->framebufferID);
        bulrShader->setBool("horizontal", horizontal);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(
            GL_TEXTURE_2D, first_iteration ? fbo->textureIDs[1] : fbos[(i+1)%2]->textureIDs[0]
        );
        bulrShader->setInt("image", 0);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        horizontal = !horizontal;
        if (first_iteration)
            first_iteration = false;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbos[1]->framebufferID);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo->framebufferID);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glDrawBuffer(GL_COLOR_ATTACHMENT1);
    glBlitFramebuffer(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
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
	const char* options[] = {"Phong","Mirror","Explode"};
	int optionCount = sizeof(options) / sizeof(options[0]);

    if (ImGui::CollapsingHeader("Model Settings")){
        if (ImGui::TreeNode("Opacity Models")) {
            for (const auto& [shader, models] : modelSource.opaqueModelsMap) {
                for (size_t i = 0; i < models.size(); ++i) {
                    std::string label = "Opaque Model: " + models[i]->GetName();
                    auto model = models[i];
                    if (ImGui::TreeNode(label.c_str())) {
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
                        ImGui::DragFloat("NormalLine Width", &OtherShader::normalLineMagnitude, 0.01, 0.0f, 0.4f);
						int curShaderIdx = ShaderManager::GetInstance().GetShaderIndexByShader(shader);

						//std::cout<< "curShaderIdx:"<< curShaderIdx << std::endl;
                        if (ImGui::Combo("Shader Type", &selectedOption, options, optionCount)) {
                            switch (selectedOption) {
                            case 0:
                                if (curShaderIdx != ShaderManager::Phong) {
                                    modelSource.DeleteOpaqueModel(model);
                                    modelSource.AddOpaqueModel(ShaderManager::GetInstance().GetShader(ShaderManager::Phong), model);
                                }
                                break;
                            case 1:
                                if (curShaderIdx != ShaderManager::Mirror) {
                                    modelSource.DeleteOpaqueModel(model);
                                    modelSource.AddOpaqueModel(ShaderManager::GetInstance().GetShader(ShaderManager::Mirror), model);
                                }
                                break;
							case 2:
                                if (curShaderIdx != ShaderManager::Explode){
									modelSource.DeleteOpaqueModel(model);
									modelSource.AddOpaqueModel(ShaderManager::GetInstance().GetShader(ShaderManager::Explode), model);
                                }
                                break;
                            }
                        }
						ImGui::TreePop();
                    }
                }
            }
			ImGui::TreePop();
        }
        if (ImGui::TreeNode("Transparent Models")) {
            for (auto& [model,shader] : modelSource.transparentModels) {
                std::string label = "Transparent Model: " + model->GetName();
                if (ImGui::TreeNode(label.c_str())) {
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
                    ImGui::TreePop();
                }
            }
            ImGui::TreePop();
		}	 
        
	}
}