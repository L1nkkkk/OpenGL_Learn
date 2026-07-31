#include "Global.h"

glm::mat4 BaseObject::getModelMatrix() {
	if (m_transformCacheValid &&
		position == m_cachedPosition &&
		rotation == m_cachedRotation &&
		scale == m_cachedScale) {
		return modelMatrix;
	}

	modelMatrix = glm::mat4(1.0f);
	modelMatrix = glm::translate(modelMatrix, position);
	modelMatrix = glm::rotate(modelMatrix, glm::radians(rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
	modelMatrix = glm::rotate(modelMatrix, glm::radians(rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
	modelMatrix = glm::rotate(modelMatrix, glm::radians(rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
	modelMatrix = glm::scale(modelMatrix, scale);
	m_cachedPosition = position;
	m_cachedRotation = rotation;
	m_cachedScale = scale;
	m_transformCacheValid = true;
	++m_transformRevision;
	return modelMatrix;
}

void BaseObject::setModelMatrix(glm::mat4 matrix) {
	modelMatrix = matrix;
	m_cachedPosition = position;
	m_cachedRotation = rotation;
	m_cachedScale = scale;
	m_transformCacheValid = true;
	++m_transformRevision;
}
