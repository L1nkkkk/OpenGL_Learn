#include "Shader.h"
#include "Profiler.h"

namespace fs = std::filesystem;

Shader::~Shader()
{
	if (ID != 0) {
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
	return false;
}

bool Shader::ReadTextFile(const std::string& path, std::string& outText, std::string& errorMessage) const
{
	std::ifstream shaderFile(path);
	if (!shaderFile.is_open()) {
		errorMessage = "Failed to open shader file: " + path;
		return false;
	}

	std::stringstream stream;
	stream << shaderFile.rdbuf();
	outText = stream.str();
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
		glDeleteProgram(ID);
	}
	ID = newProgram;
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
	PerformanceProfiler::GetInstance().RecordShaderBind();
	glUseProgram(ID);
}

void Shader::setBool(const std::string& name, bool value) const
{
	PerformanceProfiler::GetInstance().RecordUniformUpdate();
	glUniform1i(glGetUniformLocation(ID, name.c_str()), (int)value);
}

void Shader::setInt(const std::string& name, int value) const
{
	PerformanceProfiler::GetInstance().RecordUniformUpdate();
	glUniform1i(glGetUniformLocation(ID, name.c_str()), value);
}

void Shader::setFloat(const std::string& name, float value) const
{
	PerformanceProfiler::GetInstance().RecordUniformUpdate();
	glUniform1f(glGetUniformLocation(ID, name.c_str()), value);
}

void Shader::setMat4(const std::string& name, const glm::mat4& mat) const
{
	PerformanceProfiler::GetInstance().RecordUniformUpdate();
	glUniformMatrix4fv(glGetUniformLocation(ID, name.c_str()), 1, GL_FALSE, &mat[0][0]);
}

void Shader::setVec3(const std::string& name, const glm::vec3& vec) const
{
	PerformanceProfiler::GetInstance().RecordUniformUpdate();
	glUniform3fv(glGetUniformLocation(ID, name.c_str()), 1, &vec[0]);
}

void Shader::setVec4(const std::string& name, const glm::vec4& vec) const
{
	PerformanceProfiler::GetInstance().RecordUniformUpdate();
	glUniform4fv(glGetUniformLocation(ID, name.c_str()), 1, &vec[0]);
}

void Shader::setVec2(const std::string& name, const glm::vec2& vec) const
{
	PerformanceProfiler::GetInstance().RecordUniformUpdate();
	glUniform2fv(glGetUniformLocation(ID, name.c_str()), 1, &vec[0]);
}

GeometryShader::GeometryShader(const char* vertexPath, const char* geometryPath, const char* fragmentPath)
	: Shader()
{
	SetSourcePaths(vertexPath, fragmentPath, geometryPath);
	std::string errorMessage;
	Reload(true, &errorMessage);
}
