#pragma once
#include <cstdint>
#include <cstddef>

// Wire protocol for the persistent low-latency socket used by the native
// QML GlassItem plugin. Coexists with the existing hyprctl/Lua API — both
// paths call the same internal region-update functions.
//
// Wire layout for SetRegion / ClearRegion:
//   [GlassMsgHeader][namespace bytes][groupId bytes][GlassRegionPayload?]
// GlassRegionPayload is present for SetRegion only.

enum class GlassMsgType : uint8_t {
    SetRegion   = 1,
    ClearRegion = 2,
    Hello       = 3,
};

#pragma pack(push, 1)
struct GlassMsgHeader {
    uint8_t type;         // GlassMsgType
    uint8_t namespaceLen; // bytes of namespace string that follow
    uint8_t groupIdLen;   // bytes of groupId string that follow
    uint8_t presetLen;    // bytes of preset string that follow (0 = keep existing preset for this group)
    uint8_t layerBelow;   // 0 or 1 — sample already-composited target instead of raw backdrop
};

struct GlassRegionPayload {
    float x, y, w, h; // logical, surface-local — matches SGlassRegionBox
};

// Appended after GlassRegionPayload on every SetRegion message (v1: always
// present, zeroed/off when no shadow is set — see hasInner/hasOuter).
struct GlassShadowPayload {
    uint8_t hasInner;
    uint8_t hasOuter;
    float   innerOffsetX, innerOffsetY, innerBlur, innerSpread;
    float   innerR, innerG, innerB, innerA;
    float   outerOffsetX, outerOffsetY, outerBlur, outerSpread;
    float   outerR, outerG, outerB, outerA;
    uint8_t outerIsTrue; // 1 = "true" optical outer shadow, 0 = flat/false (legacy) shadow
};
#pragma pack(pop)

constexpr size_t GLASS_MAX_NAME_LEN = 255;
