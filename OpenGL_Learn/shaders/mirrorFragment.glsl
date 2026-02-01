#version 330 core
layout (location = 0) out vec4 FragColor;
layout (location = 1) out vec4 BrightColor;
in vec3 Position;
in vec3 Normal;


uniform samplerCube skybox;
uniform vec3 cameraPos;

void main()
{
	vec3 I = normalize(Position - cameraPos);
	vec3 R = reflect(I, normalize(Normal));
	FragColor = texture(skybox, R);
	BrightColor = vec4(0,0,0,1);
}