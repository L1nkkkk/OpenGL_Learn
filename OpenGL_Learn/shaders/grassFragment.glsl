#version 330 core
layout (location = 0) out vec4 FragColor;
layout (location = 1) out vec4 BrightColor;
layout (location = 2) out vec4 NormalOut;

struct Material {
	sampler2D texture_diffuse1;
};

in vec2 TexCoords;
in vec3 NormalVS;

uniform Material material;

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

vec4 SampleColorTexture(sampler2D source, vec2 uv)
{
	vec4 sampleValue = texture(source, uv);
	if (!useGamma) sampleValue.rgb = LinearToSrgb(sampleValue.rgb);
	return sampleValue;
}

void main()
{   
	vec4 color = SampleColorTexture(material.texture_diffuse1, TexCoords);
	if(color.a < 0.99) FragColor = color;
	else FragColor = vec4(color.rgb,1.0);
	BrightColor = vec4(0,0,0,1);
	NormalOut = vec4(normalize(NormalVS) * 0.5 + 0.5, 1.0);
}
