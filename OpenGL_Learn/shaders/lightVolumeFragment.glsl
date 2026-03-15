#version 330 core
layout (location = 0) out vec4 FragColor;

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

vec2 screenSize = vec2(screenWidth, screenHeight);
vec2 TexCoords;

struct PointLight{
	vec3 position;

	float constant;
	float linear;
	float quadratic;

	vec3 ambient;
	vec3 diffuse;
	vec3 specular;

	float far_plane;
	samplerCube shadowCubeMap;
	bool useShadowMap;
};

uniform PointLight pointLight;

uniform vec3 viewPos;

uniform sampler2D gPosition;
uniform sampler2D gNormal;
uniform sampler2D gAlbedoSpec;
uniform sampler2D gMaterial;

#define EPS 1e-5
#define PI 3.141592653589793
#define PI2 6.283185307179586
#define MAX_POINT_LIGHTS 16
#define MAX_DIR_LIGHTS 16
#define MAX_SPOT_LIGHTS 16
#define MAX_SAMPLE_NUM 256
#define MAX_RINGS_NUM MAX_SAMPLE_NUM
#define DEFAULT_SHADOW 0
#define PCF_SHADOW 1
#define PCSS_SHADOW 2

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

vec3 SphericalToVec(float phi, float theta) {
    float sinPhi = sin(phi);
    return vec3(
        sinPhi * cos(theta),
        sinPhi * sin(theta),
        cos(phi)
    );
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

float PCF(vec3 fragPos,float w_penumbraSize,vec3 normal,PointLight light){
	if(!light.useShadowMap) return 0.0;
	vec3 direction = normalize(fragPos - light.position);
	float bias = max(0.05 * (1.0 - dot(normal, -direction)), 0.005);
	float shadow = 0.0;
	float currentDepth = length(fragPos - light.position);
	if(currentDepth>light.far_plane) {
		return 0.0; // ??????????????????
	}
	poissonDiskSamples(direction.xy);
	vec2 sphericalCoords;
	VecToSpherial(direction,sphericalCoords.x,sphericalCoords.y);
	int shadowSamples = min(shadowSampleNum,MAX_SAMPLE_NUM);
	float stepth = 64.0/1024.0;
	for(int i = 0; i < shadowSamples;++i){
		vec2 sampleCoords = sphericalCoords + poissonDisk[i]*w_penumbraSize*stepth;
		float pcfDepth = texture(light.shadowCubeMap, SphericalToVec(sampleCoords.x,sampleCoords.y)).r;
		pcfDepth *= light.far_plane;
		if(currentDepth-bias>pcfDepth) shadow += 1.0;
	}
	shadow = shadow/float(shadowSamples);
	return shadow;
}

float PCSS(vec3 fragPos,vec3 normal,PointLight light){
	if(!light.useShadowMap) return 0.0;
	float receiverDistance = length(fragPos - light.position)/light.far_plane;
	float avgBlockerDepth = findBlocker(light.shadowCubeMap,normalize(fragPos-light.position),receiverDistance);
	if(avgBlockerDepth>=1.0) return 0.0;
	float penumbraSize = (receiverDistance - avgBlockerDepth)  / avgBlockerDepth ;
	return PCF(fragPos,penumbraSize,normal,light);
}

float ShadowCalculation(vec3 fragPos,PointLight light){
	if(!light.useShadowMap) return 0.0;
    vec3 fragToLight = fragPos - light.position;
    float closestDepth = texture(light.shadowCubeMap, fragToLight).r;
    closestDepth *= light.far_plane;
    float currentDepth = length(fragToLight);
    float bias = 0.05; 
    float shadow = currentDepth -  bias > closestDepth ? 1.0 : 0.0;

    return shadow;
}

void main()
{
	TexCoords = gl_FragCoord.xy / screenSize;

	float shadow = 0.0;
	
	vec3 color = texture(gAlbedoSpec,TexCoords).rgb;
	vec3 fragPos = texture(gPosition, TexCoords).rgb;
	vec3 normal = texture(gNormal, TexCoords).rgb;
	vec3 lightDir = normalize(pointLight.position - fragPos);
	vec3 viewDir = normalize(viewPos - fragPos);
	vec4 material = texture(gMaterial, TexCoords);
	// diffuse shading
	float diff = max(dot(normal, lightDir), 0.0);
	// specular shading
	vec3 halfwayDir = normalize(lightDir + viewDir);
	float spec = pow(max(dot(normal, halfwayDir), 0.0), material.a);
	// attenuation
	float distance = length(pointLight.position - fragPos);
	float attenuation = 1.0 / (pointLight.constant + pointLight.linear * distance + pointLight.quadratic * (distance * distance));
	// combine results
	vec3 ambient = pointLight.ambient * material.r;
	vec3 diffuse = pointLight.diffuse * material.g *diff;
	vec3 specular = pointLight.specular * material.b* spec;
	ambient *= attenuation;
	diffuse *= attenuation;
	specular *= attenuation;

	switch(shadowType){
		case DEFAULT_SHADOW:
			shadow = ShadowCalculation(fragPos,pointLight);
			break;
		case PCF_SHADOW:
			shadow = PCF(fragPos,0.1,normal,pointLight);
			break;
		case PCSS_SHADOW:
			shadow = PCSS(fragPos,normal,pointLight);
			break;
	}

	FragColor = vec4((ambient + (1.0-shadow)*(diffuse + specular)) * color,1.0);

	
}