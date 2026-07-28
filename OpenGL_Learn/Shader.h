#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <string>
#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <cstdint>
#include <unordered_map>
#include <vector>

class Shader {
public:
	unsigned int ID = 0;
	std::string shaderName;
	Shader() = default;
	Shader(std::string name) {
		shaderName = name;
		std::string vertexPath = "shaders/" + name + "Vertex.glsl";
		std::string fragmentPath = "shaders/" + name + "Fragment.glsl";
		SetSourcePaths(vertexPath, fragmentPath);
		Reload(true);
	}
	void Load(const char* vertexPath, const char* fragmentPath);
	Shader(const char* vertexPath, const char* fragmentPath) {
		SetSourcePaths(vertexPath, fragmentPath);
		Reload(true);
	}
	virtual ~Shader();
	void use();
	void setBool(const std::string& name, bool value) const;
	void setInt(const std::string& name, int value) const;
	void setFloat(const std::string& name, float value) const;
	void setMat4(const std::string& name, const glm::mat4& mat) const;
	void setVec3(const std::string& name, const glm::vec3& vec) const;
	void setVec4(const std::string& name, const glm::vec4& vec) const;
	void setVec2(const std::string& name, const glm::vec2& vec) const;
	void SetSourcePaths(const std::string& vertexPath, const std::string& fragmentPath, const std::string& geometryPath = std::string());
	bool Reload(bool force = false, std::string* errorMessage = nullptr);
	bool ReloadIfChanged(std::string* errorMessage = nullptr);
	bool HasSourceChanges() const;
	bool IsGeometryShader() const { return m_isGeometryShader; }
	std::uint64_t GetRevision() const { return m_revision; }
	const std::string& GetVertexPath() const { return m_vertexPath; }
	const std::string& GetFragmentPath() const { return m_fragmentPath; }
	const std::string& GetGeometryPath() const { return m_geometryPath; }

protected:
	bool BuildProgram(unsigned int& outProgram, std::string& errorMessage) const;
	bool ReadTextFile(const std::string& path, std::string& outText, std::string& errorMessage) const;
	bool ReadTextFileWithIncludes(
		const std::string& path,
		std::string& outText,
		std::string& errorMessage,
		std::vector<std::string>& includeStack) const;
	bool CompileStage(unsigned int& shaderHandle, unsigned int shaderType, const std::string& source, const char* stageName, std::string& errorMessage) const;
	static bool TryGetWriteTime(const std::string& path, std::filesystem::file_time_type& outTime);
	void UpdateCachedWriteTimes();
	int GetUniformLocation(const std::string& name) const;

	std::string m_vertexPath;
	std::string m_fragmentPath;
	std::string m_geometryPath;
	std::filesystem::file_time_type m_vertexWriteTime = {};
	std::filesystem::file_time_type m_fragmentWriteTime = {};
	std::filesystem::file_time_type m_geometryWriteTime = {};
	mutable std::vector<std::string> m_sourceDependencies;
	std::unordered_map<std::string, std::filesystem::file_time_type> m_dependencyWriteTimes;
	bool m_isGeometryShader = false;
	std::uint64_t m_revision = 0;
	mutable std::unordered_map<std::string, int> m_uniformLocationCache;
	inline static unsigned int s_boundProgram = 0;
};

class GeometryShader : public Shader {
public:
	GeometryShader(const char* vertexPath, const char* geometryPath, const char* fragmentPath);
};
