#include "Light.h"
#include "GLStateCache.h"
#include "Profiler.h"

void PointLight::DrawPointLight() {
	for (auto& mesh : meshes) {
		auto& vertices = mesh.vertices;
		auto VAO = mesh.GetVAO();
		GLState::BindVertexArray(VAO);
		PerformanceProfiler::GetInstance().RecordDraw(GL_TRIANGLES, vertices.size());
		glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size()));
	}
}

void PointLight::SetLightUniforms(Shader& shader, int index) {
	auto& properties = SystemProperties::GetInstance();
	if (GetActiveStatus() == false) return;
	std::string baseName = "pointLights[" + std::to_string(index) + "]";
	shader.setBool(baseName + ".isActive", GetActiveStatus());
	shader.setVec3(baseName + ".position", position);
	shader.setVec3(baseName + ".ambient", ambient);
	shader.setVec3(baseName + ".diffuse", diffuse);
	shader.setVec3(baseName + ".specular", specular);
	shader.setFloat(baseName + ".constant", constant);
	shader.setFloat(baseName + ".linear", linear);
	shader.setFloat(baseName + ".quadratic", quadratic);
	GLState::ActiveTexture(GL_TEXTURE0 + properties.USED_TEXTURE_NUM);
	GLState::BindTexture(GL_TEXTURE_2D, 0);
	GLState::BindTexture(GL_TEXTURE_CUBE_MAP, shadowFBO->textureIDs[0]);
	shader.setBool(baseName + ".useShadowMap", useShadowMap);
	shader
		.setInt(baseName + ".shadowCubeMap", properties.USED_TEXTURE_NUM++);
	shader.setFloat(baseName + ".far_plane", far);
}

void DirectionLight::SetLightUniforms(Shader& shader, int index) {
	auto& properties = SystemProperties::GetInstance();
	if (GetActiveStatus() == false) return;
	std::string baseName = "dirLights[" + std::to_string(index) + "]";
	shader.setBool(baseName + ".isActive", GetActiveStatus());
	shader.setVec3(baseName + ".direction", direction);
	shader.setVec3(baseName + ".ambient", ambient);
	shader.setVec3(baseName + ".diffuse", diffuse);
	shader.setVec3(baseName + ".specular", specular);
	GLState::ActiveTexture(GL_TEXTURE0 + properties.USED_TEXTURE_NUM);
	GLState::BindTexture(GL_TEXTURE_2D, shadowFBO->textureIDs[0]);
	GLState::BindTexture(GL_TEXTURE_CUBE_MAP, 0);
	shader.setBool(baseName + ".useShadowMap", useShadowMap);
	shader.setInt(baseName + ".shadowMap", properties.USED_TEXTURE_NUM++);
	shader.setMat4(baseName + ".lightSpaceMatrix", GetLightSpaceMatrix());
}

void SpotLight::SetLightUniforms(Shader& shader, int index) {
	if (GetActiveStatus() == false) return;
	std::string baseName = "spotLights[" + std::to_string(index) + "]";
	shader.setBool(baseName + ".isActive", GetActiveStatus());
	shader.setVec3(baseName + ".position", position);
	shader.setVec3(baseName + ".ambient", ambient);
	shader.setVec3(baseName + ".diffuse", diffuse);
	shader.setVec3(baseName + ".specular", specular);
	shader.setFloat(baseName + ".constant", constant);
	shader.setFloat(baseName + ".linear", linear);
	shader.setFloat(baseName + ".quadratic", quadratic);
	shader.setVec3(baseName + ".direction", direction);
}
