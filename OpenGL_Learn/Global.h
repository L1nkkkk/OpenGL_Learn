#pragma once
#include "GLStateCache.h"
#include "stb_image.h"
#include <iostream>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <string>
#include <cstdint>
#include <array>
#include <unordered_map>
#include <vector>
#include <functional>
#include <tuple>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace ShadowProperty {
    enum ShadowType {
        Default = 0,
        PCF,
        PCSS,
    };
    enum SamplingPattern {
        LegacyRandom = 0,
        StableVogel,
    };
    inline const char* ShadowTypeStrs[] = {
        "Hard",
        "Stable Vogel PCF",
        "Stable Vogel PCSS",
    };
}

namespace ShadowOptimization {
    enum Flag {
        ExactEarlyOut = 1 << 0,
        AdaptivePointSamples = 1 << 1,
        AdaptivePcssFilter = 1 << 2,
        StagedPcssBlocker = 1 << 3,
        HardwareDepthCompare = 1 << 4,
        HardwareLinearPcf = 1 << 5,
        HardwareReducedPcf = 1 << 6,
        TexelScaledBias = 1 << 7,
        SpotRadialBiasDirection = 1 << 8,
        SpotPcssLinearDepth = 1 << 9,
        SpotPcssReducedFilter = 1 << 10,
        SpotCasterDepthFit = 1 << 11,
        PreparedPointInputs = 1 << 12,
    };
}

namespace SSAOProperty {
    enum Mode {
        LegacyFull = 0,
        HalfRaw,
        HalfBilateral,
    };

    inline const char* ModeName(int mode) {
        switch (mode) {
        case LegacyFull:
            return "legacy-full";
        case HalfRaw:
            return "half-raw";
        case HalfBilateral:
            return "half-bilateral";
        default:
            return "unknown";
        }
    }
}

namespace GBufferPositionProperty {
    enum Mode {
        Explicit = 0,
        ReconstructFromDepth,
    };

    inline const char* ModeName(int mode) {
        switch (mode) {
        case Explicit:
            return "explicit";
        case ReconstructFromDepth:
            return "reconstruct";
        default:
            return "unknown";
        }
    }
}

namespace PointLightStencilClearProperty {
    enum Mode {
        Legacy2N = 0,
        CoalescedNPlusOne,
    };

    inline const char* ModeName(int mode) {
        switch (mode) {
        case Legacy2N:
            return "legacy-2n";
        case CoalescedNPlusOne:
            return "coalesced-n-plus-one";
        default:
            return "unknown";
        }
    }
}

namespace PointLightRenderProperty {
    enum Mode {
        CoalescedVolume = 0,
        BoundsVolume,
        ScissoredVolume,
        AnalyticVolumeFull,
        AnalyticVolume,
        AnalyticScreen,
        AnalyticFullscreen,
        Tile16,
        Cluster16,
    };

    inline const char* ModeName(int mode) {
        switch (mode) {
        case CoalescedVolume:
            return "coalesced-volume";
        case BoundsVolume:
            return "bounds-volume";
        case ScissoredVolume:
            return "scissored-volume";
        case AnalyticVolumeFull:
            return "analytic-volume-full";
        case AnalyticVolume:
            return "analytic-volume";
        case AnalyticScreen:
            return "analytic-screen";
        case AnalyticFullscreen:
            return "analytic-fullscreen";
        case Tile16:
            return "tile16";
        case Cluster16:
            return "cluster16";
        default:
            return "unknown";
        }
    }

    inline bool RequiresBounds(int mode) {
        return mode != CoalescedVolume &&
            mode != Tile16 && mode != Cluster16;
    }

    inline bool UsesScreenDraw(int mode) {
        return mode == AnalyticScreen || mode == AnalyticFullscreen ||
            mode == Tile16 || mode == Cluster16;
    }

    inline bool UsesGrid(int mode) {
        return mode == Tile16 || mode == Cluster16;
    }

    inline bool UsesAnalyticVolume(int mode) {
        return mode == AnalyticVolumeFull || mode == AnalyticVolume;
    }

    inline bool UsesScissor(int mode) {
        return mode == ScissoredVolume ||
            mode == AnalyticVolume ||
            mode == AnalyticScreen;
    }
}

namespace PointLightGridUpdateProperty {
    enum Mode {
        Cached = 0,
        RebuildEveryFrame,
    };

    inline const char* ModeName(int mode) {
        switch (mode) {
        case Cached:
            return "cached";
        case RebuildEveryFrame:
            return "rebuild";
        default:
            return "unknown";
        }
    }
}

struct PointLightGridTelemetry {
    bool valid = false;
    bool clustered = false;
    bool rebuiltThisFrame = false;
    bool cacheHit = false;
    bool overflow = false;
    int tileSize = 16;
    int sliceCount = 1;
    int tilesX = 0;
    int tilesY = 0;
    std::uint64_t logicalCells = 0;
    std::uint64_t nonEmptyCells = 0;
    std::uint64_t lightCount = 0;
    std::uint64_t totalIndices = 0;
    std::uint64_t maximumLightsPerCell = 0;
    double averageLightsPerCell = 0.0;
    std::uint64_t metadataBytes = 0;
    std::uint64_t indexBytes = 0;
    std::uint64_t lightBytes = 0;
    std::uint64_t residentBytes = 0;
    std::uint64_t uploadedBytesThisFrame = 0;
    std::uint64_t buildCount = 0;
    std::uint64_t uploadCount = 0;
    std::uint64_t cacheHitCount = 0;
    std::uint64_t inputSignature = 0;
    std::uint64_t csrSignature = 0;
    int maxTextureBufferTexels = 0;
    std::string error;
};

namespace PointLightScreenProxyProperty {
    enum Classification {
        Outside = 0,
        ConservativeRect,
        FullscreenFallback,
    };

    enum FallbackReason {
        None = 0,
        CameraInside,
        NearPlaneIntersection,
        InvalidRadius,
        InvalidProjection,
    };

    inline const char* ClassificationName(int classification) {
        switch (classification) {
        case Outside:
            return "outside";
        case ConservativeRect:
            return "conservative-rect";
        case FullscreenFallback:
            return "fullscreen-fallback";
        default:
            return "unknown";
        }
    }

    inline const char* FallbackReasonName(int reason) {
        switch (reason) {
        case None:
            return "none";
        case CameraInside:
            return "camera-inside";
        case NearPlaneIntersection:
            return "near-plane-intersection";
        case InvalidRadius:
            return "invalid-radius";
        case InvalidProjection:
            return "invalid-projection";
        default:
            return "unknown";
        }
    }
}

