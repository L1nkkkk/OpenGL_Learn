#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aTexCoords;
layout (location = 3) in vec3 aTangent;
layout (location = 4) in vec3 aBitangent;

out VS_OUT {
	vec3 FragPos;
	float ViewDepth;
	vec3 Normal;
	vec2 TexCoords;
	mat3 TBN;
} vs_out;

uniform mat4 model;

layout (std140) uniform Matrices{
	mat4 view;
	mat4 projection;
};

void main()
{
	vs_out.FragPos = vec3(model * vec4(aPos, 1.0));
	vs_out.ViewDepth = -(view * model * vec4(aPos, 1.0)).z;
	vs_out.Normal = mat3(transpose(inverse(model))) * aNormal;
	vs_out.TexCoords = aTexCoords;
	vec3 T = normalize(vec3(model * vec4(aTangent,   0.0)));
	vec3 B = normalize(vec3(model * vec4(aBitangent, 0.0)));
	vec3 N = normalize(cross(B, T));
	vs_out.TBN = mat3(T,B,N);
	gl_Position = projection * view * model * vec4(aPos, 1.0);
}