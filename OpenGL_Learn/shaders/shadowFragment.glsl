#version 330 core

in vec2 TexCoords;

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
}
