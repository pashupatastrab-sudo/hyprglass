import QtQuick
import QtQuick.Controls
import Quickshell
import Quickshell.Wayland
import Quickshell.Io
import "."
import Hyprglass

ShellRoot {
   PanelWindow {
       id: win
       screen: Quickshell.screens[0]
       WlrLayershell.namespace: "glassdemo"
       WlrLayershell.layer: WlrLayer.Overlay
       WlrLayershell.keyboardFocus: WlrKeyboardFocus.OnDemand
       color: "transparent"

       Rectangle {
           id: mainBgRect
           color: whiteBgToggle.checked ? "transparent" : Qt.rgba(1,1,1,0.02)
           anchors.fill: parent
       }

       anchors { top: true; left: true }
       margins { top: 100; left: 100 }
       implicitWidth: 900
       implicitHeight: 500

       GlassRect {
           id: card
           x: 50; y: 50
           width: 200; height: 120
           mapTarget: win.contentItem
           namespace: "glassdemo"
           groupId: "maincard"
           preset: "liquidglass_card"

           property bool atRight: false

           function moveCard() {
               atRight = !atRight
               animateTo({ x: atRight ? 600 : 50 }, 800)
           }
       }

       Row {
           anchors.bottom: parent.bottom
           anchors.horizontalCenter: parent.horizontalCenter
           anchors.bottomMargin: 40
           Button {
               text: "Move"
               onClicked: card.moveCard()
           }
           Button {
               id: whiteBgToggle
               text: checked ? "Glass BG" : "Transparent BG"
               checkable: true
               onClicked: {}
           }
       }

       Item {
           id: nativeBox
           width: 150; height: 100
           x: 400; y: 250

           GlassItem {
               anchors.fill: parent
               namespaceName: "glassdemo"
               groupId: "nativebox"
               preset: "cellclear"
               layerBelow: true
               active: win.nativeBoxActive
           }

           Behavior on x { NumberAnimation { duration: 800; easing.type: Easing.InOutQuad } }
       }

       property bool nativeBoxActive: true
       property bool stackedCellActive: true

       Row {
           anchors.bottom: parent.bottom
           anchors.horizontalCenter: parent.horizontalCenter
           anchors.bottomMargin: 90
           Button {
               text: "Move Native"
               onClicked: nativeBox.x = nativeBox.x === 400 ? 700 : 400
           }
           Button {
               text: win.nativeBoxActive ? "Deactivate Glass" : "Activate Glass"
               onClicked: win.nativeBoxActive = !win.nativeBoxActive
           }
           Button {
               text: win.stackedCellActive ? "Destruct Round" : "Reconstruct Round"
               onClicked: win.stackedCellActive = !win.stackedCellActive
           }
       }

       // Test 1: SpringAnimation
       Item {
           id: springBox
           width: 150; height: 100
           x: 400; y: 400

           GlassItem {
               anchors.fill: parent
               namespaceName: "glassdemo"
               groupId: "springbox"
               preset: "cellclear"
               layerBelow: true
           }

           Behavior on x {
               SpringAnimation { spring: 2; damping: 0.2 }
           }
       }

       Row {
           anchors.top: parent.top
           anchors.horizontalCenter: parent.horizontalCenter
           anchors.topMargin: 10
           Button {
               text: "Move Spring"
               onClicked: springBox.x = springBox.x === 400 ? 700 : 400
           }
       }

       // Test 2: Mouse drag
       Item {
           id: dragBox
           width: 150; height: 100
           x: 50; y: 300

           GlassItem {
               anchors.fill: parent
               namespaceName: "glassdemo"
               groupId: "dragbox"
               preset: "cellclear"
               layerBelow: true
               //innerShadow: "10px 10px 20px 5px  rgba(0,0,0,0.85)"
               outerShadow: "15px 15px 10px 0px rgba(0,0,0,0.7) true"
           }

           MouseArea {
               anchors.fill: parent
               drag.target: dragBox
               drag.axis: Drag.XAndYAxis
           }
       }

       // Test 4: layerBelow -- draggable, no-blur, distortion-only cell
       // stacked on top of glassdemo's own liquidglass_2 whole-window glass.
       // layerBelow=true means this region samples the already-composited
       // glass beneath it instead of the raw desktop, so it should look like
       // clear glass sitting on top of the blurred background glass, not a
       // hole punched through to the desktop.
       Item {
           id: stackedCell
           x: 350; y: 50
           width: 140; height: 140

           GlassItem {
               anchors.fill: parent
               namespaceName: "glassdemo"
               groupId: "stackedcell"
               preset: "liquidglass_round"
               layerBelow: true
               outerShadow: "0px 18px 30px 0px rgba(0,0,0,0.55) true"
               active: win.stackedCellActive
           }

           MouseArea {
               anchors.fill: parent
               drag.target: stackedCell
           }
       }

       Row {
           anchors.top: parent.top
           anchors.right: parent.right
           anchors.topMargin: 10
           anchors.rightMargin: 10
           Text {
               text: "Drag the outlined box — layerBelow test"
               color: "white"
           }
       }

       // Test 3: width/height animation
       Item {
           id: resizeBox
           x: 650; y: 300
           width: 150; height: 100

           GlassItem {
               anchors.fill: parent
               namespaceName: "glassdemo"
               groupId: "resizebox"
               preset: "cellclear"
               layerBelow: false
           }

           Behavior on width { NumberAnimation { duration: 800 } }
           Behavior on height { NumberAnimation { duration: 800 } }
       }

       Row {
           anchors.top: parent.top
           anchors.left: parent.left
           anchors.topMargin: 10
           anchors.leftMargin: 10
           Button {
               text: "Resize"
               onClicked: {
                   resizeBox.width = resizeBox.width === 150 ? 250 : 150
                   resizeBox.height = resizeBox.height === 100 ? 180 : 100
               }
           }
       }
   }
}
