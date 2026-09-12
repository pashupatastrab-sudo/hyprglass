#include "GlassLayerSurface.hpp"
#include "BuiltInPresets.hpp"
#include "GlassRenderer.hpp"
#include "Globals.hpp"
#include "LayerGeometry.hpp"

#include <algorithm>
#include <cmath>
#include <hyprland/src/desktop/Workspace.hpp>
#include <GLES3/gl32.h>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprutils/math/Misc.hpp>

static CBox transformedLayerBox(CBox pixelBox, PHLMONITOR monitor) {
    const auto transform = Math::wlTransformToHyprutils(Math::invertTransform(monitor->m_transform));
    pixelBox.transform(transform, monitor->m_transformedSize.x, monitor->m_transformedSize.y).noNegativeSize().round();
    return pixelBox;
}

static std::optional<CBox> computeRegionBox(PHLLS layerSurface, PHLMONITOR monitor, const SGlassRegionBox& region) {
    if (!layerSurface || !monitor)
        return std::nullopt;

    const Vector2D pos  = layerSurface->m_realPosition->value() + Vector2D(region.x, region.y);
    const Vector2D size = Vector2D(region.w, region.h);

    auto box = CBox{pos, size};
    box.translate(-monitor->m_position);
    box.scale(monitor->m_scale).round().noNegativeSize();

    if (!std::isfinite(box.x) || !std::isfinite(box.y) || !std::isfinite(box.w) || !std::isfinite(box.h) || box.w <= 0.0 || box.h <= 0.0)
        return std::nullopt;

    return box;
}

// Matches QML's Easing.InOutCubic so plugin-driven interpolation looks
// identical to the animation curve used for the number-fade/slide.
static double easeInOutCubic(double t) {
    return t < 0.5 ? 4.0 * t * t * t : 1.0 - std::pow(-2.0 * t + 2.0, 3.0) / 2.0;
}

static SGlassRegionBox lerpBox(const SGlassRegionBox& a, const SGlassRegionBox& b, double t) {
    return SGlassRegionBox{
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.w + (b.w - a.w) * t,
        a.h + (b.h - a.h) * t
    };
}

CGlassLayerSurface::CGlassLayerSurface(PHLLS layerSurface)
    : m_layerSurface(layerSurface) {
}

CGlassLayerSurface::~CGlassLayerSurface() {
    if (g_pHyprRenderer && m_lastSize.x > 0 && m_lastSize.y > 0 &&
        std::isfinite(m_lastPosition.x) && std::isfinite(m_lastPosition.y) &&
        std::isfinite(m_lastSize.x) && std::isfinite(m_lastSize.y)) {
        auto box = CBox{m_lastPosition, m_lastSize};
        box.expand(GlassRenderer::SAMPLE_PADDING_PX).noNegativeSize();
        if (box.w > 0.0 && box.h > 0.0)
            g_pHyprRenderer->damageBox(box);
    }
}

bool CGlassLayerSurface::resolveThemeIsDark() const {
    try {
        const auto& config = g_pGlobalState->config;
        const auto theme = readStringConfig(config.defaultTheme);
        if (!theme.empty())
            return theme != "light";
    } catch (...) {}

    return true;
}

std::string CGlassLayerSurface::resolvePresetName() const {
    try {
        const auto layerSurface = m_layerSurface.lock();
        if (layerSurface) {
            const auto& nsPresets = g_pGlobalState->layerNamespacePresets;
            auto it = nsPresets.find(layerSurface->m_namespace);
            if (it != nsPresets.end())
                return it->second;
        }

        const auto& config = g_pGlobalState->config;

        const auto layerPreset = readStringConfig(config.layersPreset);
        if (!layerPreset.empty())
            return std::string(layerPreset);

        const auto defaultPreset = readStringConfig(config.defaultPreset);
        if (!defaultPreset.empty())
            return std::string(defaultPreset);
    } catch (...) {}

    return "default";
}

std::string CGlassLayerSurface::resolvePresetNameFor(const std::string& groupPreset) const {
    if (!groupPreset.empty())
        return groupPreset;
    return resolvePresetName();
}

PHLLS CGlassLayerSurface::getLayerSurface() const {
    return m_layerSurface.lock();
}

