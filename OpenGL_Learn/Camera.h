#pragma once
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#define M_PI 3.14159265358979323846f

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
		lastFrame_CameraFront = cameraFront;
		lastFrame_CameraPosition = 
		cameraPos = pos;
		 
	}
	void CameraMouseCallback(double xpos, double ypos);
	void CameraSrollCallback(double xoffset, double yoffset);
	void SetLastPos(double x, double y) {
		lastX = static_cast<float>(x);
		lastY = static_cast<float>(y);
	}
	glm::mat4 GetViewMatrix();
	void SetCameraDirection(float pitch, float yaw);

	void UpdatePositionByDelta(glm::vec3 delta) {
		lastFrame_CameraPosition = cameraPos;
		cameraPos += delta;
	}

	bool CheckCameraMoved();
private:
	
	float yaw = -90.0f;
	float pitch = 0.0f;
	float lastX;
	float lastY;
	
	bool firstMouse = true;

	glm::vec3 lastFrame_CameraPosition;
	glm::vec3 lastFrame_CameraFront;
	glm::vec3 currentFrame_CameraFront;
};
