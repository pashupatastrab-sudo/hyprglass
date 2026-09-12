// Auto-generated shader header - Do not edit!
#pragma once

#include <unordered_map>
#include <string>

inline const std::unordered_map<std::string, const char*> SHADERS = {
    {"liquidglass.frag", R"GLSL(
#version 300 es
precision highp float;

/*
 * Apple-style Liquid Glass Fragment Shader — Thick-glass refraction model
 *
 * The window is modeled as a thick convex glass slab:
 *   - Center: flat surface → clean frosted blur, no distortion
 *   - Edges: curved surface → refraction pulls in content from beyond
 *     the window boundary, creating natural color bleeding
 *
 * Rendering layers:
 * 1. Edge refraction via smooth outward direction + exponential proximity
 * 2. Chromatic aberration (per-channel refraction scale)
 * 3. Edge raw-texture blend for vivid color pickup
 * 4. Subtle center dome lens magnification
 * 5. Frosted tint (brightness boost + desaturation)
 * 6. Configurable color tint overlay
 * 7. Fresnel edge glow
 * 8. Specular highlight (top)
 * 9. Inner shadow (bottom rim)
 */

uniform sampler2D tex;
uniform vec2 fullSize;
uniform float radius;
uniform vec2 uvPadding;
uniform float reflectionStrength;
uniform float u_time;
uniform float refractionStrength;
uniform float chromaticAberration;
uniform float fresnelStrength;
uniform float specularStrength;
uniform float glassOpacity;
uniform float edgeThickness;
uniform vec3 tintColor;
uniform float tintAlpha;
uniform float lensDistortion;
uniform float brightness;
uniform float contrast;
uniform float saturation;
uniform float vibrancy;
uniform float vibrancyDarkness;
uniform float adaptiveDim;
uniform float adaptiveBoost;
uniform float roundingPower;
uniform float refractThickness;
uniform float refractIOR;
uniform float refractScale;  

uniform sampler2D maskTex;
uniform int useMask;
uniform vec2 maskUVOffset;
uniform vec2 maskUVScale;
uniform float maskAlphaThreshold;

// User-configurable per-GlassItem inner shadow (v1: symmetric only — blur +
// spread + color, no directional offset yet). Separate from, and layered on
// top of, the always-on hardcoded bottom-rim shadow further down.
uniform float shadowHasInner;
uniform float shadowInnerBlur;
uniform float shadowInnerSpread;
uniform vec4  shadowInnerColor;
uniform vec2  shadowInnerOffset;

// User-configurable per-GlassItem outer shadow (mirrors inner shadow, but
// paints outside the region's true edge into the padded margin band that
// GlassLayerSurface reserves via trueBoxUVOffset/Scale below).
uniform float shadowHasOuter;
uniform float shadowOuterBlur;
uniform float shadowOuterSpread;
uniform vec4  shadowOuterColor;
uniform float shadowBlurStrength; // glass blur_strength (preset value), used to widen the true-shadow's edge falloff
uniform vec2  shadowOuterOffset;
uniform float shadowOuterIsTrue; // 1 = true optical shadow (frosted/tinted where it overlaps the glass body), 0 = flat/legacy

// Maps the true (unpadded) region's bounds as a UV sub-rect within this
// draw quad. offset=(0,0) scale=(1,1) when the quad isn't padded (no
// outer shadow) -- trueUV then equals uv exactly, a no-op.
uniform vec2 trueBoxUVOffset;
uniform vec2 trueBoxUVScale;

in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

// ============================================================================
// TEXTURE SAMPLING (window UV -> padded texture UV)
// ============================================================================

vec2 toTexUV(vec2 wuv) {
return wuv * (1.0 - 2.0 * uvPadding) + uvPadding;
}  

vec4 sampleBlurred(vec2 wuv) {
vec2 tuv = toTexUV(wuv);
return texture(tex, clamp(tuv, 0.001, 0.999));
}  

// ============================================================================
// SDF
// ============================================================================  

float lpNorm(vec2 v, float p) {
return pow(pow(abs(v.x), p) + pow(abs(v.y), p), 1.0 / p);
}  

float getRoundedBoxSDF(vec2 uv, float r, vec2 size) {
vec2 p = (uv - 0.5) * size;
vec2 halfSize = size * 0.5;
float clampedR = min(r, min(halfSize.x, halfSize.y));
vec2 q = abs(p) - halfSize + clampedR;
return min(max(q.x, q.y), 0.0) + lpNorm(max(q, 0.0), roundingPower) - clampedR;
}  

// Shape-defining SDF, keyed to the TRUE (unpadded) region box via
// trueUV/trueFullSize passed in as size -- not the possibly-padded draw
// quad -- so glass shape/refraction/rounding stay anchored to the real
// edges regardless of any outer-shadow margin.
float getCornerSDF(vec2 uv, vec2 size) {
return getRoundedBoxSDF(uv, radius, size);
}
// Offset-aware variant used by the user-configurable inner AND outer
// shadows, so only the shadow's shape shifts — the main glass shape
// (getCornerSDF) is never touched by shadowInnerOffset/shadowOuterOffset.
float getCornerSDFOffset(vec2 uv, vec2 offsetPx, vec2 size) {
vec2 offsetUV = offsetPx / size;
return getCornerSDF(uv - offsetUV, size);
}
// Returns vec3(d, nx, ny): signed distance and outward unit normal, used
// for Snell's-law dome refraction (see refractSnell below). Mirrors
// getRoundedBoxSDF's shape math but also computes the analytic gradient.
vec3 getCornerSDFAndNormal(vec2 uv, vec2 size) {
vec2 p = (uv - 0.5) * size;
vec2 halfSize = size * 0.5;
float r = clamp(radius, 0.0, min(halfSize.x, halfSize.y));
float n = max(roundingPower, 2.0);

vec2 q = abs(p) - halfSize + vec2(r);
float qx = max(q.x, 0.0);
float qy = max(q.y, 0.0);

float d;
vec2 nrm;

if (qx <= 0.0 && qy <= 0.0) {
    d = max(q.x, q.y) - r;
    nrm = q.x >= q.y ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
} else if (qx == 0.0) {
    d = qy - r;
    nrm = vec2(0.0, 1.0);
} else if (qy == 0.0) {
    d = qx - r;
    nrm = vec2(1.0, 0.0);
} else {
    float qxn = pow(qx, n);
    float qyn = pow(qy, n);
    float arc = pow(qxn + qyn, 1.0 / n);
    float gx = pow(qx / arc, n - 1.0);
    float gy = pow(qy / arc, n - 1.0);
    float gradLen = sqrt(gx * gx + gy * gy);
    d   = (arc - r) / max(gradLen, 1e-3);
    nrm = vec2(gx, gy) / max(gradLen, 1e-3);
}

nrm *= sign(p + vec2(1e-20));
return vec3(d, nrm);
}
// Snell's-law dome-bevel refraction. Given depth into the edge band (0
// at the true edge, 1 at the inner boundary of the band) and the
// surface normal, returns the lateral pixel-space displacement to
// sample the backdrop through — this is what gives real glass its
// lensing/bending look at the rim, replacing a flat directional push.
vec2 refractSnell(vec2 ndir, float depthIntoBand) {
float t = clamp(depthIntoBand, 0.0, 1.0);
float sinThetaI = (1.0 - t) * (1.0 - t);
float thetaI = asin(clamp(sinThetaI, 0.0, 1.0));
float sinThetaT = sinThetaI / max(refractIOR, 1.001);
float thetaT = asin(clamp(sinThetaT, 0.0, 1.0));
float mag = tan(thetaI - thetaT);
return -ndir * mag * refractScale;
}

// ============================================================================
// REFRACTION DIRECTION
// Pixel-space direction toward window center — perfectly smooth everywhere,
// no SDF gradient needed. On straight edges the perpendicular pixel distance
// dominates, giving approximately edge-normal direction. At corners it
// naturally follows the diagonal.
// ============================================================================  

vec2 refractionDir(vec2 uv) {
vec2 toCenterPx = (vec2(0.5) - uv) * fullSize;
float len = length(toCenterPx);
return len > 0.1 ? toCenterPx / len : vec2(0.0);
}  

// ============================================================================
// MAIN — Thick-glass refraction model
// ============================================================================  

// Frosted desaturation + adaptive dim/boost + contrast + vibrancy + tint
// overlay, applied to real sampled backdrop color. Extracted into its own
// function so the true outer shadow can run flat shadow color through the
// exact same treatment where it overlaps the glass body, making it read as
// visible THROUGH the lens rather than pasted flat on top.
vec3 applyFrostTreatment(vec3 rawColor) {
    vec3 c = rawColor;
    float blurredLum = dot(c, vec3(0.2126, 0.7152, 0.0722));

    c = mix(vec3(blurredLum), c, saturation);

    float lumCurve = smoothstep(0.25, 0.55, blurredLum);

    c *= brightness * (1.0 - adaptiveDim * lumCurve);

    float boostGate = smoothstep(0.0, 0.08, blurredLum) * (1.0 - lumCurve);
    c += vec3(adaptiveBoost * boostGate * 0.5);

    c = mix(vec3(0.5), c, contrast);

    float currentLum = dot(c, vec3(0.2126, 0.7152, 0.0722));
    float sat = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
    float darkFactor = 1.0 - vibrancyDarkness * (1.0 - blurredLum);
    c = mix(vec3(currentLum), c, 1.0 + vibrancy * sat * darkFactor);

    c = mix(c, tintColor, tintAlpha);

    return c;
}

float outerShadowAmtFromSdf(float shadowSdf, float spread, float blur) {
    if (blur < 0.001) {
        return (shadowSdf <= spread) ? 1.0 : 0.0;
    }
    float distFromGlass = max(shadowSdf - spread, 0.0);
    float amt = 1.0 - smoothstep(0.0, blur, distFromGlass);
    return amt * amt;
}
void main() {
vec2 uv = v_texcoord;  

// True (unpadded) region UV/size — equals uv/fullSize exactly when
// there's no outer shadow (offset=0, scale=1), a guaranteed no-op for
// every existing region. Shape-defining geometry (corner rounding,
// refraction, inner shadow, reflection) keys off these so it stays
// anchored to the real region edges even when the draw quad has been
// padded outward for an outer shadow's margin band. Background
// sampling stays on plain uv/fullSize — that's the padded-quad space
// the sampled backdrop framebuffer actually lives in.
vec2 trueUV       = (uv - trueBoxUVOffset) / trueBoxUVScale;
vec2 trueFullSize = fullSize * trueBoxUVScale;

// Outer shadow falloff, computed early -- needed by the discard checks
// below so margin-band fragments (outside the true glass edge, inside
// the padded draw quad) survive when they're within the shadow's own
// blur+spread extent, instead of being discarded before they can paint.
// Reusable: turns a corner-SDF value into a shadow amount (0..1),
// same math as before, just factored so the true-shadow block below
// can reuse it per color channel with a bent SDF.
float outerAmt = 0.0;
if (shadowHasOuter > 0.5) {
    float shadowSdf = getCornerSDFOffset(trueUV, shadowOuterOffset, trueFullSize);
    outerAmt = outerShadowAmtFromSdf(shadowSdf, shadowOuterSpread, shadowOuterBlur);
}
bool inOuterShadowBand = (shadowHasOuter > 0.5) && (outerAmt > 0.0);

// Layers only: sample the temp FBO to get the rendered surface pixel.
// Discard fully transparent fragments so glass only covers visible content.
// For windows, hasMask is false and this block is skipped entirely.
vec4 surfacePixel = vec4(0.0);
bool hasMask = (useMask == 1);
if (hasMask) {
    vec2 maskUV = trueUV * maskUVScale + maskUVOffset;
    surfacePixel = texture(maskTex, clamp(maskUV, 0.001, 0.999));
    if (surfacePixel.a < maskAlphaThreshold) discard;
}

float cornerSdf = getCornerSDF(trueUV, trueFullSize);
// Smooth 1px-ish falloff at the true glass edge -- restores anti-aliasing.
// Without this, the edge is a hard binary step which is normally hidden
// by refraction/blur distortion, but becomes visibly pixelated once an
// outer shadow sits right behind the edge in a contrasting color.
float cornerAlpha = 1.0 - smoothstep(-1.0, 1.0, cornerSdf);

// Discard only when completely outside both the glass body and the shadow margin
if (cornerAlpha < 0.001 && !inOuterShadowBand) {
    discard;
}

float minDim = min(trueFullSize.x, trueFullSize.y);
float bezelWidthPx = edgeThickness * minDim;

// ========================================
// EDGE PROXIMITY + DIRECTION
// edgeProximity: 1.0 at boundary, exponential decay inward
// inwardDir: pixel-space direction toward center (smooth everywhere)
// ========================================
float edgeProximity = exp(cornerSdf / bezelWidthPx);
vec3 sdfN = getCornerSDFAndNormal(trueUV, trueFullSize);
vec2 inwardDir = -sdfN.yz;

// ========================================
// EDGE REFRACTION
// Offset sampling UV inward (toward center) at edges — like looking
// through the curved thick edge of a glass slab. This compresses
// and distorts what's already behind the window, without reaching
// beyond the window boundary.
// ========================================
float depthPx = -cornerSdf;
float refractionMag = 0.0;
// Disable edge refraction completely outside the glass body (in the outer shadow margin)
if (cornerAlpha > 0.001 && refractThickness > 0.001 && depthPx < refractThickness) {
    float t = clamp(depthPx / refractThickness, 0.0, 1.0);
    vec2 snellOffset = refractSnell(-inwardDir, t);
    refractionMag = length(snellOffset);
}
vec2 baseOffset = (cornerAlpha > 0.001) ? (inwardDir * refractionMag / fullSize) : vec2(0.0);

// ========================================
// CHROMATIC ABERRATION — per-channel refraction scale
// Blue refracts more than red → natural spectral fringing at edges.
// ========================================
float edgeWeightRaw = (cornerAlpha > 0.001 && refractThickness > 0.001) ? (1.0 - clamp(depthPx / refractThickness, 0.0, 1.0)) : 0.0;
float edgeWeight = pow(edgeWeightRaw, 0.25);
float chromaPx = chromaticAberration * refractThickness * 0.35 * edgeWeight;
vec2 chromaUV = (cornerAlpha > 0.001) ? (inwardDir * chromaPx / fullSize) : vec2(0.0);
vec2 offsetR = baseOffset + chromaUV;
vec2 offsetG = baseOffset;
vec2 offsetB = baseOffset - chromaUV;

// ========================================
// CENTER DOME LENS (subtle magnification in the flat interior)
// Fades near edges so it doesn't interfere with edge refraction.
// ========================================
vec2 domeUV = vec2(0.0);
if (lensDistortion > 0.001) {
    vec2 c = (uv - 0.5) * 2.0;
    vec2 dGrad = vec2(
        -4.0 * c.x * (1.0 - c.y * c.y),
        -4.0 * c.y * (1.0 - c.x * c.x)
    );
    float lensMaxPx = lensDistortion * minDim * 0.006;
    float lensFade = 1.0 - edgeProximity;
    domeUV = dGrad * lensMaxPx * lensFade / fullSize;
}

// ========================================
// BACKGROUND SAMPLING (frosted blur only)
// Nearby color influence comes naturally from the Gaussian blur
// kernel crossing the window boundary — no explicit raw sampling.
// ========================================
vec3 color;
vec2 uvR = uv + offsetR + domeUV;
vec2 uvG = uv + offsetG + domeUV;
vec2 uvB = uv + offsetB + domeUV;

if (chromaticAberration > 0.001 && edgeProximity > 0.01) {
    color.r = sampleBlurred(uvR).r;
    color.g = sampleBlurred(uvG).g;
    color.b = sampleBlurred(uvB).b;
} else {
    color = sampleBlurred(uvG).rgb;
}

// ========================================
// FROSTED TINT + COLOR TINT OVERLAY (per-theme tone mapping)
// Extracted into applyFrostTreatment() -- see definition above main() --
// so the true outer shadow can reuse the identical treatment.
// ========================================
color = applyFrostTreatment(color);

// ========================================
// FRESNEL RIM GLOW (edge zone)
// ========================================
if (cornerAlpha > 0.001 && fresnelStrength > 0.001) {
    float fresnel = edgeProximity * edgeProximity * fresnelStrength * 0.15;
    color += vec3(1.0) * fresnel;
}

// SPECULAR block removed — specularStrength now solely drives edgeOpacityBoost.

// ========================================
// INNER SHADOW (bottom rim)
// ========================================
{
    float bottomBias = pow(trueUV.y, 2.0);
    float shadow = bottomBias * edgeProximity * edgeProximity * 0.06;
    color *= 1.0 - shadow;
}

// ========================================
// USER-CONFIGURABLE INNER SHADOW (per-GlassItem, v1: symmetric only)
// Separate, additive effect on top of the hardcoded bottom-rim shadow
// above. depthPx is distance inward from the edge (0 at edge, growing
// toward the center). Darkening ramps in starting at shadowInnerSpread
// px inward, reaching full strength by spread+blur px inward.
// ========================================
if (shadowHasInner > 0.5) {
    float shadowCornerSdf = getCornerSDFOffset(trueUV, shadowInnerOffset, trueFullSize);
    float depthPxShadow   = -shadowCornerSdf;
    float shadowStart = shadowInnerSpread;
    float shadowEnd   = shadowInnerSpread + max(shadowInnerBlur, 0.001);
    float shadowAmt   = 1.0 - smoothstep(shadowStart, shadowEnd, depthPxShadow);
    shadowAmt = clamp(shadowAmt * shadowInnerColor.a, 0.0, 1.0);
    color = mix(color, shadowInnerColor.rgb, shadowAmt);
}

// ========================================
// TRUE OUTER SHADOW (overlap-only, visible THROUGH the glass)
// Where the offset outer shadow's extent overlaps the actual glass body,
// blend a frosted/tinted version of the shadow color directly into
// `color` -- mirroring how the inner shadow above works -- since the
// final outer-shadow block further down is gated by glassA (~1 inside
// the body) and can only ever paint the OUTSIDE margin, never the
// interior. Outside the glass body (cornerAlpha <= 0), this block is a
// no-op and the final flat block below still handles the margin exactly
// as before, regardless of shadowOuterIsTrue.
// ========================================
float trueShadowMask = 0.0; // how strongly the true shadow is present here (0..1); used to keep the reflection highlight out of the shadowed area
if (shadowHasOuter > 0.5 && shadowOuterIsTrue > 0.5 && cornerAlpha > 0.001) {
    // Bend + fringe the true shadow the same way real backdrop content is
    // bent/fringed above: evaluate its SDF at the same per-channel refracted
    // + dome-distorted UV as offsetR/offsetG/offsetB/domeUV, instead of the
    // single unbent trueUV used by the flat/outside-glass shadow above.
    // Chromatic aberration + dome distortion only -- deliberately NOT using
    // offsetR/offsetG/offsetB directly, since those bundle in baseOffset
    // (the edge-refraction bend). The shadow should fringe and warp like
    // real content does, but should not bend/refract at the glass edge.
    vec2 shadowOffsetR =  chromaUV + domeUV;
    vec2 shadowOffsetG =  vec2(0.0) + domeUV;
    vec2 shadowOffsetB = -chromaUV + domeUV;
    float sdfR = getCornerSDFOffset(trueUV + shadowOffsetR, shadowOuterOffset, trueFullSize);
    float sdfG = getCornerSDFOffset(trueUV + shadowOffsetG, shadowOuterOffset, trueFullSize);
    float sdfB = getCornerSDFOffset(trueUV + shadowOffsetB, shadowOuterOffset, trueFullSize);
    // "Blur" approximation for the true shadow: since shadowOuterColor is a
    // flat, spatially-uniform color, there's nothing to run a gaussian kernel
    // over -- instead widen the edge falloff band by the glass's own blurRadius,
    // so the shadow's edge visually softens more under a more-blurred preset.
    // Scoped only to the true/overlap portion; the flat shadow outside the
    // glass (further below) is untouched and still uses shadowOuterBlur as-is.
    float trueShadowBlur = shadowOuterBlur + shadowBlurStrength * 6.0;
    float amtR = outerShadowAmtFromSdf(sdfR, shadowOuterSpread, trueShadowBlur);
    float amtG = outerShadowAmtFromSdf(sdfG, shadowOuterSpread, trueShadowBlur);
    float amtB = outerShadowAmtFromSdf(sdfB, shadowOuterSpread, trueShadowBlur);
    trueShadowMask = max(amtR, max(amtG, amtB));

    vec3 trueShadowColor = applyFrostTreatment(shadowOuterColor.rgb);
    vec3 trueShadowAmt = clamp(vec3(amtR, amtG, amtB) * shadowOuterColor.a, 0.0, 1.0);
    color = mix(color, trueShadowColor, trueShadowAmt);
}

float uniformOpacityBoost = 1.0 + specularStrength;
float glassA = clamp(glassOpacity * uniformOpacityBoost, 0.0, 1.0) * cornerAlpha;

// ========================================
// USER-CONFIGURABLE OUTER SHADOW
// Corrected blending: handles shadow behind/around the glass cleanly without
// any split/line artifacts or unwanted alpha clipping.
// ========================================
if (shadowHasOuter > 0.5 && outerAmt > 0.001) {
    float outerA = outerAmt * shadowOuterColor.a;
    color  = mix(shadowOuterColor.rgb, color, glassA);
    glassA = max(glassA, outerA);
}

// ========================================
// REFLECTION — broad, soft, drifting highlight (Fresnel-driven)
// Strongest at the rim, faint at center, slowly sliding over time.
// ========================================
if (cornerAlpha > 0.001 && reflectionStrength > 0.001) {
    float rimFresnel = edgeProximity * edgeProximity;

    vec2 driftCenter = vec2(0.5) + 0.35 * vec2(cos(u_time * 0.05), sin(u_time * 0.035) * 0.6);
    vec2 d = (trueUV - driftCenter) * vec2(1.0, 1.4);
    float highlight = exp(-dot(d, d) * 1.1);

    float reflectionMask = mix(highlight * 0.15, 1.0, rimFresnel) * highlight;

    vec3 reflectionColor = vec3(1.0, 0.99, 0.96);
    color += reflectionColor * reflectionMask * reflectionStrength * 0.65 * (1.0 - trueShadowMask);
}

if (hasMask) {
    // Layers only: composite the rendered surface over the glass effect
    // in a single pass. surfacePixel is premultiplied alpha from Hyprland's
    // surface rendering, so we unpremultiply before the 'over' blend.
    // Outside the true glass body (the outer-shadow margin band), there is
    // no real window content -- force surfA to 0 explicitly rather than
    // trusting maskTex there, since Wayland surface edges commonly bleed a
    // pixel or two of non-zero anti-aliased alpha past their true boundary.
    // Without this, that AA fringe gets blended into compRGB below and shows
    // up as a light contamination in the shadow's falloff band -- invisible
    // on white backgrounds, visible as a light-gray halo on dark ones.
    float surfA = (cornerAlpha > 0.001) ? surfacePixel.a : 0.0;
    vec3 surfRGB = surfA > 0.001 ? surfacePixel.rgb / surfA : vec3(0.0);

    float compA = surfA + glassA * (1.0 - surfA);
    vec3 compRGB = compA > 0.001
        ? (surfRGB * surfA + color * glassA * (1.0 - surfA)) / compA
        : vec3(0.0);

    // Hyprland's compositor expects premultiplied alpha (blend GL_ONE, GL_ONE_MINUS_SRC_ALPHA).
    fragColor = vec4(compRGB * compA, compA);
} else {
    // Windows: output the glass effect alone, surface is rendered separately by Hyprland.
    // Premultiplied: without this, a fading window's glass keeps full RGB contribution
    // because the GL_ONE source factor adds raw color regardless of alpha.
    fragColor = vec4(color * glassA, glassA);
}
}
)GLSL"},  

{"gaussianblur.frag", R"GLSL(
#version 300 es
precision highp float;  

uniform sampler2D tex;
uniform vec2 direction; // (1.0/width, 0.0) for horizontal, (0.0, 1.0/height) for vertical
uniform float blurRadius; // kernel radius in pixels

in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;  

void main() {
// Compute sigma from radius (covers ~3 sigma)
float sigma = max(blurRadius / 3.0, 0.001);
float invSigma2 = -0.5 / (sigma * sigma);  

int samples = min(int(ceil(blurRadius)), 8);

// Center tap
float w0 = 1.0;
vec4 result = texture(tex, v_texcoord) * w0;
float totalWeight = w0;

// Linear sampling: pair adjacent taps (i, i+1) into a single bilinear fetch.
// The interpolated offset between two texels yields their weighted average
// in one texture() call, halving the total tap count.
for (int i = 1; i <= samples; i += 2) {
    float x1 = float(i);
    float x2 = float(i + 1);
    float w1 = exp(x1 * x1 * invSigma2);
    float w2 = (i + 1 <= samples) ? exp(x2 * x2 * invSigma2) : 0.0;
    float wSum = w1 + w2;
    if (wSum < 0.0001) continue;

    // Offset biased toward the heavier weight
    float offset = (x1 * w1 + x2 * w2) / wSum;

    result += texture(tex, v_texcoord + direction * offset) * wSum;
    result += texture(tex, v_texcoord - direction * offset) * wSum;
    totalWeight += 2.0 * wSum;
}

fragColor = result / totalWeight;
}
)GLSL"},
};