#version 330 core
layout (location = 0) out vec4 FragColor;
in vec2 TexCoords;

uniform sampler2D gPosition;
uniform sampler2D gNormal;
uniform sampler2D gAlbedoSpec;
uniform sampler2D gMaterial;

vec3 albedoSpec;
vec4 material;//r : ambinent g:diffuse b:spec a : shinings

struct DirLight{
	vec3 direction;
	vec3 ambient;
	vec3 diffuse;
	vec3 specular;
	mat4 lightSpaceMatrix;

	sampler2D shadowMap;
	bool useShadowMap;
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

uniform vec3 viewPos;

#define EPS 1e-5
#define PI 3.141592653589793
#define PI2 6.283185307179586
#define MAX_DIR_LIGHTS 16
#define MAX_SAMPLE_NUM 256
#define MAX_RINGS_NUM MAX_SAMPLE_NUM
#define DEFAULT_SHADOW 0
#define PCF_SHADOW 1
#define PCSS_SHADOW 2

uniform int NR_DIR_LIGHTS;
uniform DirLight dirLights[MAX_DIR_LIGHTS];

vec2 poissonDisk[MAX_SAMPLE_NUM];

float rand_1to1(float x){
	return fract(sin(x)*10000.0);
}

float rand_2to1(vec2 uv){
	const float a = 12.9898, b = 78.233, c = 43758.5453;
	float dt = dot( uv.xy, vec2( a,b ) ), sn = mod( dt, PI );
	return fract(sin(sn) * c);
}

void VecToSpherial(vec3 dir,out float phi,out float theta){
	phi = acos(clamp(dir.z,-1.0,1.0));
	theta = atan(dir.y,dir.x);
}

vec3 SphericalToVec(float phi, float theta) {
    float sinPhi = sin(phi);
    return vec3(
        sinPhi * cos(theta),
        sinPhi * sin(theta),
        cos(phi)
    );
}

void poissonDiskSamples( const in vec2 randomSeed ) {
	int samplesNum = min(shadowSampleNum,MAX_SAMPLE_NUM);
	float ANGLE_STEP = PI2 * float(min(shadowSampleRings,MAX_RINGS_NUM)) / float(samplesNum);
	float INV_NUM_SAMPLES = 1.0 / float(samplesNum);

	float angle = rand_2to1( randomSeed ) * PI2;
	float radius = INV_NUM_SAMPLES;
	float radiusStep = radius;

	for(int i = 0; i < samplesNum; i ++ ) {
		poissonDisk[i] = vec2(cos(angle),sin(angle)) * pow(radius,0.75);
		radius += radiusStep;
		angle += ANGLE_STEP;
	}
}

float PCF(vec4 coords,float w_penumbraSize,vec3 normal,DirLight light){
	if(!light.useShadowMap) return 0.0;
	float bias = max(0.05 * (1.0 - dot(normal, normalize(-light.direction))), 0.005);
	float shadow = 0.0;
	vec3 projCoords = coords.xyz/coords.w;
	projCoords = (projCoords+1.0)* 0.5;

	if(projCoords.z >= 1.0) {
		return 0.0; // ����Զƽ�棬������Ӱ��
	}

	float currentDepth = projCoords.z;
	poissonDiskSamples(projCoords.xy);

	int shadowSamples = min(shadowSampleNum,MAX_SAMPLE_NUM);
	float stepth = 64.0/1024.0;
	for(int i = 0; i < shadowSamples;++i){
		float pcfDepth = texture(light.shadowMap, projCoords.xy + poissonDisk[i]*w_penumbraSize*stepth).r;
		if(currentDepth-bias>pcfDepth) shadow += 1.0;
	}
	shadow = shadow/float(shadowSamples);
	return shadow;
}


float findBlocker( sampler2D shadowMap,  vec2 uv, float zReceiver ) {
	float avgBlockerDepth = 0.0;
	int numBlockers = 0;

	poissonDiskSamples(uv);

	int shadowSamples = min(shadowSampleNum,MAX_SAMPLE_NUM);
	for(int i = 0; i < shadowSamples; ++i) {
		float pcfDepth =texture(shadowMap, uv + poissonDisk[i] * 0.1).r;
		if(pcfDepth < zReceiver-EPS) {
			avgBlockerDepth += pcfDepth;
			++numBlockers;
		}
	}
	if(numBlockers == 0)
		return zReceiver;

	avgBlockerDepth /= float(numBlockers);
	return avgBlockerDepth;
}

float findBlocker( samplerCube shadowMap, vec3 dir, float zReceiver ) {
	float avgBlockerDepth = 0.0;
	int numBlockers = 0;
	vec2 sph;
	VecToSpherial(dir,sph.x,sph.y);
	poissonDiskSamples(sph);

	int shadowSamples = min(shadowSampleNum,MAX_SAMPLE_NUM);
	for(int i = 0; i < shadowSamples; ++i) {
		vec2 sampleCoords = sph + poissonDisk[i]*0.1;
		float pcfDepth =texture(shadowMap, SphericalToVec(sampleCoords.x,sampleCoords.y)).r;
		if(pcfDepth < zReceiver-EPS) {
			avgBlockerDepth += pcfDepth;
			++numBlockers;
		}
	}
	if(numBlockers == 0)
		return zReceiver;

	avgBlockerDepth /= float(numBlockers);
	return avgBlockerDepth;
}

float PCSS(vec4 coords,vec3 normal,DirLight light){
	if(!light.useShadowMap) return 0.0;
	vec3 projCoords = coords.xyz/coords.w;
	projCoords = (projCoords+1.0)*0.5;
	float receiverDistance = projCoords.z;
	float avgBlockerDepth = findBlocker(light.shadowMap,projCoords.xy,receiverDistance);
	if(avgBlockerDepth>=1.0) return 0.0;
	float penumbraSize = (receiverDistance - avgBlockerDepth)  / avgBlockerDepth ;
	return PCF(coords,penumbraSize,normal,light);
}

float ShadowCalculation(vec4 fragPosLightSpace,vec3 normal,DirLight light){
	if(!light.useShadowMap) return 0.0;
	float bias = max(0.05 * (1.0 - dot(normal, normalize(-light.direction))), 0.005);
	vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
	projCoords = projCoords * 0.5 + 0.5;
	float closestDepth = texture(light.shadowMap, projCoords.xy).r;
	if(projCoords.z > 1.0) {
        return 0.0;
    }
	float currentDepth = projCoords.z;
	return currentDepth - bias > closestDepth ? 1.0 : 0.0;
}


vec3 CalcDirLight(DirLight light, vec3 normal, vec3 viewDir,float shadow)
{
	vec3 color = albedoSpec;
	//After this below, BUG
	
	vec3 lightDir = normalize(-light.direction);
	// diffuse shading
	float diff = max(dot(normal, lightDir), 0.0);
	// specular shading
	vec3 halfwayDir = normalize(lightDir + viewDir);
	float spec = pow(max(dot(normal, halfwayDir), 0.0), material.a);
	// combine results
	vec3 ambient = light.ambient * material.r;
	vec3 diffuse = light.diffuse * material.g *diff;
	vec3 specular = light.specular * material.b* spec;
	return (ambient + (1.0-shadow)*(diffuse + specular))* color;
}


void main(){
	albedoSpec = texture(gAlbedoSpec, TexCoords).rgb;
	material = texture(gMaterial, TexCoords);
	vec3 norm = normalize(texture(gNormal,TexCoords).rgb);
	vec3 FragPos = texture(gPosition,TexCoords).rgb;
	vec3 viewDir = normalize(viewPos - FragPos);
	vec3 results = vec3(0);

	int dirLightsNum =  min(MAX_DIR_LIGHTS,NR_DIR_LIGHTS);
	float shadow;
	for(int i = 0;i<dirLightsNum;i++){
		vec4 FragPosLightSpace =  dirLights[i].lightSpaceMatrix * vec4(FragPos,1.0);
			switch(shadowType){
				case DEFAULT_SHADOW:
					shadow = ShadowCalculation(FragPosLightSpace,norm,dirLights[i]);
					break;
				case PCF_SHADOW:
					shadow = PCF(FragPosLightSpace,0.1,norm,dirLights[i]);
					break;
				case PCSS_SHADOW:
					shadow = PCSS(FragPosLightSpace,norm,dirLights[i]);
					break;
			}
		results += CalcDirLight(dirLights[i], norm, viewDir,shadow);
	}
	FragColor = vec4(results, 1.0);
	return; 
}