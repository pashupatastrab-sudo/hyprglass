#pragma once

#include "GlassLayerSurface.hpp"
#include "GlassProtocol.hpp"
#include "PluginConfig.hpp"
#include "ShaderManager.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

class CGlassDecoration;

struct SGlobalState {
    std::vector<WP<CGlassDecoration>> decorations;
    CShaderManager                    shaderManager;
    SPluginConfig                     config;

    std::unordered_map<std::string, SCustomPreset> customPresets;

    SP<Render::IFramebuffer> blurTempFramebuffer;

    std::unordered_map<Desktop::View::CLayerSurface*, std::shared_ptr<CGlassLayerSurface>> layerSurfaces;

    std::unordered_set<std::string> layerNamespaceFilter;
    std::unordered_set<std::string> layerNamespaceExclude;
    std::unordered_map<std::string, std::string> layerNamespacePresets;
    std::unordered_map<std::string, float> layerNamespaceMaskThresholds;

    std::unordered_map<CMonitor*, uint64_t> sceneGeneration;

    uint64_t getSceneGeneration(CMonitor* mon) const {
        auto it = sceneGeneration.find(mon);
        return it != sceneGeneration.end() ? it->second : 0;
    }
    void bumpSceneGeneration(CMonitor* mon) { sceneGeneration[mon]++; }

    // Pending region-group commands, keyed by [namespace][groupId]. Applied
    // the moment a CGlassLayerSurface exists for that namespace. Generic
    // across any namespace/groupId — not hardcoded to any one app/element.
    struct SPendingRegionCommand {
        std::string preset;
        std::vector<SGlassRegionBox> boxes;
        bool layerBelow = false;
        GlassShadowPayload shadow{}; // zeroed/off by default (v1: single shadow, no stacking)
    };
    std::unordered_map<std::string, std::unordered_map<std::string, SPendingRegionCommand>> pendingRegions;

    CFunctionHook* renderLayerHook = nullptr;
};

using Render::GL::g_pHyprOpenGL;

inline HANDLE                        PHANDLE = nullptr;
inline std::unique_ptr<SGlobalState> g_pGlobalState;

inline constexpr std::string_view PLUGIN_NAME        = "hyprglass";
inline constexpr std::string_view PLUGIN_DESCRIPTION = "Apple-style Liquid Glass effect";
inline constexpr std::string_view PLUGIN_AUTHOR      = "Every Linux developer inspired by Apple design";
inline constexpr std::string_view PLUGIN_VERSION     = "1.0.0";
