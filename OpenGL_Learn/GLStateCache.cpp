#include "GLStateCache.h"

#include "Profiler.h"

#include <algorithm>
#include <array>
#include <vector>

namespace {
	template <typename T>
	struct CachedValue {
		T value{};
		bool valid = false;
	};

	struct TextureUnitState {
		CachedValue<GLuint> texture2D;
		CachedValue<GLuint> textureCube;
		CachedValue<GLuint> texture2DMultisample;
		CachedValue<GLuint> sampler;
	};

	struct StateCache {
		bool initialized = false;

		CachedValue<bool> depthTest;
		CachedValue<bool> stencilTest;
		CachedValue<bool> blend;
		CachedValue<bool> cullFace;
		CachedValue<bool> depthWrite;
		CachedValue<GLenum> depthFunction;
		CachedValue<std::array<GLenum, 2>> blendFunction;
		CachedValue<GLenum> cullMode;
		CachedValue<GLuint> stencilWriteMask;
		CachedValue<std::array<GLint, 3>> stencilFunction;
		CachedValue<std::array<GLenum, 3>> stencilOperationFront;
		CachedValue<std::array<GLenum, 3>> stencilOperationBack;
		CachedValue<std::array<GLboolean, 4>> colorWriteMask;

		CachedValue<unsigned int> activeTextureUnit;
		unsigned int maxFragmentTextureUnits = 1;
		std::vector<TextureUnitState> textureUnits;
		CachedValue<GLuint> vertexArray;
		CachedValue<GLuint> drawFramebuffer;
		CachedValue<GLuint> readFramebuffer;
	};

	StateCache& Cache()
	{
		static StateCache cache;
		return cache;
	}

	void RecordRenderState(bool cacheHit)
	{
		PerformanceProfiler::GetInstance().RecordRenderStateChange(cacheHit);
	}

	void RecordTextureState(bool cacheHit)
	{
		PerformanceProfiler::GetInstance().RecordTextureStateChange(cacheHit);
	}

	void RecordVertexArrayBind(bool cacheHit)
	{
		PerformanceProfiler::GetInstance().RecordVertexArrayBind(cacheHit);
	}

	void RecordFramebufferBind(bool cacheHit)
	{
		PerformanceProfiler::GetInstance().RecordFramebufferBind(cacheHit);
	}

	CachedValue<bool>* FindCapability(GLenum capability)
	{
		auto& cache = Cache();
		switch (capability) {
		case GL_DEPTH_TEST:
			return &cache.depthTest;
		case GL_STENCIL_TEST:
			return &cache.stencilTest;
		case GL_BLEND:
			return &cache.blend;
		case GL_CULL_FACE:
			return &cache.cullFace;
		default:
			return nullptr;
		}
	}

	CachedValue<GLuint>* FindTextureBinding(TextureUnitState& unit, GLenum target)
	{
		switch (target) {
		case GL_TEXTURE_2D:
			return &unit.texture2D;
		case GL_TEXTURE_CUBE_MAP:
			return &unit.textureCube;
		case GL_TEXTURE_2D_MULTISAMPLE:
			return &unit.texture2DMultisample;
		default:
			return nullptr;
		}
	}

	void EnsureInitialized()
	{
		auto& cache = Cache();
		if (cache.initialized) {
			return;
		}

		GLint maxTextureUnits = 1;
		glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &maxTextureUnits);
		cache.textureUnits.resize(static_cast<std::size_t>((std::max)(1, maxTextureUnits)));
		GLint maxFragmentTextureUnits = 1;
		glGetIntegerv(
			GL_MAX_TEXTURE_IMAGE_UNITS,
			&maxFragmentTextureUnits);
		cache.maxFragmentTextureUnits =
			static_cast<unsigned int>(
				(std::max)(1, maxFragmentTextureUnits));

