#include "Global.h"
#include "GLStateCache.h"
#include "Profiler.h"
#include <sstream>

namespace {
    std::uint64_t NextFramebufferResourceGeneration()
    {
        static std::uint64_t generation = 0;
        ++generation;
        if (generation == 0) {
            ++generation;
        }
        return generation;
    }

    std::uint64_t BytesPerPixel(const TextureAttributes& attr)
    {
        switch (attr.internalFormat) {
        case GL_R8: return 1;
        case GL_R16F: return 2;
        case GL_RG8: return 2;
        case GL_RG16F: return 4;
        case GL_RGB:
        case GL_RGB8:
        case GL_SRGB: return 3;
        case GL_RGB16F: return 6;
        case GL_RGB32F: return 12;
        case GL_RGBA:
        case GL_RGBA8:
        case GL_SRGB_ALPHA:
        case GL_DEPTH_COMPONENT:
        case GL_DEPTH_COMPONENT24:
        case GL_DEPTH_COMPONENT32:
        case GL_DEPTH_COMPONENT32F:
        case GL_DEPTH24_STENCIL8: return 4;
        case GL_RGBA16F: return 8;
        case GL_RGBA32F: return 16;
        default:
            // Conservative fallback for formats not yet listed above.
            return attr.format == GL_RED ? 1 : (attr.format == GL_RGB ? 3 : 4);
        }
    }

    std::uint64_t EstimateRenderTargetBytes(const FBOAttributes& attr)
    {
        const auto& properties = SystemProperties::GetInstance();
        const std::uint64_t width = static_cast<std::uint64_t>(
            attr.width > 0
                ? attr.width
                : (attr.isShadowMap ? properties.SHADOW_WIDTH : properties.SCREEN_WIDTH));
        const std::uint64_t height = static_cast<std::uint64_t>(
            attr.height > 0
                ? attr.height
                : (attr.isShadowMap ? properties.SHADOW_HEIGHT : properties.SCREEN_HEIGHT));
        std::uint64_t bytes = 0;

        for (const auto& textureAttr : attr.textureAttrs) {
            const std::uint64_t faces = textureAttr.target == GL_TEXTURE_CUBE_MAP ? 6u : 1u;
            const std::uint64_t samples =
                textureAttr.target == GL_TEXTURE_2D_MULTISAMPLE ? 4u : 1u;
            bytes += width * height * faces * samples * BytesPerPixel(textureAttr);
        }

        if (!attr.isShadowMap) {
            const std::uint64_t samples =
                attr.aaType == AntiAliasManager::AntiAliasType::MSAA ? 4u : 1u;
            // The candidate uses a sampleable D24S8 texture in place of,
            // not in addition to, the control's D24S8 renderbuffer.
            bytes += width * height * samples * 4u;
        }
        return bytes;
    }
}

void FBO::Delete() {
    if (m_trackedBytes != 0) {
        PerformanceProfiler::GetInstance().RecordMemoryRelease(
            MemoryResourceType::RenderTarget,
            m_trackedBytes);
        m_trackedBytes = 0;
    }
    if (framebufferID != 0) {
        GLState::ForgetFramebuffer(framebufferID);
        glDeleteFramebuffers(1, &framebufferID);
        framebufferID = 0;
    }
    GLState::ForgetFramebuffers(
        static_cast<GLsizei>(m_cubeFaceFramebufferIDs.size()),
        m_cubeFaceFramebufferIDs.data());
    glDeleteFramebuffers(
        static_cast<GLsizei>(m_cubeFaceFramebufferIDs.size()),
        m_cubeFaceFramebufferIDs.data());
    m_cubeFaceFramebufferIDs.fill(0);
    if (!textureIDs.empty()) {
        GLState::ForgetTextures(static_cast<GLsizei>(textureIDs.size()), textureIDs.data());
        glDeleteTextures(static_cast<GLsizei>(textureIDs.size()), textureIDs.data());
        textureIDs.clear();
    }
    if (rboID != 0) {
        glDeleteRenderbuffers(1, &rboID);
        rboID = 0;
    }
    if (depthTextureID != 0) {
        GLState::ForgetTexture(depthTextureID);
        glDeleteTextures(1, &depthTextureID);
        depthTextureID = 0;
    }
    init = false;
}

