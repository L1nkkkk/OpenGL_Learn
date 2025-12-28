#include "Scene.h"

void Scene::Draw()
{
    view = camera_ptr->GetViewMatrix();
    projection = glm::perspective(glm::radians(camera_ptr->fov), (float)SCREEN_WIDTH / (float)SCREEN_HEIGHT, 0.1f, 100.0f);
	ShaderManager& ShaderMgr = ShaderManager::GetInstance();
	ShaderMgr.SetUBOData(ShaderManager::Matrices, 0, sizeof(glm::mat4), &view);
	ShaderMgr.SetUBOData(ShaderManager::Matrices, sizeof(glm::mat4), sizeof(glm::mat4), &projection);

	//Draw scene in the following order
    DrawPointLights();
    DrawOpaqueModels();  // 先绘制所有不透明物体，记录需要outline的物体到stencil buffer
    DrawSkybox();        // 绘制天空盒（使用深度测试优化，但不影响stencil buffer）
    DrawTransparentModels();  // 绘制透明物体
	DrawOutlines();      // 最后绘制outline（禁用深度测试，基于stencil buffer绘制）
}

void Scene::DrawPointLights()
{
    glStencilMask(0x00); // 禁用stencil写入，不影响后续的stencil记录
    glm::vec3 lightColor(1.0f);
    

	lightSource.pointLightShader.use();
    lightSource.pointLightShader.setVec3("lightColor", lightColor);
    for (unsigned int i = 0; i < lightSource.pointLights.size(); ++i) {
        glm::mat4 model = glm::mat4(1.0f);
        model = glm::translate(model, lightSource.pointLights[i].position);
        model = glm::scale(model, glm::vec3(0.2f));
        // Assume lightShader is a valid Shader object already in use
		lightSource.pointLightShader.setMat4("model", model);
		
        glBindVertexArray(lightSource.pointLightVAO);
        glDrawArrays(GL_TRIANGLES, 0, 36);
    }
    
    glStencilMask(0xFF); // 恢复stencil写入，供DrawOpaqueModels使用
}

void Scene::DrawOpaqueModels()
{
    for (const auto& ourshaderPair : modelSource.opaqueModelsMap) {

        Shader& ourShader = *(ourshaderPair.first);
		//std::cout << ourshaderPair.second.size() << std::endl;
        ourShader.use();
       
        glBindTexture(GL_TEXTURE_CUBE_MAP, skyboxSource.textureCubeMap);
        ourShader.setFloat("time", glfwGetTime());
        ourShader.setVec3("viewPos", camera_ptr->cameraPos);
        SetLightUniforms(ourShader);
        for (auto& model : ourshaderPair.second) {
            if (!model->IsOtherShaderUsed(OtherShaderType::outline)) {
				glStencilMask(0x00); // Disable writing to the stencil buffer
                glStencilFunc(GL_ALWAYS, 0, 0xFF);
                ourShader.setMat4("model", model->getModelMatrix());
                model->Draw(ourShader);
                glStencilMask(0xFF);
            }
            else {
                // Draw outline
                glStencilMask(0xFF);
                glStencilFunc(GL_ALWAYS, 1, 0xFF);
                glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
                ourShader.setMat4("model", model->getModelMatrix());
                model->Draw(ourShader);
            }
        }
    }
}

void Scene::DrawTransparentModels()
{
    glStencilMask(0x00); // 禁用stencil写入，保护已记录的stencil值
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
    glStencilMask(0xFF); // 恢复stencil写入（虽然DrawOutlines会再次设置）
}