void CGlassLayerSurface::setRegionGroup(const std::string& groupId, std::vector<SGlassRegionBox> boxes, std::string presetName, bool layerBelow, const GlassShadowPayload* shadow) {
    if (boxes.empty()) {
        removeRegionGroup(groupId);
        return;
    }

    const bool isNewGroup = m_regionGroups.find(groupId) == m_regionGroups.end();

    auto& group = m_regionGroups[groupId];
    group.presetName = std::move(presetName);
    group.layerBelow = layerBelow;
    if (shadow)
        group.shadow = *shadow;
    else
        group.shadow = GlassShadowPayload{};

    if (group.regions.size() != boxes.size())
        group.regions.assign(boxes.size(), SGlassRegion{});

    for (size_t i = 0; i < boxes.size(); i++)
        group.regions[i].box = boxes[i];

    if (isNewGroup)
        m_regionGroupOrder.push_back(groupId);
}

// One-shot damage for a group that was just set instantly (no animation
// tick, so damageIfMoved()'s per-frame loop -- which is gated on
// group.animation.active -- never fires for it). Without this, an instant
// setRegionGroup() call updates state correctly but never becomes visible,
// since nothing tells Hyprland's damage tracker that pixels changed.
void CGlassLayerSurface::damageRegionGroup(const std::string& groupId) {
    const auto layerSurface = m_layerSurface.lock();
    if (!layerSurface)
        return;

    auto it = m_regionGroups.find(groupId);
    if (it == m_regionGroups.end())
        return;

    const auto monitor = layerSurface->m_monitor.lock();
    if (!monitor)
        return;

    g_pHyprRenderer->damageMonitor(monitor);
    g_pGlobalState->bumpSceneGeneration(monitor.get());
}

void CGlassLayerSurface::animateRegionGroup(const std::string& groupId, std::vector<SGlassRegionBox> fromBoxes,
                                             std::vector<SGlassRegionBox> toBoxes, double durationMs, std::string presetName) {
    if (fromBoxes.empty() || toBoxes.empty() || fromBoxes.size() != toBoxes.size()) {
        // Mismatched/empty arrays can't be interpolated index-for-index --
        // fall back to snapping straight to the end state rather than
        // guessing at a correspondence.
        setRegionGroup(groupId, std::move(toBoxes), std::move(presetName));
        return;
    }

    auto& group = m_regionGroups[groupId];
    group.presetName = std::move(presetName);

    if (group.regions.size() != toBoxes.size())
        group.regions.assign(toBoxes.size(), SGlassRegion{});

    group.animation.active     = true;
    group.animation.fromBoxes  = fromBoxes;
    group.animation.toBoxes    = toBoxes;
    group.animation.startTime  = std::chrono::steady_clock::now();
    group.animation.durationMs = durationMs > 0.0 ? durationMs : 1.0;

    for (size_t i = 0; i < fromBoxes.size(); i++)
        group.regions[i].box = fromBoxes[i];
}

void CGlassLayerSurface::removeRegionGroup(const std::string& groupId) {
    m_regionGroups.erase(groupId);
    m_regionGroupOrder.erase(
        std::remove(m_regionGroupOrder.begin(), m_regionGroupOrder.end(), groupId),
        m_regionGroupOrder.end());
}

void CGlassLayerSurface::clearAllRegionGroups() {
    m_regionGroups.clear();
    m_regionGroupOrder.clear();
}

void CGlassLayerSurface::setRegionClipBox(const SGlassRegionBox& box) {
    m_regionClipBox    = box;
    m_hasRegionClipBox = true;
}

void CGlassLayerSurface::clearRegionClipBox() {
    m_hasRegionClipBox = false;
}

void CGlassLayerSurface::forceRefresh() {
   m_hasCachedSample = false;

   for (auto& [groupId, group] : m_regionGroups) {
       for (auto& region : group.regions)
           region.hasCachedSample = false;
   }

   const auto layerSurface = m_layerSurface.lock();
   if (!layerSurface)
       return;

   const auto currentPosition = layerSurface->m_realPosition->value();
   const auto currentSize     = layerSurface->m_realSize->value();
   if (currentSize.x <= 0.0 || currentSize.y <= 0.0 ||
       !std::isfinite(currentPosition.x) || !std::isfinite(currentPosition.y) ||
       !std::isfinite(currentSize.x) || !std::isfinite(currentSize.y))
       return;

   auto box = CBox{currentPosition, currentSize};
   const auto monitor = layerSurface->m_monitor.lock();
   const float scale = monitor ? monitor->m_scale : 1.0f;
   box.expand(GlassRenderer::SAMPLE_PADDING_PX / scale).noNegativeSize();
   if (box.w > 0.0 && box.h > 0.0)
       g_pHyprRenderer->damageBox(box);

   if (monitor)
       g_pGlobalState->bumpSceneGeneration(monitor.get());
}

