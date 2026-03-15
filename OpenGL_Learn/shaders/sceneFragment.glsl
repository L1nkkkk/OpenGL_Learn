#version 330 core
out vec4 FragColor;

in vec2 TexCoords;

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

uniform sampler2D screenTexture;
uniform sampler2D bloomBlur;

const float offset = 1.0 / 300.0;  

void main()
{
    vec2 offsets[9] = vec2[](
        vec2(-offset,  offset), // ????
        vec2( 0.0f,    offset), // ????
        vec2( offset,  offset), // ????
        vec2(-offset,  0.0f),   // ??
        vec2( 0.0f,    0.0f),   // ??
        vec2( offset,  0.0f),   // ??
        vec2(-offset, -offset), // ????
        vec2( 0.0f,   -offset), // ????
        vec2( offset, -offset)  // ????
    );

    float kernel[9] = float[](
        0,0,0,
        0,1,0,
        0,0,0
    );
    FragColor = vec4(vec3(texture(screenTexture, TexCoords)), 1.0);
    if(useBloom){
        vec3 bloomColor = texture(bloomBlur, TexCoords).rgb;
        FragColor.rgb += bloomColor;
    }
    if(useHDR){
        FragColor.rgb = vec3(1.0) - exp(-FragColor.rgb * exposure);
    }
    if(useGamma){
        FragColor.rgb = pow(FragColor.rgb,vec3(1.0/gamma));
    }
    if(useShadowMap){
        FragColor = vec4(vec3(texture(screenTexture,TexCoords).r),1.0);
    }
}