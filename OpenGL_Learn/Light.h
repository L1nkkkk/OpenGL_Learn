#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

struct PointLight {
	glm::vec3 position;
	glm::vec3 ambient;
	glm::vec3 diffuse;
	glm::vec3 specular;

	float constant;
	float linear;
	float quadratic;

	PointLight(const glm::vec3& pos, const glm::vec3& amb, const glm::vec3& diff, const glm::vec3& spec)
		: position(pos), ambient(amb), diffuse(diff), specular(spec) {
		constant = 1.0f;
		linear = 0.09f;
		quadratic = 0.032f;
	}
};

struct DirectionLight {
	glm::vec3 direction;
	glm::vec3 ambient;
	glm::vec3 diffuse;
	glm::vec3 specular;
	DirectionLight(const glm::vec3& dir, const glm::vec3& amb, const glm::vec3& diff, const glm::vec3& spec)
		: direction(dir), ambient(amb), diffuse(diff), specular(spec) {}
};

struct SpotLight {
	glm::vec3 position;
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
		: position(pos), direction(dir), ambient(amb), diffuse(diff), specular(spec), cutOff(cut), outerCutOff(outerCut) {
		constant = 1.0f;
		linear = 0.09f;
		quadratic = 0.032f;
	}
};