void CGlassLayerSurface::damageIfMoved() {
    const auto layerSurface = m_layerSurface.lock();
    if (!layerSurface)
        return;

    const auto currentPosition = layerSurface->m_realPosition->value();
    const auto currentSize     = layerSurface->m_realSize->value();
    if (currentSize.x <= 0.0 || currentSize.y <= 0.0 ||
        !std::isfinite(currentPosition.x) || !std::isfinite(currentPosition.y) ||
        !std::isfinite(currentSize.x) || !std::isfinite(currentSize.y))
        return;

    const bool isAnimating = layerSurface->m_realPosition->isBeingAnimated() ||
                             layerSurface->m_realSize->isBeingAnimated() ||
                             layerSurface->m_alpha->isBeingAnimated() ||
                             layerSurface->m_fadingOut;

    const bool moved = currentPosition != m_lastPosition || currentSize != m_lastSize;

    if (moved || isAnimating) {
        m_lastPosition  = currentPosition;
        m_lastSize      = currentSize;

        auto box = CBox{currentPosition, currentSize};
        const auto monitor = layerSurface->m_monitor.lock();
        const float scale = monitor ? monitor->m_scale : 1.0f;
        box.expand(GlassRenderer::SAMPLE_PADDING_PX / scale).noNegativeSize();
        if (box.w > 0.0 && box.h > 0.0)
            g_pHyprRenderer->damageBox(box);

        if (monitor)
            g_pGlobalState->bumpSceneGeneration(monitor.get());
    }

    // Force live backdrop damage for any actively animating region group.
    // Without this, the desktop content behind a moving GlassRect never
    // gets marked dirty by Hyprland's damage tracker (since the layer
    // surface's own position/size aren't what's moving -- only the
    // region's internal box is), so the region keeps re-sampling stale
    // pixels even though the resample logic itself runs every frame.
    if (!m_regionGroups.empty()) {
        const auto monitor = layerSurface->m_monitor.lock();
        if (monitor) {
            bool anyRegionAnimating = false;
            for (auto& [groupId, group] : m_regionGroups) {
                if (!group.animation.active)
                    continue;
                anyRegionAnimating = true;
                for (auto& region : group.regions) {
                    auto regionBoxOpt = computeRegionBox(layerSurface, monitor, region.box);
                    if (!regionBoxOpt)
                        continue;
                    CBox rbox = *regionBoxOpt;
                    const float scale = monitor->m_scale;
                    rbox.expand(GlassRenderer::SAMPLE_PADDING_PX / scale).noNegativeSize();
                    if (rbox.w > 0.0 && rbox.h > 0.0)
                        g_pHyprRenderer->damageBox(rbox);
                }
            }
            if (anyRegionAnimating)
                g_pGlobalState->bumpSceneGeneration(monitor.get());
        }
    }
}

