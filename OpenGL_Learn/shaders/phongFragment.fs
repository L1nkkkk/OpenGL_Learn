#version 330 core

struct DirLight{
	vec3 direction;
	vec3 ambient;
	vec3 diffuse;
	vec3 specular;
};

struct PointLight{
	vec3 position;

	float constant;
	float linear;
	float quadratic;

	vec3 ambient;
	vec3 diffuse;
	vec3 specular;
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
};

struct Material{
	vec3 ambient;           // Ka：环境光反射系数
    vec3 diffuse;           // Kd：漫反射基础色
    vec3 specular;          // Ks：镜面反射系数
    float shininess;        // Ns：高光指数
    float opacity;          // d：透明度（1=不透明）
};

in VS_OUT {
	vec3 FragPos;
    vec3 Normal;
    vec2 TexCoords;
    vec4 FragPosLightSpace;
} fs_in;

out vec4 FragColor;

uniform vec3 viewPos;
uniform sampler2D texture_diffuse1;
uniform sampler2D texture_specular1;

uniform Material material;

uniform bool useShadowMap;
uniform sampler2D shadowMap1;


#define MAX_POINT_LIGHTS 16
#define MAX_DIR_LIGHTS 16
#define MAX_SPOT_LIGHTS 16

uniform int NR_POINT_LIGHTS;
uniform int NR_DIR_LIGHTS;
uniform int NR_SPOT_LIGHTS;
uniform PointLight pointLights[MAX_POINT_LIGHTS];
uniform DirLight dirLights[MAX_DIR_LIGHTS];
uniform SpotLight spotLights[MAX_SPOT_LIGHTS];

float ShadowCalculation(vec4 fragPosLightSpace,vec3 normal,vec3 lightDir){
	if(!useShadowMap) return 0.0;
	float bias = max(0.05 * (1.0 - dot(normal, lightDir)), 0.005);
	vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
	projCoords = projCoords * 0.5 + 0.5;
	float closestDepth = texture(shadowMap1, projCoords.xy).r;
	if(projCoords.z > 1.0) {
        return 0.0;
    }
	float currentDepth = projCoords.z;
	return currentDepth - bias > closestDepth ? 1.0 : 0.0;
}

vec3 CalcDirLight(DirLight light, vec3 normal, vec3 viewDir,float shadow)
{
	vec3 color = texture(texture_diffuse1, fs_in.TexCoords).rgb;
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
	return (ambient + (1.0-shadow)*(diffuse + specular))*color;
}

vec3 CalcPointLight(PointLight light, vec3 normal, vec3 fragPos, vec3 viewDir,float shadow)
{
	vec3 color = texture(texture_diffuse1, fs_in.TexCoords).rgb;
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
	return (ambient + (1.0-shadow)*(diffuse + specular))*color;
}

vec3 CalcSpotLight(SpotLight light, vec3 normal, vec3 fragPos, vec3 viewDir,float shadow)
{
	vec3 color = texture(texture_diffuse1, fs_in.TexCoords).rgb;
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
	vec3 norm = normalize(fs_in.Normal);
	vec3 viewDir = normalize(viewPos - fs_in.FragPos);
	float shadow;
	
	vec3 results = vec3(0);

	int pointLightsNum = min(MAX_POINT_LIGHTS,NR_POINT_LIGHTS);
	int dirLightsNum =  min(MAX_DIR_LIGHTS,NR_DIR_LIGHTS);
	int spotLightsNum = min(MAX_SPOT_LIGHTS,NR_SPOT_LIGHTS);

	for(int i = 0;i<dirLightsNum;i++){
		shadow = ShadowCalculation(fs_in.FragPosLightSpace,norm,normalize(-dirLights[i].direction));
		results += CalcDirLight(dirLights[i], norm, viewDir,shadow);
	}

	for(int i = 0;i<pointLightsNum;i++){
		results += CalcPointLight(pointLights[i], norm, fs_in.FragPos, viewDir,0);
	}

	for(int i = 0;i<spotLightsNum;i++){
		results += CalcSpotLight(spotLights[i], norm, fs_in.FragPos, viewDir,0);
	}       
	FragColor = vec4(results, 1.0);
}