struct PointLightScreenProxyTelemetry {
    std::uint64_t stableLightId = 0;
    std::uint64_t sourceIndex = 0;
    float radius = 0.0f;
    int classification = PointLightScreenProxyProperty::Outside;
    int fallbackReason = PointLightScreenProxyProperty::None;
    int rectX = 0;
    int rectY = 0;
    int rectWidth = 0;
    int rectHeight = 0;
    double coverageRatio = 0.0;
};

// Viewport ??????? FramebuffersManager ????? isBusy ?? FBO ???????????????? color/depth ??????

class SystemProperties {
public:
    SystemProperties(const SystemProperties&) = delete;
    SystemProperties& operator=(const SystemProperties&) = delete;
    SystemProperties(SystemProperties&&) = delete;
    SystemProperties& operator=(SystemProperties&&) = delete;

    static SystemProperties& GetInstance() {
        static SystemProperties instance;
        return instance;
    }

    bool DEBUG_MODE = false;
    bool AUTO_RELOAD_SHADERS = true;
    bool AUTO_RELOAD_MATERIALS = true;
    float HOT_RELOAD_POLL_INTERVAL = 0.25f;

    // Viewport ?????????? FBO ?? GetBusyFBOs() ??????????????????? FBO textureIDs ????????
    int VIEWPORT_DEBUG_FBO_INDEX = 0;
    int VIEWPORT_DEBUG_ATTACHMENT_INDEX = 0;

    int SCREEN_WIDTH = 1440;
    int SCREEN_HEIGHT = 900;

    int USED_TEXTURE_NUM = 0;

    int SHADOW_WIDTH = 1024;
    int SHADOW_HEIGHT = 1024;
    bool SHADOW_MAP_SHOW = false;
    int SHADOW_PCF_SAMPLE_NUM = 16;
    int SHADOW_PCF_RING_NUM = 10;
    int SHADOW_TYPE = ShadowProperty::Default;
    int SHADOW_SAMPLING_PATTERN = ShadowProperty::StableVogel;
    // Production uses the revision/dirty shadow-cache check. Set
    // OPENGL_LEARN_SHADOW_CACHE=none before process launch to force the
    // historical redraw-every-frame behavior while retaining the current
    // renderer, shaders, targets, and correctness instrumentation.
    bool SHADOW_CACHE_DISABLED = false;
    // Set
    // OPENGL_LEARN_SHADOW_CACHE=legacy before process launch to retain the
    // original deep-signature path for controlled same-binary A/B runs.
    bool SHADOW_CACHE_USE_LEGACY_SIGNATURE = false;
    // The revision path uses independent per-light caches by default.
    // OPENGL_LEARN_SHADOW_PER_LIGHT_CACHE=0 retains the global revision cache
    // as the same-binary control path.
    bool SHADOW_PER_LIGHT_CACHE = true;
    // Experimental third-stage cache: classify the current shadow-caster
    // state against each light projection instead of folding the same global
    // caster revision into every light. Auto-fit projections retain their
    // conservative global dependency until their fit is made local.
    bool SHADOW_SPATIAL_CASTER_CACHE = false;
    // Build a per-Mesh shadow submission list and reject casters outside each
    // light's projection. Set OPENGL_LEARN_SHADOW_CASTER_CULLING=0 for the
    // original all-caster submission path in controlled A/B runs.
    bool SHADOW_CASTER_CULLING = true;
    // The production candidate can fit directional shadows to the complete
    // caster AABB in light space. Keep the historical sphere fit available
    // through OPENGL_LEARN_DIRECTIONAL_SHADOW_FIT=sphere for same-binary A/B.
    bool DIRECTIONAL_SHADOW_LIGHT_AABB_FIT = false;
    // With the light-space AABB fit, preserve the historical sphere fit's
    // world-units-per-texel while selecting a smaller quantized square target.
    // OPENGL_LEARN_DIRECTIONAL_SHADOW_RESOLUTION=density enables it.
    bool DIRECTIONAL_SHADOW_DENSITY_RESOLUTION = false;
    // Skip shadow work only when the lighting contribution is provably zero.
    // OPENGL_LEARN_SHADOW_EXACT_EARLY_OUT=0 retains the control path.
    bool SHADOW_EXACT_EARLY_OUT = true;
    // Experimental reuse of point-light direction, distance, and N dot L
    // already computed by PBR lighting. It remains independent from exact
    // zero-contribution rejection; formal A/B did not justify enabling it.
    bool SHADOW_PREPARED_POINT_INPUTS = false;
    // Receiver-side sample budgets remain independently switchable so the
    // same executable can isolate performance and image-quality impact.
    bool SHADOW_ADAPTIVE_POINT_SAMPLES = false;
    bool SHADOW_ADAPTIVE_PCSS_FILTER = true;
    bool SHADOW_STAGED_PCSS_BLOCKER = false;
    int SHADOW_ADAPTIVE_MIN_SAMPLES = 8;
    // Accepted PCF-only path: four hardware-linear comparison lookups replace
    // sixteen manual depth reads. Hard shadows remain on the exact raw-depth
    // path, while PCSS keeps raw depth for blocker search. The switches remain
    // independently overridable for same-binary A/B.
    bool SHADOW_HARDWARE_DEPTH_COMPARE = true;
    bool SHADOW_HARDWARE_LINEAR_PCF = true;
    bool SHADOW_HARDWARE_REDUCED_PCF = true;
    // Scale receiver bias from the actual shadow texel footprint instead of
    // using one normalized-depth constant for every projection and distance.
    // The legacy constants remain available through
    // OPENGL_LEARN_SHADOW_TEXEL_BIAS=0 for controlled A/B validation.
    bool SHADOW_TEXEL_SCALED_BIAS = true;
    // A spot light is positional, so its receiver-bias angle must use the
    // fragment-to-light vector rather than the cone's center direction.
    // Keep the correction independently switchable for single-variable A/B.
    bool SHADOW_SPOT_RADIAL_BIAS_DIRECTION = true;
    // Spot shadows use a perspective projection, so PCSS must estimate the
    // receiver/blocker separation in linear light-view distance. The legacy
    // projected-depth approximation remains available through
    // OPENGL_LEARN_SHADOW_SPOT_PCSS_LINEAR_DEPTH=0 for controlled diagnosis.
    bool SHADOW_SPOT_PCSS_LINEAR_DEPTH = true;
    // A wide, correctly linearized Spot penumbra can promote the adaptive
    // filter from 8 to 16 raw-depth taps. Production caps only that final
    // Spot PCSS filter at eight well-distributed stable taps; blocker search
    // and every other light/mode stay unchanged. Set the environment flag to
    // zero to restore the complete adaptive 8/12/16 filter.
    bool SHADOW_SPOT_PCSS_REDUCED_FILTER = true;
    // Experimental: fit Spot near/far against Mesh OBBs that conservatively
    // intersect the light's square projection. Formal production scenes kept
    // identical output and draw counts while dynamic updates paid extra CPU,
    // so the time-first production default retains the global scene sphere.
    bool SHADOW_SPOT_CASTER_DEPTH_FIT = false;
    // Receiver-bias coefficients are expressed in shadow-map texels and can
    // be overridden independently for controlled, same-binary parameter
    // sweeps through OPENGL_LEARN_SHADOW_BIAS_{2D,CUBE}_{MIN,SLOPE}_TEXELS.
    float SHADOW_BIAS_2D_MIN_TEXELS = 0.75f;
    float SHADOW_BIAS_2D_SLOPE_TEXELS = 2.0f;
    float SHADOW_BIAS_CUBE_MIN_TEXELS = 5.0f;
    float SHADOW_BIAS_CUBE_SLOPE_TEXELS = 8.0f;
    // Point shadows can either fan every triangle to all cubemap layers in a
    // geometry shader or render one conventional pass per face. The latter is
    // kept independently switchable so per-face caster culling can be measured
    // without conflating it with the removal of geometry-shader amplification.
    // Production selects by caster complexity. The environment accepts
    // layered, six-face, or adaptive for controlled same-binary A/B.
    bool POINT_SHADOW_ADAPTIVE_RENDERING = true;
    // OPENGL_LEARN_POINT_SHADOW_RENDER_PATH=six-face forces the six-pass path.
    bool POINT_SHADOW_SIX_FACE_RENDERING = false;
    // OPENGL_LEARN_POINT_SHADOW_FACE_CULLING=1 enables a separate frustum test
    // for every cubemap face. It has no effect on the layered path.
    bool POINT_SHADOW_FACE_CULLING = true;
    // Cache Point shadow content independently per cubemap face. Required
    // faces are derived conservatively from camera-visible receivers, while
    // per-face caster signatures preserve unaffected faces across updates.
    bool POINT_SHADOW_PER_FACE_CACHE = false;
    // Validation-only demand override. It materializes all six faces while
    // retaining per-face dirty decisions so complete cubemap convergence can
    // be compared against the six-face oracle outside performance runs.
    bool POINT_SHADOW_FORCE_ALL_FACES_REQUIRED = false;