void Scene::SetLightUniforms(Shader& shader)
{
	shader.setInt("NR_POINT_LIGHTS", static_cast<int>(lightSource.pointLights.size()));
    for(int i = 0; i < lightSource.pointLights.size(); ++i) {
        std::string baseName = "pointLights[" + std::to_string(i) + "]";
        shader.setVec3(baseName + ".position", lightSource.pointLights[i].position);
        shader.setVec3(baseName + ".ambient", lightSource.pointLights[i].ambient);
        shader.setVec3(baseName + ".diffuse", lightSource.pointLights[i].diffuse);
        shader.setVec3(baseName + ".specular", lightSource.pointLights[i].specular);
        shader.setFloat(baseName + ".constant", lightSource.pointLights[i].constant);
        shader.setFloat(baseName + ".linear", lightSource.pointLights[i].linear);
        shader.setFloat(baseName + ".quadratic", lightSource.pointLights[i].quadratic);
	}

	shader.setInt("NR_DIR_LIGHTS", static_cast<int>(lightSource.directionLights.size()));
    for(int i = 0; i < lightSource.directionLights.size(); ++i) {
        std::string baseName = "dirLights[" + std::to_string(i) + "]";
        shader.setVec3(baseName + ".direction", lightSource.directionLights[i].direction);
        shader.setVec3(baseName + ".ambient", lightSource.directionLights[i].ambient);
        shader.setVec3(baseName + ".diffuse", lightSource.directionLights[i].diffuse);
        shader.setVec3(baseName + ".specular", lightSource.directionLights[i].specular);
	}

    shader.setInt("NR_SPOT_LIGHTS", static_cast<int>(lightSource.spotLights.size()));
    for (int i = 0; i < lightSource.spotLights.size(); ++i) {
        std::string baseName = "spotLights[" + std::to_string(i) + "]";
        shader.setVec3(baseName + ".position", lightSource.spotLights[i].position);
        shader.setVec3(baseName + ".ambient", lightSource.spotLights[i].ambient);
        shader.setVec3(baseName + ".diffuse", lightSource.spotLights[i].diffuse);
        shader.setVec3(baseName + ".specular", lightSource.spotLights[i].specular);
        shader.setFloat(baseName + ".constant", lightSource.spotLights[i].constant);
        shader.setFloat(baseName + ".linear", lightSource.spotLights[i].linear);
        shader.setFloat(baseName + ".quadratic", lightSource.spotLights[i].quadratic);
        shader.setVec3(baseName + ".direction", lightSource.spotLights[i].direction);
    }
}

void Scene::DrawSkybox()
{
    glDepthFunc(GL_LEQUAL);
	glStencilMask(0x00); // Disable writing to stencil buffer for skybox
	skyboxSource.skyboxShader_ptr->use();
    glBindVertexArray(skyboxSource.cubeMapVAO);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, skyboxSource.textureCubeMap);
	skyboxSource.skyboxShader_ptr->setInt("skybox", 0);
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


