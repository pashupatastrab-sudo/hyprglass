#pragma once

#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprutils/math/Box.hpp>
#include <cmath>
#include <optional>

namespace LayerGeometry {

[[nodiscard]] inline std::optional<CBox> computeLayerBox(PHLLS layerSurface, PHLMONITOR monitor) {
    if (!layerSurface || !monitor)
        return std::nullopt;

    // Full animated layer geometry in monitor-local framebuffer pixels.
    auto box = CBox{layerSurface->m_realPosition->value(), layerSurface->m_realSize->value()};
    box.translate(-monitor->m_position);
    box.scale(monitor->m_scale).round().noNegativeSize();

    if (!std::isfinite(box.x) || !std::isfinite(box.y) || !std::isfinite(box.w) || !std::isfinite(box.h) || box.w <= 0.0 || box.h <= 0.0)
        return std::nullopt;

    return box;
}

} // namespace LayerGeometry