    int GetShadowOptimizationFlags() const {
        int flags = 0;
        if (SHADOW_EXACT_EARLY_OUT) {
            flags |= ShadowOptimization::ExactEarlyOut;
        }
        if (SHADOW_ADAPTIVE_POINT_SAMPLES) {
            flags |= ShadowOptimization::AdaptivePointSamples;
        }
        if (SHADOW_ADAPTIVE_PCSS_FILTER) {
            flags |= ShadowOptimization::AdaptivePcssFilter;
        }
        if (SHADOW_STAGED_PCSS_BLOCKER) {
            flags |= ShadowOptimization::StagedPcssBlocker;
        }
        if (SHADOW_HARDWARE_DEPTH_COMPARE) {
            flags |= ShadowOptimization::HardwareDepthCompare;
        }
        if (SHADOW_HARDWARE_LINEAR_PCF) {
            flags |= ShadowOptimization::HardwareLinearPcf;
        }
        if (SHADOW_HARDWARE_REDUCED_PCF) {
            flags |= ShadowOptimization::HardwareReducedPcf;
        }
        if (SHADOW_TEXEL_SCALED_BIAS) {
            flags |= ShadowOptimization::TexelScaledBias;
        }
        if (SHADOW_SPOT_RADIAL_BIAS_DIRECTION) {
            flags |= ShadowOptimization::SpotRadialBiasDirection;
        }
        if (SHADOW_SPOT_PCSS_LINEAR_DEPTH) {
            flags |= ShadowOptimization::SpotPcssLinearDepth;
        }
        if (SHADOW_SPOT_PCSS_REDUCED_FILTER) {
            flags |= ShadowOptimization::SpotPcssReducedFilter;
        }
        if (SHADOW_SPOT_CASTER_DEPTH_FIT) {
            flags |= ShadowOptimization::SpotCasterDepthFit;
        }
        if (SHADOW_PREPARED_POINT_INPUTS) {
            flags |= ShadowOptimization::PreparedPointInputs;
        }
        return flags;
    }

    bool GAMMA_CORRECTION = true;
    float GAMMA_VALUE = 2.2f;

    bool USE_HDR = false;
    float HDR_EXPOSURE = 1.0;

    bool BLOOM = false;
    float BLOOM_THRESHOLD = 1.0f;
    int BLOOM_BLUR_ITERATIONS = 5;

