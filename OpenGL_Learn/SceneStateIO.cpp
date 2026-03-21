#include "SceneStateIO.h"

#include <fstream>
#include <direct.h>

#include "../assimp/contrib/rapidjson/include/rapidjson/document.h"
#include "../assimp/contrib/rapidjson/include/rapidjson/prettywriter.h"
#include "../assimp/contrib/rapidjson/include/rapidjson/stringbuffer.h"

using rapidjson::Document;
using rapidjson::Value;

namespace {
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
}

bool SceneStateIO::Exists(const std::string& path) {
	std::ifstream ifs(path);
	return ifs.good();
}

bool SceneStateIO::Save(const Scene& scene, const Camera& camera, const std::string& path) {
	Document doc;
	doc.SetObject();
	auto& alloc = doc.GetAllocator();

	doc.AddMember("version", 1, alloc);

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

	if (doc.HasMember("models") && doc["models"].IsArray()) {
		for (const auto& m : doc["models"].GetArray()) {
			if (!m.IsObject() || !m.HasMember("name") || !m["name"].IsString()) continue;
			Model* model = FindModelByName(scene, m["name"].GetString());
			if (!model) continue;
			if (m.HasMember("active") && m["active"].IsBool()) model->SetActiveStatus(m["active"].GetBool());
			if (m.HasMember("position")) model->SetPosition(GetVec3(m["position"], model->position));
			if (m.HasMember("rotation")) model->SetRotation(GetVec3(m["rotation"], model->rotation));
			if (m.HasMember("scale")) model->SetScale(GetVec3(m["scale"], model->scale));

			if (!m.HasMember("meshMaterials") || !m["meshMaterials"].IsArray()) continue;
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
							case MaterialPropertyType::Vec2:
								if (v.IsArray() && v.Size() >= 2) p.vec2Value = glm::vec2(v[0].GetFloat(), v[1].GetFloat()); break;
							case MaterialPropertyType::Vec3:
							case MaterialPropertyType::Color:
								if (v.IsArray() && v.Size() >= 3) p.vec3Value = glm::vec3(v[0].GetFloat(), v[1].GetFloat(), v[2].GetFloat()); break;
							case MaterialPropertyType::Vec4:
								if (v.IsArray() && v.Size() >= 4) p.vec4Value = glm::vec4(v[0].GetFloat(), v[1].GetFloat(), v[2].GetFloat(), v[3].GetFloat()); break;
							default: break;
							}
						}
						mat->AddProperty(it->name.GetString(), p);
					}
				}
			}
		}
	}
	return true;
}

