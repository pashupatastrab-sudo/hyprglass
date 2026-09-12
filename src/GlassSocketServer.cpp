#include "GlassSocketServer.hpp"
#include "GlassProtocol.hpp"
#include "GlassLayerSurface.hpp"
#include "Globals.hpp"
#include <cstdio>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/debug/log/Logger.hpp>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>

CGlassSocketServer g_glassSocketServer;

// ── Shared internal region-update logic ─────────────────────────────────
// Same effect as handleLuaSetRegions's body (main.cpp): update
// pendingRegions (for future remaps) and apply immediately to any live
// surface matching this namespace. Both the Lua path and this socket path
// call this one function so behavior never diverges.
void glassApplySetRegion(const std::string& ns, const std::string& groupId,
                          const std::string& preset,
                          const std::vector<SGlassRegionBox>& boxes,
                          bool layerBelow,
                          const GlassShadowPayload* shadow) {
    if (!g_pGlobalState)
        return;

    if (boxes.empty())
        g_pGlobalState->pendingRegions[ns].erase(groupId);
    else {
        SGlobalState::SPendingRegionCommand cmd{preset, boxes, layerBelow};
        if (shadow)
            cmd.shadow = *shadow;
        g_pGlobalState->pendingRegions[ns][groupId] = cmd;
    }

    for (auto& [rawPtr, layerState] : g_pGlobalState->layerSurfaces) {
        auto ls = layerState->getLayerSurface();
        if (ls && ls->m_namespace == ns) {
            if (boxes.empty())
                layerState->damageRegionGroup(groupId);  // damage BEFORE removal — group must still exist
            layerState->setRegionGroup(groupId, boxes, preset, layerBelow, shadow);
            if (!boxes.empty())
                layerState->damageRegionGroup(groupId);  // damage AFTER set for new/updated regions
        }
    }
}

// Same effect as handleLuaClearNamespace's body.
void glassApplyClearNamespace(const std::string& ns) {
    if (!g_pGlobalState)
        return;

    g_pGlobalState->pendingRegions.erase(ns);

    for (auto& [rawPtr, layerState] : g_pGlobalState->layerSurfaces) {
        auto ls = layerState->getLayerSurface();
        if (ls && ls->m_namespace == ns)
            layerState->clearAllRegionGroups();
    }
}

// ── Server ───────────────────────────────────────────────────────────────

bool CGlassSocketServer::start() {
    const char* runtimeDir = getenv("XDG_RUNTIME_DIR");
    const char* sig        = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!runtimeDir || !sig) {
        Log::logger->log(Log::ERR, "[hyprglass] socket server: missing XDG_RUNTIME_DIR or HYPRLAND_INSTANCE_SIGNATURE");
        return false;
    }

    std::string dir = std::string(runtimeDir) + "/hyprglass";
    // best-effort mkdir; ignore failure if it already exists
    std::string mkdirCmd = "mkdir -p '" + dir + "'";
    system(mkdirCmd.c_str());

    m_socketPath = dir + "/" + sig + ".sock";
    unlink(m_socketPath.c_str()); // remove stale socket from a prior crash

    m_listenFd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (m_listenFd < 0) {
        Log::logger->log(Log::ERR, "[hyprglass] socket server: socket() failed");
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, m_socketPath.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(m_listenFd, (sockaddr*)&addr, sizeof(addr)) != 0) {
        Log::logger->log(Log::ERR, "[hyprglass] socket server: bind() failed for {}", m_socketPath);
        close(m_listenFd);
        m_listenFd = -1;
        return false;
    }

    if (listen(m_listenFd, 16) != 0) {
        Log::logger->log(Log::ERR, "[hyprglass] socket server: listen() failed");
        close(m_listenFd);
        m_listenFd = -1;
        return false;
    }

    m_listenSource = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, m_listenFd,
                                           WL_EVENT_READABLE, &CGlassSocketServer::onListenReadable, this);

    Log::logger->log(Log::INFO, "[hyprglass] socket server listening at {}", m_socketPath);
    return m_listenSource != nullptr;
}

void CGlassSocketServer::stop() {
    for (auto& [fd, state] : m_clients) {
        if (state.source)
            wl_event_source_remove(state.source);
        close(fd);
    }
    m_clients.clear();

    if (m_listenSource)
        wl_event_source_remove(m_listenSource);
    if (m_listenFd >= 0)
        close(m_listenFd);
    if (!m_socketPath.empty())
        unlink(m_socketPath.c_str());

    m_listenSource = nullptr;
    m_listenFd     = -1;
}

int CGlassSocketServer::onListenReadable(int fd, uint32_t mask, void* data) {
    static_cast<CGlassSocketServer*>(data)->acceptClient();
    return 0;
}

void CGlassSocketServer::acceptClient() {
    int clientFd = accept4(m_listenFd, nullptr, nullptr, SOCK_NONBLOCK);
    if (clientFd < 0)
        return;

    SClientState state{};
    state.source = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, clientFd,
                                         WL_EVENT_READABLE, &CGlassSocketServer::onClientReadable, this);
    m_clients[clientFd] = state;
    Log::logger->log(Log::INFO, "[hyprglass] socket server: client connected (fd {})", clientFd);
}