    bool DEFER_RENDERING = false;
    // Production remains on the explicit RGBA16F position attachment. The
    // reconstruction path is selected only by an explicit CLI/benchmark flag.
    int GBUFFER_POSITION_MODE = GBufferPositionProperty::Explicit;
    // The formal A/B gate passed; normal Deferred rendering uses the
    // coalesced lifecycle. Legacy2N remains explicitly selectable.
    int POINT_LIGHT_STENCIL_CLEAR_MODE =
        PointLightStencilClearProperty::CoalescedNPlusOne;
    bool POINT_LIGHT_STENCIL_CLEAR_MODE_EXPLICIT = false;
    // The fixed Analytic Screen path passed the five-process correctness and
    // performance gates across all measured coverage buckets. Explicit Volume,
    // Coalesced, and Legacy2N switches remain available for reproduction.
    int POINT_LIGHT_RENDER_MODE = PointLightRenderProperty::AnalyticScreen;
    bool POINT_LIGHT_RENDER_MODE_EXPLICIT = false;
    int POINT_LIGHT_GRID_UPDATE_MODE = PointLightGridUpdateProperty::Cached;
    bool POINT_LIGHT_GRID_UPDATE_MODE_EXPLICIT = false;
    // Zero preserves legacy mode inference: Tile16 -> 1, Cluster16 -> 16.
    // Benchmarks may explicitly select 1/2/4/8/16 while sharing one runtime.
    int POINT_LIGHT_GRID_SLICE_COUNT = 0;
    bool POINT_LIGHT_GRID_SLICE_COUNT_EXPLICIT = false;
    PointLightGridTelemetry POINT_LIGHT_GRID_TELEMETRY;
    bool POINT_LIGHT_OFFSCREEN_CULLING = false;
    bool POINT_LIGHT_OFFSCREEN_CULLING_EXPLICIT = false;
    bool POINT_LIGHT_BOUNDS_TELEMETRY_REQUESTED = false;
    bool POINT_LIGHT_BOUNDS_TELEMETRY_EXECUTED = false;
    std::vector<PointLightScreenProxyTelemetry> POINT_LIGHT_BOUNDS_TELEMETRY;
    // Diagnostic readback is opt-in and is never enabled by formal timing runs.
    bool POINT_LIGHT_STENCIL_LIFECYCLE_CHECK = false;
    bool POINT_LIGHT_STENCIL_LIFECYCLE_CHECK_EXECUTED = false;
    std::uint64_t POINT_LIGHT_STENCIL_NONZERO_PIXELS = 0;
    bool FRUSTUM_CULLING = true;
    bool FORWARD_NORMAL_BUFFER = false;
    /// 屏幕空间环境光遮蔽（仅延迟管线）：当前为采样 Pass 输出 R8/R16 可视度纹理，后续再接入模糊与光照。
    bool SSAO = false;
    float SSAO_RADIUS = 0.35f;
    float SSAO_BIAS = 0.025f;
    int SSAO_KERNEL_SIZE = 64;
    int SSAO_MODE = SSAOProperty::LegacyFull;
    float SSAO_BILATERAL_DEPTH_SIGMA = 0.02f;
    float SSAO_BILATERAL_NORMAL_POWER = 32.0f;
    bool LIGHT_VOLUME = false;
    /// ??????? = ?????? ?? ???????????/??????????????
    float LIGHT_VOLUME_RADIUS_SCALE = 1.0f;
    /// ?????????? (256/5)*diffuseMax ?????????????????????????
    float LIGHT_VOLUME_CUTOFF_SCALE = 1.0f;

    void ResetUsedTextureNum() {
        USED_TEXTURE_NUM = 0;
	}

private:
    SystemProperties();
};

/// ???????????????? defer / lightVolume ?????????
inline float ComputePointLightStencilVolumeRadius(
	float constantTerm, float linearTerm, float quadraticTerm,
	const glm::vec3& diffuse,
	float cutoffScale, float radiusScale)
{
	const float lightMax = std::fmaxf(std::fmaxf(diffuse.r, diffuse.g), diffuse.b);
	const float threshold = (256.0f / 5.0f) * cutoffScale * lightMax;
	const float discriminant = linearTerm * linearTerm
		- 4.0f * quadraticTerm * (constantTerm - threshold);
	float radius = 1.0f;
	if (discriminant >= 0.0f && std::fabs(quadraticTerm) > 1e-6f) {
		radius = (-linearTerm + std::sqrt(discriminant)) / (2.0f * quadraticTerm);
	}
	return radius * radiusScale;
}

struct GlobalVAOs {
    unsigned int quadVAO, quadVBO;
    unsigned int cubeVAO, cubeVBO;
    unsigned int sphereVAO, sphereVBO;
};
extern GlobalVAOs globalVAOs;

inline float screenVertices[] = { // vertex attributes for a quad that fills the entire screen in Normalized Device Coordinates.
	// positions   // texCoords
	-1.0f,  1.0f,  0.0f, 1.0f,
	-1.0f, -1.0f,  0.0f, 0.0f,
	 1.0f, -1.0f,  1.0f, 0.0f,

	-1.0f,  1.0f,  0.0f, 1.0f,
	 1.0f, -1.0f,  1.0f, 0.0f,
	 1.0f,  1.0f,  1.0f, 1.0f
};

inline float cubeVertices[] = {
		-0.5f, -0.5f, -0.5f,
		 0.5f, -0.5f, -0.5f,
		 0.5f,  0.5f, -0.5f,
		 0.5f,  0.5f, -0.5f,
		-0.5f,  0.5f, -0.5f,
		-0.5f, -0.5f, -0.5f,

		-0.5f, -0.5f,  0.5f,
		 0.5f, -0.5f,  0.5f,
		 0.5f,  0.5f,  0.5f,
		 0.5f,  0.5f,  0.5f,
		-0.5f,  0.5f,  0.5f,
		-0.5f, -0.5f,  0.5f,

		-0.5f,  0.5f,  0.5f,
		-0.5f,  0.5f, -0.5f,
		-0.5f, -0.5f, -0.5f,
		-0.5f, -0.5f, -0.5f,
		-0.5f, -0.5f,  0.5f,
		-0.5f,  0.5f,  0.5f,

		 0.5f,  0.5f,  0.5f,
		 0.5f,  0.5f, -0.5f,
		 0.5f, -0.5f, -0.5f,
		 0.5f, -0.5f, -0.5f,
		 0.5f, -0.5f,  0.5f,
		 0.5f,  0.5f,  0.5f,

		-0.5f, -0.5f, -0.5f,
		 0.5f, -0.5f, -0.5f,
		 0.5f, -0.5f,  0.5f,
		 0.5f, -0.5f,  0.5f,
		-0.5f, -0.5f,  0.5f,
		-0.5f, -0.5f, -0.5f,

		-0.5f,  0.5f, -0.5f,
		 0.5f,  0.5f, -0.5f,
		 0.5f,  0.5f,  0.5f,
		 0.5f,  0.5f,  0.5f,
		-0.5f,  0.5f,  0.5f,
		-0.5f,  0.5f, -0.5f
};

