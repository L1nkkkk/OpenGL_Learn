#version 330 core
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;

in VS_OUT {
	vec2 texCoords;
} gs_in[];

out vec2 TexCoords;

void main()
{

}