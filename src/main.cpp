#include "GlassDecoration.hpp"
#include "GlassSocketServer.hpp"
#include "GlassLayerCompositeElement.hpp"
#include "GlassLayerPassElement.hpp"
#include "GlassLayerSurface.hpp"
#include "GlassRenderer.hpp"
#include "Globals.hpp"
#include "PluginConfig.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/event/EventBus.hpp>

#include <sstream>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

static void clearLayerGlassOnClose(PHLLS layerSurface) {
    if (!g_pGlobalState || !layerSurface)
        return;

    std::erase_if(g_pGlobalState->layerSurfaces, [&](const auto& pair) {
        return pair.first == layerSurface.get() || pair.second->getLayerSurface() == layerSurface;
    });

    if (auto monitor = layerSurface->m_monitor.lock())
        g_pHyprRenderer->damageMonitor(monitor);
}

static void onNewWindow(PHLWINDOW window) {
    if (std::ranges::any_of(window->m_windowDecorations,
                            [](const auto& decoration) { return decoration->getDisplayName() == "HyprGlass"; }))
        return;

    auto decoration = makeUnique<CGlassDecoration>(window);
    g_pGlobalState->decorations.emplace_back(decoration);
    decoration->m_self = decoration;
    HyprlandAPI::addWindowDecoration(PHANDLE, window, std::move(decoration));
}

static void onCloseWindow(PHLWINDOW window) {
    std::erase_if(g_pGlobalState->decorations, [&window](const auto& decoration) {
        auto* deco = decoration.get();
        return !deco || deco->getOwner() == window;
    });
}

static void parseCommaSeparated(StringConfigPtr configPtr, std::unordered_set<std::string>& out) {
    out.clear();
    const auto raw = readStringConfig(configPtr);
    if (raw.empty()) return;

    std::istringstream stream{std::string(raw)};
    std::string token;
    while (std::getline(stream, token, ',')) {
        auto start = token.find_first_not_of(" \t");
        auto end   = token.find_last_not_of(" \t");
        if (start != std::string::npos)
            out.insert(token.substr(start, end - start + 1));
    }
}

template <typename Fn>
static void parseKeyValuePairs(StringConfigPtr configPtr, char separator, Fn&& callback) {
    const auto raw = readStringConfig(configPtr);
    if (raw.empty()) return;

    std::istringstream stream{std::string(raw)};
    std::string token;
    while (std::getline(stream, token, ',')) {
        auto sepPos = token.rfind(separator);
        if (sepPos == std::string::npos) continue;

        auto kStart = token.find_first_not_of(" \t");
        auto kEnd   = token.find_last_not_of(" \t", sepPos - 1);
        auto vStart = token.find_first_not_of(" \t", sepPos + 1);
        auto vEnd   = token.find_last_not_of(" \t");

        if (kStart != std::string::npos && kEnd != std::string::npos &&
            vStart != std::string::npos && vEnd != std::string::npos && kStart <= kEnd && vStart <= vEnd) {
            callback(token.substr(kStart, kEnd - kStart + 1),
                     token.substr(vStart, vEnd - vStart + 1));
        }
    }
}

static void parseLayerNamespaceFilters() {
    const auto& config = g_pGlobalState->config;
    parseCommaSeparated(config.layersNamespaces, g_pGlobalState->layerNamespaceFilter);
    parseCommaSeparated(config.layersExcludeNamespaces, g_pGlobalState->layerNamespaceExclude);

    g_pGlobalState->layerNamespacePresets.clear();
    parseKeyValuePairs(config.layersNamespacePresets, ':', [&](const std::string& ns, const std::string& preset) {
        g_pGlobalState->layerNamespacePresets.emplace(ns, preset);
    });

    g_pGlobalState->layerNamespaceMaskThresholds.clear();
    parseKeyValuePairs(config.layersNamespaceMaskThresholds, '=', [&](const std::string& ns, const std::string& val) {
        try { g_pGlobalState->layerNamespaceMaskThresholds.emplace(ns, std::stof(val)); } catch (...) {}
    });
}

static bool shouldGlassLayer(PHLLS layerSurface) {
    if (!layerSurface)
        return false;

    const auto& ns = layerSurface->m_namespace;

    if (g_pGlobalState->layerNamespaceExclude.contains(ns))
        return false;

    const auto& include = g_pGlobalState->layerNamespaceFilter;
    if (include.empty())
        return true;

    return include.contains(ns);
}

using renderLayerFn = void (*)(Render::IHyprRenderer*, PHLLS, PHLMONITOR, const Time::steady_tp&, bool, bool);

