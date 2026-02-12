#include "Model.h"

Mesh::Mesh(std::vector<Vertex> vertices,
		std::vector<unsigned int> indices,
		   Material& material)
{
	this->vertices = ComputeTBNVertices(vertices,indices);
	this->material = material;
	this->start_tex_index = 0;
	setupMesh();
}

void Mesh::setupMesh()
{
	glGenVertexArrays(1, &VAO);
	glGenBuffers(1, &VBO);
	glGenBuffers(1, &EBO);

	glBindVertexArray(VAO);
	glBindBuffer(GL_ARRAY_BUFFER, VBO);
	glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), &vertices[0], GL_STATIC_DRAW);

	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Normal));
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, TexCoords));
	glEnableVertexAttribArray(3);
	glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Tangent));
	glEnableVertexAttribArray(4);
	glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Bitangent));
	glBindVertexArray(0);
}

void Mesh::Draw(Shader& shader)
{
	auto& properties = SystemProperties::GetInstance();
	shader.setVec3("material.ambient", material.ambient);
	shader.setVec3("material.diffuse", material.diffuse);
	shader.setVec3("material.specular", material.specular);
	shader.setFloat("material.shininess", material.shininess);
	shader.setFloat("material.opacity", material.opacity);

	shader.setBool("hasDiffuseMap", !material.diffuseTextures.empty());
	shader.setBool("hasSpecularMap", !material.specularTextures.empty());
	shader.setBool("hasNormalMap", !material.normalTextures.empty());
	unsigned int diffuseNr = 1;
	unsigned int specularNr = 1;
	unsigned int normalNr = 1;
	for (unsigned int i = 0; i < material.diffuseTextures.size(); ++i) {
		glActiveTexture(GL_TEXTURE0 + properties.USED_TEXTURE_NUM);
		std::string number;
		number = std::to_string(diffuseNr++);
		if(properties.GAMMA_CORRECTION)
			glBindTexture(GL_TEXTURE_2D, material.diffuseTextures[i].textureGammaID);
		else
			glBindTexture(GL_TEXTURE_2D, material.diffuseTextures[i].textureID);
		shader.setInt(("texture_diffuse" + number).c_str(), properties.USED_TEXTURE_NUM++);
		shader.setBool("hasDiffuseMap", true);
	}

	for (unsigned int i = 0; i < material.specularTextures.size(); ++i) {
		glActiveTexture(GL_TEXTURE0 + properties.USED_TEXTURE_NUM);
		std::string number;
		number = std::to_string(specularNr++);
		glBindTexture(GL_TEXTURE_2D, material.specularTextures[i].textureID);
		shader.setInt(("texture_specular" + number).c_str(), properties.USED_TEXTURE_NUM++);
		shader.setBool("hasSpecularMap", true);
	}

	for (unsigned int i = 0; i < material.normalTextures.size(); ++i) {
		glActiveTexture(GL_TEXTURE0 + properties.USED_TEXTURE_NUM);
		std::string number = std::to_string(normalNr++);
		glBindTexture(GL_TEXTURE_2D, material.normalTextures[i].textureID);
		shader.setInt(("texture_normal" + number).c_str(), properties.USED_TEXTURE_NUM++);
		shader.setBool("hasNormalMap", true);
	}

	glActiveTexture(GL_TEXTURE0);
	glBindVertexArray(VAO);
	glDrawArrays(GL_TRIANGLES, 0, vertices.size());
	glBindVertexArray(0);
}

void Model::Draw(Shader& shader, unsigned int start_tex_index )
{
	for (unsigned int i = 0; i < meshes.size(); ++i) {
		meshes[i].start_tex_index = start_tex_index;
		meshes[i].Draw(shader);
	}
}

void Model::loadModel(std::string path)
{
	Assimp::Importer importer;
	const aiScene* scene = importer.ReadFile(path, aiProcess_Triangulate  | aiProcess_CalcTangentSpace | aiProcess_FlipUVs);

	if(!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
		std::cout << "ERROR::ASSIMP:: " << importer.GetErrorString() << std::endl;
		return;
	}
	directory = path.substr(0, path.find_last_of('/'));
	processNode(scene->mRootNode, scene);
}

