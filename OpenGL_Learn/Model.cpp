#include "Model.h"
#include "XmlMaterialManager.h"

Mesh::Mesh(std::vector<Vertex> vertices,
		std::vector<unsigned int> indices,
		   Material* material,
           const std::string& materialXmlPath)
{
	this->vertices = ComputeTBNVertices(vertices,indices);
	this->material_ptr = material;
    this->materialXmlPath = materialXmlPath;
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

void Mesh::Draw()
{
	auto& properties = SystemProperties::GetInstance();
    Material* runtimeMat = material_ptr;

    // 如果为该 Mesh 指定了 XML 材质文件，则优先从 XmlMaterialManager 获取 / 热重载
    if (!materialXmlPath.empty()) {
        if (Material* xmlMat = XmlMaterialManager::GetInstance().GetOrLoadMaterialByFile(materialXmlPath)) {
            runtimeMat = xmlMat;
        }
    }

    if (!runtimeMat) {
        return;
    }

	auto materialGaurd = MaterialGaurd(*runtimeMat);
	glActiveTexture(GL_TEXTURE0);
	glBindVertexArray(VAO);
	glDrawArrays(GL_TRIANGLES, 0, vertices.size());
	glBindVertexArray(0);
}

void Model::Draw(Shader& shader, unsigned int start_tex_index )
{
	shader.use();
	shader.setMat4("model", getModelMatrix());
	auto& properties = SystemProperties::GetInstance();
	int usedTextures = properties.USED_TEXTURE_NUM;
	for (unsigned int i = 0; i < meshes.size(); ++i) {
		meshes[i].start_tex_index = start_tex_index;
		meshes[i].Draw();
		properties.USED_TEXTURE_NUM = usedTextures;
	}
}

void Model::loadModel(std::string path,Material* mat)
{
	Assimp::Importer importer;
	const aiScene* scene = importer.ReadFile(path, aiProcess_Triangulate  | aiProcess_CalcTangentSpace | aiProcess_FlipUVs);

	if(!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
		std::cout << "ERROR::ASSIMP:: " << importer.GetErrorString() << std::endl;
		return;
	}
	directory = path.substr(0, path.find_last_of('/'));
	processNode(scene->mRootNode, scene,mat);
}

void Model::processNode(aiNode* node, const aiScene* scene,Material* mat)
{
	for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
		aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
		meshes.push_back(processMesh(mesh, scene,mat));
	}
	for (unsigned int i = 0; i < node->mNumChildren; ++i) {
		processNode(node->mChildren[i], scene,mat);
	}
}

Mesh Model::processMesh(aiMesh* mesh, const aiScene* scene,Material* mat)
{
	std::vector<Vertex> vertices;
	std::vector<unsigned int>indices;
	Material* material = mat;
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
	std::string xmlPath;
	if(mat) {
		return Mesh(vertices, indices, mat);
	}
	else if (mesh->mMaterialIndex >= 0) {
		aiMaterial* aiMat = scene->mMaterials[mesh->mMaterialIndex];

		aiString aiName;
		if (aiMat->Get(AI_MATKEY_NAME, aiName) == AI_SUCCESS) {
            // 约定：一个材质对应一个 xml 文件，如 materials/Wood.xml
			material = new Material(m_shader->shaderName);
			prosessMaterial(aiMat, material);
		}

		if (!material) {
			xmlPath = "materials/";
			xmlPath += aiName.C_Str();
			xmlPath += ".xml";
			if (Material* xmlMat = XmlMaterialManager::GetInstance().GetOrLoadMaterialByFile(xmlPath)) {
				material = xmlMat;
			}
		}
	}
	else {
        // 没有关联 aiMaterial，则尝试使用一个默认 XML 材质（如 materials/Default.xml）
        xmlPath = "materials/Default.xml";
        if (Material* xmlMat = XmlMaterialManager::GetInstance().GetOrLoadMaterialByFile(xmlPath)) {
            material = xmlMat;
        } else {
		    material = new Material(m_shader->shaderName);
        }
	}
	return Mesh(vertices,indices, material, xmlPath);
}

void Model::prosessMaterial(aiMaterial* mat,Material* material)
{
	aiColor3D color(0.0f, 0.0f, 0.0f);
	float shininess = 0.0f;
	float opacity = 1.0f;
	if (mat->Get(AI_MATKEY_COLOR_AMBIENT, color) == AI_SUCCESS)
		material->AddProperty("ambient", MaterialProperty::CreateVec3(glm::vec3(color.r, color.g, color.b)));
	if (mat->Get(AI_MATKEY_COLOR_DIFFUSE, color) == AI_SUCCESS)
		material->AddProperty("diffuse", MaterialProperty::CreateVec3(glm::vec3(color.r, color.g, color.b)));
	if (mat->Get(AI_MATKEY_COLOR_SPECULAR, color) == AI_SUCCESS)
		material->AddProperty("specular", MaterialProperty::CreateVec3(glm::vec3(color.r, color.g, color.b)));
	if (mat->Get(AI_MATKEY_SHININESS, shininess) == AI_SUCCESS)
		material->AddProperty("shininess", MaterialProperty::CreateFloat(shininess));
	if (mat->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS)
		material->AddProperty("opacity", MaterialProperty::CreateFloat(opacity));
	
	material->AddProperty("texture_diffuse", MaterialProperty::CreateTexture(loadMaterialTextures(mat, aiTextureType_DIFFUSE, "texture_diffuse")));
	material->AddProperty("texture_specular", MaterialProperty::CreateTexture(loadMaterialTextures(mat, aiTextureType_SPECULAR, "texture_specular")));
	material->AddProperty("texture_normal", MaterialProperty::CreateTexture(loadMaterialTextures(mat, aiTextureType_NORMALS, "texture_normal")));
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
		{   // ???????????��?????????????
			Texture texture;
			texture.textureID = TextureFromFile(str.C_Str(), directory,false,false);
			texture.textureGammaID = TextureFromFile(str.C_Str(), directory, false, true);
			texture.type = typeName;
			texture.path = str.C_Str();
			textures.push_back(texture);
			textures_loaded.push_back(texture); // ?????????????????
		}
	}
	return textures;
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