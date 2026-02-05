#include "Light.h"

void PointLight::DrawPointLight() {
	for (auto& mesh : meshes) {
		auto& vertices = mesh.vertices;
		auto VAO = mesh.GetVAO();
		glBindVertexArray(VAO);
		glDrawArrays(GL_TRIANGLES, 0, vertices.size());
		glBindVertexArray(0);
	}
}