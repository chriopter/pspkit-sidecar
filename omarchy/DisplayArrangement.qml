import QtQuick

Item {
  id: layout
  required property string position
  required property color foreground
  required property string fontFamily
  required property real textSize
  signal placed(string value)

  property string preview: ""
  readonly property string current: preview || position
  readonly property string edge: {
    const legacy = { "top-left": "left", "bottom-left": "left", "top-right": "right",
                     "bottom-right": "right", "above": "top", "below": "bottom" }
    return legacy[current] || current.split("-")[0]
  }
  readonly property real fraction: {
    if (current === "above" || current === "below") return 0.5
    if (current.startsWith("top-") && /left|right/.test(current)) return 0
    if (current.startsWith("bottom-") && /left|right/.test(current)) return 1
    return Math.max(0, Math.min(1, Number(current.split("-")[1]) / 100))
  }
  readonly property bool horizontal: edge === "top" || edge === "bottom"
  Rectangle {
    id: monitor
    anchors.centerIn: parent
    width: Math.min(parent.width * 0.58, parent.height * 0.55 * 16 / 9)
    height: width * 9 / 16
    color: "transparent"
    border.color: Qt.darker(layout.foreground, 1.6)
    Text {
      anchors.centerIn: parent
      text: "Monitor"
      font.family: layout.fontFamily
      font.pixelSize: layout.textSize
      color: layout.foreground
    }
  }
  Rectangle {
    id: psp
    width: Math.max(layout.textSize * 3, monitor.width * 0.25)
    height: width * 272 / 480
    x: layout.horizontal ? monitor.x + layout.fraction * (monitor.width - width)
       : layout.edge === "left" ? monitor.x - width : monitor.x + monitor.width
    y: !layout.horizontal ? monitor.y + layout.fraction * (monitor.height - height)
       : layout.edge === "top" ? monitor.y - height : monitor.y + monitor.height
    color: Qt.alpha(layout.foreground, pointer.pressed ? 0.28 : 0.14)
    border.color: layout.foreground
    Text {
      anchors.centerIn: parent
      text: "PSP"
      font.family: layout.fontFamily
      font.pixelSize: layout.textSize
      color: layout.foreground
    }
  }
  MouseArea {
    id: pointer
    anchors.fill: parent
    hoverEnabled: true
    preventStealing: true
    property real gripX
    property real gripY
    function onPsp(x, y) {
      return x >= psp.x && x <= psp.x + psp.width && y >= psp.y && y <= psp.y + psp.height
    }
    cursorShape: pressed ? Qt.ClosedHandCursor : onPsp(mouseX, mouseY) ? Qt.OpenHandCursor : Qt.ArrowCursor
    onPressed: mouse => {
      if (!onPsp(mouse.x, mouse.y)) { mouse.accepted = false; return }
      gripX = mouse.x - psp.x
      gripY = mouse.y - psp.y
    }
    onPositionChanged: mouse => {
      if (!pressed) return
      const x = mouse.x - gripX - monitor.x, y = mouse.y - gripY - monitor.y
      const dx = monitor.width - psp.width, dy = monitor.height - psp.height
      const alongX = Math.max(0, Math.min(dx, x)), alongY = Math.max(0, Math.min(dy, y))
      const edges = [["left", -psp.width, alongY, alongY / dy],
                     ["right", monitor.width, alongY, alongY / dy],
                     ["top", alongX, -psp.height, alongX / dx],
                     ["bottom", alongX, monitor.height, alongX / dx]]
      let nearest = null, distance = Infinity
      for (const candidate of edges) {
        const d = (x - candidate[1]) ** 2 + (y - candidate[2]) ** 2
        if (d < distance) { nearest = candidate; distance = d }
      }
      layout.preview = nearest[0] + "-" + Math.round(100 * nearest[3])
    }
    onReleased: {
      if (layout.preview && layout.preview !== layout.position) layout.placed(layout.preview)
      layout.preview = ""
    }
    onCanceled: layout.preview = ""
  }
}