static void hkRenderLayer(Render::IHyprRenderer* thisptr, PHLLS layerSurface, PHLMONITOR monitor,
                           const Time::steady_tp& now, bool popups, bool lockscreen) {
    const auto& config = g_pGlobalState->config;

    if (g_pHyprRenderer->m_bRenderingSnapshot) {
        ((renderLayerFn)g_pGlobalState->renderLayerHook->m_original)(thisptr, layerSurface, monitor, now, popups, lockscreen);
        return;
    }

    std::erase_if(g_pGlobalState->layerSurfaces, [](const auto& pair) {
        return !pair.second->getLayerSurface();
    });

    if (!popups && config.layersEnabled && **config.layersEnabled && shouldGlassLayer(layerSurface)) {
        auto* rawPtr = layerSurface.get();
        auto& layerStates = g_pGlobalState->layerSurfaces;
        auto it = layerStates.find(rawPtr);
        bool freshlyCreated = false;
        if (it != layerStates.end() && !it->second->getLayerSurface()) {
            it->second = std::make_shared<CGlassLayerSurface>(layerSurface);
            freshlyCreated = true;
        } else if (it == layerStates.end()) {
            it = layerStates.emplace(rawPtr, std::make_shared<CGlassLayerSurface>(layerSurface)).first;
            freshlyCreated = true;
        }

        // Apply any pending region-group commands queued for this namespace
        // (via config or a future runtime mechanism). Generic across any
        // namespace/groupId — applied once on creation.
        if (freshlyCreated) {
            if (auto pendingIt = g_pGlobalState->pendingRegions.find(layerSurface->m_namespace);
                pendingIt != g_pGlobalState->pendingRegions.end()) {
                for (auto& [groupId, cmd] : pendingIt->second)
                    it->second->setRegionGroup(groupId, cmd.boxes, cmd.preset, cmd.layerBelow, &cmd.shadow);
            }
        }

        if (layerSurface->m_fadingOut) {
            ((renderLayerFn)g_pGlobalState->renderLayerHook->m_original)(thisptr, layerSurface, monitor, now, popups, lockscreen);
            return;
        }

        float alpha = layerSurface->m_alpha->value();
        if (alpha < 0.001f) {
            ((renderLayerFn)g_pGlobalState->renderLayerHook->m_original)(thisptr, layerSurface, monitor, now, popups, lockscreen);
            return;
        }

        CGlassLayerPassElement::SGlassLayerPassData preData{it->second, alpha};
        g_pHyprRenderer->m_renderPass.add(makeUnique<CGlassLayerPassElement>(preData));

        ((renderLayerFn)g_pGlobalState->renderLayerHook->m_original)(thisptr, layerSurface, monitor, now, popups, lockscreen);

        CGlassLayerCompositeElement::SGlassLayerCompositeData postData{it->second, alpha};
        g_pHyprRenderer->m_renderPass.add(makeUnique<CGlassLayerCompositeElement>(postData));

        it->second->damageIfMoved();
        return;
    }

    ((renderLayerFn)g_pGlobalState->renderLayerHook->m_original)(thisptr, layerSurface, monitor, now, popups, lockscreen);
}



// ── hyprglass.setregions(namespace, groupId, preset, {boxes}) ──────
// Same call mechanism as hyprglass.preset/.layer/.config (addLuaFunction),
// proven working on this Hyprland build. Reachable at runtime via:
//   hyprctl eval 'hyprglass.setregions("calendar","cells","liquidglass_1",{{x=40,y=100,w=120,h=80}})'
// boxes: array of tables with numeric x/y/w/h fields (surface-local
// logical pixels). Passing an empty {} table removes that groupId.
// Works identically for ANY namespace/groupId — not calendar-specific.

