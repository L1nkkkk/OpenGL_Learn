#include "SceneStateIO.h"

#include <fstream>
#include <direct.h>
#include <queue>

#include "../assimp/contrib/rapidjson/include/rapidjson/document.h"
#include "../assimp/contrib/rapidjson/include/rapidjson/prettywriter.h"
#include "../assimp/contrib/rapidjson/include/rapidjson/stringbuffer.h"
#include "ShaderManager.h"

using rapidjson::Document;
using rapidjson::Value;

namespace {
	std::queue<std::string> g_pendingModelJsonQueue;
	int g_totalAsyncModelCount = 0;

	std::string SerializeJsonValue(const Value& v) {
		rapidjson::StringBuffer sb;
		rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(sb);
		v.Accept(writer);
		return sb.GetString();
	}

	void SetVec3(Value& out, rapidjson::Document::AllocatorType& alloc, const glm::vec3& v) {
		out.SetArray();
		out.PushBack(v.x, alloc).PushBack(v.y, alloc).PushBack(v.z, alloc);
	}

	glm::vec3 GetVec3(const Value& in, const glm::vec3& fallback = glm::vec3(0.0f)) {
		if (!in.IsArray() || in.Size() < 3) return fallback;
		if (!in[0].IsNumber() || !in[1].IsNumber() || !in[2].IsNumber()) return fallback;
		return glm::vec3(in[0].GetFloat(), in[1].GetFloat(), in[2].GetFloat());
	}

	Model* FindModelByName(Scene& scene, const std::string& name) {
		for (auto& m : scene.modelSource.models) {
			if (!m) continue;
			if (m->GetName() == name) return m.get();
		}
		return nullptr;
	}

	void RemoveModelByName(Scene& scene, const std::string& name) {
		scene.modelSource.models.erase(
			std::remove_if(scene.modelSource.models.begin(), scene.modelSource.models.end(),
				[&](const std::shared_ptr<Model>& m) { return m && m->GetName() == name; }),
			scene.modelSource.models.end());
	}

