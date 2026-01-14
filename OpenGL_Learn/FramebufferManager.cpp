#include "Global.h"

void FBO::Init(FBOAttributes attr) {
	float borderColor[] = { 1.0, 1.0, 1.0, 1.0 };
	glGenFramebuffers(1, &framebufferID);
	glBindFramebuffer(GL_FRAMEBUFFER, framebufferID);
	this->attr = attr;
	if (attr.isBloom) {
		textureIDs.resize(2);
	}
	else textureIDs.resize(1);
	//ÊÇ·ñÎªÒõÓ°ÌùÍ¼
	if (attr.isShadowMap) {
		glGenTextures(textureIDs.size(), textureIDs.data());
		switch (attr.shadowType) {
		case FBOAttributes::ShadowMap:
			glBindTexture(GL_TEXTURE_2D, textureIDs[0]);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, SHADOW_WIDTH, SHADOW_HEIGHT, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
			glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);
			glBindTexture(GL_TEXTURE_2D, 0);

			glBindFramebuffer(GL_FRAMEBUFFER, framebufferID);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, textureIDs[0], 0);
			glDrawBuffer(GL_NONE);
			glReadBuffer(GL_NONE);
			glBindFramebuffer(GL_FRAMEBUFFER, 0);

			if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
				std::cout << "ERROR::FRAMEBUFFER:: ShadowMap Framebuffer is not complete!" << std::endl;
			}
			break;
		case FBOAttributes::ShadowBox:
			glBindTexture(GL_TEXTURE_CUBE_MAP, textureIDs[0]);
			for (int i = 0; i < 6; ++i) {
				glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_DEPTH_COMPONENT, SHADOW_WIDTH, SHADOW_HEIGHT, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
			}
			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

			glBindFramebuffer(GL_FRAMEBUFFER, framebufferID);
			glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, textureIDs[0], 0);
			glDrawBuffer(GL_NONE);
			glReadBuffer(GL_NONE);
			glBindFramebuffer(GL_FRAMEBUFFER, 0);

			if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
				std::cout << "ERROR::FRAMEBUFFER:: ShadowBox Framebuffer is not complete!" << std::endl;
			}
			break;
		}
		this->init = true;
		return;
	}
	else {
		glGenTextures(textureIDs.size(),textureIDs.data());
		std::vector<GLenum> colorAttachments;
		colorAttachments.reserve(textureIDs.size());
		for (int i = 0; i < textureIDs.size(); ++i) {
			colorAttachments.push_back(GL_COLOR_ATTACHMENT0 + i);
		}
		switch (attr.aaType) {
		case AntiAliasManager::AntiAliasType::Default:
			for (int i = 0; i < textureIDs.size(); ++i) {
				glBindTexture(GL_TEXTURE_2D, textureIDs[i]);
				if (attr.isHDR)
					glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, SCREEN_WIDTH, SCREEN_HEIGHT, 0, GL_RGB, GL_FLOAT, NULL);
				else
					glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, SCREEN_WIDTH, SCREEN_HEIGHT, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);

				glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				glBindTexture(GL_TEXTURE_2D, 0);
				glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, textureIDs[i], 0);
			}
			glGenRenderbuffers(1, &rboID);
			glBindRenderbuffer(GL_RENDERBUFFER, rboID);
			glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, SCREEN_WIDTH, SCREEN_HEIGHT);
			glBindRenderbuffer(GL_RENDERBUFFER, 0);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rboID);

			if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
				std::cout << "ERROR::FRAMEBUFFER:: Framebuffer is not complete!" << std::endl;
			}
			break;
		case AntiAliasManager::AntiAliasType::MSAA:
			for (int i = 0; i < textureIDs.size(); ++i) {
				glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, textureIDs[i]);
				if (attr.isHDR)
					glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 4, GL_RGB16F, SCREEN_WIDTH, SCREEN_HEIGHT, GL_TRUE);
				else
					glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 4, GL_RGB, SCREEN_WIDTH, SCREEN_HEIGHT, GL_TRUE);
				glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
				glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D_MULTISAMPLE, textureIDs[i], 0);
			}
			glGenRenderbuffers(1, &rboID);
			glBindRenderbuffer(GL_RENDERBUFFER, rboID);
			glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_DEPTH24_STENCIL8, SCREEN_WIDTH, SCREEN_HEIGHT);
			glBindRenderbuffer(GL_RENDERBUFFER, 0);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rboID);

			if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
				std::cout << "ERROR::FRAMEBUFFER:: Framebuffer is not complete!" << std::endl;
			}
			break;
		}
		glDrawBuffers(colorAttachments.size(), colorAttachments.data());
		this->init = true;
		return;
	}
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
	std::cout << "Add FBO£¬total::" << m_hashMapFBO[attr].size() << std::endl;
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