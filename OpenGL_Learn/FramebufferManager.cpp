#include "Global.h"

void FBO::Init(FBOAttributes attr) {
    this->attr = attr;
    float borderColor[] = { 1.0f, 1.0f, 1.0f, 1.0f };

    glGenFramebuffers(1, &framebufferID);
    glBindFramebuffer(GL_FRAMEBUFFER, framebufferID);

    // 1. 根据 textureAttrs 的数量分配 ID
    textureIDs.resize(attr.textureAttrs.size());
    if (!textureIDs.empty()) {
        glGenTextures(textureIDs.size(), textureIDs.data());
    }

    // 2. 遍历 textureAttrs 进行数据驱动的初始化
    std::vector<GLenum> colorAttachments;
    for (size_t i = 0; i < attr.textureAttrs.size(); ++i) {
        const auto& tAttr = attr.textureAttrs[i];
        GLuint texID = textureIDs[i];

        glBindTexture(tAttr.target, texID);

        // 处理多重采样 (MSAA)
        if (tAttr.target == GL_TEXTURE_2D_MULTISAMPLE) {
            glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 4, tAttr.internalFormat,
                properties.SCREEN_WIDTH, properties.SCREEN_HEIGHT, GL_TRUE);
        }
        // 处理立方体贴图 (ShadowBox)
        else if (tAttr.target == GL_TEXTURE_CUBE_MAP) {
            for (int j = 0; j < 6; ++j) {
                glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + j, 0, tAttr.internalFormat,
                    properties.SHADOW_WIDTH, properties.SHADOW_HEIGHT, 0, tAttr.format, tAttr.type, NULL);
            }
            // 为深度立方体阴影贴图配置采样器，否则由于默认使用 mipmap 过滤会导致纹理不 complete
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        }
        // 处理普通 2D 贴图 (HDR, Defer, ShadowMap 等)
        else {
            int w = attr.isShadowMap ? properties.SHADOW_WIDTH : properties.SCREEN_WIDTH;
            int h = attr.isShadowMap ? properties.SHADOW_HEIGHT : properties.SCREEN_HEIGHT;

            glTexImage2D(GL_TEXTURE_2D, 0, tAttr.internalFormat, w, h, 0, tAttr.format, tAttr.type, NULL);

            // 采样器设置
            GLint filter = (attr.isShadowMap) ? GL_NEAREST : GL_LINEAR;
            glTexParameteri(tAttr.target, GL_TEXTURE_MIN_FILTER, filter);
            glTexParameteri(tAttr.target, GL_TEXTURE_MAG_FILTER, filter);

            if (attr.isShadowMap) {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
                glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);
            }
        }

        // 3. 将贴图挂载到帧缓冲
        if (attr.isShadowMap) {
            // 阴影贴图只需要深度附件
            if (tAttr.target == GL_TEXTURE_CUBE_MAP) {
                // 立方体深度贴图：使用 glFramebufferTexture 一次性附加整个 cubemap
                glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, texID, 0);
            }
            else {
                // 普通 2D 深度贴图
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, tAttr.target, texID, 0);
            }
        } else {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, tAttr.target, texID, 0);
            colorAttachments.push_back(GL_COLOR_ATTACHMENT0 + i);
        }
    }

    // 4. 配置 DrawBuffers
    if (attr.isShadowMap) {
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
    }
    else {
        if (!colorAttachments.empty()) {
            glDrawBuffers(colorAttachments.size(), colorAttachments.data());
        }

        // 5. 只有非阴影贴图通常才需要 Depth/Stencil RBO
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

    // 检查状态
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
	std::cout << "Add FBO，total::" << m_hashMapFBO[attr].size() << std::endl;
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