#version 330 core
out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D screenTexture;
uniform float gamma;
uniform bool useGamma;
uniform bool useShadowMap;

uniform bool hdr;
uniform float exposure;

uniform bool bloom;
uniform sampler2D bloomBlur;

const float offset = 1.0 / 300.0;  

void main()
{
    vec2 offsets[9] = vec2[](
        vec2(-offset,  offset), // 左上
        vec2( 0.0f,    offset), // 正上
        vec2( offset,  offset), // 右上
        vec2(-offset,  0.0f),   // 左
        vec2( 0.0f,    0.0f),   // 中
        vec2( offset,  0.0f),   // 右
        vec2(-offset, -offset), // 左下
        vec2( 0.0f,   -offset), // 正下
        vec2( offset, -offset)  // 右下
    );

    float kernel[9] = float[](
        0,0,0,
        0,1,0,
        0,0,0
    );
    FragColor = vec4(vec3(texture(screenTexture, TexCoords)), 1.0);
    if(bloom){
        vec3 bloomColor = texture(bloomBlur, TexCoords).rgb;
        FragColor.rgb += bloomColor;
    }
    if(hdr){
        FragColor.rgb = vec3(1.0) - exp(-FragColor.rgb * exposure);
    }
    if(useGamma){
        FragColor.rgb = pow(FragColor.rgb,vec3(1.0/gamma));
    }
    if(useShadowMap){
        FragColor = vec4(vec3(texture(screenTexture,TexCoords).r),1.0);
    }
}