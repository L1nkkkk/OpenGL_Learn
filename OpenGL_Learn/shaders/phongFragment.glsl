#version 330 core
layout (location = 0) out vec4 FragColor;
layout (location = 1) out vec4 BrightColor;
layout (location = 2) out vec4 NormalOut;

struct DirLight{
	vec3 direction;
	vec3 ambient;
	vec3 diffuse;
	vec3 specular;

	sampler2D shadowMap;
	bool useShadowMap;
	mat4 lightSpaceMatrix;

	bool isActive;
};

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

	bool isActive;
};

struct SpotLight{
	vec3 position;
	vec3 direction;
	float cutOff;
	float outerCutOff;
	float constant;
	float linear;
	float quadratic;
	vec3 ambient;
	vec3 diffuse;
	vec3 specular;

	bool isActive;
};

struct Material{
	vec3 ambient;           // Ka?????????????
    vec3 diffuse;           // Kd????????????
    vec3 specular;          // Ks??????X?????
    float shininess;        // Ns????????
    float opacity;          // d????????1=???????
	float alphaCutoff;
	bool useAlphaCutoff;
	sampler2D texture_diffuse1;
	bool use_texture_diffuse;
	sampler2D texture_normal1;
	bool use_texture_normal;
	sampler2D texture_specular1;
	bool use_texture_specular;
	bool hasBloom;
};

in VS_OUT {
	vec3 FragPos;
    vec3 Normal;
    vec2 TexCoords;
	mat3 TBN;
} fs_in;

uniform vec3 viewPos;

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