inline float sphereVertices[] = {
    // ???????? x, y, z
    0.0000f,  0.5000f,  0.0000f,
    0.0975f,  0.4903f,  0.0000f,
    0.0935f,  0.4903f,  0.0309f,
    0.0814f,  0.4806f,  0.0618f,
    0.0618f,  0.4806f,  0.0814f,
    0.0309f,  0.4903f,  0.0935f,
    0.0000f,  0.5000f,  0.0975f,
    -0.0309f,  0.4903f,  0.0935f,
    -0.0618f,  0.4806f,  0.0814f,
    -0.0814f,  0.4806f,  0.0618f,
    -0.0935f,  0.4903f,  0.0309f,
    -0.0975f,  0.4903f,  0.0000f,
    -0.0935f,  0.4903f,  -0.0309f,
    -0.0814f,  0.4806f,  -0.0618f,
    -0.0618f,  0.4806f,  -0.0814f,
    -0.0309f,  0.4903f,  -0.0935f,
    0.0000f,  0.5000f,  -0.0975f,
    0.0309f,  0.4903f,  -0.0935f,
    0.0618f,  0.4806f,  -0.0814f,
    0.0814f,  0.4806f,  -0.0618f,
    0.0935f,  0.4903f,  -0.0309f,
    0.1951f,  0.4619f,  0.0000f,
    0.1871f,  0.4619f,  0.0618f,
    0.1629f,  0.4438f,  0.1236f,
    0.1236f,  0.4438f,  0.1629f,
    0.0618f,  0.4619f,  0.1871f,
    0.0000f,  0.4755f,  0.1951f,
    -0.0618f,  0.4619f,  0.1871f,
    -0.1236f,  0.4438f,  0.1629f,
    -0.1629f,  0.4438f,  0.1236f,
    -0.1871f,  0.4619f,  0.0618f,
    -0.1951f,  0.4619f,  0.0000f,
    -0.1871f,  0.4619f,  -0.0618f,
    -0.1629f,  0.4438f,  -0.1236f,
    -0.1236f,  0.4438f,  -0.1629f,
    -0.0618f,  0.4619f,  -0.1871f,
    0.0000f,  0.4755f,  -0.1951f,
    0.0618f,  0.4619f,  -0.1871f,
    0.1236f,  0.4438f,  -0.1629f,
    0.1629f,  0.4438f,  -0.1236f,
    0.1871f,  0.4619f,  -0.0618f,
    0.2929f,  0.4157f,  0.0000f,
    0.2806f,  0.4157f,  0.0924f,
    0.2443f,  0.3928f,  0.1848f,
    0.1848f,  0.3928f,  0.2443f,
    0.0924f,  0.4157f,  0.2806f,
    0.0000f,  0.4330f,  0.2929f,
    -0.0924f,  0.4157f,  0.2806f,
    -0.1848f,  0.3928f,  0.2443f,
    -0.2443f,  0.3928f,  0.1848f,
    -0.2806f,  0.4157f,  0.0924f,
    -0.2929f,  0.4157f,  0.0000f,
    -0.2806f,  0.4157f,  -0.0924f,
    -0.2443f,  0.3928f,  -0.1848f,
    -0.1848f,  0.3928f,  -0.2443f,
    -0.0924f,  0.4157f,  -0.2806f,
    0.0000f,  0.4330f,  -0.2929f,
    0.0924f,  0.4157f,  -0.2806f,
    0.1848f,  0.3928f,  -0.2443f,
    0.2443f,  0.3928f,  -0.1848f,
    0.2806f,  0.4157f,  -0.0924f,
    0.3827f,  0.3536f,  0.0000f,
    0.3660f,  0.3536f,  0.1225f,
    0.3165f,  0.3268f,  0.2449f,
    0.2449f,  0.3268f,  0.3165f,
    0.1225f,  0.3536f,  0.3660f,
    0.0000f,  0.3750f,  0.3827f,
    -0.1225f,  0.3536f,  0.3660f,
    -0.2449f,  0.3268f,  0.3165f,
    -0.3165f,  0.3268f,  0.2449f,
    -0.3660f,  0.3536f,  0.1225f,
    -0.3827f,  0.3536f,  0.0000f,
    -0.3660f,  0.3536f,  -0.1225f,
    -0.3165f,  0.3268f,  -0.2449f,
    -0.2449f,  0.3268f,  -0.3165f,
    -0.1225f,  0.3536f,  -0.3660f,
    0.0000f,  0.3750f,  -0.3827f,
    0.1225f,  0.3536f,  -0.3660f,
    0.2449f,  0.3268f,  -0.3165f,
    0.3165f,  0.3268f,  -0.2449f,
    0.3660f,  0.3536f,  -0.1225f,
    0.4619f,  0.2706f,  0.0000f,
    0.4414f,  0.2706f,  0.1414f,
    0.3794f,  0.2480f,  0.2828f,
    0.2828f,  0.2480f,  0.3794f,
    0.1414f,  0.2706f,  0.4414f,
    0.0000f,  0.2903f,  0.4619f,
    -0.1414f,  0.2706f,  0.4414f,
    -0.2828f,  0.2480f,  0.3794f,
    -0.3794f,  0.2480f,  0.2828f,
    -0.4414f,  0.2706f,  0.1414f,
    -0.4619f,  0.2706f,  0.0000f,
    -0.4414f,  0.2706f,  -0.1414f,
    -0.3794f,  0.2480f,  -0.2828f,
    -0.2828f,  0.2480f,  -0.3794f,
    -0.1414f,  0.2706f,  -0.4414f,
    0.0000f,  0.2903f,  -0.4619f,
    0.1414f,  0.2706f,  -0.4414f,
    0.2828f,  0.2480f,  -0.3794f,
    0.3794f,  0.2480f,  -0.2828f,
    0.4414f,  0.2706f,  -0.1414f,
    0.5000f,  0.1768f,  0.0000f,
    0.4755f,  0.1768f,  0.1564f,
    0.4157f,  0.1587f,  0.3128f,
    0.3128f,  0.1587f,  0.4157f,
    0.1564f,  0.1768f,  0.4755f,
    0.0000f,  0.1951f,  0.5000f,
    -0.1564f,  0.1768f,  0.4755f,
    -0.3128f,  0.1587f,  0.4157f,
    -0.4157f,  0.1587f,  0.3128f,
    -0.4755f,  0.1768f,  0.1564f,
    -0.5000f,  0.1768f,  0.0000f,
    -0.4755f,  0.1768f,  -0.1564f,
    -0.4157f,  0.1587f,  -0.3128f,
    -0.3128f,  0.1587f,  -0.4157f,
    -0.1564f,  0.1768f,  -0.4755f,
    0.0000f,  0.1951f,  -0.5000f,
    0.1564f,  0.1768f,  -0.4755f,
    0.3128f,  0.1587f,  -0.4157f,
    0.4157f,  0.1587f,  -0.3128f,
    0.4755f,  0.1768f,  -0.1564f,
    0.4903f,  0.0975f,  0.0000f,
    0.4665f,  0.0975f,  0.1654f,
    0.4090f,  0.0905f,  0.3308f,
    0.3308f,  0.0905f,  0.4090f,
    0.1654f,  0.0975f,  0.4665f,
    0.0000f,  0.1082f,  0.4903f,
    -0.1654f,  0.0975f,  0.4665f,
    -0.3308f,  0.0905f,  0.4090f,
    -0.4090f,  0.0905f,  0.3308f,
    -0.4665f,  0.0975f,  0.1654f,
    -0.4903f,  0.0975f,  0.0000f,
    -0.4665f,  0.0975f,  -0.1654f,
    -0.4090f,  0.0905f,  -0.3308f,
    -0.3308f,  0.0905f,  -0.4090f,
    -0.1654f,  0.0975f,  -0.4665f,
    0.0000f,  0.1082f,  -0.4903f,
    0.1654f,  0.0975f,  -0.4665f,
    0.3308f,  0.0905f,  -0.4090f,
    0.4090f,  0.0905f,  -0.3308f,
    0.4665f,  0.0975f,  -0.1654f,
    0.4330f,  0.0000f,  0.0000f,
    0.4157f,  0.0000f,  0.1710f,
    0.3660f,  0.0000f,  0.3420f,
    0.2929f,  0.0000f,  0.4330f,
    0.1710f,  0.0000f,  0.4157f,
    0.0000f,  0.0000f,  0.4330f,
    -0.1710f,  0.0000f,  0.4157f,
    -0.2929f,  0.0000f,  0.4330f,
    -0.3660f,  0.0000f,  0.3420f,
    -0.4157f,  0.0000f,  0.1710f,
    -0.4330f,  0.0000f,  0.0000f,
    -0.4157f,  0.0000f,  -0.1710f,
    -0.3660f,  0.0000f,  -0.3420f,
    -0.2929f,  0.0000f,  -0.4330f,
    -0.1710f,  0.0000f,  -0.4157f,
    0.0000f,  0.0000f,  -0.4330f,
    0.1710f,  0.0000f,  -0.4157f,
    0.2929f,  0.0000f,  -0.4330f,
    0.3660f,  0.0000f,  -0.3420f,
    0.4157f,  0.0000f,  -0.1710f,
    0.3827f,  -0.1768f,  0.0000f,
    0.3660f,  -0.1768f,  0.1564f,
    0.3165f,  -0.1587f,  0.3128f,
    0.2449f,  -0.1587f,  0.4157f,
    0.1225f,  -0.1768f,  0.4755f,
    0.0000f,  -0.1951f,  0.5000f,
    -0.1225f,  -0.1768f,  0.4755f,
    -0.2449f,  -0.1587f,  0.4157f,
    -0.3165f,  -0.1587f,  0.3128f,
    -0.3660f,  -0.1768f,  0.1564f,
    -0.3827f,  -0.1768f,  0.0000f,
    -0.3660f,  -0.1768f,  -0.1564f,
    -0.3165f,  -0.1587f,  -0.3128f,
    -0.2449f,  -0.1587f,  -0.4157f,
    -0.1225f,  -0.1768f,  -0.4755f,
    0.0000f,  -0.1951f,  -0.5000f,
    0.1225f,  -0.1768f,  -0.4755f,
    0.2449f,  -0.1587f,  -0.4157f,
    0.3165f,  -0.1587f,  -0.3128f,
    0.3660f,  -0.1768f,  -0.1564f,
    0.2929f,  -0.2706f,  0.0000f,
    0.2806f,  -0.2706f,  0.1414f,
    0.2443f,  -0.2480f,  0.2828f,
    0.1848f,  -0.2480f,  0.3794f,
    0.0924f,  -0.2706f,  0.4414f,
    0.0000f,  -0.2903f,  0.4619f,
    -0.0924f,  -0.2706f,  0.4414f,
    -0.1848f,  -0.2480f,  0.3794f,
    -0.2443f,  -0.2480f,  0.2828f,
    -0.2806f,  -0.2706f,  0.1414f,
    -0.2929f,  -0.2706f,  0.0000f,
    -0.2806f,  -0.2706f,  -0.1414f,
    -0.2443f,  -0.2480f,  -0.2828f,
    -0.1848f,  -0.2480f,  -0.3794f,
    -0.0924f,  -0.2706f,  -0.4414f,
    0.0000f,  -0.2903f,  -0.4619f,
    0.0924f,  -0.2706f,  -0.4414f,
    0.1848f,  -0.2480f,  -0.3794f,
    0.2443f,  -0.2480f,  -0.2828f,
    0.2806f,  -0.2706f,  -0.1414f,
    0.1951f,  -0.3536f,  0.0000f,
    0.1871f,  -0.3536f,  0.1225f,
    0.1629f,  -0.3268f,  0.2449f,
    0.1236f,  -0.3268f,  0.3165f,
    0.0618f,  -0.3536f,  0.3660f,
    0.0000f,  -0.3750f,  0.3827f,
    -0.0618f,  -0.3536f,  0.3660f,
    -0.1236f,  -0.3268f,  0.3165f,
    -0.1629f,  -0.3268f,  0.2449f,
    -0.1871f,  -0.3536f,  0.1225f,
    -0.1951f,  -0.3536f,  0.0000f,
    -0.1871f,  -0.3536f,  -0.1225f,
    -0.1629f,  -0.3268f,  -0.2449f,
    -0.1236f,  -0.3268f,  -0.3165f,
    -0.0618f,  -0.3536f,  -0.3660f,
    0.0000f,  -0.3750f,  -0.3827f,
    0.0618f,  -0.3536f,  -0.3660f,
    0.1236f,  -0.3268f,  -0.3165f,
    0.1629f,  -0.3268f,  -0.2449f,
    0.1871f,  -0.3536f,  -0.1225f,
    0.0975f,  -0.4157f,  0.0000f,
    0.0935f,  -0.4157f,  0.0924f,
    0.0814f,  -0.3928f,  0.1848f,
    0.0618f,  -0.3928f,  0.2443f,
    0.0309f,  -0.4157f,  0.2806f,
    0.0000f,  -0.4330f,  0.2929f,
    -0.0309f,  -0.4157f,  0.2806f,
    -0.0618f,  -0.3928f,  0.2443f,
    -0.0814f,  -0.3928f,  0.1848f,
    -0.0935f,  -0.4157f,  0.0924f,
    -0.0975f,  -0.4157f,  0.0000f,
    -0.0935f,  -0.4157f,  -0.0924f,
    -0.0814f,  -0.3928f,  -0.1848f,
    -0.0618f,  -0.3928f,  -0.2443f,
    -0.0309f,  -0.4157f,  -0.2806f,
    0.0000f,  -0.4330f,  -0.2929f,
    0.0309f,  -0.4157f,  -0.2806f,
    0.0618f,  -0.3928f,  -0.2443f,
    0.0814f,  -0.3928f,  -0.1848f,
    0.0935f,  -0.4157f,  -0.0924f,
    0.0000f,  -0.4619f,  0.0000f,
    0.0000f,  -0.4619f,  0.0618f,
    0.0000f,  -0.4438f,  0.1236f,
    0.0000f,  -0.4438f,  0.1629f,
    0.0000f,  -0.4619f,  0.1871f,
    0.0000f,  -0.4755f,  0.1951f,
    0.0000f,  -0.4619f,  0.1871f,
    0.0000f,  -0.4438f,  0.1629f,
    0.0000f,  -0.4438f,  0.1236f,
    0.0000f,  -0.4619f,  0.0618f,
    0.0000f,  -0.4619f,  0.0000f,
    0.0000f,  -0.4619f,  -0.0618f,
    0.0000f,  -0.4438f,  -0.1236f,
    0.0000f,  -0.4438f,  -0.1629f,
    0.0000f,  -0.4619f,  -0.1871f,
    0.0000f,  -0.4755f,  -0.1951f,
    0.0000f,  -0.4619f,  -0.1871f,
    0.0000f,  -0.4438f,  -0.1629f,
    0.0000f,  -0.4438f,  -0.1236f,
    0.0000f,  -0.4619f,  -0.0618f,
    0.0000f,  -0.5000f,  0.0000f
};