// ── hyprglass.animateregions(namespace, groupId, preset, {fromBoxes}, {toBoxes}, durationMs) ──
// Plugin-driven interpolation: call ONCE at the start of a slide/animation
// instead of per-tick. The plugin tweens region position internally on its
// own render clock (same frame as everything else it draws), eliminating
// the QML-frame/IPC/compositor-frame desync that per-tick setregions() had.
static int handleLuaAnimateRegions(lua_State* L) {
    int nargs = lua_gettop(L);
    if (nargs < 6 || !lua_isstring(L, 1) || !lua_isstring(L, 2) || !lua_isstring(L, 3) ||
        !lua_istable(L, 4) || !lua_istable(L, 5) || !lua_isnumber(L, 6))
        return luaL_error(L, "hyprglass.animateregions: expected (namespace, groupId, preset, {fromBoxes}, {toBoxes}, durationMs)");

    std::string ns       = lua_tostring(L, 1);
    std::string groupId  = lua_tostring(L, 2);
    std::string preset   = lua_tostring(L, 3);
    double durationMs    = lua_tonumber(L, 6);

    auto readBoxes = [&](int stackIdx) {
        std::vector<SGlassRegionBox> boxes;
        lua_Integer len = luaL_len(L, stackIdx);
        for (lua_Integer i = 1; i <= len; i++) {
            lua_rawgeti(L, stackIdx, i);
            if (lua_istable(L, -1)) {
                SGlassRegionBox box{};
                lua_getfield(L, -1, "x"); if (lua_isnumber(L, -1)) box.x = lua_tonumber(L, -1); lua_pop(L, 1);
                lua_getfield(L, -1, "y"); if (lua_isnumber(L, -1)) box.y = lua_tonumber(L, -1); lua_pop(L, 1);
                lua_getfield(L, -1, "w"); if (lua_isnumber(L, -1)) box.w = lua_tonumber(L, -1); lua_pop(L, 1);
                lua_getfield(L, -1, "h"); if (lua_isnumber(L, -1)) box.h = lua_tonumber(L, -1); lua_pop(L, 1);
                boxes.push_back(box);
            }
            lua_pop(L, 1);
        }
        return boxes;
    };

    std::vector<SGlassRegionBox> fromBoxes = readBoxes(4);
    std::vector<SGlassRegionBox> toBoxes   = readBoxes(5);

    if (!g_pGlobalState)
        return 0;

    for (auto& [rawPtr, layerState] : g_pGlobalState->layerSurfaces) {
        auto ls = layerState->getLayerSurface();
        if (ls && ls->m_namespace == ns)
            layerState->animateRegionGroup(groupId, fromBoxes, toBoxes, durationMs, preset);
    }

    return 0;
}

// ── hyprglass.setregionclip(namespace, x, y, w, h) ──
// Sets a static scissor rect (surface-local logical pixels, same units as
// setregions) that all region-group draws are clipped to. Call once (e.g.
// on load/resize), not per-frame -- this is for a fixed UI boundary like a
// sidebar edge, not something that needs to track animation. Pass w<=0 or
// h<=0 to clear the clip and let regions draw unclipped again.
static int handleLuaSetRegionClip(lua_State* L) {
    int nargs = lua_gettop(L);
    if (nargs < 5 || !lua_isstring(L, 1) || !lua_isnumber(L, 2) || !lua_isnumber(L, 3) ||
        !lua_isnumber(L, 4) || !lua_isnumber(L, 5))
        return luaL_error(L, "hyprglass.setregionclip: expected (namespace, x, y, w, h)");

    std::string ns = lua_tostring(L, 1);
    SGlassRegionBox box{};
    box.x = lua_tonumber(L, 2);
    box.y = lua_tonumber(L, 3);
    box.w = lua_tonumber(L, 4);
    box.h = lua_tonumber(L, 5);

    if (!g_pGlobalState)
        return 0;

    for (auto& [rawPtr, layerState] : g_pGlobalState->layerSurfaces) {
        auto ls = layerState->getLayerSurface();
        if (!ls || ls->m_namespace != ns)
            continue;
        if (box.w <= 0.0 || box.h <= 0.0)
            layerState->clearRegionClipBox();
        else
            layerState->setRegionClipBox(box);
    }

    return 0;
}

static int handleLuaClearNamespace(lua_State* L) {
    int nargs = lua_gettop(L);
    if (nargs < 1 || !lua_isstring(L, 1))
        return luaL_error(L, "hyprglass.clearnamespace: expected (namespace)");

    std::string ns = lua_tostring(L, 1);

    if (!g_pGlobalState)
        return 0;

    // Bulk-erase every groupId under this namespace's pending regions.
    g_pGlobalState->pendingRegions.erase(ns);

    // Also clear any live surface's region groups for this namespace, same
    // as setregions() does per-groupId, but for everything at once.
    for (auto& [rawPtr, layerState] : g_pGlobalState->layerSurfaces) {
        auto ls = layerState->getLayerSurface();
        if (ls && ls->m_namespace == ns)
            layerState->clearAllRegionGroups();
    }

    return 0;
}

