#pragma once
#include <string>
#include <unordered_map>

enum class OtherShaderType {
	outline = 0,
	normalLines
};



class OtherShader {
public:
	static std::string OtherShaderTypeToString(OtherShaderType type) {
		switch (type) {
		case OtherShaderType::outline:
			return "outline";
		case OtherShaderType::normalLines:
			return "normalLines";
		default:
			return "unknown";
		}
	}

	inline static float normalLineMagnitude = 0.01;
};