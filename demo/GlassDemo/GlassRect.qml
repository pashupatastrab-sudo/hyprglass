import QtQuick
import Quickshell.Io

Item {
    id: root

    // ---- Public API ----
    property string preset: "liquidglass_card"
    property real radius: 24
    property real roundingPower: 3
    property string namespace: "glassdemo"
    property string groupId: "glassrect_" + Math.floor(Math.random() * 1000000)
    property Item mapTarget: null

    // ---- Internal state ----
    property bool _registered: false
    property bool _animating: false
    property var _fromBox: null

    function _currentBox() {
        if (!mapTarget) return null
        var p = root.mapToItem(mapTarget, 0, 0)
        return { x: Math.round(p.x), y: Math.round(p.y), w: Math.round(root.width), h: Math.round(root.height) }
    }

    function _boxToLua(b) {
        return "{x=" + b.x + ",y=" + b.y + ",w=" + b.w + ",h=" + b.h + "}"
    }

    // Public: force a re-registration of this item's current on-screen box.
    // Call this when something OTHER than this item's own x/y/width/height
    // moved it -- e.g. a parent window resize, or a parent layout reflow --
    // since onXChanged/onYChanged only fire when THIS item's own properties
    // change, not when its ancestor's geometry shifts under it.
    function refreshBox() {
        snapGlass()
    }

    function snapGlass() {
        if (_animating) return
        var b = _currentBox()
        if (!b) return
        setRegionsProcess.command = [
            "hyprctl", "eval",
            "hl.plugin.hyprglass.setregions('" + namespace + "','" + groupId + "','" + preset + "',{" + _boxToLua(b) + "})"
        ]
        setRegionsProcess.running = true
        _registered = true
    }

    function _fireAnimate() {
        var toBox = _currentBox()
        if (!toBox || !_fromBox) return
        animateRegionsProcess.command = [
            "hyprctl", "eval",
            "hl.plugin.hyprglass.animateregions('" + namespace + "','" + groupId + "','" + preset +
            "',{" + _boxToLua(_fromBox) + "},{" + _boxToLua(toBox) + "}," + _lastDuration + ")"
        ]
        animateRegionsProcess.running = true
        _registered = true
    }

    function deregister() {
        if (!_registered) return
        deregisterProcess.command = [
            "hyprctl", "eval",
            "hl.plugin.hyprglass.setregions('" + namespace + "','" + groupId + "','" + preset + "',{})"
        ]
        deregisterProcess.running = true
        _registered = false
    }

    // ---- Explicit animation trigger ----
    // Call this instead of just assigning x/y/width/height directly when you
    // want glass to follow smoothly. Example:
    //   myRect.animateTo({ x: 400 }, 800)
    //   myRect.animateTo({ x: 400, y: 100, width: 250 }, 600, Easing.InOutCubic)
    property int _lastDuration: 400

    function animateTo(props, duration, easingType) {
        if (duration === undefined) duration = 400
        if (easingType === undefined) easingType = Easing.InOutCubic

        _fromBox = _currentBox()
        _animating = true
        _lastDuration = duration

        // Compute the target box up front (without actually moving yet) so
        // we can kick off the plugin's own animateregions() call AT THE SAME
        // TIME as the QML animation starts, instead of waiting until the QML
        // animation finishes. Both animations then run in parallel, on their
        // own clocks, over the same duration -- matching each other visually.
        var savedX = root.x, savedY = root.y, savedW = root.width, savedH = root.height
        if (props.x !== undefined) root.x = props.x
        if (props.y !== undefined) root.y = props.y
        if (props.width !== undefined) root.width = props.width
        if (props.height !== undefined) root.height = props.height
        var toBox = _currentBox()
        root.x = savedX; root.y = savedY; root.width = savedW; root.height = savedH

        if (toBox && _fromBox) {
            animateRegionsProcess.command = [
                "hyprctl", "eval",
                "hl.plugin.hyprglass.animateregions('" + namespace + "','" + groupId + "','" + preset +
                "',{" + _boxToLua(_fromBox) + "},{" + _boxToLua(toBox) + "}," + duration + ")"
            ]
            animateRegionsProcess.running = true
            _registered = true
        }

        _anim.stop()

        var targets = []
        for (var key in props) {
            if (key === "x" || key === "y" || key === "width" || key === "height") {
                targets.push({
                    target: root,
                    property: key,
                    to: props[key],
                    duration: duration,
                    easing: { type: easingType }
                })
            }
        }
        _anim.animations = targets.map(function(t) {
            return propAnimComponent.createObject(root, t)
        })
        _anim.start()
    }

    Component {
        id: propAnimComponent
        NumberAnimation {}
    }

    ParallelAnimation {
        id: _anim
        onStopped: {
            root._animating = false
            // clean up dynamically created child animations
            for (var i = 0; i < animations.length; i++) {
                animations[i].destroy()
            }
        }
    }

    // Snap for any geometry change NOT driven by animateTo (e.g. initial
    // placement, or a direct assignment).
    onXChanged: if (_registered && !_animating) snapGlass()
    onYChanged: if (_registered && !_animating) snapGlass()
    onWidthChanged: if (_registered && !_animating) snapGlass()
    onHeightChanged: if (_registered && !_animating) snapGlass()

    Component.onCompleted: {
        if (mapTarget) snapGlass()
    }

    Component.onDestruction: deregister()

    Process { id: setRegionsProcess }
    Process { id: animateRegionsProcess }
    Process { id: deregisterProcess }
}
