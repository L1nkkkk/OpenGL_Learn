#pragma once
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

class Camera {
public:
	glm::vec3 cameraFront = glm::vec3(0.0f, 0.0f, -1.0f);
	glm::vec3 cameraPos = glm::vec3(0.0f,0.0f,3.0f);
	glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
	glm::vec3 cameraDirection;
	float cameraSpeed;
	float fov = 45.0f;

	Camera(float speed, glm::vec3 pos, float lastx, float lasty) {
		cameraSpeed = speed;
		lastX = lastx;
		lastY = lasty;
		cameraPos = pos;
	}
	void CameraMouseCallback(double xpos, double ypos);
	void CameraSrollCallback(double xoffset, double yoffset);
	void SetLastPos(float x, float y) {
		lastX = x;
		lastY = y;
	}
	glm::mat4 GetViewMatrix();
	void SetCameraDirection(float pitch, float yaw);
private:
	
	float yaw = 90.0f;
	float pitch = 0.0f;
	float lastX;
	float lastY;
	
	bool firstMouse = true;
};