unsigned int FBO::GetCubeFaceFramebuffer(int face) {
    if (face < 0 ||
        face >= static_cast<int>(m_cubeFaceFramebufferIDs.size()) ||
        !IsComplete() ||
        attr.shadowType != FBOAttributes::ShadowBox ||
        textureIDs.empty() ||
        textureIDs.front() == 0 ||
        framebufferID == 0) {
        return 0;
    }

    unsigned int& faceFramebuffer =
        m_cubeFaceFramebufferIDs[static_cast<std::size_t>(face)];
    if (faceFramebuffer != 0) {
        return faceFramebuffer;
    }

    glGenFramebuffers(1, &faceFramebuffer);
    if (faceFramebuffer == 0) {
        return 0;
    }
    GLState::BindFramebuffer(GL_FRAMEBUFFER, faceFramebuffer);
    glFramebufferTexture2D(
        GL_FRAMEBUFFER,
        GL_DEPTH_ATTACHMENT,
        GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
        textureIDs.front(),
        0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    const bool complete =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    GLState::BindFramebuffer(GL_FRAMEBUFFER, framebufferID);
    if (!complete) {
        GLState::ForgetFramebuffer(faceFramebuffer);
        glDeleteFramebuffers(1, &faceFramebuffer);
        faceFramebuffer = 0;
        return 0;
    }
    return faceFramebuffer;
}

void FBO::Init(FBOAttributes attr) {
    init = false;
    m_resourceGeneration = NextFramebufferResourceGeneration();
    this->attr = attr;
    width = attr.width > 0
        ? attr.width
        : (attr.isShadowMap ? properties.SHADOW_WIDTH : properties.SCREEN_WIDTH);
    height = attr.height > 0
        ? attr.height
        : (attr.isShadowMap ? properties.SHADOW_HEIGHT : properties.SCREEN_HEIGHT);
    float borderColor[] = { 1.0f, 1.0f, 1.0f, 1.0f };
    depthTextureID = 0;

    glGenFramebuffers(1, &framebufferID);
    GLState::BindFramebuffer(GL_FRAMEBUFFER, framebufferID);

    // 1. ???? textureAttrs ?????????? ID
    textureIDs.resize(attr.textureAttrs.size());
    if (!textureIDs.empty()) {
        glGenTextures(static_cast<GLsizei>(textureIDs.size()), textureIDs.data());
    }

    // 2. ???? textureAttrs ??????????????????
    std::vector<GLenum> colorAttachments;
    for (size_t i = 0; i < attr.textureAttrs.size(); ++i) {
        const auto& tAttr = attr.textureAttrs[i];
        GLuint texID = textureIDs[i];

        GLState::BindTexture(tAttr.target, texID);

        // ??????????? (MSAA)
        if (tAttr.target == GL_TEXTURE_2D_MULTISAMPLE) {
            glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 4, tAttr.internalFormat,
                width, height, GL_TRUE);
        }
        // ????????????? (ShadowBox)
        else if (tAttr.target == GL_TEXTURE_CUBE_MAP) {
            for (int j = 0; j < 6; ++j) {
                glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + j, 0, tAttr.internalFormat,
                    width, height, 0, tAttr.format, tAttr.type, NULL);
            }
            // ???????????????????ò????????????????????? mipmap ????????????? complete
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        }
        // ??????? 2D ??? (HDR, Defer, ShadowMap ??)
        else {
            glTexImage2D(GL_TEXTURE_2D, 0, tAttr.internalFormat, width, height, 0, tAttr.format, tAttr.type, NULL);

            // ??????????
            GLint filter = (attr.isShadowMap) ? GL_NEAREST : GL_LINEAR;
            glTexParameteri(tAttr.target, GL_TEXTURE_MIN_FILTER, filter);
            glTexParameteri(tAttr.target, GL_TEXTURE_MAG_FILTER, filter);

            if (attr.isShadowMap) {
                glTexParameteri(
                    GL_TEXTURE_2D,
                    GL_TEXTURE_COMPARE_MODE,
                    GL_NONE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
                glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);
            }
        }

        // 3. ???????????????
        if (attr.isShadowMap) {
            // ????????????????
            if (tAttr.target == GL_TEXTURE_CUBE_MAP) {
                // ????????????????? glFramebufferTexture ???????????? cubemap
                glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, texID, 0);
            }
            else {
                // ??? 2D ??????
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, tAttr.target, texID, 0);
            }
        } else {
            const GLenum attachment = GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i);
            glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, tAttr.target, texID, 0);
            colorAttachments.push_back(attachment);
        }
    }

    // Candidate path: replace the control's D24S8 renderbuffer with an
    // identically formatted D24S8 texture. The depth aspect is sampleable and
    // the stencil aspect remains available to existing stencil operations.
    if (attr.hasDepthTexture && !attr.isShadowMap) {
        glGenTextures(1, &depthTextureID);
        GLState::BindTexture(GL_TEXTURE_2D, depthTextureID);
        glTexImage2D(
            GL_TEXTURE_2D, 0,
            GL_DEPTH24_STENCIL8,
            width, height,
            0,
            GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8,
            NULL
        );
		// Match the control gPosition attachment's sampling state. SSAO samples
		// projected coordinates between pixel centers, so NEAREST would change
		// the guide algorithm in addition to changing position storage.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        // For sampling: don't use hardware depth compare mode
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glFramebufferTexture2D(
			GL_FRAMEBUFFER,
			GL_DEPTH_STENCIL_ATTACHMENT,
			GL_TEXTURE_2D,
			depthTextureID,
			0);
    }

    // 4. ???? DrawBuffers
    if (attr.isShadowMap) {
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
    }
    else {
        if (!colorAttachments.empty()) {
            glDrawBuffers(static_cast<GLsizei>(colorAttachments.size()), colorAttachments.data());
        }

		// Control path keeps the original D24S8 renderbuffer. Candidate already
		// attached its sampleable D24S8 texture above.
		if (!attr.hasDepthTexture) {
			glGenRenderbuffers(1, &rboID);
			glBindRenderbuffer(GL_RENDERBUFFER, rboID);
			if (attr.aaType == AntiAliasManager::AntiAliasType::MSAA) {
				glRenderbufferStorageMultisample(
					GL_RENDERBUFFER, 4,
					GL_DEPTH24_STENCIL8,
					width, height
				);
			}
			else {
				glRenderbufferStorage(
					GL_RENDERBUFFER,
					GL_DEPTH24_STENCIL8,
					width, height
				);
			}
			glFramebufferRenderbuffer(
				GL_FRAMEBUFFER,
				GL_DEPTH_STENCIL_ATTACHMENT,
				GL_RENDERBUFFER,
				rboID);
		}
    }

    const bool framebufferComplete =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (!framebufferComplete) {
        std::cerr << "ERROR::FRAMEBUFFER:: Framebuffer is not complete!" << std::endl;
    }

    GLState::BindFramebuffer(GL_FRAMEBUFFER, 0);
    this->init = framebufferComplete;
    m_trackedBytes = EstimateRenderTargetBytes(attr);
    PerformanceProfiler::GetInstance().RecordMemoryAllocation(
        MemoryResourceType::RenderTarget,
        m_trackedBytes);
}

