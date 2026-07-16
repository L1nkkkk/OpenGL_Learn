#version 330 core

layout (location = 0) out vec4 NormalOut;

in VS_OUT {
    vec3 normalVS;
    vec2 TexCoords;
} fs_in;

struct Material{
    vec3 ambient;
    vec3 diffuse;
    vec3 specular;
    float shininess;
    float opacity;
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

uniform Material material;

void main()
{
    // Match alpha-cutout behavior from phongFragment.glsl so normal/depth remain consistent.
    if (material.useAlphaCutoff && material.use_texture_diffuse) {
        float alpha = texture(material.texture_diffuse1, fs_in.TexCoords).a;
        if (alpha < material.alphaCutoff) discard;
    }

    vec3 n = normalize(fs_in.normalVS);
    // Encode normal to 0..1 so it can be stored in a standard floating texture.
    NormalOut = vec4(n * 0.5 + 0.5, 1.0);
}
