#pragma once

#include <GLES3/gl32.h>
#include <hyprland/src/render/Shader.hpp>
#include <string>

struct SGlassUniforms {
    GLint refractionStrength = -1;
    GLint chromaticAberration = -1;
    GLint reflectionStrength = -1;
    GLint roundingPower = -1;
    GLint refractThickness = -1;
    GLint refractIOR = -1;
    GLint refractScale = -1;
    GLint u_time = -1;
    GLint fresnelStrength = -1;
    GLint specularStrength = -1;
    GLint glassOpacity = -1;
    GLint edgeThickness = -1;
    GLint uvPadding = -1;
    GLint tintColor = -1;
    GLint tintAlpha = -1;
    GLint lensDistortion = -1;
    GLint saturation = -1;
    GLint vibrancyDarkness = -1;
    GLint adaptiveDim = -1;
    GLint adaptiveBoost = -1;

    // Layers only: temp FBO surface mask for content-aware glass
    GLint maskTex = -1;
    GLint useMask = -1;
    GLint maskUVOffset = -1;
    GLint maskUVScale = -1;
    GLint maskAlphaThreshold = -1;

    // User-configurable per-GlassItem inner shadow (v1: symmetric only —
    // blur + spread + color, no directional offset yet). Separate from,
    // and layered on top of, the always-on hardcoded bottom-rim shadow.
    GLint shadowHasInner = -1;
    GLint shadowInnerBlur = -1;
    GLint shadowInnerSpread = -1;
    GLint shadowInnerColor = -1; // vec4 rgba
    GLint shadowInnerOffset = -1; // vec2 px, offset-x/offset-y

    // User-configurable per-GlassItem outer shadow (mirrors inner shadow).
    // Paints outside the region's true edge, into the padded draw quad
    // GlassLayerSurface inflates for outer-shadow regions.
    GLint shadowHasOuter = -1;
    GLint shadowOuterBlur = -1;
    GLint shadowOuterSpread = -1;
    GLint shadowOuterColor = -1; // vec4 rgba
    GLint shadowOuterOffset = -1; // vec2 px, offset-x/offset-y
    GLint shadowOuterIsTrue = -1;
    GLint shadowBlurStrength = -1; // 1 = true optical shadow, 0 = flat/legacy

    // Maps the true (unpadded) region's bounds as a UV sub-rect within the
    // (possibly padded) draw quad. offset=(0,0) scale=(1,1) when the quad
    // is not padded (no outer shadow) -- a no-op remap in that case.
    GLint trueBoxUVOffset = -1;
    GLint trueBoxUVScale = -1;
};

struct SBlurUniforms {
    GLint direction = -1;
    GLint radius    = -1;
};

class CShaderManager {
  public:
    [[nodiscard]] bool isInitialized() const noexcept { return m_initialized; }

    void initializeIfNeeded();
    void destroy() noexcept;

    SP<CShader>    glassShader = makeShared<CShader>();
    SGlassUniforms glassUniforms;

    SP<CShader>    blurShader = makeShared<CShader>();
    SBlurUniforms  blurUniforms;

  private:
    bool m_initialized = false;

    [[nodiscard]] static std::string loadShaderSource(const char* fileName);
    [[nodiscard]] bool compileGlassShader();
    [[nodiscard]] bool compileBlurShader();
};
