#pragma once

#include <cstdint>

class Shader;

class ImageBasedLighting {
public:
	ImageBasedLighting() = default;
	~ImageBasedLighting();

	ImageBasedLighting(const ImageBasedLighting&) = delete;
	ImageBasedLighting& operator=(const ImageBasedLighting&) = delete;

	bool Initialize(
		unsigned int environmentCubemap,
		unsigned int cubeVAO,
		unsigned int quadVAO,
		int restoreViewportWidth,
		int restoreViewportHeight);
	void Destroy();

	unsigned int Bind(Shader& shader, unsigned int firstTextureUnit) const;
	bool IsReady() const { return m_ready; }
	unsigned int GetIrradianceMap() const { return m_irradianceMap; }
	unsigned int GetPrefilterMap() const { return m_prefilterMap; }
	unsigned int GetBrdfLut() const { return m_brdfLut; }

private:
	unsigned int m_irradianceMap = 0;
	unsigned int m_prefilterMap = 0;
	unsigned int m_brdfLut = 0;
	std::uint64_t m_trackedBytes = 0;
	int m_maxTextureUnits = 0;
	bool m_ready = false;
	mutable bool m_textureUnitWarningPrinted = false;
};
