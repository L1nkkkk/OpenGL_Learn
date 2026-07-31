#version 330 core

in vec4 FragPos;
in vec2 TexCoords;

uniform vec3 lightPos;
uniform float far_plane;
uniform sampler2D shadowDiffuseMap;
uniform sampler2D shadowOpacityMap;
uniform bool shadowHasDiffuseMap;
uniform bool shadowHasOpacityMap;
uniform bool shadowUseAlphaCutoff;
uniform float shadowOpacity;
uniform float shadowAlphaCutoff;

void main(){
    float alpha = shadowOpacity;
    if (shadowHasDiffuseMap) {
        alpha *= texture(shadowDiffuseMap, TexCoords).a;
    }
    if (shadowHasOpacityMap) {
        alpha *= texture(shadowOpacityMap, TexCoords).r;
    }
    if ((shadowUseAlphaCutoff && alpha < shadowAlphaCutoff) || alpha <= 0.001) {
        discard;
    }

    float lightDistance = length(FragPos.xyz - lightPos);

    lightDistance = lightDistance/far_plane;
    gl_FragDepth = lightDistance;
}
