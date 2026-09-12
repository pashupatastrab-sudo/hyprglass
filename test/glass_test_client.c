#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#pragma pack(push, 1)
struct GlassMsgHeader {
    uint8_t type;
    uint8_t namespaceLen;
    uint8_t groupIdLen;
    uint8_t presetLen;
};
struct GlassRegionPayload {
    float x, y, w, h;
};
#pragma pack(pop)

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <namespace> [groupId]\n", argv[0]);
        return 1;
    }
    const char* ns = argv[1];
    const char* groupId = argc > 2 ? argv[2] : "testgroup";
    const char* preset = argc > 3 ? argv[3] : "";

    const char* sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!sig) { fprintf(stderr, "no HYPRLAND_INSTANCE_SIGNATURE\n"); return 1; }

    char path[256];
    snprintf(path, sizeof(path), "%s/hyprglass/%s.sock", getenv("XDG_RUNTIME_DIR"), sig);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        perror("connect");
        return 1;
    }
    printf("connected to %s\n", path);

    struct GlassMsgHeader hdr = {
        .type = 1, // SetRegion
        .namespaceLen = (uint8_t)strlen(ns),
        .groupIdLen = (uint8_t)strlen(groupId),
        .presetLen = (uint8_t)strlen(preset)
    };
    struct GlassRegionPayload payload = { .x = 100, .y = 100, .w = 200, .h = 150 };

    write(fd, &hdr, sizeof(hdr));
    write(fd, ns, hdr.namespaceLen);
    write(fd, groupId, hdr.groupIdLen);
    write(fd, preset, hdr.presetLen);
    write(fd, &payload, sizeof(payload));

    printf("sent SetRegion ns='%s' group='%s' box=(100,100,200,150)\n", ns, groupId);
    printf("holding connection open 5s so you can check the effect, then disconnecting...\n");
    sleep(5);

    close(fd);
    printf("disconnected (should trigger ghost-region cleanup)\n");
    return 0;
}