/// �� InitVAOs �� sphereVAO һ�£�������ԭ�㣬ģ�Ϳռ�뾶 0.5��ֱ�� 1��
inline GLsizei GetUnitSphereVertexCount() {
	return static_cast<GLsizei>(sizeof(sphereVertices) / (sizeof(float) * 3u));
}

inline glm::mat4 MakePointLightVolumeModelMatrix(const glm::vec3& worldCenter, float worldRadius) {
	constexpr float kMeshRadius = 0.5f;
	const float s = worldRadius / kMeshRadius;
	return glm::translate(glm::mat4(1.0f), worldCenter) * glm::scale(glm::mat4(1.0f), glm::vec3(s));
}

extern unsigned int quadVAO, quadVBO;
extern unsigned int cubeVAO, cubeVBO;
extern unsigned int sphereVAO, sphereVBO;

class AntiAliasManager {
public:
	static AntiAliasManager& GetInstance() {
		static AntiAliasManager instance;
		return instance;
	}
	AntiAliasManager(const AntiAliasManager&) = delete;
	AntiAliasManager& operator=(const AntiAliasManager&) = delete;

	AntiAliasManager() = default;

	enum AntiAliasType {
		Default = 0,
		MSAA = 1,
	};

	inline static const char* optionsAA[] = {
		"DEFAULT",
		"MSAA"
	};