	std::shared_ptr<Model> CreateGeneratedModelById(const std::string& generatorId) {
		// 生成模型逻辑与 ModelsLoader 保持一致（只负责把 mesh + materialXmlPath 组合起来）。
		if (generatorId == "transparent_window_quad") {
			std::vector<Vertex> grassVertices;
			grassVertices.emplace_back(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(0.0f, 0.0f));
			grassVertices.emplace_back(glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(1.0f, 0.0f));
			grassVertices.emplace_back(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(0.0f, 1.0f));
			grassVertices.emplace_back(glm::vec3(1.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(1.0f, 1.0f));

			std::vector<unsigned int> grassIndices = {
				0, 1, 2,
				1, 3, 2
			};

			std::vector<Mesh> grassMeshes;
			grassMeshes.emplace_back(grassVertices, grassIndices, nullptr, "materials/transparent_window/transparent_window.xml");

			auto model = std::make_shared<Model>(grassMeshes);
			model->SetDataSourceGenerated(generatorId);
			return model;
		}
		else if (generatorId == "plane_quad") {
			std::vector<Vertex> planeVertices;
			planeVertices.emplace_back(glm::vec3(-5.0f, 0.0f, -5.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(0.0f, 0.0f));
			planeVertices.emplace_back(glm::vec3(5.0f, 0.0f, -5.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(2.0f, 0.0f));
			planeVertices.emplace_back(glm::vec3(-5.0f, 0.0f, 5.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(0.0f, 2.0f));
			planeVertices.emplace_back(glm::vec3(5.0f, 0.0f, 5.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(2.0f, 2.0f));
			std::vector<unsigned int> planeIndices = {
				0, 1, 2,
				1, 3, 2
			};

			std::vector<Mesh> planeMeshes;
			planeMeshes.emplace_back(planeVertices, planeIndices, nullptr, "materials/brickwall/brickwall.xml");

			auto model = std::make_shared<Model>(planeMeshes);
			model->SetDataSourceGenerated(generatorId);
			return model;
		}
		else if (generatorId == "wall_quad") {
			std::vector<Vertex> wallVertices;
			wallVertices.emplace_back(glm::vec3(0.0f, 0.0f, -5.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(0.0f, 0.0f));
			wallVertices.emplace_back(glm::vec3(5.0f, 0.0f, -5.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(1.0f, 0.0f));
			wallVertices.emplace_back(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(0.0f, 2.0f));
			wallVertices.emplace_back(glm::vec3(5.0f, 0.0f, 5.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(1.0f, 2.0f));
			std::vector<unsigned int> wallIndices = {
				0, 1, 2,
				1, 3, 2
			};

			std::vector<Mesh> wallMeshes;
			wallMeshes.emplace_back(wallVertices, wallIndices, nullptr, "materials/brickwall/brickwall.xml");

			auto model = std::make_shared<Model>(wallMeshes);
			model->SetDataSourceGenerated(generatorId);
			return model;
		}

		return nullptr;
	}

	const char* PropTypeName(MaterialPropertyType t) {
		switch (t) {
		case MaterialPropertyType::Float: return "float";
		case MaterialPropertyType::Int: return "int";
		case MaterialPropertyType::Bool: return "bool";
		case MaterialPropertyType::Vec2: return "vec2";
		case MaterialPropertyType::Vec3: return "vec3";
		case MaterialPropertyType::Vec4: return "vec4";
		case MaterialPropertyType::Color: return "color";
		default: return "unsupported";
		}
	}

	MaterialPropertyType PropTypeFromName(const std::string& s) {
		if (s == "float") return MaterialPropertyType::Float;
		if (s == "int") return MaterialPropertyType::Int;
		if (s == "bool") return MaterialPropertyType::Bool;
		if (s == "vec2") return MaterialPropertyType::Vec2;
		if (s == "vec3") return MaterialPropertyType::Vec3;
		if (s == "vec4") return MaterialPropertyType::Vec4;
		if (s == "color") return MaterialPropertyType::Color;
		return MaterialPropertyType::Int;
	}

	void ApplyModelEntry(Scene& scene, const Value& m) {
		if (!m.IsObject() || !m.HasMember("name") || !m["name"].IsString()) return;
		const std::string modelName = m["name"].GetString();

		Model* model = nullptr;
		std::shared_ptr<Model> createdModel;

		if (m.HasMember("source") && m["source"].IsObject()) {
			const auto& src = m["source"];
			if (src.HasMember("type") && src["type"].IsString()) {
				const std::string srcType = src["type"].GetString();
				Model* existing = FindModelByName(scene, modelName);
				if (srcType == "file") {
					if (src.HasMember("path") && src["path"].IsString()) {
						const std::string modelPath = src["path"].GetString();
						if (existing &&
							existing->GetDataSourceType() == Model::DataSourceType::File &&
							existing->GetDataSourceFilePath() == modelPath) {
							model = existing;
						}
						else {
							std::string shaderName = "phong";
							if (m.HasMember("shader") && m["shader"].IsString()) shaderName = m["shader"].GetString();
							auto shaderPtr = ShaderManager::GetInstance().GetShaderByName(shaderName);
							if (!shaderPtr) shaderPtr = ShaderManager::GetInstance().GetShader(ShaderManager::Phong);
							createdModel = std::make_shared<Model>(modelPath, shaderPtr);
						}
					}
				}
				else if (srcType == "generated") {
					if (src.HasMember("generator") && src["generator"].IsString()) {
						const std::string generatorId = src["generator"].GetString();
						if (existing &&
							existing->GetDataSourceType() == Model::DataSourceType::Generated &&
							existing->GetDataSourceGeneratorId() == generatorId) {
							model = existing;
						}
						else {
							createdModel = CreateGeneratedModelById(generatorId);
							if (createdModel) createdModel->SetDataSourceGenerated(generatorId);
						}
					}
				}
			}
		}

		if (createdModel) {
			createdModel->SetName(modelName);
			RemoveModelByName(scene, modelName);
			scene.modelSource.AddModel(createdModel);
			model = createdModel.get();
		}
		else if (!model) {
			model = FindModelByName(scene, modelName);
			if (!model) return;
		}

		if (m.HasMember("active") && m["active"].IsBool()) model->SetActiveStatus(m["active"].GetBool());
		if (m.HasMember("position")) model->SetPosition(GetVec3(m["position"], model->position));
		if (m.HasMember("rotation")) model->SetRotation(GetVec3(m["rotation"], model->rotation));
		if (m.HasMember("scale")) model->SetScale(GetVec3(m["scale"], model->scale));

		if (m.HasMember("shader") && m["shader"].IsString()) {
			const std::string shaderName = m["shader"].GetString();
			auto shaderPtr = ShaderManager::GetInstance().GetShaderByName(shaderName);
			if (!shaderPtr) shaderPtr = ShaderManager::GetInstance().GetShader(ShaderManager::Phong);
			model->SetShader(shaderPtr);
		}

		if (m.HasMember("otherShaders") && m["otherShaders"].IsObject()) {
			const auto& os = m["otherShaders"];
			model->AddOtherShader(OtherShaderType::outline, ShaderManager::GetInstance().GetShader(ShaderManager::Outline));
			model->AddOtherShader(OtherShaderType::normalLines, ShaderManager::GetInstance().GetShader(ShaderManager::NormalLines));
			if (os.HasMember("outline") && os["outline"].IsBool()) model->otherShaderUse[static_cast<int>(OtherShaderType::outline)] = os["outline"].GetBool();
			if (os.HasMember("normalLines") && os["normalLines"].IsBool()) model->otherShaderUse[static_cast<int>(OtherShaderType::normalLines)] = os["normalLines"].GetBool();
		}

		if (m.HasMember("outlineWidth") && m["outlineWidth"].IsNumber()) model->outlineWidth = m["outlineWidth"].GetFloat();
		if (m.HasMember("outlineColor") && m["outlineColor"].IsArray()) model->outlineColor = GetVec3(m["outlineColor"], model->outlineColor);

		if (!m.HasMember("meshMaterials") || !m["meshMaterials"].IsArray()) return;
		auto& meshes = model->GetMeshes();
		for (const auto& mm : m["meshMaterials"].GetArray()) {
			if (!mm.IsObject() || !mm.HasMember("meshIndex") || !mm["meshIndex"].IsInt()) continue;
			int idx = mm["meshIndex"].GetInt();
			if (idx < 0 || idx >= static_cast<int>(meshes.size())) continue;
			Material* mat = meshes[static_cast<size_t>(idx)].material_ptr;
			if (!mat) continue;
			if (mm.HasMember("renderState") && mm["renderState"].IsObject()) {
				const auto& rsIn = mm["renderState"];
				RenderState rs = mat->GetRenderState();
				if (rsIn.HasMember("depthTest")) rs.depthTest = rsIn["depthTest"].GetBool();
				if (rsIn.HasMember("depthWrite")) rs.depthWrite = rsIn["depthWrite"].GetBool();
				if (rsIn.HasMember("stencilTest")) rs.stencilTest = rsIn["stencilTest"].GetBool();
				if (rsIn.HasMember("blendMode")) rs.blendMode = static_cast<BlendMode>(rsIn["blendMode"].GetInt());
				if (rsIn.HasMember("cullMode")) rs.cullMode = static_cast<CullMode>(rsIn["cullMode"].GetInt());
				mat->SetRenderState(rs);
			}
			if (mm.HasMember("properties") && mm["properties"].IsObject()) {
				for (auto it = mm["properties"].MemberBegin(); it != mm["properties"].MemberEnd(); ++it) {
					if (!it->name.IsString() || !it->value.IsObject()) continue;
					const auto& po = it->value;
					if (!po.HasMember("type") || !po["type"].IsString()) continue;
					MaterialProperty p;
					p.type = PropTypeFromName(po["type"].GetString());
					if (po.HasMember("min") && po["min"].IsNumber()) p.minVal = po["min"].GetFloat();
					if (po.HasMember("max") && po["max"].IsNumber()) p.maxVal = po["max"].GetFloat();
					if (po.HasMember("step") && po["step"].IsNumber()) p.step = po["step"].GetFloat();
					if (po.HasMember("value")) {
						const auto& v = po["value"];
						switch (p.type) {
						case MaterialPropertyType::Float: if (v.IsNumber()) p.scalarValue.floatValue = v.GetFloat(); break;
						case MaterialPropertyType::Int: if (v.IsInt()) p.scalarValue.intValue = v.GetInt(); break;
						case MaterialPropertyType::Bool: if (v.IsBool()) p.scalarValue.boolValue = v.GetBool(); break;
						case MaterialPropertyType::Vec2: if (v.IsArray() && v.Size() >= 2) p.vec2Value = glm::vec2(v[0].GetFloat(), v[1].GetFloat()); break;
						case MaterialPropertyType::Vec3:
						case MaterialPropertyType::Color: if (v.IsArray() && v.Size() >= 3) p.vec3Value = glm::vec3(v[0].GetFloat(), v[1].GetFloat(), v[2].GetFloat()); break;
						case MaterialPropertyType::Vec4: if (v.IsArray() && v.Size() >= 4) p.vec4Value = glm::vec4(v[0].GetFloat(), v[1].GetFloat(), v[2].GetFloat(), v[3].GetFloat()); break;
						default: break;
						}
					}
					mat->AddProperty(it->name.GetString(), p);
				}
			}
		}
	}
}

bool SceneStateIO::Exists(const std::string& path) {
	std::ifstream ifs(path);
	return ifs.good();
}

bool SceneStateIO::Save(const Scene& scene, const Camera& camera, const std::string& path) {
	Document doc;
	doc.SetObject();
	auto& alloc = doc.GetAllocator();

	doc.AddMember("version", 2, alloc);

	Value cameraObj(rapidjson::kObjectType);
	Value cpos, cfront, cup;
	SetVec3(cpos, alloc, camera.cameraPos);
	SetVec3(cfront, alloc, camera.cameraFront);
	SetVec3(cup, alloc, camera.up);
	cameraObj.AddMember("position", cpos, alloc);
	cameraObj.AddMember("front", cfront, alloc);
	cameraObj.AddMember("up", cup, alloc);
	cameraObj.AddMember("fov", camera.fov, alloc);
	doc.AddMember("camera", cameraObj, alloc);

	Value models(rapidjson::kArrayType);
	for (const auto& m : scene.modelSource.models) {
		if (!m) continue;
		Value one(rapidjson::kObjectType);
		one.AddMember("name", Value(m->GetName().c_str(), alloc), alloc);
		one.AddMember("active", m->GetAcitveStatus(), alloc);

		// source：用于区分“文件加载模型”与“程序生成模型”，从而不依赖 ModelLoader
		Value sourceObj(rapidjson::kObjectType);
		if (m->GetDataSourceType() == Model::DataSourceType::File) {
			sourceObj.AddMember("type", Value("file", alloc), alloc);
			sourceObj.AddMember("path", Value(m->GetDataSourceFilePath().c_str(), alloc), alloc);
		} else {
			sourceObj.AddMember("type", Value("generated", alloc), alloc);
			sourceObj.AddMember("generator", Value(m->GetDataSourceGeneratorId().c_str(), alloc), alloc);
		}
		one.AddMember("source", sourceObj, alloc);

		// 主 shader（Model::m_shader 依赖它；否则 Model::Draw(shader==nullptr) 会失败）
		std::string shaderName = "phong";
		if (auto shaderPtr = m->GetShader()) {
			if (!shaderPtr->shaderName.empty()) shaderName = shaderPtr->shaderName;
		}
		one.AddMember("shader", Value(shaderName.c_str(), alloc), alloc);

		// outline/normalLines 开关 + 描边参数
		Value otherShaders(rapidjson::kObjectType);
		bool outlineUsed = false;
		bool normalLinesUsed = false;
		{
			auto it = m->otherShaderUse.find(static_cast<int>(OtherShaderType::outline));
			if (it != m->otherShaderUse.end()) outlineUsed = it->second;
			it = m->otherShaderUse.find(static_cast<int>(OtherShaderType::normalLines));
			if (it != m->otherShaderUse.end()) normalLinesUsed = it->second;
		}
		otherShaders.AddMember("outline", outlineUsed, alloc);
		otherShaders.AddMember("normalLines", normalLinesUsed, alloc);
		one.AddMember("otherShaders", otherShaders, alloc);
		Value outlineColor;
		SetVec3(outlineColor, alloc, m->outlineColor);
		one.AddMember("outlineColor", outlineColor, alloc);
		one.AddMember("outlineWidth", m->outlineWidth, alloc);

		Value pos, rot, scale;
		SetVec3(pos, alloc, m->position);
		SetVec3(rot, alloc, m->rotation);
		SetVec3(scale, alloc, m->scale);
		one.AddMember("position", pos, alloc);
		one.AddMember("rotation", rot, alloc);
		one.AddMember("scale", scale, alloc);

		Value meshMats(rapidjson::kArrayType);
		auto& meshes = m->GetMeshes();
		for (size_t i = 0; i < meshes.size(); ++i) {
			Material* mat = meshes[i].material_ptr;
			if (!mat) continue;
			Value mm(rapidjson::kObjectType);
			mm.AddMember("meshIndex", static_cast<int>(i), alloc);

			RenderState rs = mat->GetRenderState();
			Value rsObj(rapidjson::kObjectType);
			rsObj.AddMember("depthTest", rs.depthTest, alloc);
			rsObj.AddMember("depthWrite", rs.depthWrite, alloc);
			rsObj.AddMember("stencilTest", rs.stencilTest, alloc);
			rsObj.AddMember("blendMode", static_cast<int>(rs.blendMode), alloc);
			rsObj.AddMember("cullMode", static_cast<int>(rs.cullMode), alloc);
			mm.AddMember("renderState", rsObj, alloc);

			Value props(rapidjson::kObjectType);
			for (const auto& [name, prop] : mat->GetProperties()) {
				if (prop.type == MaterialPropertyType::Texture) continue;
				Value po(rapidjson::kObjectType);
				po.AddMember("type", Value(PropTypeName(prop.type), alloc), alloc);
				po.AddMember("min", prop.minVal, alloc);
				po.AddMember("max", prop.maxVal, alloc);
				po.AddMember("step", prop.step, alloc);
				switch (prop.type) {
				case MaterialPropertyType::Float: po.AddMember("value", prop.scalarValue.floatValue, alloc); break;
				case MaterialPropertyType::Int: po.AddMember("value", prop.scalarValue.intValue, alloc); break;
				case MaterialPropertyType::Bool: po.AddMember("value", prop.scalarValue.boolValue, alloc); break;
				case MaterialPropertyType::Vec2: {
					Value arr(rapidjson::kArrayType);
					arr.PushBack(prop.vec2Value.x, alloc).PushBack(prop.vec2Value.y, alloc);
					po.AddMember("value", arr, alloc); break;
				}
				case MaterialPropertyType::Vec3:
				case MaterialPropertyType::Color: {
					Value arr(rapidjson::kArrayType);
					arr.PushBack(prop.vec3Value.x, alloc).PushBack(prop.vec3Value.y, alloc).PushBack(prop.vec3Value.z, alloc);
					po.AddMember("value", arr, alloc); break;
				}
				case MaterialPropertyType::Vec4: {
					Value arr(rapidjson::kArrayType);
					arr.PushBack(prop.vec4Value.x, alloc).PushBack(prop.vec4Value.y, alloc).PushBack(prop.vec4Value.z, alloc).PushBack(prop.vec4Value.w, alloc);
					po.AddMember("value", arr, alloc); break;
				}
				default: break;
				}
				props.AddMember(Value(name.c_str(), alloc), po, alloc);
			}
			mm.AddMember("properties", props, alloc);
			meshMats.PushBack(mm, alloc);
		}
		one.AddMember("meshMaterials", meshMats, alloc);
		models.PushBack(one, alloc);
	}
	doc.AddMember("models", models, alloc);

	// Lights
	Value lightsObj(rapidjson::kObjectType);
	Value pointLights(rapidjson::kArrayType);
	for (const auto& pl : scene.lightSource.pointLights) {
		Value one(rapidjson::kObjectType);
		one.AddMember("active", pl.m_active, alloc);
		one.AddMember("useShadowMap", pl.useShadowMap, alloc);
		Value pos, scale, amb, diff, spec;
		SetVec3(pos, alloc, pl.position);
		SetVec3(scale, alloc, pl.scale);
		SetVec3(amb, alloc, pl.ambient);
		SetVec3(diff, alloc, pl.diffuse);
		SetVec3(spec, alloc, pl.specular);
		one.AddMember("position", pos, alloc);
		one.AddMember("scale", scale, alloc);
		one.AddMember("ambient", amb, alloc);
		one.AddMember("diffuse", diff, alloc);
		one.AddMember("specular", spec, alloc);
		one.AddMember("constant", pl.constant, alloc);
		one.AddMember("linear", pl.linear, alloc);
		one.AddMember("quadratic", pl.quadratic, alloc);
		one.AddMember("near", pl.near, alloc);
		one.AddMember("far", pl.far, alloc);
		pointLights.PushBack(one, alloc);
	}
	lightsObj.AddMember("point", pointLights, alloc);

	Value dirLights(rapidjson::kArrayType);
	for (const auto& dl : scene.lightSource.directionLights) {
		Value one(rapidjson::kObjectType);
		one.AddMember("active", dl.m_active, alloc);
		one.AddMember("useShadowMap", dl.useShadowMap, alloc);
		Value dir, amb, diff, spec;
		SetVec3(dir, alloc, dl.direction);
		SetVec3(amb, alloc, dl.ambient);
		SetVec3(diff, alloc, dl.diffuse);
		SetVec3(spec, alloc, dl.specular);
		one.AddMember("direction", dir, alloc);
		one.AddMember("ambient", amb, alloc);
		one.AddMember("diffuse", diff, alloc);
		one.AddMember("specular", spec, alloc);
		one.AddMember("nearPlane", dl.near_plane, alloc);
		one.AddMember("farPlane", dl.far_plane, alloc);
		one.AddMember("distance", dl.distance, alloc);
		one.AddMember("width", dl.width, alloc);
		dirLights.PushBack(one, alloc);
	}
	lightsObj.AddMember("direction", dirLights, alloc);

	Value spotLights(rapidjson::kArrayType);
	for (const auto& sl : scene.lightSource.spotLights) {
		Value one(rapidjson::kObjectType);
		one.AddMember("active", sl.m_active, alloc);
		Value pos, dir, amb, diff, spec;
		SetVec3(pos, alloc, sl.position);
		SetVec3(dir, alloc, sl.direction);
		SetVec3(amb, alloc, sl.ambient);
		SetVec3(diff, alloc, sl.diffuse);
		SetVec3(spec, alloc, sl.specular);
		one.AddMember("position", pos, alloc);
		one.AddMember("direction", dir, alloc);
		one.AddMember("ambient", amb, alloc);
		one.AddMember("diffuse", diff, alloc);
		one.AddMember("specular", spec, alloc);
		one.AddMember("cutOff", sl.cutOff, alloc);
		one.AddMember("outerCutOff", sl.outerCutOff, alloc);
		one.AddMember("constant", sl.constant, alloc);
		one.AddMember("linear", sl.linear, alloc);
		one.AddMember("quadratic", sl.quadratic, alloc);
		spotLights.PushBack(one, alloc);
	}
	lightsObj.AddMember("spot", spotLights, alloc);
	doc.AddMember("lights", lightsObj, alloc);

	size_t sep = path.find_last_of("/\\");
	if (sep != std::string::npos) {
		std::string dir = path.substr(0, sep);
		if (!dir.empty()) _mkdir(dir.c_str());
	}
	std::ofstream ofs(path, std::ios::out | std::ios::trunc);
	if (!ofs.is_open()) return false;
	rapidjson::StringBuffer sb;
	rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(sb);
	doc.Accept(writer);
	ofs << sb.GetString();
	return true;
}

bool SceneStateIO::Load(Scene& scene, Camera& camera, const std::string& path) {
	std::ifstream ifs(path);
	if (!ifs.is_open()) return false;
	std::string text((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
	if (text.empty()) return false;

	Document doc;
	doc.Parse(text.c_str());
	if (doc.HasParseError() || !doc.IsObject()) return false;

	if (doc.HasMember("camera") && doc["camera"].IsObject()) {
		const auto& c = doc["camera"];
		if (c.HasMember("position")) camera.cameraPos = GetVec3(c["position"], camera.cameraPos);
		if (c.HasMember("front")) camera.cameraFront = glm::normalize(GetVec3(c["front"], camera.cameraFront));
		if (c.HasMember("up")) camera.up = glm::normalize(GetVec3(c["up"], camera.up));
		if (c.HasMember("fov") && c["fov"].IsNumber()) camera.fov = c["fov"].GetFloat();
	}

	if (doc.HasMember("lights") && doc["lights"].IsObject()) {
		const auto& lights = doc["lights"];

		if (lights.HasMember("point") && lights["point"].IsArray()) {
			const auto& pointArr = lights["point"];
			if (!pointArr.Empty()) {
				scene.lightSource.pointLights.clear();
			}
			for (const auto& p : pointArr.GetArray()) {
				if (!p.IsObject()) continue;
				auto pl = PointLight(
					glm::vec3(0.0f),
					glm::vec3(0.05f),
					glm::vec3(0.8f),
					glm::vec3(1.0f),
					"models/sphere/sphere.obj",
					XmlMaterialManager::GetInstance().GetMaterialRaw("Light")
				);
				if (p.HasMember("position")) pl.position = GetVec3(p["position"], pl.position);
				if (p.HasMember("scale")) pl.scale = GetVec3(p["scale"], pl.scale);
				if (p.HasMember("ambient")) pl.ambient = GetVec3(p["ambient"], pl.ambient);
				if (p.HasMember("diffuse")) pl.diffuse = GetVec3(p["diffuse"], pl.diffuse);
				if (p.HasMember("specular")) pl.specular = GetVec3(p["specular"], pl.specular);
				if (p.HasMember("constant") && p["constant"].IsNumber()) pl.constant = p["constant"].GetFloat();
				if (p.HasMember("linear") && p["linear"].IsNumber()) pl.linear = p["linear"].GetFloat();
				if (p.HasMember("quadratic") && p["quadratic"].IsNumber()) pl.quadratic = p["quadratic"].GetFloat();
				if (p.HasMember("near") && p["near"].IsNumber()) pl.near = p["near"].GetFloat();
				if (p.HasMember("far") && p["far"].IsNumber()) pl.far = p["far"].GetFloat();
				if (p.HasMember("useShadowMap") && p["useShadowMap"].IsBool()) pl.useShadowMap = p["useShadowMap"].GetBool();
				if (p.HasMember("active") && p["active"].IsBool()) pl.m_active = p["active"].GetBool();
				scene.lightSource.AddPointLight(pl);
			}
		}

		if (lights.HasMember("direction") && lights["direction"].IsArray()) {
			const auto& dirArr = lights["direction"];
			if (!dirArr.Empty()) {
				scene.lightSource.directionLights.clear();
			}
			for (const auto& d : dirArr.GetArray()) {
				if (!d.IsObject()) continue;
				auto dl = DirectionLight(
					glm::vec3(-0.2f, -1.0f, -0.3f),
					glm::vec3(10.f),
					glm::vec3(0.4f),
					glm::vec3(0.5f)
				);
				if (d.HasMember("direction")) dl.direction = GetVec3(d["direction"], dl.direction);
				if (d.HasMember("ambient")) dl.ambient = GetVec3(d["ambient"], dl.ambient);
				if (d.HasMember("diffuse")) dl.diffuse = GetVec3(d["diffuse"], dl.diffuse);
				if (d.HasMember("specular")) dl.specular = GetVec3(d["specular"], dl.specular);
				if (d.HasMember("nearPlane") && d["nearPlane"].IsNumber()) dl.near_plane = d["nearPlane"].GetFloat();
				if (d.HasMember("farPlane") && d["farPlane"].IsNumber()) dl.far_plane = d["farPlane"].GetFloat();
				if (d.HasMember("distance") && d["distance"].IsNumber()) dl.distance = d["distance"].GetFloat();
				if (d.HasMember("width") && d["width"].IsNumber()) dl.width = d["width"].GetFloat();
				if (d.HasMember("useShadowMap") && d["useShadowMap"].IsBool()) dl.useShadowMap = d["useShadowMap"].GetBool();
				if (d.HasMember("active") && d["active"].IsBool()) dl.m_active = d["active"].GetBool();
				scene.lightSource.AddDirectionLight(dl);
			}
		}

		if (lights.HasMember("spot") && lights["spot"].IsArray()) {
			const auto& spotArr = lights["spot"];
			if (!spotArr.Empty()) {
				scene.lightSource.spotLights.clear();
			}
			for (const auto& s : spotArr.GetArray()) {
				if (!s.IsObject()) continue;
				auto sl = SpotLight(
					glm::vec3(0.0f),
					glm::vec3(0.0f, -1.0f, 0.0f),
					glm::vec3(0.0f),
					glm::vec3(1.0f),
					glm::vec3(1.0f),
					12.5f,
					17.5f
				);
				if (s.HasMember("position")) sl.position = GetVec3(s["position"], sl.position);
				if (s.HasMember("direction")) sl.direction = GetVec3(s["direction"], sl.direction);
				if (s.HasMember("ambient")) sl.ambient = GetVec3(s["ambient"], sl.ambient);
				if (s.HasMember("diffuse")) sl.diffuse = GetVec3(s["diffuse"], sl.diffuse);
				if (s.HasMember("specular")) sl.specular = GetVec3(s["specular"], sl.specular);
				if (s.HasMember("cutOff") && s["cutOff"].IsNumber()) sl.cutOff = s["cutOff"].GetFloat();
				if (s.HasMember("outerCutOff") && s["outerCutOff"].IsNumber()) sl.outerCutOff = s["outerCutOff"].GetFloat();
				if (s.HasMember("constant") && s["constant"].IsNumber()) sl.constant = s["constant"].GetFloat();
				if (s.HasMember("linear") && s["linear"].IsNumber()) sl.linear = s["linear"].GetFloat();
				if (s.HasMember("quadratic") && s["quadratic"].IsNumber()) sl.quadratic = s["quadratic"].GetFloat();
				if (s.HasMember("active") && s["active"].IsBool()) sl.m_active = s["active"].GetBool();
				scene.lightSource.AddSpotLight(sl);
			}
		}
	}

	if (doc.HasMember("models") && doc["models"].IsArray()) {
		for (const auto& m : doc["models"].GetArray()) {
			ApplyModelEntry(scene, m);
		}
	}
	return true;
}

bool SceneStateIO::LoadAsync(Scene& scene, Camera& camera, const std::string& path) {
	while (!g_pendingModelJsonQueue.empty()) g_pendingModelJsonQueue.pop();
	g_totalAsyncModelCount = 0;

	std::ifstream ifs(path);
	if (!ifs.is_open()) return false;
	std::string text((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
	if (text.empty()) return false;

	Document doc;
	doc.Parse(text.c_str());
	if (doc.HasParseError() || !doc.IsObject()) return false;

	// 先复用同步加载逻辑中的 camera/lights 恢复，保证行为一致
	// 这里直接调用一次同步逻辑处理 lights/camera，然后仅对 models 改为分帧。
	if (doc.HasMember("camera") && doc["camera"].IsObject()) {
		const auto& c = doc["camera"];
		if (c.HasMember("position")) camera.cameraPos = GetVec3(c["position"], camera.cameraPos);
		if (c.HasMember("front")) camera.cameraFront = glm::normalize(GetVec3(c["front"], camera.cameraFront));
		if (c.HasMember("up")) camera.up = glm::normalize(GetVec3(c["up"], camera.up));
		if (c.HasMember("fov") && c["fov"].IsNumber()) camera.fov = c["fov"].GetFloat();
	}
	if (doc.HasMember("lights") && doc["lights"].IsObject()) {
		const auto& lights = doc["lights"];
		if (lights.HasMember("point") && lights["point"].IsArray()) {
			const auto& pointArr = lights["point"];
			if (!pointArr.Empty()) scene.lightSource.pointLights.clear();
			for (const auto& p : pointArr.GetArray()) {
				if (!p.IsObject()) continue;
				auto pl = PointLight(glm::vec3(0.0f), glm::vec3(0.05f), glm::vec3(0.8f), glm::vec3(1.0f), "models/sphere/sphere.obj", XmlMaterialManager::GetInstance().GetMaterialRaw("Light"));
				if (p.HasMember("position")) pl.position = GetVec3(p["position"], pl.position);
				if (p.HasMember("scale")) pl.scale = GetVec3(p["scale"], pl.scale);
				if (p.HasMember("ambient")) pl.ambient = GetVec3(p["ambient"], pl.ambient);
				if (p.HasMember("diffuse")) pl.diffuse = GetVec3(p["diffuse"], pl.diffuse);
				if (p.HasMember("specular")) pl.specular = GetVec3(p["specular"], pl.specular);
				if (p.HasMember("constant") && p["constant"].IsNumber()) pl.constant = p["constant"].GetFloat();
				if (p.HasMember("linear") && p["linear"].IsNumber()) pl.linear = p["linear"].GetFloat();
				if (p.HasMember("quadratic") && p["quadratic"].IsNumber()) pl.quadratic = p["quadratic"].GetFloat();
				if (p.HasMember("near") && p["near"].IsNumber()) pl.near = p["near"].GetFloat();
				if (p.HasMember("far") && p["far"].IsNumber()) pl.far = p["far"].GetFloat();
				if (p.HasMember("useShadowMap") && p["useShadowMap"].IsBool()) pl.useShadowMap = p["useShadowMap"].GetBool();
				if (p.HasMember("active") && p["active"].IsBool()) pl.m_active = p["active"].GetBool();
				scene.lightSource.AddPointLight(pl);
			}
		}
		if (lights.HasMember("direction") && lights["direction"].IsArray()) {
			const auto& dirArr = lights["direction"];
			if (!dirArr.Empty()) scene.lightSource.directionLights.clear();
			for (const auto& d : dirArr.GetArray()) {
				if (!d.IsObject()) continue;
				auto dl = DirectionLight(glm::vec3(-0.2f, -1.0f, -0.3f), glm::vec3(10.f), glm::vec3(0.4f), glm::vec3(0.5f));
				if (d.HasMember("direction")) dl.direction = GetVec3(d["direction"], dl.direction);
				if (d.HasMember("ambient")) dl.ambient = GetVec3(d["ambient"], dl.ambient);
				if (d.HasMember("diffuse")) dl.diffuse = GetVec3(d["diffuse"], dl.diffuse);
				if (d.HasMember("specular")) dl.specular = GetVec3(d["specular"], dl.specular);
				if (d.HasMember("nearPlane") && d["nearPlane"].IsNumber()) dl.near_plane = d["nearPlane"].GetFloat();
				if (d.HasMember("farPlane") && d["farPlane"].IsNumber()) dl.far_plane = d["farPlane"].GetFloat();
				if (d.HasMember("distance") && d["distance"].IsNumber()) dl.distance = d["distance"].GetFloat();
				if (d.HasMember("width") && d["width"].IsNumber()) dl.width = d["width"].GetFloat();
				if (d.HasMember("useShadowMap") && d["useShadowMap"].IsBool()) dl.useShadowMap = d["useShadowMap"].GetBool();
				if (d.HasMember("active") && d["active"].IsBool()) dl.m_active = d["active"].GetBool();
				scene.lightSource.AddDirectionLight(dl);
			}
		}
		if (lights.HasMember("spot") && lights["spot"].IsArray()) {
			const auto& spotArr = lights["spot"];
			if (!spotArr.Empty()) scene.lightSource.spotLights.clear();
			for (const auto& s : spotArr.GetArray()) {
				if (!s.IsObject()) continue;
				auto sl = SpotLight(glm::vec3(0.0f), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f), glm::vec3(1.0f), 12.5f, 17.5f);
				if (s.HasMember("position")) sl.position = GetVec3(s["position"], sl.position);
				if (s.HasMember("direction")) sl.direction = GetVec3(s["direction"], sl.direction);
				if (s.HasMember("ambient")) sl.ambient = GetVec3(s["ambient"], sl.ambient);
				if (s.HasMember("diffuse")) sl.diffuse = GetVec3(s["diffuse"], sl.diffuse);
				if (s.HasMember("specular")) sl.specular = GetVec3(s["specular"], sl.specular);
				if (s.HasMember("cutOff") && s["cutOff"].IsNumber()) sl.cutOff = s["cutOff"].GetFloat();
				if (s.HasMember("outerCutOff") && s["outerCutOff"].IsNumber()) sl.outerCutOff = s["outerCutOff"].GetFloat();
				if (s.HasMember("constant") && s["constant"].IsNumber()) sl.constant = s["constant"].GetFloat();
				if (s.HasMember("linear") && s["linear"].IsNumber()) sl.linear = s["linear"].GetFloat();
				if (s.HasMember("quadratic") && s["quadratic"].IsNumber()) sl.quadratic = s["quadratic"].GetFloat();
				if (s.HasMember("active") && s["active"].IsBool()) sl.m_active = s["active"].GetBool();
				scene.lightSource.AddSpotLight(sl);
			}
		}
	}

	if (doc.HasMember("models") && doc["models"].IsArray()) {
		for (const auto& m : doc["models"].GetArray()) {
			if (!m.IsObject()) continue;
			bool isFileSource = false;
			if (m.HasMember("source") && m["source"].IsObject()) {
				const auto& src = m["source"];
				if (src.HasMember("type") && src["type"].IsString()) {
					isFileSource = (std::string(src["type"].GetString()) == "file");
				}
			}
			if (isFileSource) {
				g_pendingModelJsonQueue.push(SerializeJsonValue(m));
				++g_totalAsyncModelCount;
			}
			else {
				ApplyModelEntry(scene, m);
			}
		}
	}
	return true;
}

void SceneStateIO::UpdateAsyncLoads(Scene& scene, int maxModelsPerFrame) {
	if (maxModelsPerFrame <= 0) maxModelsPerFrame = 1;
	for (int i = 0; i < maxModelsPerFrame && !g_pendingModelJsonQueue.empty(); ++i) {
		const std::string oneModelJson = g_pendingModelJsonQueue.front();
		g_pendingModelJsonQueue.pop();
		Document mdoc;
		mdoc.Parse(oneModelJson.c_str());
		if (!mdoc.HasParseError() && mdoc.IsObject()) {
			ApplyModelEntry(scene, mdoc);
		}
	}
}

bool SceneStateIO::HasPendingAsyncLoads() {
	return !g_pendingModelJsonQueue.empty();
}

int SceneStateIO::GetPendingAsyncLoadCount() {
	return static_cast<int>(g_pendingModelJsonQueue.size());
}

int SceneStateIO::GetTotalAsyncLoadCount() {
	return g_totalAsyncModelCount;
}