FBO* FramebuffersManager::GetFBO(FBOAttributes attr) {
	if (m_hashMapFBO.find(attr) == m_hashMapFBO.end()) {
		m_hashMapFBO[attr] = {};
	}
	for (auto fboPtr : m_hashMapFBO[attr]) {
		if (!fboPtr->isBusy) {
			fboPtr->isBusy = true;
			return fboPtr;
		}
	}
	FBO* fboPtr = new FBO(attr);
	fboPtr->isBusy = true;
	m_hashMapFBO[attr].push_back(fboPtr);
	std::cout << "Add FBO??total::" << m_hashMapFBO[attr].size() << std::endl;
	return fboPtr;
}

unsigned int FramebuffersManager::GetShadowCompareSampler(
    bool linearFiltering) {
    unsigned int& sampler = linearFiltering
        ? m_shadowCompareLinearSampler
        : m_shadowCompareNearestSampler;
    if (sampler != 0) {
        return sampler;
    }

    glGenSamplers(1, &sampler);
    if (sampler == 0) {
        return 0;
    }
    const GLint filter = linearFiltering ? GL_LINEAR : GL_NEAREST;
    glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, filter);
    glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, filter);
    glSamplerParameteri(
        sampler,
        GL_TEXTURE_COMPARE_MODE,
        GL_COMPARE_REF_TO_TEXTURE);
    glSamplerParameteri(sampler, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    const GLfloat borderDepth[] = { 1.0f, 1.0f, 1.0f, 1.0f };
    glSamplerParameterfv(
        sampler,
        GL_TEXTURE_BORDER_COLOR,
        borderDepth);
    return sampler;
}

