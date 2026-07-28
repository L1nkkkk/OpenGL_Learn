#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <array>
#include "GLobal.h"
#include "Model.h"

struct ShadowMapCacheState {
	bool valid = false;
	bool contentSampleable = false;
	std::size_t signature = 0;
	unsigned int framebufferID = 0;
	unsigned int textureID = 0;
	int width = 0;
	int height = 0;
	std::uint64_t resourceGeneration = 0;

	void Invalidate() {
		valid = false;
		contentSampleable = false;
		signature = 0;
		framebufferID = 0;
		textureID = 0;
		width = 0;
		height = 0;
		resourceGeneration = 0;
	}

	static bool IsTargetReady(const FBO* target) {
		return target &&
			target->IsComplete() &&
			target->framebufferID != 0 &&
			target->width > 0 &&
			target->height > 0 &&
			target->textureIDs.size() == 1 &&
			target->textureIDs.front() != 0 &&
			target->GetResourceGeneration() != 0;
	}

	bool MatchesTarget(const FBO* target) const {
		if (!IsTargetReady(target)) {
			return false;
		}
		return framebufferID == target->framebufferID &&
			textureID == target->textureIDs.front() &&
			width == target->width &&
			height == target->height &&
			resourceGeneration == target->GetResourceGeneration();
	}

	bool IsCacheHit(
		std::size_t currentSignature,
		const FBO* target) const {
		return valid &&
			contentSampleable &&
			signature == currentSignature &&
			MatchesTarget(target);
	}

	bool IsSampleable(const FBO* target) const {
		return contentSampleable && MatchesTarget(target);
	}

	void CommitContent(const FBO* target) {
		Invalidate();
		if (!IsTargetReady(target)) {
			return;
		}
		framebufferID = target->framebufferID;
		textureID = target->textureIDs.front();
		width = target->width;
		height = target->height;
		resourceGeneration = target->GetResourceGeneration();
		contentSampleable = true;
	}

	void Commit(std::size_t newSignature, const FBO* target) {
		CommitContent(target);
		if (!contentSampleable) {
			return;
		}
		signature = newSignature;
		valid = true;
	}
};

class PointLight : public Model{
public:
	glm::vec3 ambient;
	glm::vec3 diffuse;
	glm::vec3 specular;

	float constant;
	float linear;
	float quadratic;

	bool useShadowMap = false;
	FBO* shadowFBO = nullptr;
	ShadowMapCacheState shadowCache;
	bool autoFitShadow = true;
	int shadowResolution = 1024;

	float near = 0.05f;
	float far = 25.0f;
	glm::mat4 shadowProj;
	std::array<glm::mat4, 6> lightSpaceMatrices;

	PointLight(const glm::vec3& pos, const glm::vec3& amb, const glm::vec3& diff, const glm::vec3& spec, std::string path,Material* mat)
		: Model(path,mat),ambient(amb), diffuse(diff), specular(spec) {
		position = pos;
		constant = 1.0f;
		linear = 0.09f;
		quadratic = 0.032f;
	}

	void DrawPointLight();
	FBO* EnsureShadowFBO();
	void FitShadowToBounds(const glm::vec3& center, float radius);

	void SetLightUniforms(Shader& shader, int index);
	std::array<glm::mat4, 6>& GetLightSpaceMatrices();
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
	glm::vec3 shadowCenter = glm::vec3(0.0f);
	bool lightSpaceAabbFitActive = false;
	float fittedHalfHeight = 10.0f;

	bool useShadowMap = false;
	FBO* shadowFBO = nullptr;
	ShadowMapCacheState shadowCache;
	bool autoFitShadow = true;
	int shadowResolution = 2048;
	int effectiveShadowResolution = 0;

	DirectionLight(const glm::vec3& dir, const glm::vec3& amb, const glm::vec3& diff, const glm::vec3& spec)
		: direction(dir), ambient(amb), diffuse(diff), specular(spec) {
		near_plane = 0.05f;
		far_plane = 25.0f;
		distance = 5.f;
		width = 10.f;
	}
	FBO* EnsureShadowFBO();
	void FitShadowToBounds(const glm::vec3& center, float radius);
	void ApplyLightSpaceAabbFit(
		const glm::vec3& center,
		float halfWidth,
		float halfHeight,
		float eyeDistance,
		float nearPlane,
		float farPlane,
		int effectiveResolution);
	int GetEffectiveShadowResolution() const {
		return (std::max)(
			1,
			effectiveShadowResolution > 0
				? effectiveShadowResolution
				: shadowResolution);
	}
	glm::mat4 GetLightSpaceMatrix() const;

	void SetLightUniforms(Shader& shader, int index);
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
	bool useShadowMap = false;
	FBO* shadowFBO = nullptr;
	ShadowMapCacheState shadowCache;
	bool autoFitShadow = true;
	int shadowResolution = 1024;
	float near_plane = 0.05f;
	float far_plane = 25.0f;
	SpotLight(const glm::vec3& pos, const glm::vec3& dir,const glm::vec3& amb, const glm::vec3& diff, const glm::vec3& spec,const float& cut,const float& outerCut)
		: direction(dir), ambient(amb), diffuse(diff), specular(spec), cutOff(cut), outerCutOff(outerCut) {
		position = pos;
		constant = 1.0f;
		linear = 0.09f;
		quadratic = 0.032f;
	}

	FBO* EnsureShadowFBO();
	void FitShadowToBounds(const glm::vec3& center, float radius);
	bool GetShadowViewBasis(
		glm::vec3& forward,
		glm::vec3& right,
		glm::vec3& up) const;
	float GetShadowHalfAngleRadians() const;
	glm::mat4 GetLightSpaceMatrix() const;
	void SetLightUniforms(Shader& shader, int index);
};
