#include "Camera.h"

void Camera::CameraMouseCallback(double xpos, double ypos) {
	if (firstMouse) {
		lastX = xpos;
		lastY = ypos;
		firstMouse = false;
	}
	float deltaX = xpos - lastX;
	float deltaY = ypos - lastY;
	lastX = xpos;
	lastY = ypos;
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

	cameraFront = -cameraDirection;
}

void Camera::SetCameraDirection(float deltapitch, float deltayaw)
{
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