		GLint activeTexture = GL_TEXTURE0;
		glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
		cache.activeTextureUnit.value = static_cast<unsigned int>(activeTexture - GL_TEXTURE0);
		cache.activeTextureUnit.valid = true;
		cache.initialized = true;
	}

	void SetSamplerBinding(GLuint textureUnit, GLuint sampler)
	{
		EnsureInitialized();
		auto& cache = Cache();
		if (textureUnit >= cache.textureUnits.size()) {
			RecordTextureState(false);
			return;
		}

		auto& cached = cache.textureUnits[textureUnit].sampler;
		if (cached.valid && cached.value == sampler) {
			RecordTextureState(true);
			return;
		}

		glBindSampler(textureUnit, sampler);
		cached.value = sampler;
		cached.valid = true;
		RecordTextureState(false);
	}

	void SetCapability(GLenum capability, bool enabled)
	{
		EnsureInitialized();
		if (auto* cached = FindCapability(capability)) {
			if (cached->valid && cached->value == enabled) {
				RecordRenderState(true);
				return;
			}

			if (enabled) {
				glEnable(capability);
			}
			else {
				glDisable(capability);
			}
			cached->value = enabled;
			cached->valid = true;
			RecordRenderState(false);
			return;
		}

		if (enabled) {
			glEnable(capability);
		}
		else {
			glDisable(capability);
		}
		RecordRenderState(false);
	}
}

void GLState::Initialize()
{
	auto& cache = Cache();
	cache = {};
	EnsureInitialized();
}

void GLState::Invalidate()
{
	InvalidateRenderState();
	InvalidateTextureState();
	InvalidateBindingState();
}

void GLState::InvalidateRenderState()
{
	auto& cache = Cache();
	cache.depthTest.valid = false;
	cache.stencilTest.valid = false;
	cache.blend.valid = false;
	cache.cullFace.valid = false;
	cache.depthWrite.valid = false;
	cache.depthFunction.valid = false;
	cache.blendFunction.valid = false;
	cache.cullMode.valid = false;
	cache.stencilWriteMask.valid = false;
	cache.stencilFunction.valid = false;
	cache.stencilOperationFront.valid = false;
	cache.stencilOperationBack.valid = false;
	cache.colorWriteMask.valid = false;
}

void GLState::InvalidateTextureState()
{
	auto& cache = Cache();
	cache.activeTextureUnit.valid = false;
	for (auto& unit : cache.textureUnits) {
		unit.texture2D.valid = false;
		unit.textureCube.valid = false;
		unit.texture2DMultisample.valid = false;
		unit.sampler.valid = false;
	}
}

void GLState::InvalidateBindingState()
{
	auto& cache = Cache();
	cache.vertexArray.valid = false;
	cache.drawFramebuffer.valid = false;
	cache.readFramebuffer.valid = false;
}

void GLState::Enable(GLenum capability)
{
	SetCapability(capability, true);
}

void GLState::Disable(GLenum capability)
{
	SetCapability(capability, false);
}

bool GLState::IsEnabled(GLenum capability)
{
	EnsureInitialized();
	if (auto* cached = FindCapability(capability)) {
		if (!cached->valid) {
			cached->value = glIsEnabled(capability) == GL_TRUE;
			cached->valid = true;
			PerformanceProfiler::GetInstance().RecordRenderStateQuery();
		}
		return cached->value;
	}

	PerformanceProfiler::GetInstance().RecordRenderStateQuery();
	return glIsEnabled(capability) == GL_TRUE;
}

void GLState::DepthMask(bool enabled)
{
	EnsureInitialized();
	auto& cached = Cache().depthWrite;
	if (cached.valid && cached.value == enabled) {
		RecordRenderState(true);
		return;
	}

	glDepthMask(enabled ? GL_TRUE : GL_FALSE);
	cached.value = enabled;
	cached.valid = true;
	RecordRenderState(false);
}

bool GLState::GetDepthMask()
{
	EnsureInitialized();
	auto& cached = Cache().depthWrite;
	if (!cached.valid) {
		GLboolean value = GL_TRUE;
		glGetBooleanv(GL_DEPTH_WRITEMASK, &value);
		cached.value = value == GL_TRUE;
		cached.valid = true;
		PerformanceProfiler::GetInstance().RecordRenderStateQuery();
	}
	return cached.value;
}

void GLState::DepthFunc(GLenum function)
{
	EnsureInitialized();
	auto& cached = Cache().depthFunction;
	if (cached.valid && cached.value == function) {
		RecordRenderState(true);
		return;
	}

	glDepthFunc(function);
	cached.value = function;
	cached.valid = true;
	RecordRenderState(false);
}

