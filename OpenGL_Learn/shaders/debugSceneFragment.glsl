#version 330 core
out vec4 FragColor;

in vec2 TexCoords;

uniform float div;
uniform sampler2D screenTexture[9];
 

void main()
{
    int x = int(TexCoords.x * div);
    int y = int(TexCoords.y * div);
    FragColor = texture(screenTexture[y * int(div) + x], TexCoords * div - vec2(float(x),float(y)));
}