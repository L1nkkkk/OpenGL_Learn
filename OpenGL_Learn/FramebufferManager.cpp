#include "Global.h"
#include <sstream>

void FBO::Init(FBOAttributes attr) {
    this->attr = attr;
    float borderColor[] = { 1.0f, 1.0f, 1.0f, 1.0f };

    glGenFramebuffers(1, &framebufferID);
    glBindFramebuffer(GL_FRAMEBUFFER, framebufferID);

    // 1. ???? textureAttrs ?????????? ID
    textureIDs.resize(attr.textureAttrs.size());
    if (!textureIDs.empty()) {
        glGenTextures(textureIDs.size(), textureIDs.data());
    }

    // 2. ???? textureAttrs ??????????????????
    std::vector<GLenum> colorAttachments;
    for (size_t i = 0; i < attr.textureAttrs.size(); ++i) {
        const auto& tAttr = attr.textureAttrs[i];
        GLuint texID = textureIDs[i];

        glBindTexture(tAttr.target, texID);

        // ??????????? (MSAA)
        if (tAttr.target == GL_TEXTURE_2D_MULTISAMPLE) {
            glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 4, tAttr.internalFormat,
                properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT, GL_TRUE);
        }
        // ????????????? (ShadowBox)
        else if (tAttr.target == GL_TEXTURE_CUBE_MAP) {
            for (int j = 0; j < 6; ++j) {
                glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + j, 0, tAttr.internalFormat,
                    properties.SHADOW_WIDTH, properties.SHADOW_HEIGHT, 0, tAttr.format, tAttr.type, NULL);
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
            int w = attr.isShadowMap ? properties.SHADOW_WIDTH : properties.SCREEN_WIDTH;
            int h = attr.isShadowMap ? properties.SHADOW_HEIGHT : properties.SCREEN_HEIGHT;

            glTexImage2D(GL_TEXTURE_2D, 0, tAttr.internalFormat, w, h, 0, tAttr.format, tAttr.type, NULL);

            // ??????????
            GLint filter = (attr.isShadowMap) ? GL_NEAREST : GL_LINEAR;
            glTexParameteri(tAttr.target, GL_TEXTURE_MIN_FILTER, filter);
            glTexParameteri(tAttr.target, GL_TEXTURE_MAG_FILTER, filter);

            if (attr.isShadowMap) {
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
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, tAttr.target, texID, 0);
            colorAttachments.push_back(GL_COLOR_ATTACHMENT0 + i);
        }
    }

    // 4. ???? DrawBuffers
    if (attr.isShadowMap) {
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
    }
    else {
        if (!colorAttachments.empty()) {
            glDrawBuffers(colorAttachments.size(), colorAttachments.data());
        }

        // 5. ??з??????????????? Depth/Stencil RBO
        glGenRenderbuffers(1, &rboID);
        glBindRenderbuffer(GL_RENDERBUFFER, rboID);
        if (attr.aaType == AntiAliasManager::AntiAliasType::MSAA) {
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_DEPTH24_STENCIL8, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT);
        }
        else {
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT);
        }
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rboID);
    }

    // ?????
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "ERROR::FRAMEBUFFER:: Framebuffer is not complete!" << std::endl;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    this->init = true;
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

void FramebuffersManager::Resize() {
	for (auto& [attr, fbos] : m_hashMapFBO) {
		if (attr.isShadowMap) continue;
		for (auto& fbo : fbos) {
			fbo->Resize();
		}
	}
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