#include "Light.h"
#include <algorithm>
#include <cmath>

namespace {
	bool IsUsableShadowTarget(
		const FBO* target,
		int resolution,
		FBOAttributes::FramebufferType shadowType,
		unsigned int textureTarget) {
		return ShadowMapCacheState::IsTargetReady(target) &&
			target->width == resolution &&
			target->height == resolution &&
			target->attr.isShadowMap &&
			target->attr.shadowType == shadowType &&
			target->attr.textureAttrs.size() == 1 &&
			target->attr.textureAttrs.front().target == textureTarget;
	}

	void ReleaseInvalidShadowTarget(FBO*& target) {
		if (!target) {
			return;
		}
		auto& manager = FramebuffersManager::GetInstance();
		manager.ReleaseFBO(target);
		target = nullptr;
		manager.TrimUnusedFBOs();
	}
}

FBO* PointLight::EnsureShadowFBO() {
	const int resolution = (std::max)(1, shadowResolution);
	if (IsUsableShadowTarget(
		shadowFBO,
		resolution,
		FBOAttributes::FramebufferType::ShadowBox,
		GL_TEXTURE_CUBE_MAP)) {
		return shadowFBO;
	}
	if (shadowFBO) {
		ReleaseInvalidShadowTarget(shadowFBO);
	}
	shadowCache.Invalidate();
	FBOAttributes attr;
	attr.isShadowMap = true;
	attr.shadowType = FBOAttributes::FramebufferType::ShadowBox;
	attr.width = resolution;
	attr.height = resolution;
	attr.textureAttrs.push_back({ GL_TEXTURE_CUBE_MAP, GL_DEPTH_COMPONENT, GL_DEPTH_COMPONENT, GL_FLOAT });
	shadowFBO = FramebuffersManager::GetInstance().GetFBO(attr);
	if (!IsUsableShadowTarget(
		shadowFBO,
		resolution,
		FBOAttributes::FramebufferType::ShadowBox,
		GL_TEXTURE_CUBE_MAP)) {
		ReleaseInvalidShadowTarget(shadowFBO);
		return nullptr;
	}
	shadowFBO->passName = "PointLight_ShadowCube";
	return shadowFBO;
}

void PointLight::FitShadowToBounds(const glm::vec3& center, float radius) {
	if (!autoFitShadow) {
		return;
	}
	near = 0.05f;
	far = (std::max)(near + 0.1f, glm::length(position - center) + radius * 1.1f);
}

std::array<glm::mat4, 6>& PointLight::GetLightSpaceMatrices() {
	const float safeNear = (std::max)(0.001f, near);
	const float safeFar = (std::max)(safeNear + 0.001f, far);
	shadowProj = glm::perspective(glm::radians(90.0f), 1.0f, safeNear, safeFar);

	lightSpaceMatrices[0] = shadowProj *
		glm::lookAt(position, position + glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f));
	lightSpaceMatrices[1] = shadowProj *
		glm::lookAt(position, position + glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f));
	lightSpaceMatrices[2] = shadowProj *
		glm::lookAt(position, position + glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
	lightSpaceMatrices[3] = shadowProj *
		glm::lookAt(position, position + glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f));
	lightSpaceMatrices[4] = shadowProj *
		glm::lookAt(position, position + glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, -1.0f, 0.0f));
	lightSpaceMatrices[5] = shadowProj *
		glm::lookAt(position, position + glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, -1.0f, 0.0f));
	return lightSpaceMatrices;
}

void PointLight::SetLightUniforms(Shader& shader, int index) {
	std::string baseName = "pointLights[" + std::to_string(index) + "]";
	const bool active = GetActiveStatus();
	shader.setBool(baseName + ".isActive", active);
	shader.setBool(
		baseName + ".useShadowMap",
		active && useShadowMap && shadowCache.IsSampleable(shadowFBO));
	if (!active) return;
	shader.setVec3(baseName + ".position", position);
	shader.setVec3(baseName + ".ambient", ambient);
	shader.setVec3(baseName + ".diffuse", diffuse);
	shader.setVec3(baseName + ".specular", specular);
	shader.setFloat(baseName + ".constant", constant);
	shader.setFloat(baseName + ".linear", linear);
	shader.setFloat(baseName + ".quadratic", quadratic);
	shader.setFloat(baseName + ".far_plane", far);
}

