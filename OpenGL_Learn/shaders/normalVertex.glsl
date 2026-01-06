#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aTexCoords;

out VS_OUT {
	vec3 normal;
} gs_in;

uniform mat4 model;

layout (std140) uniform Matrices{
	mat4 view;
	mat4 projection;
};

void main()
{
	gs_in.normal = mat3(transpose(inverse(view * model))) * aNormal;
	gl_Position =  view * model * vec4(aPos, 1.0);
}