void Scene::SetSceneGui()
{
	ImGui::SetNextWindowSize(ImVec2(300, 400), ImGuiCond_FirstUseEver);
    ImGui::Begin("Scene Settings");
    if (ImGui::CollapsingHeader("Light Settings")) {
        if (ImGui::TreeNode("Direction Lights")) {
            for (size_t i = 0; i < lightSource.directionLights.size(); ++i) {
                std::string label = "Direction Light " + std::to_string(i);
                if (ImGui::TreeNode(label.c_str())) {
                    ImGui::ColorEdit3("Ambient", &lightSource.directionLights[i].ambient[0]);
                    ImGui::ColorEdit3("Diffuse", &lightSource.directionLights[i].diffuse[0]);
                    ImGui::ColorEdit3("Specular", &lightSource.directionLights[i].specular[0]);
                    ImGui::DragFloat3("Direction", &lightSource.directionLights[i].direction[0], 0.1f);
                    ImGui::TreePop();
                }
            }
			ImGui::TreePop();
        }
        if (ImGui::TreeNode("Point Lights")) {
            for (size_t i = 0; i < lightSource.pointLights.size(); ++i) {
                std::string label = "Point Light " + std::to_string(i);
                if (ImGui::TreeNode(label.c_str())) {
                    ImGui::ColorEdit3("Ambient", &lightSource.pointLights[i].ambient[0]);
                    ImGui::ColorEdit3("Diffuse", &lightSource.pointLights[i].diffuse[0]);
                    ImGui::ColorEdit3("Specular", &lightSource.pointLights[i].specular[0]);
                    ImGui::DragFloat3("Position", &lightSource.pointLights[i].position[0], 0.1f);
                    ImGui::DragFloat("Constant", &lightSource.pointLights[i].constant, 0.01f, 0.0f, 10.0f);
                    ImGui::DragFloat("Linear", &lightSource.pointLights[i].linear, 0.001f, 0.0f, 1.0f);
                    ImGui::DragFloat("Quadratic", &lightSource.pointLights[i].quadratic, 0.0001f, 0.0f, 1.0f);
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
                    std::string label = "Opaque Model " + std::to_string(i);
                    if (ImGui::TreeNode(label.c_str())) {
                        ImGui::DragFloat3("Position", &models[i]->position[0], 0.1f);
                        ImGui::DragFloat3("Rotation", &models[i]->rotation[0], 0.5f);
                        ImGui::DragFloat3("Scale", &models[i]->scale[0], 0.01f, 0.01f, 10.0f);
                        if (ImGui::TreeNode("Other Shader Use")) {
                            for (auto& [key, value] : models[i]->otherShaderUse)
                            {
								ImGui::Checkbox(OtherShader::OtherShaderTypeToString(static_cast<OtherShaderType>(key)).c_str(), &value);
                            }
                            ImGui::TreePop();
                        }
						ImGui::DragFloat("Outline Width", &models[i]->outlineWidth, 0.01f, 0.0f, 0.5f);
						ImGui::ColorEdit3("Outline Color", &models[i]->outlineColor[0]);
						int curShaderIdx = ShaderManager::GetInstance().GetShaderIndexByShader(shader);
						//std::cout<< "curShaderIdx:"<< curShaderIdx << std::endl;
                        if (ImGui::Combo("Shader Type", &selectedOption, options, optionCount)) {
                            switch (selectedOption) {
                            case 0:
                                if (curShaderIdx != ShaderManager::Phong) {
                                    modelSource.DeleteOpaqueModel(models[i]);
                                    modelSource.AddOpaqueModel(ShaderManager::GetInstance().GetShader(ShaderManager::Phong), models[i]);
                                }
                                break;
                            case 1:
                                if (curShaderIdx != ShaderManager::Mirror) {
                                    modelSource.DeleteOpaqueModel(models[i]);
                                    modelSource.AddOpaqueModel(ShaderManager::GetInstance().GetShader(ShaderManager::Mirror), models[i]);
                                }
                                break;
							case 2:
                                if (curShaderIdx != ShaderManager::Explode){
									modelSource.DeleteOpaqueModel(models[i]);
									modelSource.AddOpaqueModel(ShaderManager::GetInstance().GetShader(ShaderManager::Explode), models[i]);
                                }
                            }
                        }
						ImGui::TreePop();
                    }
                }
            }
			ImGui::TreePop();
        }
        if (ImGui::TreeNode("Transparent Models")) {
            for (size_t i = 0; i < modelSource.transparentModels.size(); ++i) {
                std::string label = "Transparent Model " + std::to_string(i);
                if (ImGui::TreeNode(label.c_str())) {
                    ImGui::DragFloat3("Position", &modelSource.transparentModels[i].first->position[0], 0.1f);
                    ImGui::DragFloat3("Rotation", &modelSource.transparentModels[i].first->rotation[0], 0.5f);
                    ImGui::DragFloat3("Scale", &modelSource.transparentModels[i].first->scale[0], 0.01f, 0.01f, 10.0f);
                    if (ImGui::TreeNode("Other Shader Use")) {
                        for (auto& [key, value] : modelSource.transparentModels[i].first->otherShaderUse)
                        {
                            ImGui::Checkbox(OtherShader::OtherShaderTypeToString(static_cast<OtherShaderType>(key)).c_str(), &value);
                        }
                        ImGui::TreePop();
                    }
                    ImGui::DragFloat("Outline Width", &modelSource.transparentModels[i].first->outlineWidth, 0.01f, 0.0f, 0.5f);
                    ImGui::ColorEdit3("Outline Color", &modelSource.transparentModels[i].first->outlineColor[0]);
                    ImGui::TreePop();
                }
            }
            ImGui::TreePop();
		}	 
        
	}
    ImGui::End();
}