FBO* DirectionLight::EnsureShadowFBO() {
	const int resolution = GetEffectiveShadowResolution();
	if (IsUsableShadowTarget(
		shadowFBO,
		resolution,
		FBOAttributes::FramebufferType::ShadowMap,
		GL_TEXTURE_2D)) {
		return shadowFBO;
	}
	if (shadowFBO) {
		ReleaseInvalidShadowTarget(shadowFBO);
	}
	shadowCache.Invalidate();
	FBOAttributes attr;
	attr.isShadowMap = true;
	attr.shadowType = FBOAttributes::FramebufferType::ShadowMap;
	attr.width = resolution;
	attr.height = resolution;
	attr.textureAttrs.push_back({ GL_TEXTURE_2D, GL_DEPTH_COMPONENT, GL_DEPTH_COMPONENT, GL_FLOAT });
	shadowFBO = FramebuffersManager::GetInstance().GetFBO(attr);
	if (!IsUsableShadowTarget(
		shadowFBO,
		resolution,
		FBOAttributes::FramebufferType::ShadowMap,
		GL_TEXTURE_2D)) {
		ReleaseInvalidShadowTarget(shadowFBO);
		return nullptr;
	}
	shadowFBO->passName = "DirectionLight_ShadowMap";
	return shadowFBO;
}

void DirectionLight::FitShadowToBounds(const glm::vec3& center, float radius) {
	lightSpaceAabbFitActive = false;
	fittedHalfHeight = width;
	effectiveShadowResolution = (std::max)(1, shadowResolution);
	if (!autoFitShadow) {
		return;
	}
	const float safeRadius = (std::max)(0.5f, radius);
	shadowCenter = center;
	width = safeRadius * 1.08f;
	distance = safeRadius * 2.2f;
	near_plane = (std::max)(0.05f, distance - safeRadius * 1.2f);
	far_plane = distance + safeRadius * 1.2f;
	fittedHalfHeight = width;
}

void DirectionLight::ApplyLightSpaceAabbFit(
	const glm::vec3& center,
	float halfWidth,
	float halfHeight,
	float eyeDistance,
	float nearPlane,
	float farPlane,
	int effectiveResolution) {
	lightSpaceAabbFitActive = true;
	shadowCenter = center;
	width = (std::max)(0.01f, halfWidth);
	fittedHalfHeight = (std::max)(0.01f, halfHeight);
	distance = (std::max)(0.01f, eyeDistance);
	near_plane = (std::max)(0.001f, nearPlane);
	far_plane = (std::max)(near_plane + 0.001f, farPlane);
	effectiveShadowResolution = (std::max)(1, effectiveResolution);
}

glm::mat4 DirectionLight::GetLightSpaceMatrix() const {
	const float safeWidth = (std::max)(0.01f, width);
	const float safeHalfHeight = lightSpaceAabbFitActive
		? (std::max)(0.01f, fittedHalfHeight)
		: safeWidth;
	const float safeNear = (std::max)(0.001f, near_plane);
	const float safeFar = (std::max)(safeNear + 0.001f, far_plane);
	glm::mat4 lightProjection = glm::ortho(
		-safeWidth,
		safeWidth,
		-safeHalfHeight,
		safeHalfHeight,
		safeNear,
		safeFar);
	glm::vec3 lightDir = glm::length(direction) > 0.0001f
		? glm::normalize(direction)
		: glm::vec3(0.0f, -1.0f, 0.0f);
	const glm::vec3 eyePos = shadowCenter - lightDir * distance;
	glm::vec3 usableUp(0.0f, 1.0f, 0.0f);
	if (glm::abs(glm::dot(lightDir, usableUp)) > 0.999f) {
		usableUp = glm::vec3(1.0f, 0.0f, 0.0f);
	}
	const glm::mat4 lightView = glm::lookAt(eyePos, shadowCenter, usableUp);
	if (lightSpaceAabbFitActive) {
		// The tight-fit path snaps shadowCenter directly in light space, so
		// applying the historical world-origin offset again would double-snap.
		return lightProjection * lightView;
	}

	// Keep the projection aligned to shadow texels to avoid shimmering when an
	// automatically-fitted scene or light moves by sub-texel amounts.
	const int resolution = GetEffectiveShadowResolution();
	glm::vec4 shadowOrigin = lightProjection * lightView * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
	shadowOrigin *= static_cast<float>(resolution) * 0.5f;
	const glm::vec4 roundedOrigin = glm::round(shadowOrigin);
	const glm::vec4 roundOffset =
		(roundedOrigin - shadowOrigin) * (2.0f / static_cast<float>(resolution));
	lightProjection[3][0] += roundOffset.x;
	lightProjection[3][1] += roundOffset.y;
	return lightProjection * lightView;
}

