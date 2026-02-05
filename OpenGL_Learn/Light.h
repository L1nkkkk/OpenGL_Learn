#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <array>
#include "GLobal.h"
#include "Model.h"

class PointLight : public Model{
public:
	glm::vec3 ambient;
	glm::vec3 diffuse;
	glm::vec3 specular;

	float constant;
	float linear;
	float quadratic;

	bool useShadowMap;
	FBO* shadowFBO;
	
	float near = 1.0f;
	float far = 25.0f;
	glm::mat4 shadowProj;
	std::array<glm::mat4, 6> lightSpaceMatrices;

	PointLight(const glm::vec3& pos, const glm::vec3& amb, const glm::vec3& diff, const glm::vec3& spec, std::string path)
		: Model(path),ambient(amb), diffuse(diff), specular(spec) {
		position = pos;
		constant = 1.0f;
		linear = 0.09f;
		quadratic = 0.032f;
		FBOAttributes attr;
		attr.isShadowMap = true;
		attr.shadowType = FBOAttributes::FramebufferType::ShadowBox;
		shadowFBO = FramebuffersManager::GetInstance().GetFBO(attr);
		useShadowMap = false;
	}

	void DrawPointLight();

	std::array<glm::mat4, 6>& GetLightSpaceMatrices() {
		shadowProj = glm::perspective(glm::radians(90.0f), (float)SHADOW_WIDTH / (float)SHADOW_HEIGHT, near, far);

		lightSpaceMatrices[0] = (shadowProj *
			glm::lookAt(position, position + glm::vec3(1.0, 0.0, 0.0), glm::vec3(0.0, -1.0, 0.0)));
		lightSpaceMatrices[1] = (shadowProj *
			glm::lookAt(position, position + glm::vec3(-1.0, 0.0, 0.0), glm::vec3(0.0, -1.0, 0.0)));
		lightSpaceMatrices[2] = (shadowProj *
			glm::lookAt(position, position + glm::vec3(0.0, 1.0, 0.0), glm::vec3(0.0, 0.0, 1.0)));
		lightSpaceMatrices[3] = (shadowProj *
			glm::lookAt(position, position + glm::vec3(0.0, -1.0, 0.0), glm::vec3(0.0, 0.0, -1.0)));
		lightSpaceMatrices[4] = (shadowProj *
			glm::lookAt(position, position + glm::vec3(0.0, 0.0, 1.0), glm::vec3(0.0, -1.0, 0.0)));
		lightSpaceMatrices[5] = (shadowProj *
			glm::lookAt(position, position + glm::vec3(0.0, 0.0, -1.0), glm::vec3(0.0, -1.0, 0.0)));

		return lightSpaceMatrices;
	}
};

class DirectionLight : public BaseObject{
public:
	glm::vec3 direction;
	glm::vec3 ambient;
	glm::vec3 diffuse;
	glm::vec3 specular;

	float near_plane;
	float far_plane;
	float distance;
	float width;

	bool useShadowMap;
	FBO* shadowFBO;

	DirectionLight(const glm::vec3& dir, const glm::vec3& amb, const glm::vec3& diff, const glm::vec3& spec)
		: direction(dir), ambient(amb), diffuse(diff), specular(spec) {
		near_plane = 1.0f;
		far_plane = 7.5f;
		distance = 5.f;
		width = 10.f;
		FBOAttributes attr;
		attr.isShadowMap = true;
		attr.shadowType = FBOAttributes::FramebufferType::ShadowMap;
		shadowFBO = FramebuffersManager::GetInstance().GetFBO(attr);
		useShadowMap = false;
	}
	glm::mat4 GetLightSpaceMatrix() {
		glm::mat4 lightProjection = glm::ortho(
			-width,width,-width,width,
			near_plane,
			far_plane
		);
		glm::vec3 lightDir = glm::normalize(direction);
		glm::vec3 eyePos = -lightDir * distance;
		glm::vec3 targetPos = glm::vec3(0.0f);
		glm::vec3 originalUp = glm::vec3(0.0f, 1.0f, 0.0f);

		glm::vec3 viewDir = glm::normalize(targetPos - eyePos);
		float dotProduct = glm::abs(glm::dot(viewDir, originalUp));
		glm::vec3 usableUp = originalUp;

		const float parallelThreshold = 0.999f;
		if (dotProduct > parallelThreshold) {
			usableUp = glm::vec3(1.0f, 0.0f, 0.0f);
			if (glm::abs(glm::dot(viewDir, usableUp)) > parallelThreshold) {
				usableUp = glm::vec3(0.0f, 0.0f, 1.0f);
			}
		}
		glm::mat4 lightView = glm::lookAt(
			eyePos,
			targetPos,
			usableUp
		);
		return lightProjection * lightView;
	}
};

class SpotLight : public BaseObject{
public:
	glm::vec3 direction;
	float cutOff;
	float outerCutOff;
	float constant;
	float linear;
	float quadratic;
	glm:: vec3 ambient;
	glm:: vec3 diffuse;
	glm:: vec3 specular;
	SpotLight(const glm::vec3& pos, const glm::vec3& dir,const glm::vec3& amb, const glm::vec3& diff, const glm::vec3& spec,const float& cut,const float& outerCut)
		: direction(dir), ambient(amb), diffuse(diff), specular(spec), cutOff(cut), outerCutOff(outerCut) {
		position = pos;
		constant = 1.0f;
		linear = 0.09f;
		quadratic = 0.032f;
	}
};