void CGlassLayerSurface::sampleAndRedirect(PHLMONITOR monitor, float alpha) {
    auto& shaderManager = g_pGlobalState->shaderManager;
    shaderManager.initializeIfNeeded();

    if (!shaderManager.isInitialized())
        return;

    const auto layerSurface = m_layerSurface.lock();
    if (!layerSurface)
        return;

    auto source = g_pHyprRenderer->m_renderData.currentFB;
    if (!source)
        return;

    auto layerBox = LayerGeometry::computeLayerBox(layerSurface, monitor);
    if (!layerBox)
        return;

    CBox transformBox = transformedLayerBox(*layerBox, monitor);

    const uint64_t currentGeneration = g_pGlobalState->getSceneGeneration(monitor.get());
    const auto activeWs = monitor->m_activeWorkspace;
    const bool isAnimating = layerSurface->m_realPosition->isBeingAnimated() ||
                             layerSurface->m_realSize->isBeingAnimated() ||
                             layerSurface->m_alpha->isBeingAnimated() ||
                             (activeWs && activeWs->m_renderOffset->isBeingAnimated());

    {
        const bool backgroundChanged = !m_hasCachedSample ||
                                       currentGeneration != m_lastSceneGeneration ||
                                       isAnimating;

        if (layerSurface->m_fadingOut) {
            // keep whatever cached sample exists
        } else if (backgroundChanged) {
            const bool isDark          = resolveThemeIsDark();
            const std::string preset   = resolvePresetName();
            const SResolveContext ctx  = {preset, isDark, g_pGlobalState->config, g_pGlobalState->customPresets};

            float blurStrength   = resolvePresetFloat(ctx, &SPresetValues::blurStrength, &SOverridableConfig::blurStrength);
            int downscale        = blurStrength >= GlassRenderer::BLUR_DOWNSCALE_THRESHOLD ? GlassRenderer::BLUR_DOWNSCALE_MAX : 1;

            GlassRenderer::sampleBackground(m_sampleFramebuffer, source, transformBox, m_samplePaddingRatio, downscale);

            float blurRadius     = blurStrength * 12.0f / downscale;
            int blurIterations   = std::clamp(static_cast<int>(resolvePresetInt(ctx, &SPresetValues::blurIterations, &SOverridableConfig::blurIterations)), 1, 5);
            int viewportWidth    = static_cast<int>(g_pHyprRenderer->m_renderData.pMonitor->m_transformedSize.x);
            int viewportHeight   = static_cast<int>(g_pHyprRenderer->m_renderData.pMonitor->m_transformedSize.y);
            GlassRenderer::blurBackground(m_sampleFramebuffer, blurRadius, blurIterations, dynamic_cast<Render::GL::CGLFramebuffer*>(source.get())->getFBID(), viewportWidth, viewportHeight);

            m_hasCachedSample      = true;
            m_lastSceneGeneration  = currentGeneration;
        }
    }

    if (!m_regionGroups.empty()) {
        for (auto& groupId : m_regionGroupOrder) {
            auto groupIt = m_regionGroups.find(groupId);
            if (groupIt == m_regionGroups.end())
                continue;
            auto& group = groupIt->second;

            const bool isDark        = resolveThemeIsDark();
            const std::string preset = resolvePresetNameFor(group.presetName);
            const SResolveContext ctx = {preset, isDark, g_pGlobalState->config, g_pGlobalState->customPresets};

            float blurStrength = resolvePresetFloat(ctx, &SPresetValues::blurStrength, &SOverridableConfig::blurStrength);
            int downscale       = blurStrength >= GlassRenderer::BLUR_DOWNSCALE_THRESHOLD ? GlassRenderer::BLUR_DOWNSCALE_MAX : 1;
            float blurRadius    = blurStrength * 12.0f / downscale;
            int blurIterations  = std::clamp(static_cast<int>(resolvePresetInt(ctx, &SPresetValues::blurIterations, &SOverridableConfig::blurIterations)), 1, 5);
            int viewportWidth   = static_cast<int>(g_pHyprRenderer->m_renderData.pMonitor->m_transformedSize.x);
            int viewportHeight  = static_cast<int>(g_pHyprRenderer->m_renderData.pMonitor->m_transformedSize.y);

            if (group.animation.active) {
                const double elapsedMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - group.animation.startTime).count();
                double t = elapsedMs / group.animation.durationMs;

                if (t >= 1.0) {
                    // Animation finished: snap to final boxes and clear.
                    for (size_t i = 0; i < group.regions.size() && i < group.animation.toBoxes.size(); i++)
                        group.regions[i].box = group.animation.toBoxes[i];
                    group.animation.active = false;
                } else {
                    const double eased = easeInOutCubic(std::clamp(t, 0.0, 1.0));
                    for (size_t i = 0; i < group.regions.size() &&
                                       i < group.animation.fromBoxes.size() &&
                                       i < group.animation.toBoxes.size(); i++) {
                        group.regions[i].box = lerpBox(group.animation.fromBoxes[i], group.animation.toBoxes[i], eased);
                    }
                }
            }

            for (auto& region : group.regions) {
                auto regionBoxOpt = computeRegionBox(layerSurface, monitor, region.box);
                if (!regionBoxOpt)
                    continue;

                CBox regionTransformBox = transformedLayerBox(*regionBoxOpt, monitor);

                // Outer shadow paints outside the region's true bounds. Inflate the
                // sample box by the shadow's own extent (blur + spread + offset) so
                // sampleBackground() captures enough backdrop for the margin band
                // drawn later. No-op when there's no outer shadow set.
                if (group.shadow.hasOuter) {
                    float outerMargin = 4.0 + std::max(
                        group.shadow.outerBlur + group.shadow.outerSpread + std::abs(group.shadow.outerOffsetX),
                        group.shadow.outerBlur + group.shadow.outerSpread + std::abs(group.shadow.outerOffsetY));
                    outerMargin *= monitor->m_scale;
                    regionTransformBox.x -= outerMargin;
                    regionTransformBox.y -= outerMargin;
                    regionTransformBox.w += outerMargin * 2.0;
                    regionTransformBox.h += outerMargin * 2.0;
                }

                const bool regionChanged = !region.hasCachedSample ||
                                           currentGeneration != region.lastSceneGeneration ||
                                           isAnimating ||
                                           group.animation.active;

                if (group.layerBelow) {
                    // Skip the raw-backdrop sample entirely — this group's
                    // sample is taken fresh from `target` in compositeAndRestore(),
                    // after everything drawn before it in stack order. Mark
                    // hasCachedSample so compositeAndRestore() doesn't skip
                    // drawing it (that flag just means "has been rendered
                    // this cycle" not "was sampled from raw backdrop").
                    region.hasCachedSample     = true;
                    region.lastSceneGeneration = currentGeneration;
                    continue;
                }
                if (layerSurface->m_fadingOut) {
                    if (!region.hasCachedSample)
                        continue;
                } else if (regionChanged) {
                    GlassRenderer::sampleBackground(region.sampleFramebuffer, source, regionTransformBox, region.samplePaddingRatio, downscale);
                    GlassRenderer::blurBackground(region.sampleFramebuffer, blurRadius, blurIterations,
                                                   dynamic_cast<Render::GL::CGLFramebuffer*>(source.get())->getFBID(), viewportWidth, viewportHeight);

                    region.hasCachedSample     = true;
                    region.lastSceneGeneration = currentGeneration;
                }
            }
        }
    }

    int monitorWidth  = static_cast<int>(monitor->m_transformedSize.x);
    int monitorHeight = static_cast<int>(monitor->m_transformedSize.y);

    if (!m_surfaceTempFramebuffer)
        m_surfaceTempFramebuffer = g_pHyprRenderer->createFB("hyprglass-layer-temp");

    if (m_surfaceTempFramebuffer->m_size.x != monitorWidth || m_surfaceTempFramebuffer->m_size.y != monitorHeight)
        m_surfaceTempFramebuffer->alloc(monitorWidth, monitorHeight, DRM_FORMAT_ARGB8888);

    m_savedCurrentFB = source;

    g_pHyprRenderer->m_renderData.currentFB = m_surfaceTempFramebuffer;
    glBindFramebuffer(GL_FRAMEBUFFER, dynamic_cast<Render::GL::CGLFramebuffer*>(m_surfaceTempFramebuffer.get())->getFBID());

    CBox clearBox = transformBox;
    clearBox.expand(GlassRenderer::SAMPLE_PADDING_PX);
    clearBox = clearBox.intersection(CBox{0.0, 0.0, static_cast<double>(monitorWidth), static_cast<double>(monitorHeight)}).noNegativeSize().round();

    if (std::isfinite(clearBox.x) && std::isfinite(clearBox.y) && std::isfinite(clearBox.w) && std::isfinite(clearBox.h) &&
        clearBox.w > 0.0 && clearBox.h > 0.0) {
        g_pHyprOpenGL->scissor(clearBox, false);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        g_pHyprOpenGL->scissor(nullptr);
    }
}

