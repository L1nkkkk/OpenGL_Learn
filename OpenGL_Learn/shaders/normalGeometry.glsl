#version 330 core

layout(triangles) in;
layout(line_strip, max_vertices = 6) out;

layout (std140) uniform Matrices{
	mat4 view;
	mat4 projection;
};

in VS_OUT {
	vec3 normal;
} gs_in[];

uniform float MAGNITUDE;


void GenerateLine(int index){
	gl_Position = projection * gl_in[index].gl_Position;
    EmitVertex();
    gl_Position = projection * (gl_in[index].gl_Position + 
                                vec4(gs_in[index].normal, 0.0) * MAGNITUDE);
    EmitVertex();
    EndPrimitive();
}

void main()
{
	GenerateLine(0); // 第一个顶点法线
    GenerateLine(1); // 第二个顶点法线
    GenerateLine(2); // 第三个顶点法线
}