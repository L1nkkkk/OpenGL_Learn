#include "Shader.h"
#include "Profiler.h"
#include <algorithm>

namespace fs = std::filesystem;

Shader::~Shader()
{
	if (ID != 0) {
		if (s_boundProgram == ID) {
			s_boundProgram = 0;
		}
		glDeleteProgram(ID);
		ID = 0;
	}
}

void Shader::SetSourcePaths(const std::string& vertexPath, const std::string& fragmentPath, const std::string& geometryPath)
{
	m_vertexPath = vertexPath;
	m_fragmentPath = fragmentPath;
	m_geometryPath = geometryPath;
	m_isGeometryShader = !geometryPath.empty();
}

bool Shader::TryGetWriteTime(const std::string& path, fs::file_time_type& outTime)
{
	PerformanceProfiler::GetInstance().RecordFileSystemCheck();
	try {
		if (!fs::exists(path)) {
			return false;
		}
		outTime = fs::last_write_time(path);
		return true;
	}
	catch (...) {
		return false;
	}
}

void Shader::UpdateCachedWriteTimes()
{
	TryGetWriteTime(m_vertexPath, m_vertexWriteTime);
	TryGetWriteTime(m_fragmentPath, m_fragmentWriteTime);
	if (m_isGeometryShader) {
		TryGetWriteTime(m_geometryPath, m_geometryWriteTime);
	}
	m_dependencyWriteTimes.clear();
	for (const std::string& dependency : m_sourceDependencies) {
		fs::file_time_type writeTime;
		if (TryGetWriteTime(dependency, writeTime)) {
			m_dependencyWriteTimes.emplace(dependency, writeTime);
		}
	}
}

bool Shader::HasSourceChanges() const
{
	fs::file_time_type currentTime;
	if (TryGetWriteTime(m_vertexPath, currentTime) && currentTime != m_vertexWriteTime) {
		return true;
	}
	if (TryGetWriteTime(m_fragmentPath, currentTime) && currentTime != m_fragmentWriteTime) {
		return true;
	}
	if (m_isGeometryShader && TryGetWriteTime(m_geometryPath, currentTime) && currentTime != m_geometryWriteTime) {
		return true;
	}
	for (const auto& [dependency, cachedWriteTime] : m_dependencyWriteTimes) {
		if (!TryGetWriteTime(dependency, currentTime) || currentTime != cachedWriteTime) {
			return true;
		}
	}
	return false;
}

bool Shader::ReadTextFile(const std::string& path, std::string& outText, std::string& errorMessage) const
{
	std::vector<std::string> includeStack;
	return ReadTextFileWithIncludes(path, outText, errorMessage, includeStack);
}

bool Shader::ReadTextFileWithIncludes(
	const std::string& path,
	std::string& outText,
	std::string& errorMessage,
	std::vector<std::string>& includeStack) const
{
	const std::string normalizedPath = fs::path(path).lexically_normal().generic_string();
	if (std::find(includeStack.begin(), includeStack.end(), normalizedPath) != includeStack.end()) {
		errorMessage = "Cyclic shader include detected: " + normalizedPath;
		return false;
	}

	std::ifstream shaderFile(path);
	if (!shaderFile.is_open()) {
		errorMessage = "Failed to open shader file: " + path;
		return false;
	}

	if (!includeStack.empty() &&
		std::find(m_sourceDependencies.begin(), m_sourceDependencies.end(), normalizedPath) ==
			m_sourceDependencies.end()) {
		m_sourceDependencies.push_back(normalizedPath);
	}
	includeStack.push_back(normalizedPath);

	std::stringstream expanded;
	std::string line;
	while (std::getline(shaderFile, line)) {
		const size_t first = line.find_first_not_of(" \t");
		const bool isInclude =
			first != std::string::npos &&
			line.compare(first, 8, "#include") == 0;
		if (!isInclude) {
			expanded << line << '\n';
			continue;
		}

		const size_t openingQuote = line.find('"', first + 8);
		const size_t closingQuote =
			openingQuote == std::string::npos
				? std::string::npos
				: line.find('"', openingQuote + 1);
		if (openingQuote == std::string::npos || closingQuote == std::string::npos) {
			errorMessage = "Malformed shader include in " + normalizedPath + ": " + line;
			includeStack.pop_back();
			return false;
		}

		const fs::path includePath =
			fs::path(normalizedPath).parent_path() /
			line.substr(openingQuote + 1, closingQuote - openingQuote - 1);
		std::string includedText;
		if (!ReadTextFileWithIncludes(
				includePath.lexically_normal().generic_string(),
				includedText,
				errorMessage,
				includeStack)) {
			includeStack.pop_back();
			return false;
		}
		expanded << includedText;
		if (!includedText.empty() && includedText.back() != '\n') {
			expanded << '\n';
		}
	}

	includeStack.pop_back();
	outText = expanded.str();
	return true;
}