static int handleLuaSetRegions(lua_State* L) {
    int nargs = lua_gettop(L);
    if (nargs < 4 || !lua_isstring(L, 1) || !lua_isstring(L, 2) || !lua_isstring(L, 3) || !lua_istable(L, 4))
        return luaL_error(L, "hyprglass.setregions: expected (namespace, groupId, preset, {boxes}, [layerBelow])");

    std::string ns      = lua_tostring(L, 1);
    std::string groupId = lua_tostring(L, 2);
    std::string preset  = lua_tostring(L, 3);
    // Optional 5th arg. Defaults to false so existing 4-arg call sites are
    // unaffected. When true, this group samples its backdrop from whatever
    // has already been composited into `target` (whole-window glass + any
    // earlier region groups in stack order) instead of the raw pre-glass
    // desktop capture — see GlassLayerSurface.cpp for the render-side logic.
    bool layerBelow = (nargs >= 5 && lua_isboolean(L, 5)) ? lua_toboolean(L, 5) : false;

    std::vector<SGlassRegionBox> boxes;
    lua_Integer len = luaL_len(L, 4);
    for (lua_Integer i = 1; i <= len; i++) {
        lua_rawgeti(L, 4, i);
        if (lua_istable(L, -1)) {
            SGlassRegionBox box{};
            lua_getfield(L, -1, "x"); if (lua_isnumber(L, -1)) box.x = lua_tonumber(L, -1); lua_pop(L, 1);
            lua_getfield(L, -1, "y"); if (lua_isnumber(L, -1)) box.y = lua_tonumber(L, -1); lua_pop(L, 1);
            lua_getfield(L, -1, "w"); if (lua_isnumber(L, -1)) box.w = lua_tonumber(L, -1); lua_pop(L, 1);
            lua_getfield(L, -1, "h"); if (lua_isnumber(L, -1)) box.h = lua_tonumber(L, -1); lua_pop(L, 1);
            boxes.push_back(box);
        }
        lua_pop(L, 1);
    }

    if (!g_pGlobalState)
        return 0;

    if (boxes.empty())
        g_pGlobalState->pendingRegions[ns].erase(groupId);
    else
        g_pGlobalState->pendingRegions[ns][groupId] = SGlobalState::SPendingRegionCommand{preset, boxes, layerBelow};

    // Apply immediately to any live surface for this namespace too, so the
    // caller doesn't have to wait for a remap.
    for (auto& [rawPtr, layerState] : g_pGlobalState->layerSurfaces) {
        auto ls = layerState->getLayerSurface();
        if (ls && ls->m_namespace == ns) {
            layerState->setRegionGroup(groupId, boxes, preset, layerBelow);
            layerState->damageRegionGroup(groupId);
        }
    }

    return 0;
}

// ── hyprglass.refresh(namespace) ──────
// Forces an immediate backdrop re-capture (whole-window + all region
// groups) for every live layer surface matching this namespace, bypassing
// the scene-generation damage-gate for this one call. Reachable via:
//   hyprctl eval \'hl.plugin.hyprglass.refresh("calendar")\'
static int handleLuaRefresh(lua_State* L) {
   int nargs = lua_gettop(L);
   if (nargs < 1 || !lua_isstring(L, 1))
       return luaL_error(L, "hyprglass.refresh: expected (namespace)");

   std::string ns = lua_tostring(L, 1);

   if (!g_pGlobalState)
       return 0;

   for (auto& [rawPtr, layerState] : g_pGlobalState->layerSurfaces) {
       auto ls = layerState->getLayerSurface();
       if (ls && ls->m_namespace == ns)
           layerState->forceRefresh();
   }

   return 0;
}

