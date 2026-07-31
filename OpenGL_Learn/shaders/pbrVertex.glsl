#version 330 core

layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aTexCoords;
layout (location = 3) in vec3 aTangent;
layout (location = 4) in vec3 aBitangent;

out VS_OUT {
	vec3 FragPos;
	vec3 Normal;
	vec2 TexCoords;
	mat3 TBN;
} vs_out;

uniform mat4 model;

layout (std140) uniform Matrices {
	mat4 view;
	mat4 projection;
};

void main()
{
	mat3 normalMatrix = mat3(transpose(inverse(model)));
	vec3 normal = normalize(normalMatrix * aNormal);
	vec3 tangent = normalize(normalMatrix * aTangent);
	tangent = normalize(tangent - normal * dot(normal, tangent));
	vec3 importedBitangent = normalize(normalMatrix * aBitangent);
	float handedness = dot(cross(normal, tangent), importedBitangent) < 0.0 ? -1.0 : 1.0;
	vec3 bitangent = normalize(cross(normal, tangent)) * handedness;

	vs_out.FragPos = vec3(model * vec4(aPos, 1.0));
	vs_out.Normal = normal;
	vs_out.TexCoords = aTexCoords;
	vs_out.TBN = mat3(tangent, bitangent, normal);
	gl_Position = projection * view * vec4(vs_out.FragPos, 1.0);
}
