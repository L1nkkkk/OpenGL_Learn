#include "Learn.h"
#include "Profiler.h"
#include <cstdint>

void Planet::Init() {
    planetModel = new Model("models/planet/planet.obj");
    rockModel = new Model("models/rock/rock.obj");
    planetShader = ShaderManager::GetInstance().GetShader(ShaderManager::Default);
    rockShader = ShaderManager::GetInstance().GetShader(ShaderManager::Diffuse);

    modelMatrices = new glm::mat4[amount];
    srand(static_cast<unsigned int>(glfwGetTime()));
    float radius = 50.0f;
    float offset = 2.5f;
    for (unsigned int i = 0; i < amount; i++)
    {
        glm::mat4 model = glm::mat4(1.0f);
        float angle = static_cast<float>(i) / static_cast<float>(amount) * 360.0f;
        float angleRadians = glm::radians(angle);
        float displacement = (rand() % (int)(2 * offset * 100)) / 100.0f - offset;
        float x = std::sin(angleRadians) * radius + displacement;
        displacement = (rand() % (int)(2 * offset * 100)) / 100.0f - offset;
        float y = displacement * 0.4f; 
        displacement = (rand() % (int)(2 * offset * 100)) / 100.0f - offset;
        float z = std::cos(angleRadians) * radius + displacement;
        model = glm::translate(model, glm::vec3(x, y, z));

        float scale = (rand() % 20) / 100.0f + 0.05f;
        model = glm::scale(model, glm::vec3(scale));

        float rotAngle = static_cast<float>(rand() % 360);
        model = glm::rotate(model, glm::radians(rotAngle), glm::vec3(0.4f, 0.6f, 0.8f));

        modelMatrices[i] = model;
    }
    glGenBuffers(1, &rockVBO);
    glBindBuffer(GL_ARRAY_BUFFER, rockVBO);
    glBufferData(GL_ARRAY_BUFFER, amount * sizeof(glm::mat4), &modelMatrices[0], GL_STATIC_DRAW);
    auto& meshes = rockModel->GetMeshes();
    for (unsigned int i = 0; i < meshes.size(); i++)
    {
        unsigned int VAO = meshes[i].GetVAO();
        glBindVertexArray(VAO);


        GLsizei vec4Size = static_cast<GLsizei>(sizeof(glm::vec4));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, 4 * vec4Size, (void*)0);
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, 4 * vec4Size,
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(vec4Size)));
        glEnableVertexAttribArray(5);
        glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, 4 * vec4Size,
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(2 * vec4Size)));
        glEnableVertexAttribArray(6);
        glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, 4 * vec4Size,
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(3 * vec4Size)));

        glVertexAttribDivisor(3, 1);
        glVertexAttribDivisor(4, 1);
        glVertexAttribDivisor(5, 1);
        glVertexAttribDivisor(6, 1);

        glBindVertexArray(0);
    }
 }

void Planet::Draw() {
	
    glEnable(GL_DEPTH_TEST);

    planetShader->use();
    glm::mat4 model = glm::mat4(1.0f);
    model = glm::translate(model, glm::vec3(0.0f, -3.0f, 0.0f));
    model = glm::scale(model, glm::vec3(4.f, 4.f, 4.f));
    
    planetShader->setMat4("model", model);
    planetModel->Draw(planetShader.get());

    
    auto& meshes = rockModel->GetMeshes();
    rockShader->use();
    rockShader->setInt("texture_diffuse1", 0);
    glActiveTexture(GL_TEXTURE0);
    
    glBindTexture(GL_TEXTURE_2D, rockModel->GetTextureID(0));
    for (unsigned int i = 0; i < meshes.size(); i++)
    {
        glBindVertexArray(meshes[i].GetVAO());
        PerformanceProfiler::GetInstance().RecordDraw(
            GL_TRIANGLES,
            meshes[i].vertices.size(),
            amount);
        glDrawArraysInstanced(
            GL_TRIANGLES, 0,
            static_cast<GLsizei>(meshes[i].vertices.size()),
            static_cast<GLsizei>(amount)
        );
    }
    /*for (unsigned int i = 0; i < amount; ++i) {
        rockShader->setMat4("model", modelMatrices[i]);
        rockModel->Draw(*rockShader);
    }*/
}
