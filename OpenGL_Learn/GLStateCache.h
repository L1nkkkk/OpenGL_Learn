#pragma once

#include <glad/glad.h>

namespace GLState {
	void Initialize();
	void Invalidate();
	void InvalidateRenderState();
	void InvalidateTextureState();
	void InvalidateBindingState();

	void Enable(GLenum capability);
	void Disable(GLenum capability);
	bool IsEnabled(GLenum capability);

	void DepthMask(bool enabled);
	bool GetDepthMask();
	void DepthFunc(GLenum function);

	void BlendFunc(GLenum source, GLenum destination);
	void GetBlendFunc(GLenum& source, GLenum& destination);

	void CullFace(GLenum mode);
	GLenum GetCullFace();

	void StencilMask(GLuint mask);
	void StencilFunc(GLenum function, GLint reference, GLuint mask);
	void StencilOp(GLenum stencilFail, GLenum depthFail, GLenum depthPass);
	void StencilOpSeparate(GLenum face, GLenum stencilFail, GLenum depthFail, GLenum depthPass);
	void ColorMask(bool red, bool green, bool blue, bool alpha);

	void ActiveTexture(GLenum textureUnit);
	unsigned int GetMaxFragmentTextureUnits();
	void BindTexture(GLenum target, GLuint texture);
	void BindSampler(GLuint textureUnit, GLuint sampler);
	void ForgetTexture(GLuint texture);
	void ForgetTextures(GLsizei count, const GLuint* textures);
	void ForgetSampler(GLuint sampler);
	void ForgetSamplers(GLsizei count, const GLuint* samplers);

	void BindVertexArray(GLuint vertexArray);
	void ForgetVertexArray(GLuint vertexArray);
	void ForgetVertexArrays(GLsizei count, const GLuint* vertexArrays);

	void BindFramebuffer(GLenum target, GLuint framebuffer);
	void ForgetFramebuffer(GLuint framebuffer);
	void ForgetFramebuffers(GLsizei count, const GLuint* framebuffers);
}