void Model::processNode(aiNode* node, const aiScene* scene)
{
	for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
		aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
		meshes.push_back(processMesh(mesh, scene));
	}
	for (unsigned int i = 0; i < node->mNumChildren; ++i) {
		processNode(node->mChildren[i], scene);
	}
}

Mesh Model::processMesh(aiMesh* mesh, const aiScene* scene)
{
	std::vector<Vertex> vertices;
	std::vector<unsigned int>indices;
	Material material;
	for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
		Vertex vertex;
		glm::vec3 vector;
		vector.x = mesh->mVertices[i].x;
		vector.y = mesh->mVertices[i].y;
		vector.z = mesh->mVertices[i].z;
		vertex.Position = vector;
		vector.x = mesh->mNormals[i].x;
		vector.y = mesh->mNormals[i].y;
		vector.z = mesh->mNormals[i].z;
		vertex.Normal = vector;
		if (mesh->mTextureCoords[0]) {
			glm::vec2 vec;
			vec.x = mesh->mTextureCoords[0][i].x;
			vec.y = mesh->mTextureCoords[0][i].y;
			vertex.TexCoords = vec;
		}
		else {
			vertex.TexCoords = glm::vec2(0.0f, 0.0f);
		}
		vertices.push_back(vertex);
	}
	for (unsigned int i = 0; i < mesh->mNumFaces; ++i) {
		aiFace face = mesh->mFaces[i];
		for (unsigned int j = 0; j < face.mNumIndices; ++j) {
			indices.push_back(face.mIndices[j]);
		}
	}
	if (mesh->mMaterialIndex >= 0) {
		material = prosessMaterial(scene->mMaterials[mesh->mMaterialIndex]);
	}
	return Mesh(vertices,indices, material);
}

Material Model::prosessMaterial(aiMaterial* mat)
{
	Material material;
	aiColor3D color(0.0f, 0.0f, 0.0f);
	float shininess = 0.0f;
	float opacity = 1.0f;
	if (mat->Get(AI_MATKEY_COLOR_AMBIENT, color) == AI_SUCCESS)
		material.ambient = glm::vec3(color.r, color.g, color.b);
	if (mat->Get(AI_MATKEY_COLOR_DIFFUSE, color) == AI_SUCCESS)
		material.diffuse = glm::vec3(color.r, color.g, color.b);
	if (mat->Get(AI_MATKEY_COLOR_SPECULAR, color) == AI_SUCCESS)
		material.specular = glm::vec3(color.r, color.g, color.b);
	if (mat->Get(AI_MATKEY_SHININESS, shininess) == AI_SUCCESS)
		material.shininess = shininess;
	if (mat->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS)
		material.opacity = opacity;
	
	material.diffuseTextures = loadMaterialTextures(mat, aiTextureType_DIFFUSE, "texture_diffuse");
	material.specularTextures = loadMaterialTextures(mat, aiTextureType_SPECULAR, "texture_specular");
	material.normalTextures = loadMaterialTextures(mat, aiTextureType_NORMALS, "texture_normal");
	
	return material;
}

std::vector<Texture> Model::loadMaterialTextures(aiMaterial* mat, aiTextureType type, std::string typeName)
{
	std::vector<Texture> textures;
	for (unsigned int i = 0; i < mat->GetTextureCount(type); i++)
	{
		aiString str;
		mat->GetTexture(type, i, &str);
		bool skip = false;
		for (unsigned int j = 0; j < textures_loaded.size(); j++)
		{

			if (std::strcmp(textures_loaded[j].path.C_Str(), str.C_Str()) == 0)
			{
				textures.push_back(textures_loaded[j]);
				skip = true;
				break;
			}
		}
		if (!skip)
		{   // 如果纹理还没有被加载，则加载它
			Texture texture;
			texture.textureID = TextureFromFile(str.C_Str(), directory,false,false);
			texture.textureGammaID = TextureFromFile(str.C_Str(), directory, false, true);
			texture.type = typeName;
			texture.path = str.C_Str();
			textures.push_back(texture);
			textures_loaded.push_back(texture); // 添加到已加载的纹理中
		}
	}
	return textures;
}