void GLState::BlendFunc(GLenum source, GLenum destination)
{
	EnsureInitialized();
	auto& cached = Cache().blendFunction;
	const std::array<GLenum, 2> value = { source, destination };
	if (cached.valid && cached.value == value) {
		RecordRenderState(true);
		return;
	}

	glBlendFunc(source, destination);
	cached.value = value;
	cached.valid = true;
	RecordRenderState(false);
}

void GLState::GetBlendFunc(GLenum& source, GLenum& destination)
{
	EnsureInitialized();
	auto& cached = Cache().blendFunction;
	if (!cached.valid) {
		GLint sourceValue = GL_ONE;
		GLint destinationValue = GL_ZERO;
		glGetIntegerv(GL_BLEND_SRC_RGB, &sourceValue);
		glGetIntegerv(GL_BLEND_DST_RGB, &destinationValue);
		cached.value = {
			static_cast<GLenum>(sourceValue),
			static_cast<GLenum>(destinationValue)
		};
		cached.valid = true;
		PerformanceProfiler::GetInstance().RecordRenderStateQuery();
		PerformanceProfiler::GetInstance().RecordRenderStateQuery();
	}
	source = cached.value[0];
	destination = cached.value[1];
}

void GLState::CullFace(GLenum mode)
{
	EnsureInitialized();
	auto& cached = Cache().cullMode;
	if (cached.valid && cached.value == mode) {
		RecordRenderState(true);
		return;
	}

	glCullFace(mode);
	cached.value = mode;
	cached.valid = true;
	RecordRenderState(false);
}

GLenum GLState::GetCullFace()
{
	EnsureInitialized();
	auto& cached = Cache().cullMode;
	if (!cached.valid) {
		GLint value = GL_BACK;
		glGetIntegerv(GL_CULL_FACE_MODE, &value);
		cached.value = static_cast<GLenum>(value);
		cached.valid = true;
		PerformanceProfiler::GetInstance().RecordRenderStateQuery();
	}
	return cached.value;
}

void GLState::StencilMask(GLuint mask)
{
	EnsureInitialized();
	auto& cached = Cache().stencilWriteMask;
	if (cached.valid && cached.value == mask) {
		RecordRenderState(true);
		return;
	}

	glStencilMask(mask);
	cached.value = mask;
	cached.valid = true;
	RecordRenderState(false);
}

void GLState::StencilFunc(GLenum function, GLint reference, GLuint mask)
{
	EnsureInitialized();
	auto& cached = Cache().stencilFunction;
	const std::array<GLint, 3> value = {
		static_cast<GLint>(function),
		reference,
		static_cast<GLint>(mask)
	};
	if (cached.valid && cached.value == value) {
		RecordRenderState(true);
		return;
	}

	glStencilFunc(function, reference, mask);
	cached.value = value;
	cached.valid = true;
	RecordRenderState(false);
}

void GLState::StencilOp(GLenum stencilFail, GLenum depthFail, GLenum depthPass)
{
	EnsureInitialized();
	auto& cache = Cache();
	const std::array<GLenum, 3> value = { stencilFail, depthFail, depthPass };
	if (cache.stencilOperationFront.valid &&
		cache.stencilOperationBack.valid &&
		cache.stencilOperationFront.value == value &&
		cache.stencilOperationBack.value == value) {
		RecordRenderState(true);
		return;
	}

	glStencilOp(stencilFail, depthFail, depthPass);
	cache.stencilOperationFront.value = value;
	cache.stencilOperationFront.valid = true;
	cache.stencilOperationBack.value = value;
	cache.stencilOperationBack.valid = true;
	RecordRenderState(false);
}