int CGlassSocketServer::onClientReadable(int fd, uint32_t mask, void* data) {
    auto* self = static_cast<CGlassSocketServer*>(data);
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        self->dropClient(fd);
        return 0;
    }
    self->handleClientData(fd);
    return 0;
}

void CGlassSocketServer::handleClientData(int clientFd) {
    GlassMsgHeader hdr{};
    ssize_t n = recv(clientFd, &hdr, sizeof(hdr), 0);
    if (n <= 0) {
        dropClient(clientFd);
        return;
    }
    if (n != sizeof(hdr)) {
        Log::logger->log(Log::WARN, "[hyprglass] socket server: short header read, dropping client");
        dropClient(clientFd);
        return;
    }

    char nsBuf[GLASS_MAX_NAME_LEN + 1]      = {0};
    char groupIdBuf[GLASS_MAX_NAME_LEN + 1] = {0};
    char presetBuf[GLASS_MAX_NAME_LEN + 1]  = {0};

    if (hdr.namespaceLen > 0 && recv(clientFd, nsBuf, hdr.namespaceLen, 0) != hdr.namespaceLen) {
        dropClient(clientFd);
        return;
    }
    if (hdr.groupIdLen > 0 && recv(clientFd, groupIdBuf, hdr.groupIdLen, 0) != hdr.groupIdLen) {
        dropClient(clientFd);
        return;
    }
    if (hdr.presetLen > 0 && recv(clientFd, presetBuf, hdr.presetLen, 0) != hdr.presetLen) {
        dropClient(clientFd);
        return;
    }

    std::string ns(nsBuf, hdr.namespaceLen);
    std::string groupId(groupIdBuf, hdr.groupIdLen);
    std::string preset(presetBuf, hdr.presetLen);

    auto& state = m_clients[clientFd];
    if (!ns.empty()) {
        state.ns    = ns;
        state.known = true;
    }

    switch (static_cast<GlassMsgType>(hdr.type)) {
        case GlassMsgType::Hello:
            // namespace already recorded above; nothing else to do
            break;

        case GlassMsgType::SetRegion: {
            GlassRegionPayload payload{};
            if (recv(clientFd, &payload, sizeof(payload), 0) != sizeof(payload)) {
                dropClient(clientFd);
                return;
            }
            GlassShadowPayload shadowPayload{};
            if (recv(clientFd, &shadowPayload, sizeof(shadowPayload), 0) != sizeof(shadowPayload)) {
                dropClient(clientFd);
                return;
            }
            SGlassRegionBox box{payload.x, payload.y, payload.w, payload.h};
            // presetLen == 0 means "keep whatever preset this group already has" —
            // look it up from pendingRegions rather than clobbering it with "".
            std::string effectivePreset = preset;
            if (effectivePreset.empty() && g_pGlobalState) {
                auto nsIt = g_pGlobalState->pendingRegions.find(ns.empty() ? state.ns : ns);
                if (nsIt != g_pGlobalState->pendingRegions.end()) {
                    auto groupIt = nsIt->second.find(groupId);
                    if (groupIt != nsIt->second.end())
                        effectivePreset = groupIt->second.preset;
                }
            }
            const GlassShadowPayload* shadowArg = (shadowPayload.hasInner || shadowPayload.hasOuter) ? &shadowPayload : nullptr;
            glassApplySetRegion(ns.empty() ? state.ns : ns, groupId, effectivePreset, {box}, hdr.layerBelow != 0, shadowArg);
            break;
        }

        case GlassMsgType::ClearRegion: {
            const std::string& clearNs = ns.empty() ? state.ns : ns;
            if (!groupId.empty())
                glassApplySetRegion(clearNs, groupId, "", {}, false, nullptr);
            else
                glassApplyClearNamespace(clearNs);
            // Force a new frame so compositeAndRestore() runs with the
            // updated (now-empty) region list — damageBox/damageMonitor
            // alone don't guarantee hkRenderLayer is re-invoked.
            if (g_pCompositor) {
                for (auto& monitor : g_pCompositor->m_monitors)
                    g_pCompositor->scheduleFrameForMonitor(monitor);
            }
            break;
        }

        default:
            Log::logger->log(Log::WARN, "[hyprglass] socket server: unknown message type {}", (int)hdr.type);
            break;
    }
}

void CGlassSocketServer::dropClient(int clientFd) {
    auto it = m_clients.find(clientFd);
    if (it != m_clients.end()) {
        if (it->second.source)
            wl_event_source_remove(it->second.source);

        // Ghost-region protection: an ungracefully-killed GlassItem client
        // (crash, kill -9) still triggers this via WL_EVENT_HANGUP/read()==0,
        // so its regions get cleared automatically — no dependency on QML's
        // Component.onDestruction.
        if (it->second.known && !it->second.ns.empty())
            glassApplyClearNamespace(it->second.ns);

        m_clients.erase(it);
    }
    close(clientFd);
    Log::logger->log(Log::INFO, "[hyprglass] socket server: client disconnected (fd {})", clientFd);
}