// ── hyprglass.refresh(namespace) ──────
// Forces an immediate backdrop re-capture (whole-window + all region
// groups) for every live layer surface matching this namespace, bypassing
// the scene-generation damage-gate for this one call. Reachable via:
//   hyprctl eval \'hl.plugin.hyprglass.refresh("calendar")\'
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(PHANDLE,
            std::format("[{}] Version mismatch!", PLUGIN_NAME),
            CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("Version mismatch");
    }

    g_pGlobalState = std::make_unique<SGlobalState>();

    static auto onOpen = Event::bus()->m_events.window.open.listen([&](PHLWINDOW w) { onNewWindow(w); });

    static auto onClose = Event::bus()->m_events.window.close.listen([&](PHLWINDOW w) { onCloseWindow(w); });

    static auto onLayerClosed = Event::bus()->m_events.layer.closed.listen([&](PHLLS layerSurface) { clearLayerGlassOnClose(layerSurface); });

    auto bumpWindowMonitor = [&](PHLWINDOW w) {
        if (w) if (auto mon = w->m_monitor.lock()) g_pGlobalState->bumpSceneGeneration(mon.get());
    };
    static auto onWindowActive = Event::bus()->m_events.window.active.listen(
        [=](PHLWINDOW w, Desktop::eFocusReason) { bumpWindowMonitor(w); });
    static auto onWindowFullscreen = Event::bus()->m_events.window.fullscreen.listen(
        [=](PHLWINDOW w) { bumpWindowMonitor(w); });
    static auto onWindowMoveToWorkspace = Event::bus()->m_events.window.moveToWorkspace.listen(
        [=](PHLWINDOW w, PHLWORKSPACE) { bumpWindowMonitor(w); });
    static auto onWorkspaceActive = Event::bus()->m_events.workspace.active.listen(
        [&](PHLWORKSPACE ws) {
            if (ws) if (auto mon = ws->m_monitor.lock()) g_pGlobalState->bumpSceneGeneration(mon.get());
        });

    static auto onPreConfigReload = Event::bus()->m_events.config.preReload.listen([&]() {
        clearPendingPresets();
        clearPendingLayers();
    });

    static auto onConfigReloaded = Event::bus()->m_events.config.reloaded.listen([&]() {
        initConfigPointers(PHANDLE, g_pGlobalState->config);
        commitPendingPresets();
        parseLayerNamespaceFilters();
        commitPendingLayers();
        validateConfig();
    });


    registerConfig(PHANDLE);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprglass", "setregions", handleLuaSetRegions);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprglass", "clearnamespace", handleLuaClearNamespace);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprglass", "animateregions", handleLuaAnimateRegions);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprglass", "setregionclip", handleLuaSetRegionClip);
   HyprlandAPI::addLuaFunction(PHANDLE, "hyprglass", "refresh", handleLuaRefresh);
    g_glassSocketServer.start();
    initConfigPointers(PHANDLE, g_pGlobalState->config);

    const auto shadowEnabled = Config::mgr()->getConfigValue("decoration:shadow:enabled");
    auto* const PSHADOWENABLED = reinterpret_cast<Hyprlang::INT* const*>(shadowEnabled.dataptr);
    if (PSHADOWENABLED && !**PSHADOWENABLED) {
        HyprlandAPI::invokeHyprctlCommand("keyword", "decoration:shadow:enabled true");
    }

    for (auto& window : g_pCompositor->m_windows) {
        if (window->isHidden() || !window->m_isMapped)
            continue;
        onNewWindow(window);
    }

    auto renderLayerMatches = HyprlandAPI::findFunctionsByName(PHANDLE, "renderLayer");
    for (const auto& match : renderLayerMatches) {
        if (match.demangled.contains("renderLayer") && match.demangled.contains("LayerSurface")) {
            g_pGlobalState->renderLayerHook = HyprlandAPI::createFunctionHook(PHANDLE, match.address, (void*)hkRenderLayer);
            if (g_pGlobalState->renderLayerHook)
                g_pGlobalState->renderLayerHook->hook();
            break;
        }
    }

    if (!g_pGlobalState->renderLayerHook) {
        HyprlandAPI::addNotificationV2(PHANDLE, {
            {"text", std::string("[hyprglass] Could not hook renderLayer — layer glass disabled")},
            {"time", (uint64_t)5000},
            {"color", CHyprColor{1.0, 0.8, 0.2, 1.0}},
        });
    }

    HyprlandAPI::reloadConfig();
    initConfigPointers(PHANDLE, g_pGlobalState->config);
    commitPendingPresets();
    parseLayerNamespaceFilters();
    commitPendingLayers();
    validateConfig();

    return {std::string(PLUGIN_NAME), std::string(PLUGIN_DESCRIPTION), std::string(PLUGIN_AUTHOR), std::string(PLUGIN_VERSION)};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_glassSocketServer.stop();

    if (!g_pGlobalState)
        return;

    g_pHyprRenderer->m_renderPass.removeAllOfType("CGlassPassElement");
    g_pHyprRenderer->m_renderPass.removeAllOfType("CGlassLayerPassElement");
    g_pHyprRenderer->m_renderPass.removeAllOfType("CGlassLayerCompositeElement");

    for (auto& decoration : g_pGlobalState->decorations) {
        if (auto* deco = decoration.get())
            HyprlandAPI::removeWindowDecoration(PHANDLE, deco);
    }
    g_pGlobalState->decorations.clear();

    if (g_pGlobalState->renderLayerHook) {
        HyprlandAPI::removeFunctionHook(PHANDLE, g_pGlobalState->renderLayerHook);
        g_pGlobalState->renderLayerHook = nullptr;
    }

    g_pGlobalState->layerSurfaces.clear();
    g_pGlobalState->shaderManager.destroy();
    g_pGlobalState.reset();
}
