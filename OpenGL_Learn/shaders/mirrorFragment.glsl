#version 330 core
layout (location = 0) out vec4 FragColor;
layout (location = 1) out vec4 BrightColor;
layout (location = 2) out vec4 NormalOut;
in vec3 Position;
in vec3 Normal;


uniform samplerCube skybox;
uniform vec3 cameraPos;
layout(std140) uniform Matrices{
	mat4 view;
	mat4 projection;
};

layout(std140) uniform SystemProperties {
    bool useBloom;
    bool useShadowMap;
    bool useGamma;
    bool useHDR;
    float bloomThreshold;
    float gamma;
    float exposure;
    int bloomBlurIterations;
    int shadowSampleNum;
    int shadowSampleRings;
    int shadowType;
    int screenWidth;
    int screenHeight;
};

vec3 LinearToSrgb(vec3 value)
{
	vec3 low = value * 12.92;
	vec3 high = 1.055 * pow(max(value, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
	return mix(low, high, step(vec3(0.0031308), value));
}

void main()
{
	vec3 I = normalize(Position - cameraPos);
	vec3 R = reflect(I, normalize(Normal));
	FragColor = texture(skybox, R);
	if (!useGamma) FragColor.rgb = LinearToSrgb(FragColor.rgb);
	BrightColor = vec4(0,0,0,1);
	// Transform world normal to view space then encode for attachment2.
	vec3 nVS = normalize(mat3(transpose(inverse(view))) * Normal);
	NormalOut = vec4(nVS * 0.5 + 0.5, 1.0);
}
