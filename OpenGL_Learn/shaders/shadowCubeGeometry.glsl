#version 330 core

layout (triangles) in;
layout (triangle_strip,max_vertices = 18) out;

uniform mat4 shadowMatrices[6];

out vec4 FragPos;
out vec2 TexCoords;
in vec2 VertexTexCoords[];

void main(){
    for(int face = 0;face<6;++face){
        for(int i = 0;i<3;++i){
            // Geometry outputs become undefined after EmitVertex in GLSL
            // 3.30, so the destination cubemap layer must be written for
            // every emitted vertex.
            gl_Layer = face;
            FragPos = gl_in[i].gl_Position;
            TexCoords = VertexTexCoords[i];
            gl_Position = shadowMatrices[face]*FragPos;
            EmitVertex();
        }
        EndPrimitive();
    }
}