void GLState::StencilOpSeparate(GLenum face, GLenum stencilFail, GLenum depthFail, GLenum depthPass)
{
	EnsureInitialized();
	auto& cache = Cache();
	const std::array<GLenum, 3> value = { stencilFail, depthFail, depthPass };
	CachedValue<std::array<GLenum, 3>>* cached = nullptr;
	if (face == GL_FRONT) {
		cached = &cache.stencilOperationFront;
	}
	else if (face == GL_BACK) {
		cached = &cache.stencilOperationBack;
	}

	if (cached && cached->valid && cached->value == value) {
		RecordRenderState(true);
		return;
	}

	glStencilOpSeparate(face, stencilFail, depthFail, depthPass);
	if (cached) {
		cached->value = value;
		cached->valid = true;
	}
	else {
		cache.stencilOperationFront.valid = false;
		cache.stencilOperationBack.valid = false;
	}
	RecordRenderState(false);
}

void GLState::ColorMask(bool red, bool green, bool blue, bool alpha)
{
	EnsureInitialized();
	auto& cached = Cache().colorWriteMask;
	const std::array<GLboolean, 4> value = {
		static_cast<GLboolean>(red ? GL_TRUE : GL_FALSE),
		static_cast<GLboolean>(green ? GL_TRUE : GL_FALSE),
		static_cast<GLboolean>(blue ? GL_TRUE : GL_FALSE),
		static_cast<GLboolean>(alpha ? GL_TRUE : GL_FALSE)
	};
	if (cached.valid && cached.value == value) {
		RecordRenderState(true);
		return;
	}

	glColorMask(value[0], value[1], value[2], value[3]);
	cached.value = value;
	cached.valid = true;
	RecordRenderState(false);
}

void GLState::ActiveTexture(GLenum textureUnit)
{
	EnsureInitialized();
	auto& cache = Cache();
	const unsigned int unit = static_cast<unsigned int>(textureUnit - GL_TEXTURE0);
	if (unit >= cache.textureUnits.size()) {
		RecordTextureState(false);
		return;
	}
	if (cache.activeTextureUnit.valid && cache.activeTextureUnit.value == unit) {
		RecordTextureState(true);
		return;
	}

	glActiveTexture(textureUnit);
	cache.activeTextureUnit.value = unit;
	cache.activeTextureUnit.valid = true;
	RecordTextureState(false);
}

unsigned int GLState::GetMaxFragmentTextureUnits()
{
	EnsureInitialized();
	return Cache().maxFragmentTextureUnits;
}

void GLState::BindTexture(GLenum target, GLuint texture)
{
	EnsureInitialized();
	auto& cache = Cache();
	if (!cache.activeTextureUnit.valid) {
		GLint activeTexture = GL_TEXTURE0;
		glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
		cache.activeTextureUnit.value = static_cast<unsigned int>(activeTexture - GL_TEXTURE0);
		cache.activeTextureUnit.valid = true;
		PerformanceProfiler::GetInstance().RecordRenderStateQuery();
	}

	if (cache.activeTextureUnit.value >= cache.textureUnits.size()) {
		glBindTexture(target, texture);
		RecordTextureState(false);
		return;
	}

	// Generic texture binds use the texture object's own sampling state. Clear
	// any specialized sampler left by a previous shadow comparison binding.
	auto& sampler =
		cache.textureUnits[cache.activeTextureUnit.value].sampler;
	if (!sampler.valid || sampler.value != 0) {
		SetSamplerBinding(cache.activeTextureUnit.value, 0);
	}

	auto& unit = cache.textureUnits[cache.activeTextureUnit.value];
	if (auto* cached = FindTextureBinding(unit, target)) {
		if (cached->valid && cached->value == texture) {
			RecordTextureState(true);
			return;
		}

		glBindTexture(target, texture);
		cached->value = texture;
		cached->valid = true;
		RecordTextureState(false);
		return;
	}

	glBindTexture(target, texture);
	RecordTextureState(false);
}

void GLState::BindSampler(GLuint textureUnit, GLuint sampler)
{
	SetSamplerBinding(textureUnit, sampler);
}

void GLState::ForgetTexture(GLuint texture)
{
	if (texture == 0) {
		return;
	}

	auto& cache = Cache();
	for (auto& unit : cache.textureUnits) {
		for (CachedValue<GLuint>* binding : {
			&unit.texture2D,
			&unit.textureCube,
			&unit.texture2DMultisample }) {
			if (binding->valid && binding->value == texture) {
				binding->value = 0;
			}
		}
	}
}