void DirectionLight::SetLightUniforms(Shader& shader, int index) {
	std::string baseName = "dirLights[" + std::to_string(index) + "]";
	const bool active = GetActiveStatus();
	shader.setBool(baseName + ".isActive", active);
	shader.setBool(
		baseName + ".useShadowMap",
		active && useShadowMap && shadowCache.IsSampleable(shadowFBO));
	if (!active) return;
	shader.setVec3(baseName + ".direction", direction);
	shader.setVec3(baseName + ".ambient", ambient);
	shader.setVec3(baseName + ".diffuse", diffuse);
	shader.setVec3(baseName + ".specular", specular);
	shader.setMat4(baseName + ".lightSpaceMatrix", GetLightSpaceMatrix());
	const float halfHeight = lightSpaceAabbFitActive
		? (std::max)(0.01f, fittedHalfHeight)
		: (std::max)(0.01f, width);
	const float worldUnitsPerTexel =
		2.0f * (std::max)((std::max)(0.01f, width), halfHeight) /
		static_cast<float>(GetEffectiveShadowResolution());
	const float normalizedDepthPerTexel =
		worldUnitsPerTexel /
		(std::max)(far_plane - near_plane, 0.001f);
	shader.setVec4(
		baseName + ".shadowBiasParams",
		glm::vec4(normalizedDepthPerTexel, 0.0f, 0.0f, 0.0f));
}

FBO* SpotLight::EnsureShadowFBO() {
	const int resolution = (std::max)(1, shadowResolution);
	if (IsUsableShadowTarget(
		shadowFBO,
		resolution,
		FBOAttributes::FramebufferType::ShadowMap,
		GL_TEXTURE_2D)) {
		return shadowFBO;
	}
	if (shadowFBO) {
		ReleaseInvalidShadowTarget(shadowFBO);
	}
	shadowCache.Invalidate();
	FBOAttributes attr;
	attr.isShadowMap = true;
	attr.shadowType = FBOAttributes::FramebufferType::ShadowMap;
	attr.width = resolution;
	attr.height = resolution;
	attr.textureAttrs.push_back({ GL_TEXTURE_2D, GL_DEPTH_COMPONENT, GL_DEPTH_COMPONENT, GL_FLOAT });
	shadowFBO = FramebuffersManager::GetInstance().GetFBO(attr);
	if (!IsUsableShadowTarget(
		shadowFBO,
		resolution,
		FBOAttributes::FramebufferType::ShadowMap,
		GL_TEXTURE_2D)) {
		ReleaseInvalidShadowTarget(shadowFBO);
		return nullptr;
	}
	shadowFBO->passName = "SpotLight_ShadowMap";
	return shadowFBO;
}

void SpotLight::FitShadowToBounds(const glm::vec3& center, float radius) {
	if (!autoFitShadow) {
		return;
	}
	near_plane = 0.05f;
	far_plane = (std::max)(
		near_plane + 0.1f,
		glm::length(position - center) + radius * 1.1f);
}

