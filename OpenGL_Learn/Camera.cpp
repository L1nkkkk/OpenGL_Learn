#include "Camera.h"

void Camera::CameraMouseCallback(double xpos, double ypos) {
	if (firstMouse) {
		lastX = static_cast<float>(xpos);
		lastY = static_cast<float>(ypos);
		firstMouse = false;
	}
	float deltaX = static_cast<float>(xpos) - lastX;
	float deltaY = static_cast<float>(ypos) - lastY;
	lastX = static_cast<float>(xpos);
	lastY = static_cast<float>(ypos);
	pitch += deltaY * 0.05f;
	if (pitch > 89.0f)
		pitch = 89.0f;
	if (pitch < -89.0f)
		pitch = -89.0f;
	yaw += deltaX * 0.05f;

	cameraDirection = glm::normalize(glm::vec3(
		cos(glm::radians(pitch)) * cos(glm::radians(yaw)),
		sin(glm::radians(pitch)),
		cos(glm::radians(pitch)) * sin(glm::radians(yaw))
	));
	lastFrame_CameraFront = cameraFront;
	cameraFront = -cameraDirection;
}

void Camera::SetCameraDirection(float deltapitch, float deltayaw)
{
	lastFrame_CameraFront = cameraFront;

	pitch += deltapitch;
	yaw += deltayaw;
	cameraDirection = glm::normalize(glm::vec3(
		cos(glm::radians(pitch)) * cos(glm::radians(yaw)),
		sin(glm::radians(pitch)),
		cos(glm::radians(pitch)) * sin(glm::radians(yaw))
	));

	cameraFront = -cameraDirection;
}

void Camera::CameraSrollCallback(double xoffset, double yoffset) {
	if (fov >= 1.0f && fov <= 45.0f)
		fov -= (float)yoffset;
	if (fov < 1.0f)
		fov = 1.0f;
	if (fov > 45.0f)
		fov = 45.0f;
}

glm::mat4 Camera::GetViewMatrix() {
	return glm::lookAt(cameraPos, cameraPos - cameraDirection, up);
}

glm::mat4 Camera::GetProjectionMatrix(float aspectRatio, float nearPlane, float farPlane) const {
	return glm::perspective(glm::radians(fov), aspectRatio, nearPlane, farPlane);
}

bool Camera::CheckCameraMoved() {
	float positionDelta = glm::length(cameraPos - lastFrame_CameraPosition);
	float frontDeltaCosin = glm::dot(cameraFront, lastFrame_CameraFront);
	if (frontDeltaCosin <= 0) return true; //camera turned more than 90 degree, consider it as moved
	float frontDelta = 2.0f * glm::acos(frontDeltaCosin) * 180.0f / M_PI;
	if (positionDelta > 0.01f || frontDelta > 0.1f) {
		return true;
	}
	else {
		return false;
	}
}