unsigned int TextureFromFile(const char* path, const std::string& directory,bool alpha ,bool gamma)
{
	std::string filename = std::string(path);
	filename = directory + '/' + filename;

	unsigned int textureID;
	glGenTextures(1, &textureID);
	stbi_set_flip_vertically_on_load(true);
	int width, height, nrComponents;
	unsigned char* data = stbi_load(filename.c_str(), &width, &height, &nrComponents, 0);
	if (data)
	{
		GLenum format;
		GLenum internalFormat;
		if (nrComponents == 1) {
			internalFormat = format = GL_RED;
		}
		else if (nrComponents == 2) {
			internalFormat = format = GL_RG;
		}
		else if (nrComponents == 3) {
			internalFormat = gamma ? GL_SRGB : GL_RGB;
			format = GL_RGB;
		}
		else if (nrComponents == 4) {
			internalFormat = gamma ? GL_SRGB_ALPHA : GL_RGBA;
			format = GL_RGBA;
		}
		glBindTexture(GL_TEXTURE_2D, textureID);
		glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, GL_UNSIGNED_BYTE, data);
		glGenerateMipmap(GL_TEXTURE_2D);

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

		if (alpha) {
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		}
		stbi_image_free(data);
	}
	else
	{
		std::cout << "Texture failed to load at path: " << path << std::endl;
		stbi_image_free(data);
	}

	return textureID;
}

glm::vec3 Model::CalculateLocalCenter()
{
	bool first = true;
	float minX, maxX, minY, maxY, minZ, maxZ;
	for (auto& mesh : meshes) {
		for (auto& vertice : mesh.vertices) {
			glm::vec3& pos = vertice.Position;
			if (first) {
				first = false;
				minX = pos.x; maxX = pos.x;
				minY = pos.y; maxY = pos.y;
				minZ = pos.z; maxZ = pos.z;
			}
			else {
				minX = std::min(minX, pos.x);
				maxX = std::max(maxX, pos.x);

				minY = std::min(minY, pos.y);
				maxY = std::max(maxY, pos.y);

				minZ = std::min(minZ, pos.z);
				maxZ = std::max(maxZ, pos.z);
			}
		}
	}
	return glm::vec3((minX+maxX)/2.f, (minY+maxY)/2.f, (minZ+maxZ) / 2.f);
}

std::vector<Vertex> ComputeTBNVertices(std::vector<Vertex>& vertices,std::vector<unsigned int> indices) {
	std::vector<Vertex> ret;
	if (indices.size() % 3) {
		std::cout << "this model has a non-multiple-of-three number of points" << std::endl;
		return ret;
	}
	for (int i = 0; i < indices.size(); i += 3) {
		auto aPoint = vertices[indices[i]];
		auto bPoint = vertices[indices[i + 1]];
		auto cPoint = vertices[indices[i + 2]];
		ComputeTBN(aPoint,bPoint,cPoint);
		ret.push_back(aPoint);
		ret.push_back(bPoint);
		ret.push_back(cPoint);
	}
	return ret;
}

void ComputeTBN(Vertex& aPoint, Vertex& bPoint, Vertex& cPoint) {
	auto edge1 = bPoint.Position - aPoint.Position;
	auto edge2 = cPoint.Position - aPoint.Position;
	auto deltaUV1 = bPoint.TexCoords - aPoint.TexCoords;
	auto deltaUV2 = cPoint.TexCoords - aPoint.TexCoords;

	GLfloat f = 1.0f / (deltaUV1.x * deltaUV2.y - deltaUV2.x * deltaUV1.y);
	glm::vec3 tangent, bitangent;
	tangent.x = f * (deltaUV2.y * edge1.x - deltaUV1.y * edge2.x);
	tangent.y = f * (deltaUV2.y * edge1.y - deltaUV1.y * edge2.y);
	tangent.z = f * (deltaUV2.y * edge1.z - deltaUV1.y * edge2.z);
	tangent = glm::normalize(tangent);

	bitangent.x = f * (-deltaUV2.x * edge1.x + deltaUV1.x * edge2.x);
	bitangent.y = f * (-deltaUV2.x * edge1.y + deltaUV1.x * edge2.y);
	bitangent.z = f * (-deltaUV2.x * edge1.z + deltaUV1.x * edge2.z);
	bitangent = glm::normalize(bitangent - glm::dot(bitangent,tangent)*tangent);

	aPoint.Tangent = tangent;
	bPoint.Tangent = tangent;
	cPoint.Tangent = tangent;
	aPoint.Bitangent = bitangent;
	bPoint.Bitangent = bitangent;
	cPoint.Bitangent = bitangent;
	
	return;
}