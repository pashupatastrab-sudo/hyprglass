#pragma once

#include "GlassProtocol.hpp"
#include "GlassRenderer.hpp"
#include "PluginConfig.hpp"

#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/render/Framebuffer.hpp>

#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>

struct SGlassRegionBox {
    double x = 0.0;
    double y = 0.0;
    double w = 0.0;
    double h = 0.0;
};

struct SGlassRegion {
    SGlassRegionBox           box;
    SP<Render::IFramebuffer>  sampleFramebuffer;
    Vector2D                  samplePaddingRatio;
    bool                      hasCachedSample     = false;
    uint64_t                  lastSceneGeneration = 0;
};

struct SGlassRegionAnimation {
    bool                          active = false;
    std::vector<SGlassRegionBox>  fromBoxes;
    std::vector<SGlassRegionBox>  toBoxes;
    std::chrono::steady_clock::time_point startTime;
    double                        durationMs = 0.0;
};

struct SGlassRegionGroup {
    std::vector<SGlassRegion>   regions;
    std::string                 presetName;
    SGlassRegionAnimation       animation;
    bool                        layerBelow = false; // sample this group's backdrop from
                                                       // the already-composited target
                                                       // (whole-window glass + earlier
                                                       // groups in stack order) instead of
                                                       // the raw pre-glass desktop capture.
    GlassShadowPayload          shadow{}; // zeroed/off by default (v1: single shadow, no stacking)
};

class CGlassLayerSurface {
  public:
    explicit CGlassLayerSurface(PHLLS layerSurface);
    ~CGlassLayerSurface();

    void sampleAndRedirect(PHLMONITOR monitor, float alpha);
    void compositeAndRestore(PHLMONITOR monitor, float alpha);
    void damageIfMoved();

    [[nodiscard]] PHLLS getLayerSurface() const;

    void setRegionGroup(const std::string& groupId, std::vector<SGlassRegionBox> boxes, std::string presetName, bool layerBelow = false, const GlassShadowPayload* shadow = nullptr);
    void damageRegionGroup(const std::string& groupId); // one-shot damage for an instant (non-animated) region set
    void clearAllRegionGroups();
    void animateRegionGroup(const std::string& groupId, std::vector<SGlassRegionBox> fromBoxes,
                             std::vector<SGlassRegionBox> toBoxes, double durationMs, std::string presetName);
    void removeRegionGroup(const std::string& groupId);
    void setRegionClipBox(const SGlassRegionBox& box);
    void clearRegionClipBox();
   void forceRefresh();

  private:
    PHLLSREF     m_layerSurface;
    SP<Render::IFramebuffer> m_sampleFramebuffer;
    SP<Render::IFramebuffer> m_surfaceTempFramebuffer;
    Vector2D     m_samplePaddingRatio;
    bool         m_hasCachedSample = false;

    Vector2D     m_lastPosition;
    Vector2D     m_lastSize;
    uint64_t     m_lastSceneGeneration = 0;

    SP<Render::IFramebuffer> m_savedCurrentFB;

    std::unordered_map<std::string, SGlassRegionGroup> m_regionGroups;
    std::vector<std::string> m_regionGroupOrder; // insertion order — draw order for
                                                    // compositeAndRestore(); load-bearing
                                                    // for layerBelow stacking correctness.
    bool             m_hasRegionClipBox = false;
    SGlassRegionBox  m_regionClipBox;

    [[nodiscard]] bool        resolveThemeIsDark() const;
    [[nodiscard]] std::string resolvePresetName() const;
    [[nodiscard]] std::string resolvePresetNameFor(const std::string& groupPreset) const;
};