void GLState::ForgetTextures(GLsizei count, const GLuint* textures)
{
	if (!textures) {
		return;
	}
	for (GLsizei i = 0; i < count; ++i) {
		ForgetTexture(textures[i]);
	}
}

void GLState::ForgetSampler(GLuint sampler)
{
	if (sampler == 0) {
		return;
	}

	auto& cache = Cache();
	for (auto& unit : cache.textureUnits) {
		if (unit.sampler.valid && unit.sampler.value == sampler) {
			unit.sampler.value = 0;
		}
	}
}

void GLState::ForgetSamplers(GLsizei count, const GLuint* samplers)
{
	if (!samplers) {
		return;
	}
	for (GLsizei i = 0; i < count; ++i) {
		ForgetSampler(samplers[i]);
	}
}

void GLState::BindVertexArray(GLuint vertexArray)
{
	EnsureInitialized();
	auto& cached = Cache().vertexArray;
	if (cached.valid && cached.value == vertexArray) {
		RecordVertexArrayBind(true);
		return;
	}

	glBindVertexArray(vertexArray);
	cached.value = vertexArray;
	cached.valid = true;
	RecordVertexArrayBind(false);
}

void GLState::ForgetVertexArray(GLuint vertexArray)
{
	if (vertexArray == 0) {
		return;
	}

	auto& cached = Cache().vertexArray;
	if (cached.valid && cached.value == vertexArray) {
		cached.value = 0;
	}
}

void GLState::ForgetVertexArrays(GLsizei count, const GLuint* vertexArrays)
{
	if (!vertexArrays) {
		return;
	}
	for (GLsizei i = 0; i < count; ++i) {
		ForgetVertexArray(vertexArrays[i]);
	}
}

void GLState::BindFramebuffer(GLenum target, GLuint framebuffer)
{
	EnsureInitialized();
	auto& cache = Cache();

	switch (target) {
	case GL_FRAMEBUFFER:
		if (cache.drawFramebuffer.valid &&
			cache.readFramebuffer.valid &&
			cache.drawFramebuffer.value == framebuffer &&
			cache.readFramebuffer.value == framebuffer) {
			RecordFramebufferBind(true);
			return;
		}
		glBindFramebuffer(target, framebuffer);
		cache.drawFramebuffer.value = framebuffer;
		cache.drawFramebuffer.valid = true;
		cache.readFramebuffer.value = framebuffer;
		cache.readFramebuffer.valid = true;
		break;
	case GL_DRAW_FRAMEBUFFER:
		if (cache.drawFramebuffer.valid && cache.drawFramebuffer.value == framebuffer) {
			RecordFramebufferBind(true);
			return;
		}
		glBindFramebuffer(target, framebuffer);
		cache.drawFramebuffer.value = framebuffer;
		cache.drawFramebuffer.valid = true;
		break;
	case GL_READ_FRAMEBUFFER:
		if (cache.readFramebuffer.valid && cache.readFramebuffer.value == framebuffer) {
			RecordFramebufferBind(true);
			return;
		}
		glBindFramebuffer(target, framebuffer);
		cache.readFramebuffer.value = framebuffer;
		cache.readFramebuffer.valid = true;
		break;
	default:
		glBindFramebuffer(target, framebuffer);
		cache.drawFramebuffer.valid = false;
		cache.readFramebuffer.valid = false;
		break;
	}

	RecordFramebufferBind(false);
}

void GLState::ForgetFramebuffer(GLuint framebuffer)
{
	if (framebuffer == 0) {
		return;
	}

	auto& cache = Cache();
	if (cache.drawFramebuffer.valid && cache.drawFramebuffer.value == framebuffer) {
		cache.drawFramebuffer.value = 0;
	}
	if (cache.readFramebuffer.valid && cache.readFramebuffer.value == framebuffer) {
		cache.readFramebuffer.value = 0;
	}
}

void GLState::ForgetFramebuffers(GLsizei count, const GLuint* framebuffers)
{
	if (!framebuffers) {
		return;
	}
	for (GLsizei i = 0; i < count; ++i) {
		ForgetFramebuffer(framebuffers[i]);
	}
}
