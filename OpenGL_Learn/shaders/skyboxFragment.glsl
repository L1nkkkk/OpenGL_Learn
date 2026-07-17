#version 330 core
layout (location = 0) out vec4 FragColor;
layout (location = 1) out vec4 BrightColor;

in vec3 TexCoords;


uniform samplerCube skybox;

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
	FragColor = texture(skybox, TexCoords);
	if (!useGamma) FragColor.rgb = LinearToSrgb(FragColor.rgb);
	BrightColor = vec4(0,0,0,1);
}