	inline static std::vector<unsigned int> frameBuffers;

	void AntiAliasByType(AntiAliasType);
	AntiAliasType antiAliasType;
private:

};
//Frambuffer Begin
struct TextureAttributes {
    GLenum target;
	GLint internalFormat;
    GLenum format;
	GLenum type;

    bool operator==(const TextureAttributes& other) const {
        return std::tie(target, internalFormat, format, type) ==
            std::tie(other.target, other.internalFormat, other.format, other.type);
    }
};

template <class T>
inline void hash_combine(std::size_t& seed, const T& v) {
    std::hash<T> hasher;
    // ????? 0x9e3779b9 ??????????????????????????????????????
    seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

struct FBOAttributes {
	enum FramebufferType {
		Framebuffer = 0,
		Multisample,
		ShadowMap,
		ShadowBox,
		HDR,
	};

	AntiAliasManager::AntiAliasType aaType = AntiAliasManager::AntiAliasType::Default;
	bool isHDR = false; 
	bool isShadowMap = false;
	bool isGamma = false;
	FramebufferType shadowType = FramebufferType::ShadowMap;
	bool isBloom = false;
	bool isDefer = false;
	// For forward/AO 等后处理：需要把深度作为 texture 供采样。
	bool hasDepthTexture = false;
	int width = 0;
	int height = 0;
	std::vector<TextureAttributes> textureAttrs;


    bool operator==(const FBOAttributes& other) const {
        return std::tie(aaType, isHDR, isGamma, isShadowMap, shadowType, isBloom, isDefer, hasDepthTexture, width, height) ==
            std::tie(other.aaType, other.isHDR, other.isGamma, other.isShadowMap, other.shadowType, other.isBloom, other.isDefer, other.hasDepthTexture, other.width, other.height)
            && textureAttrs == other.textureAttrs;
    }
};

namespace std {
    template<> struct hash<TextureAttributes> {
        size_t operator()(const TextureAttributes& attr) const {
            size_t seed = 0;
            hash_combine(seed, static_cast<unsigned int>(attr.target));
            hash_combine(seed, static_cast<int>(attr.internalFormat));
            hash_combine(seed, static_cast<unsigned int>(attr.format));
            hash_combine(seed, static_cast<unsigned int>(attr.type));
            return seed;
        }
    };

    template<> struct hash<FBOAttributes> {
        size_t operator()(const FBOAttributes& attr) const {
            size_t seed = 0;
            hash_combine(seed, static_cast<int>(attr.aaType));
            hash_combine(seed, attr.isHDR);
            hash_combine(seed, attr.isShadowMap);
            hash_combine(seed, attr.isGamma);
            hash_combine(seed, static_cast<int>(attr.shadowType));
            hash_combine(seed, attr.isBloom);
            hash_combine(seed, attr.isDefer);
            hash_combine(seed, attr.hasDepthTexture);
            hash_combine(seed, attr.width);
            hash_combine(seed, attr.height);

            for (const auto& tex : attr.textureAttrs) {
                hash_combine(seed, tex);
            }

            return seed;
        }
    };
}

class FBO {
public:
	enum FrameRenderType {
		Default_FrameRenderType = 0,
		ShadowMap_FrameRenderType,
		BrightColor_FrameRenderType,
	};

	inline static const char* optionFrame[] = {
		"Default",
		"ShadowMap",
		"BrightColor",
	};
	bool isBusy = false;
	unsigned int framebufferID = 0;
	std::vector<unsigned int> textureIDs;
	unsigned int rboID = 0;
	// When attr.hasDepthTexture is true, this is the main D24S8 attachment;
	// sampling reads its depth aspect while stencil tests use the same storage.
	unsigned int depthTextureID = 0;
	bool init = false;
    std::string passName;
    int width;
    int height;

	FBOAttributes attr;

	FBO(FBOAttributes attr) {
		width = properties.SCREEN_WIDTH;
		height = properties.SCREEN_HEIGHT;
		passName = "Default";
		Init(attr);
	}

    FBO(int w, int h, FBOAttributes attr) {
        width = w;
        height = h;
        passName = "Default";
        Init(attr);
	}

	FBO(int w, int h, FBOAttributes attr, std::string pass) {
        width = w;
        height = h;
        passName = pass;
		Init(attr);
	}

	~FBO() { Delete(); }
	void Delete();
	void Init(FBOAttributes attr);
	unsigned int GetCubeFaceFramebuffer(int face);
	bool IsComplete() const {
		return init;
	}
	std::uint64_t GetResourceGeneration() const {
		return m_resourceGeneration;
	}
	std::uint64_t GetTrackedBytes() const {
		return m_trackedBytes;
	}
	void Resize() {
		Delete();
		Init(attr);
	}
private:
	SystemProperties& properties = SystemProperties::GetInstance();
	std::uint64_t m_trackedBytes = 0;
	std::uint64_t m_resourceGeneration = 0;
	std::array<unsigned int, 6> m_cubeFaceFramebufferIDs{};
};

class FramebuffersManager {
public:
    static unsigned int renderFBO;
    inline static FBO::FrameRenderType useType = FBO::Default_FrameRenderType;

    static FramebuffersManager& GetInstance() {
        static FramebuffersManager instance;
        return instance;
    }

    void RegisterFBO(const std::string& passName, FBO* fbo) {
        m_fboMap[passName] = fbo;
    }

    FBO* GetFBOByPassName(const std::string& passName) {
        if(m_fboMap.find(passName) != m_fboMap.end()) {
            return m_fboMap[passName];
		}
		std::cout << "There is no FBO registered for pass name: " << passName << std::endl;
		return nullptr;
    }

	static FBOAttributes GenCurrentAttr() {
		FBOAttributes attr;
        auto& properties = SystemProperties::GetInstance();
		attr.aaType = AntiAliasManager::GetInstance().antiAliasType;
		attr.isGamma = properties.GAMMA_CORRECTION;
		attr.isHDR = properties.USE_HDR;
		attr.isBloom = properties.BLOOM;
		attr.isDefer = false;
		return attr;
	}

	void ReleaseFBO(FBO* fbo) {
		//ClearFBOBuffers(fbo);
		if (fbo == nullptr) return;
		fbo->isBusy = false;
	}

	void ClearFBOBuffers(FBO* fbo) {
		GLState::BindFramebuffer(GL_FRAMEBUFFER, fbo->framebufferID);
		if (fbo->attr.isShadowMap) {
			glClear(GL_DEPTH_BUFFER_BIT);
		}
		else {
			for(auto& texID : fbo->textureIDs) {
				glClearBufferfv(GL_COLOR, 0, glm::value_ptr(glm::vec4(0.0f)));
			}
		}
		GLState::BindFramebuffer(GL_FRAMEBUFFER, 0);
	}

	FBO* GetFBO(FBOAttributes);
	unsigned int GetShadowCompareSampler(bool linearFiltering);

	void Resize();
	void TrimUnusedFBOs();
	void Shutdown();

	// ?????????? isBusy ?? FBO ?????????? Viewport ??????? FBO ????????????
	std::vector<FBO*> GetBusyFBOs() const;
	// ???? FBO ?? attr ?????????????????????????? UI ??????
	static std::string GetFBODisplayName(const FBOAttributes& attr, int indexInList);
private:
	inline static FBOAttributes::FramebufferType FBOResizeableTpye[] = {
		FBOAttributes::FramebufferType::Framebuffer,
		FBOAttributes::FramebufferType::Multisample
	};
    //FBO objcet pool
	std::unordered_map<FBOAttributes, std::vector<FBO*>> m_hashMapFBO;
    //record the active FBOs by name
    std::unordered_map<std::string, FBO*> m_fboMap;
	unsigned int m_shadowCompareNearestSampler = 0;
	unsigned int m_shadowCompareLinearSampler = 0;

};
//Frambuffer End
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

	inline static float normalLineMagnitude = 0.01f;
};