bool Shader::CompileStage(unsigned int& shaderHandle, unsigned int shaderType, const std::string& source, const char* stageName, std::string& errorMessage) const
{
	shaderHandle = glCreateShader(shaderType);
	const char* sourcePtr = source.c_str();
	glShaderSource(shaderHandle, 1, &sourcePtr, nullptr);
	glCompileShader(shaderHandle);

	int success = 0;
	glGetShaderiv(shaderHandle, GL_COMPILE_STATUS, &success);
	if (success) {
		return true;
	}

	char infoLog[1024] = {};
	glGetShaderInfoLog(shaderHandle, sizeof(infoLog), nullptr, infoLog);
	errorMessage = std::string("ERROR::SHADER::") + stageName + "::COMPILATION_FAILED\n" + infoLog;
	glDeleteShader(shaderHandle);
	shaderHandle = 0;
	return false;
}

bool Shader::BuildProgram(unsigned int& outProgram, std::string& errorMessage) const
{
	std::string vertexCode;
	std::string fragmentCode;
	std::string geometryCode;
	m_sourceDependencies.clear();

	if (!ReadTextFile(m_vertexPath, vertexCode, errorMessage)) {
		return false;
	}
	if (!ReadTextFile(m_fragmentPath, fragmentCode, errorMessage)) {
		return false;
	}
	if (m_isGeometryShader && !ReadTextFile(m_geometryPath, geometryCode, errorMessage)) {
		return false;
	}

	unsigned int vertexShader = 0;
	unsigned int geometryShader = 0;
	unsigned int fragmentShader = 0;

	if (!CompileStage(vertexShader, GL_VERTEX_SHADER, vertexCode, "VERTEX", errorMessage)) {
		return false;
	}
	if (m_isGeometryShader && !CompileStage(geometryShader, GL_GEOMETRY_SHADER, geometryCode, "GEOMETRY", errorMessage)) {
		glDeleteShader(vertexShader);
		return false;
	}
	if (!CompileStage(fragmentShader, GL_FRAGMENT_SHADER, fragmentCode, "FRAGMENT", errorMessage)) {
		glDeleteShader(vertexShader);
		if (geometryShader != 0) {
			glDeleteShader(geometryShader);
		}
		return false;
	}

	outProgram = glCreateProgram();
	glAttachShader(outProgram, vertexShader);
	if (geometryShader != 0) {
		glAttachShader(outProgram, geometryShader);
	}
	glAttachShader(outProgram, fragmentShader);
	glLinkProgram(outProgram);

	int success = 0;
	glGetProgramiv(outProgram, GL_LINK_STATUS, &success);
	if (!success) {
		char infoLog[1024] = {};
		glGetProgramInfoLog(outProgram, sizeof(infoLog), nullptr, infoLog);
		errorMessage = std::string("ERROR::SHADER::PROGRAM::LINKING_FAILED\n") + infoLog;
		glDeleteProgram(outProgram);
		outProgram = 0;
	}

	glDeleteShader(vertexShader);
	if (geometryShader != 0) {
		glDeleteShader(geometryShader);
	}
	glDeleteShader(fragmentShader);
	return success != 0;
}

