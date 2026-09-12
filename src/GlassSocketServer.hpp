#pragma once
#include <string>
#include <unordered_map>
#include <wayland-server-core.h>

// Owns the shared listener socket at
//   $XDG_RUNTIME_DIR/hyprglass/<HYPRLAND_INSTANCE_SIGNATURE>.sock
// and one wl_event_source per connected client. Each client's namespace is
// learned from the first message it sends (Hello, or implicitly from the
// first SetRegion). On disconnect, that namespace's regions are cleared —
// this closes the ghost-region gap for GlassItem clients without depending
// on QML-side cleanup running.
class CGlassSocketServer {
  public:
    bool start();   // call from PLUGIN_INIT
    void stop();    // call from PLUGIN_EXIT

  private:
    static int onListenReadable(int fd, uint32_t mask, void* data);
    static int onClientReadable(int fd, uint32_t mask, void* data);

    void acceptClient();
    void handleClientData(int clientFd);
    void dropClient(int clientFd);

    int                                            m_listenFd = -1;
    wl_event_source*                               m_listenSource = nullptr;
    std::string                                    m_socketPath;

    struct SClientState {
        wl_event_source* source = nullptr;
        std::string      ns;      // learned once first message arrives
        bool             known = false;
    };
    std::unordered_map<int, SClientState> m_clients; // fd -> state
};

extern CGlassSocketServer g_glassSocketServer;