unsigned int TextureFromFile(const char* path, const std::string& directory, bool alpha = false, bool gamma = false);
void DestroyTextureCache();

class BaseObject {
public:
	glm::vec3 position = glm::vec3(0);
	glm::vec3 rotation = glm::vec3(0);
	glm::vec3 scale = glm::vec3(1);
	bool m_active = true;
	glm::mat4 getModelMatrix();
	void setModelMatrix(glm::mat4);
	std::uint64_t GetTransformRevision() {
		getModelMatrix();
		return m_transformRevision;
	}

	bool GetActiveStatus() {
		return m_active;
	}

	void SetActiveStatus(bool val) {
		m_active = val;
	}

    void SetScale(glm::vec3 s) {
		if (scale == s) {
			return;
		}
        scale = s;
		m_transformCacheValid = false;
	}
    
    void SetScale(float s) {
		const glm::vec3 uniformScale(s);
		if (scale == uniformScale) {
			return;
		}
        scale = uniformScale;
		m_transformCacheValid = false;
    }

    void SetPosition(glm::vec3 p) {
		if (position == p) {
			return;
		}
        position = p;
		m_transformCacheValid = false;
    }

    void SetRotation(glm::vec3 r) {
		if (rotation == r) {
			return;
		}
        rotation = r;
		m_transformCacheValid = false;
    }
protected:
	glm::mat4 modelMatrix;
	glm::vec3 m_cachedPosition = glm::vec3(0);
	glm::vec3 m_cachedRotation = glm::vec3(0);
	glm::vec3 m_cachedScale = glm::vec3(1);
	bool m_transformCacheValid = false;
	std::uint64_t m_transformRevision = 0;
};