bool Shader::Reload(bool force, std::string* errorMessage)
{
	if (!force && !HasSourceChanges()) {
		return false;
	}

	unsigned int newProgram = 0;
	std::string localError;
	if (!BuildProgram(newProgram, localError)) {
		if (errorMessage) {
			*errorMessage = localError;
		}
		std::cout << localError << std::endl;
		return false;
	}

	if (ID != 0) {
		if (s_boundProgram == ID) {
			s_boundProgram = 0;
		}
		glDeleteProgram(ID);
	}
	ID = newProgram;
	++m_revision;
	m_uniformLocationCache.clear();
	UpdateCachedWriteTimes();
	if (errorMessage) {
		errorMessage->clear();
	}
	return true;
}

bool Shader::ReloadIfChanged(std::string* errorMessage)
{
	return Reload(false, errorMessage);
}

void Shader::Load(const char* vertexPath, const char* fragmentPath)
{
	SetSourcePaths(vertexPath, fragmentPath);
	std::string errorMessage;
	Reload(true, &errorMessage);
}

void Shader::use()
{
	if (ID == 0 || s_boundProgram == ID) {
		return;
	}

	glUseProgram(ID);
	s_boundProgram = ID;
	PerformanceProfiler::GetInstance().RecordShaderBind();
}

int Shader::GetUniformLocation(const std::string& name) const
{
	const auto cached = m_uniformLocationCache.find(name);
	if (cached != m_uniformLocationCache.end()) {
		PerformanceProfiler::GetInstance().RecordUniformLocationLookup(true);
		return cached->second;
	}

	const int location = ID != 0 ? glGetUniformLocation(ID, name.c_str()) : -1;
	m_uniformLocationCache.emplace(name, location);
	PerformanceProfiler::GetInstance().RecordUniformLocationLookup(false);
	return location;
}

void Shader::setBool(const std::string& name, bool value) const
{
	PerformanceProfiler::GetInstance().RecordUniformUpdate();
	const int location = GetUniformLocation(name);
	if (location >= 0) {
		glUniform1i(location, static_cast<int>(value));
	}
}

void Shader::setInt(const std::string& name, int value) const
{
	PerformanceProfiler::GetInstance().RecordUniformUpdate();
	const int location = GetUniformLocation(name);
	if (location >= 0) {
		glUniform1i(location, value);
	}
}

void Shader::setFloat(const std::string& name, float value) const
{
	PerformanceProfiler::GetInstance().RecordUniformUpdate();
	const int location = GetUniformLocation(name);
	if (location >= 0) {
		glUniform1f(location, value);
	}
}

void Shader::setMat4(const std::string& name, const glm::mat4& mat) const
{
	PerformanceProfiler::GetInstance().RecordUniformUpdate();
	const int location = GetUniformLocation(name);
	if (location >= 0) {
		glUniformMatrix4fv(location, 1, GL_FALSE, &mat[0][0]);
	}
}

void Shader::setVec3(const std::string& name, const glm::vec3& vec) const
{
	PerformanceProfiler::GetInstance().RecordUniformUpdate();
	const int location = GetUniformLocation(name);
	if (location >= 0) {
		glUniform3fv(location, 1, &vec[0]);
	}
}

void Shader::setVec4(const std::string& name, const glm::vec4& vec) const
{
	PerformanceProfiler::GetInstance().RecordUniformUpdate();
	const int location = GetUniformLocation(name);
	if (location >= 0) {
		glUniform4fv(location, 1, &vec[0]);
	}
}

void Shader::setVec2(const std::string& name, const glm::vec2& vec) const
{
	PerformanceProfiler::GetInstance().RecordUniformUpdate();
	const int location = GetUniformLocation(name);
	if (location >= 0) {
		glUniform2fv(location, 1, &vec[0]);
	}
}

GeometryShader::GeometryShader(const char* vertexPath, const char* geometryPath, const char* fragmentPath)
	: Shader()
{
	SetSourcePaths(vertexPath, fragmentPath, geometryPath);
	std::string errorMessage;
	Reload(true, &errorMessage);
}
