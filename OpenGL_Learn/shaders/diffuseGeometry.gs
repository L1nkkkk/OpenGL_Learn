#version 330 core
layout(triangles) in;
layout(triangle_strip,max_vertices = 3) out;

in VS_OUT {
	vec3 Normal;
	vec2 TexCoords;
} gs_in[];

out GS_OUT {
	vec2 TexCoords;
} gs_out;

void passToFragment(int i){
	gs_out.TexCoords = gs_in[i].TexCoords;
	EmitVertex();
}

void main(){
	passToFragment(0);
	passToFragment(1);
	passToFragment(2);
	EndPrimitive();
}