layout(std140) uniform Matrices{
    mat4 view;
    mat4 projection;
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

uniform int NR_POINT_LIGHTS;
uniform int NR_DIR_LIGHTS;
uniform int NR_SPOT_LIGHTS;
uniform PointLight pointLights[MAX_POINT_LIGHTS];
uniform DirLight dirLights[MAX_DIR_LIGHTS];
uniform SpotLight spotLights[MAX_SPOT_LIGHTS];


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
		return 0.0; // ??????????????????
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

float PCSS(vec3 fragPos,vec3 normal,PointLight light){
	if(!light.useShadowMap) return 0.0;
	float receiverDistance = length(fragPos - light.position)/light.far_plane;
	float avgBlockerDepth = findBlocker(light.shadowCubeMap,normalize(fragPos-light.position),receiverDistance);
	if(avgBlockerDepth>=1.0) return 0.0;
	float penumbraSize = (receiverDistance - avgBlockerDepth)  / avgBlockerDepth ;
	return PCF(fragPos,penumbraSize,normal,light);
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

vec3 CalcDirLight(DirLight light, vec3 normal, vec3 viewDir,float shadow)
{
	vec3 color = SampleColorTexture(material.texture_diffuse1, fs_in.TexCoords).rgb;
	//After this below, BUG
	
	vec3 lightDir = normalize(-light.direction);
	// diffuse shading
	float diff = max(dot(normal, lightDir), 0.0);
	// specular shading
	vec3 halfwayDir = normalize(lightDir + viewDir);
	float spec = pow(max(dot(normal, halfwayDir), 0.0), material.shininess);
	// combine results
	vec3 ambient = light.ambient * material.ambient;
	vec3 diffuse = light.diffuse * material.diffuse * diff;
	vec3 specular = light.specular * material.specular * spec;
	return (ambient + (1.0-shadow)*(diffuse + specular))* color;
}

vec3 CalcPointLight(PointLight light, vec3 normal, vec3 fragPos, vec3 viewDir,float shadow)
{
	vec3 color = SampleColorTexture(material.texture_diffuse1, fs_in.TexCoords).rgb;
	vec3 lightDir = normalize(light.position - fragPos);
	// diffuse shading
	float diff = max(dot(normal, lightDir), 0.0);
	// specular shading
	vec3 halfwayDir = normalize(lightDir + viewDir);
	float spec = pow(max(dot(normal, halfwayDir), 0.0), material.shininess);
	// attenuation
	float distance = length(light.position - fragPos);
	float attenuation = 1.0 / (light.constant + light.linear * distance + light.quadratic * (distance * distance));
	// combine results
	vec3 ambient = light.ambient * material.ambient;
	vec3 diffuse = light.diffuse * material.diffuse * diff;
	vec3 specular = light.specular * material.specular * spec;
	ambient *= attenuation;
	diffuse *= attenuation;
	specular *= attenuation;
	return (ambient + (1.0-shadow)*(diffuse + specular)) * color;
}

vec3 CalcSpotLight(SpotLight light, vec3 normal, vec3 fragPos, vec3 viewDir,float shadow)
{
	vec3 color = SampleColorTexture(material.texture_diffuse1, fs_in.TexCoords).rgb;
	vec3 lightDir = normalize(light.position - fragPos);
	// diffuse shading
	float diff = max(dot(normal, lightDir), 0.0);
	// specular shading
	vec3 reflectDir = reflect(-lightDir, normal);
	float spec = pow(max(dot(viewDir, reflectDir), 0.0),material.shininess);
	// attenuation
	float distance = length(light.position - fragPos);
	float attenuation = 1.0 / (light.constant + light.linear * distance + light.quadratic * (distance * distance));
	// spotlight intensity
	float theta = dot(lightDir, normalize(-light.direction));
	float epsilon = light.cutOff - light.outerCutOff;
	float intensity = clamp((theta - light.outerCutOff) / epsilon, 0.0, 1.0);
	// combine results
	vec3 ambient = light.ambient * material.ambient;
	vec3 diffuse = light.diffuse * material.diffuse * diff;
	vec3 specular = light.specular * material.specular * spec;
	ambient *= attenuation * intensity;
	diffuse *= attenuation * intensity;
	specular *= attenuation * intensity;
	return (ambient + (1.0-shadow)*(diffuse + specular))*color;
}

void main()
{
	vec3 norm;
	if(material.use_texture_normal){
		norm = texture(material.texture_normal1,fs_in.TexCoords).rgb;
		norm = normalize(norm * 2.0 - 1.0);  
		norm = normalize(fs_in.TBN * norm);
	}
	else
		norm = normalize(fs_in.Normal);
	vec3 viewDir = normalize(viewPos - fs_in.FragPos);
	float shadow = 0.0;
	
	vec3 results = vec3(0);
	float alpha = 1.0;
	if(material.use_texture_diffuse) alpha = SampleColorTexture(material.texture_diffuse1,fs_in.TexCoords).a;
	if (material.useAlphaCutoff && alpha < material.alphaCutoff) {
		discard;
	}
	// Write view-space normal into attachment2 for MRT debug/AO input.
	vec3 nVS = normalize(mat3(transpose(inverse(view))) * norm);
	NormalOut = vec4(nVS * 0.5 + 0.5, 1.0);
	int pointLightsNum = min(MAX_POINT_LIGHTS,NR_POINT_LIGHTS);
	int dirLightsNum =  min(MAX_DIR_LIGHTS,NR_DIR_LIGHTS);
	int spotLightsNum = min(MAX_SPOT_LIGHTS,NR_SPOT_LIGHTS);
	
	for(int i = 0;i<dirLightsNum;i++){
		vec4 FragPosLightSpace =  dirLights[i].lightSpaceMatrix * vec4(fs_in.FragPos,1.0);
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

	for(int i = 0;i<pointLightsNum;i++){
		switch(shadowType){
			case DEFAULT_SHADOW:
				shadow = ShadowCalculation(fs_in.FragPos,pointLights[i]);
				break;
			case PCF_SHADOW:
				shadow = PCF(fs_in.FragPos,0.1,norm,pointLights[i]);
				break;
			case PCSS_SHADOW:
				shadow = PCSS(fs_in.FragPos,norm,pointLights[i]);
				break;
		}

		results += CalcPointLight(pointLights[i], norm, fs_in.FragPos, viewDir,shadow);
	}

	for(int i = 0;i<spotLightsNum;i++){
		results += CalcSpotLight(spotLights[i], norm, fs_in.FragPos, viewDir,0);
	}
	FragColor = vec4(results, alpha);
	float brightness = dot(FragColor.rgb, vec3(0.2126, 0.7152, 0.0722));
    if(brightness > bloomThreshold)
        BrightColor = vec4(FragColor.rgb, 1.0);
	else BrightColor = vec4(0,0,0,1);
	return;
}