bool SpotLight::GetShadowViewBasis(
	glm::vec3& forward,
	glm::vec3& right,
	glm::vec3& up) const {
	auto finiteVector = [](const glm::vec3& value) {
		return std::isfinite(value.x) &&
			std::isfinite(value.y) &&
			std::isfinite(value.z);
	};
	if (!finiteVector(direction)) {
		return false;
	}

	const float directionLength = glm::length(direction);
	if (!std::isfinite(directionLength)) {
		return false;
	}
	forward = directionLength > 0.0001f
		? direction / directionLength
		: glm::vec3(0.0f, -1.0f, 0.0f);
	glm::vec3 usableUp(0.0f, 1.0f, 0.0f);
	if (glm::abs(glm::dot(forward, usableUp)) > 0.999f) {
		usableUp = glm::vec3(1.0f, 0.0f, 0.0f);
	}
	right = glm::normalize(glm::cross(forward, usableUp));
	up = glm::normalize(glm::cross(right, forward));
	return finiteVector(forward) &&
		finiteVector(right) &&
		finiteVector(up);
}

float SpotLight::GetShadowHalfAngleRadians() const {
	const float safeOuterCutOff =
		std::isfinite(outerCutOff) ? outerCutOff : 35.0f;
	return glm::radians(glm::clamp(safeOuterCutOff, 0.5f, 87.5f));
}

glm::mat4 SpotLight::GetLightSpaceMatrix() const {
	const float safeNear = (std::max)(0.001f, near_plane);
	const float safeFar = (std::max)(safeNear + 0.001f, far_plane);
	glm::vec3 lightDir(0.0f, -1.0f, 0.0f);
	glm::vec3 lightRight(1.0f, 0.0f, 0.0f);
	glm::vec3 lightUp(0.0f, 0.0f, -1.0f);
	if (!GetShadowViewBasis(lightDir, lightRight, lightUp)) {
		lightDir = glm::vec3(0.0f, -1.0f, 0.0f);
		lightUp = glm::vec3(1.0f, 0.0f, 0.0f);
	}
	const float fieldOfViewRadians = GetShadowHalfAngleRadians() * 2.0f;
	return glm::perspective(fieldOfViewRadians, 1.0f, safeNear, safeFar) *
		glm::lookAt(position, position + lightDir, lightUp);
}

void SpotLight::SetLightUniforms(Shader& shader, int index) {
	std::string baseName = "spotLights[" + std::to_string(index) + "]";
	const bool active = GetActiveStatus();
	shader.setBool(baseName + ".isActive", active);
	shader.setBool(
		baseName + ".useShadowMap",
		active && useShadowMap && shadowCache.IsSampleable(shadowFBO));
	if (!active) return;
	shader.setVec3(baseName + ".position", position);
	shader.setVec3(baseName + ".ambient", ambient);
	shader.setVec3(baseName + ".diffuse", diffuse);
	shader.setVec3(baseName + ".specular", specular);
	shader.setFloat(baseName + ".constant", constant);
	shader.setFloat(baseName + ".linear", linear);
	shader.setFloat(baseName + ".quadratic", quadratic);
	shader.setVec3(baseName + ".direction", direction);
	shader.setFloat(baseName + ".cutOff", std::cos(glm::radians(cutOff)));
	shader.setFloat(baseName + ".outerCutOff", std::cos(glm::radians(outerCutOff)));
	shader.setMat4(baseName + ".lightSpaceMatrix", GetLightSpaceMatrix());
	const float safeNear = (std::max)(0.001f, near_plane);
	const float safeFar = (std::max)(safeNear + 0.001f, far_plane);
	const float halfFovRadians = GetShadowHalfAngleRadians();
	const float worldFootprintPerDistance =
		2.0f * std::tan(halfFovRadians) /
		static_cast<float>((std::max)(1, shadowResolution));
	const float perspectiveDepthOffset =
		safeFar / (safeFar - safeNear);
	// For depth = A - B / distance, PCSS only needs all distances in the
	// same B-scaled domain. Passing 1 / B lets the Shader scale the receiver
	// once instead of multiplying every accepted blocker by B.
	const float inversePerspectiveDepthScale =
		(safeFar - safeNear) / (safeFar * safeNear);
	shader.setVec4(
		baseName + ".shadowBiasParams",
		glm::vec4(
			perspectiveDepthOffset,
			inversePerspectiveDepthScale,
			worldFootprintPerDistance,
			1.0f));
}