void CGlassLayerSurface::compositeAndRestore(PHLMONITOR monitor, float alpha) {
    if (m_savedCurrentFB) {
        g_pHyprRenderer->m_renderData.currentFB = m_savedCurrentFB;
        glBindFramebuffer(GL_FRAMEBUFFER, dynamic_cast<Render::GL::CGLFramebuffer*>(m_savedCurrentFB.get())->getFBID());
        m_savedCurrentFB.reset();
    }

    auto& shaderManager = g_pGlobalState->shaderManager;
    if (!shaderManager.isInitialized())
        return;

    const auto layerSurface = m_layerSurface.lock();
    if (!layerSurface)
        return;

    auto target = g_pHyprRenderer->m_renderData.currentFB;
    if (!target)
        return;

    int monitorWidth  = static_cast<int>(monitor->m_transformedSize.x);
    int monitorHeight = static_cast<int>(monitor->m_transformedSize.y);

    float maskThreshold = 0.001f;
    auto threshIt = g_pGlobalState->layerNamespaceMaskThresholds.find(layerSurface->m_namespace);
    if (threshIt != g_pGlobalState->layerNamespaceMaskThresholds.end())
        maskThreshold = threshIt->second;
    maskThreshold *= std::clamp(alpha, 0.0f, 1.0f);

    if (m_hasCachedSample) {
        auto layerBox = LayerGeometry::computeLayerBox(layerSurface, monitor);
        if (layerBox) {
            CBox rawBox       = *layerBox;
            CBox transformBox = transformedLayerBox(rawBox, monitor);

            const bool isDark          = resolveThemeIsDark();
            const std::string preset   = resolvePresetName();
            const SResolveContext ctx  = {preset, isDark, g_pGlobalState->config, g_pGlobalState->customPresets};

            float cornerRadius     = resolvePresetFloat(ctx, &SPresetValues::cornerRadius, &SOverridableConfig::cornerRadius) * monitor->m_scale;
            float roundingPower    = resolvePresetFloat(ctx, &SPresetValues::roundingPower, &SOverridableConfig::roundingPower);

            GlassRenderer::SMaskInfo maskInfo{
                .textureId         = m_surfaceTempFramebuffer->getTexture()->m_texID,
                .target            = GL_TEXTURE_2D,
                .uvOffset          = {transformBox.x / monitorWidth, transformBox.y / monitorHeight},
                .uvScale           = {transformBox.w / monitorWidth, transformBox.h / monitorHeight},
                .alphaThreshold    = maskThreshold,
            };

            GlassRenderer::applyGlassEffect(m_sampleFramebuffer, target,
                                             rawBox, transformBox, alpha,
                                             cornerRadius, roundingPower, m_samplePaddingRatio, ctx,
                                             &maskInfo, nullptr);
        }
    }

    // Region draw pass is scissor-clipped to m_regionClipBox when set, so
    // regions animating past a fixed UI boundary (e.g. a sidebar) get cut
    // off there instead of painting glass over content that isn't theirs.
    bool clipApplied = false;
    CBox clipTransformBox;
    if (m_hasRegionClipBox) {
        auto clipBoxOpt = computeRegionBox(layerSurface, monitor, m_regionClipBox);
        if (clipBoxOpt) {
            clipTransformBox = *clipBoxOpt;
            clipTransformBox = clipTransformBox
                .intersection(CBox{0.0, 0.0, static_cast<double>(monitorWidth), static_cast<double>(monitorHeight)})
                .noNegativeSize().round();
            if (std::isfinite(clipTransformBox.x) && std::isfinite(clipTransformBox.y) &&
                std::isfinite(clipTransformBox.w) && std::isfinite(clipTransformBox.h) &&
                clipTransformBox.w > 0.0 && clipTransformBox.h > 0.0) {
                clipApplied = true;
            }
        }
    }

    if (clipApplied)
        GlassRenderer::g_pActiveScissorClip = &clipTransformBox;

    for (auto& groupId : m_regionGroupOrder) {
        auto groupIt = m_regionGroups.find(groupId);
        if (groupIt == m_regionGroups.end())
            continue;
        auto& group = groupIt->second;

        const bool isDark        = resolveThemeIsDark();
        const std::string preset = resolvePresetNameFor(group.presetName);
        const SResolveContext ctx = {preset, isDark, g_pGlobalState->config, g_pGlobalState->customPresets};

        float cornerRadius  = resolvePresetFloat(ctx, &SPresetValues::cornerRadius, &SOverridableConfig::cornerRadius) * monitor->m_scale;
        float roundingPower = resolvePresetFloat(ctx, &SPresetValues::roundingPower, &SOverridableConfig::roundingPower);

        // layerBelow groups sample fresh from `target` here, rather than
        // using region.sampleFramebuffer's stale raw-backdrop content from
        // sampleAndRedirect() (which was intentionally skipped for these
        // groups — see there). `target` at this point in the draw already
        // contains the whole-window glass pass and any earlier region
        // groups in stack order, so this is what makes natural glass-on-
        // glass stacking work.
        float layerBelowBlurRadius   = 0.0f;
        int   layerBelowBlurIters    = 0;
        int   layerBelowDownscale    = 1;
        if (group.layerBelow) {
            float blurStrength = resolvePresetFloat(ctx, &SPresetValues::blurStrength, &SOverridableConfig::blurStrength);
            layerBelowDownscale = blurStrength >= GlassRenderer::BLUR_DOWNSCALE_THRESHOLD ? GlassRenderer::BLUR_DOWNSCALE_MAX : 1;
            layerBelowBlurRadius = blurStrength * 12.0f / layerBelowDownscale;
            layerBelowBlurIters  = std::clamp(static_cast<int>(resolvePresetInt(ctx, &SPresetValues::blurIterations, &SOverridableConfig::blurIterations)), 1, 5);
        }

        for (auto& region : group.regions) {
            if (!region.hasCachedSample)
                continue;

            auto regionBoxOpt = computeRegionBox(layerSurface, monitor, region.box);
            if (!regionBoxOpt)
                continue;

            CBox rawBox       = *regionBoxOpt;
            CBox transformBox = transformedLayerBox(rawBox, monitor);

            // Outer shadow needs a bigger draw quad than the region's true bounds.
            // drawRawBox/drawTransformBox are that padded quad; rawBox/transformBox
            // (unchanged above) remain the TRUE region bounds, still used for the
            // mask UV mapping below since m_surfaceTempFramebuffer only has real
            // content at the true region location, not the padded margin.
            CBox drawRawBox = rawBox;
            Vector2D trueBoxUVOffset{0.0, 0.0};
            Vector2D trueBoxUVScale{1.0, 1.0};
            if (group.shadow.hasOuter) {
                float outerMargin = 4.0 + std::max(
                    group.shadow.outerBlur + group.shadow.outerSpread + std::abs(group.shadow.outerOffsetX),
                    group.shadow.outerBlur + group.shadow.outerSpread + std::abs(group.shadow.outerOffsetY));
                drawRawBox.x -= outerMargin;
                drawRawBox.y -= outerMargin;
                drawRawBox.w += outerMargin * 2.0;
                drawRawBox.h += outerMargin * 2.0;

                trueBoxUVOffset = {outerMargin / drawRawBox.w, outerMargin / drawRawBox.h};
                trueBoxUVScale  = {rawBox.w / drawRawBox.w, rawBox.h / drawRawBox.h};
            }
            CBox drawTransformBox = transformedLayerBox(drawRawBox, monitor);

            if (group.layerBelow) {
                GlassRenderer::sampleBackground(region.sampleFramebuffer, target, drawTransformBox, region.samplePaddingRatio, layerBelowDownscale);
                GlassRenderer::blurBackground(region.sampleFramebuffer, layerBelowBlurRadius, layerBelowBlurIters,
                                               dynamic_cast<Render::GL::CGLFramebuffer*>(target.get())->getFBID(), monitorWidth, monitorHeight);
            }

            GlassRenderer::SMaskInfo maskInfo{
                .textureId         = m_surfaceTempFramebuffer->getTexture()->m_texID,
                .target            = GL_TEXTURE_2D,
                .uvOffset          = {transformBox.x / monitorWidth, transformBox.y / monitorHeight},
                .uvScale           = {transformBox.w / monitorWidth, transformBox.h / monitorHeight},
                .alphaThreshold    = maskThreshold,
            };

            GlassRenderer::applyGlassEffect(region.sampleFramebuffer, target,
                                             drawRawBox, drawTransformBox, alpha,
                                             cornerRadius, roundingPower, region.samplePaddingRatio, ctx,
                                             &maskInfo, &group.shadow,
                                             trueBoxUVOffset, trueBoxUVScale);
        }
    }

    if (clipApplied)
        GlassRenderer::g_pActiveScissorClip = nullptr;

    if (clipApplied)
        g_pHyprOpenGL->scissor(nullptr);
}