void FramebuffersManager::Resize() {
	for (auto& [attr, fbos] : m_hashMapFBO) {
		if (attr.isShadowMap) continue;
		for (auto& fbo : fbos) {
			fbo->Resize();
		}
	}
}

void FramebuffersManager::TrimUnusedFBOs() {
    for (auto mapIt = m_hashMapFBO.begin(); mapIt != m_hashMapFBO.end();) {
        auto& fbos = mapIt->second;
        for (auto fboIt = fbos.begin(); fboIt != fbos.end();) {
            FBO* fbo = *fboIt;
            if (fbo && fbo->isBusy) {
                ++fboIt;
                continue;
            }

            for (auto registryIt = m_fboMap.begin(); registryIt != m_fboMap.end();) {
                if (registryIt->second == fbo) {
                    registryIt = m_fboMap.erase(registryIt);
                }
                else {
                    ++registryIt;
                }
            }
            delete fbo;
            fboIt = fbos.erase(fboIt);
        }

        if (fbos.empty()) {
            mapIt = m_hashMapFBO.erase(mapIt);
        }
        else {
            ++mapIt;
        }
    }
}

void FramebuffersManager::Shutdown() {
    for (auto& entry : m_hashMapFBO) {
        for (FBO* fbo : entry.second) {
            delete fbo;
        }
    }
    m_hashMapFBO.clear();
    m_fboMap.clear();
    const unsigned int samplers[] = {
        m_shadowCompareNearestSampler,
        m_shadowCompareLinearSampler
    };
    for (unsigned int sampler : samplers) {
        if (sampler == 0) {
            continue;
        }
        GLState::ForgetSampler(sampler);
        glDeleteSamplers(1, &sampler);
    }
    m_shadowCompareNearestSampler = 0;
    m_shadowCompareLinearSampler = 0;
}

std::vector<FBO*> FramebuffersManager::GetBusyFBOs() const {
	std::vector<FBO*> out;
	for (const auto& [attr, fbos] : m_hashMapFBO) {
		for (FBO* fbo : fbos) {
			if (fbo && fbo->isBusy)
				out.push_back(fbo);
		}
	}
	return out;
}

std::string FramebuffersManager::GetFBODisplayName(const FBOAttributes& attr, int indexInList) {
	std::ostringstream ss;
	if (attr.isShadowMap) {
		if (attr.shadowType == FBOAttributes::ShadowBox)
			ss << "ShadowBox";
		else
			ss << "ShadowMap";
	} else if (attr.isDefer) {
		ss << "Defer";
	} else {
		ss << "Forward";
		if (attr.isHDR) ss << "+HDR";
		if (attr.isBloom) ss << "+Bloom";
		if (attr.isGamma) ss << "+Gamma";
		if (attr.aaType == AntiAliasManager::AntiAliasType::MSAA) ss << "+MSAA";
	}
	ss << " [" << indexInList << "]";
	return ss.str();
}
