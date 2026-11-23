import QtQuick
import Quickshell
import Quickshell.Io
import qs.Ui
import qs.Commons

// The PSP display in the Omarchy bar: a small monitor, placed beside the
// bar's own display icon. A click opens a card: the PSP on (power cycle
// through the relay, PSPLink, the display) or off (the display, PSPLink's
// poweroff; the relay stays on), the relay alone, the corner of the main
// monitor the display goes to, its scale and the one workspace on it. All the
// work is in ../tray/bench.sh; the state comes from sysfs.
BarWidget {
  id: root
  moduleName: "chriopter.sidecar"

  // off | psplink | loaded | display, as bench.sh state prints it
  property string state: "off"
  property string position: "bottom-right"
  property string scale: "1"
  property string workspace: "9"
  property string relay: ""   // on | off | "" not known yet
  property string step: ""    // the line bench.sh printed last
  property string note: ""    // after the relay alone: its last line, what that does to the PSP
  property bool relayAlone: false
  property bool busy: false
  property bool popupOpen: false
  property string lastError: ""
  readonly property string stateText: state === "display" ? "PSP on, display on"
    : state === "loaded" ? "PSP on, Linux has not taken the display"
    : state === "psplink" ? "PSP on, display off"
    : "PSP off"
  readonly property string relayText: relay === "" ? "Relay: …" : "Relay " + relay

  // The plugin folder is usually a link to the repository's omarchy/: bench.sh
  // is found beside the real file.
  readonly property string self: Qt.resolvedUrl("BarWidget.qml").toString().replace(/^file:\/\//, "")
  function bench(args) {
    return ["bash", "-c", 'exec "$(dirname "$(readlink -f "$0")")/../tray/bench.sh" "$@"', self].concat(args)
  }
  function close() { popupOpen = false }   // PopupCard calls this on its owner
  onPopupOpenChanged: if (popupOpen && !busy) { query.running = true; relayQuery.running = true }
  function run(args) {
    if (busy) return
    busy = true
    lastError = ""
    step = "Starting…"
    note = ""
    relayAlone = args[0] === "relay"
    action.command = bench(args)
    action.running = true
  }

  implicitWidth: label.implicitWidth + Style.space(14)
  implicitHeight: barSize

  Process {
    id: query
    command: root.bench(["state"])
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: root.state = text.trim() || "off"
    }
  }

  Process {
    id: corner
    command: root.bench(["position"])
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: root.position = text.trim() || "bottom-right"
    }
  }

  Process {
    id: scaleQuery
    command: root.bench(["scale"])
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: root.scale = text.trim() || "1"
    }
  }

  Process {
    id: workspaceQuery
    command: root.bench(["workspace"])
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: root.workspace = text.trim() || "9"
    }
  }

  // The relay is asked over the network: only when the card opens and after
  // an action, never on the timer.
  Process {
    id: relayQuery
    command: root.bench(["relay", "status"])
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: root.relay = text.trim()
    }
  }

  Process {
    id: action
    stdout: SplitParser { onRead: function(line) { root.step = line } }
    stderr: StdioCollector { id: errors; waitForEnd: true }
    onExited: function(code) {
      root.busy = false
      root.note = code === 0 && root.relayAlone ? root.step : ""
      root.step = ""
      query.running = true
      corner.running = true
      scaleQuery.running = true
      workspaceQuery.running = true
      relayQuery.running = true
      root.lastError = code !== 0 ? (errors.text.trim().split("\n")[0] || "exit " + code) : ""
    }
  }

  // The PSP can also be switched manually or by a script: look now and
  // then. One short read of sysfs, no network and nothing sent to the PSP.
  Timer {
    interval: 10000
    repeat: true
    running: !root.busy
    triggeredOnStart: true
    onTriggered: query.running = true
  }
  Component.onCompleted: { corner.running = true; scaleQuery.running = true; workspaceQuery.running = true }

  Text {
    id: label
    anchors.centerIn: parent
    textFormat: Text.PlainText
    // A small monitor: dim while the PSP is off, half while it is on without
    // the display, full with the display.
    text: root.busy ? "󰔟" : "󱡶"
    color: root.busy || root.state === "display" ? root.bar.barForeground
      : Qt.darker(root.bar.barForeground, root.state === "off" ? 2.2 : 1.4)
    font.family: root.bar.fontFamily
    font.pixelSize: Style.font.icon
  }

  MouseArea {
    anchors.fill: parent
    cursorShape: Qt.PointingHandCursor
    onClicked: root.popupOpen = !root.popupOpen
  }

  component Action: Button {
    width: parent.width
    leftAlign: true
    bordered: true
    foreground: root.bar.foreground
    enabled: !root.busy
    opacity: enabled ? 1 : 0.4
  }

  // One row of the card: its name on the left, the control on the right.
  component Field: Item {
    id: field
    property string label: ""
    default property alias control: holder.children
    readonly property real controlWidth: holder.width
    width: parent.width
    height: Math.max(name.implicitHeight, holder.childrenRect.height)
    Text {
      id: name
      anchors.verticalCenter: parent.verticalCenter
      textFormat: Text.PlainText
      text: field.label
      color: Qt.darker(root.bar.foreground, 1.3)
      font.family: root.bar.fontFamily
      font.pixelSize: Style.font.bodySmall
    }
    Item {
      id: holder
      anchors.right: parent.right
      width: parent.width - Style.space(84)
      height: childrenRect.height
    }
  }

  component Pair: Row {
    width: parent.width
    spacing: Style.space(6)
  }

  component Half: Action {
    width: (parent.width - parent.spacing) / 2
    leftAlign: false
  }

  // A choice kept by bench.sh: the list shows `chosen` again once it has answered.
  component Choice: Field {
    id: choice
    property string what: ""
    property string chosen: ""
    property var options: []
    readonly property real below: list.popupOpen
      ? y + height + Math.min(options.length, 8) * (Style.spacing.popupRowHeight + Style.spacing.labelGap) + Style.space(12) - column.implicitHeight
      : 0
    Dropdown {
      id: list
      width: choice.controlWidth
      showLabel: false
      fontFamily: root.bar.fontFamily
      foreground: root.bar.foreground
      enabled: !root.busy
      opacity: enabled ? 1 : 0.4
      options: choice.options
      value: choice.chosen
      onChanged: function(v) {
        root.run([choice.what, v])
        value = Qt.binding(function() { return choice.chosen })
      }
    }
  }

  PopupCard {
    id: popup
    anchorItem: root
    bar: root.bar
    owner: root
    open: root.popupOpen
    contentWidth: popup.fittedContentWidth(Style.space(260))
    contentHeight: popup.fittedContentHeight(column.implicitHeight + column.overflow)

    Column {
      id: column
      width: parent.width
      // A dropdown's list opens inside the card's window: the card grows by
      // what the open list needs below the last row.
      readonly property real overflow: Math.max(0, scaleField.below, workspaceField.below)
      spacing: Style.space(8)

      Text {
        textFormat: Text.PlainText
        text: "PSP Sidecar"
        color: root.bar.foreground
        font.family: root.bar.fontFamily
        font.pixelSize: Style.font.subtitle
        font.bold: true
      }
      Text {
        width: parent.width
        textFormat: Text.PlainText
        text: root.stateText + " · " + root.relayText
        color: Qt.darker(root.bar.foreground, 1.3)
        font.family: root.bar.fontFamily
        font.pixelSize: Style.font.bodySmall
        wrapMode: Text.WordWrap
      }
      // What is happening now: a power cycle alone takes 15 s and more.
      Text {
        width: parent.width
        visible: root.busy
        textFormat: Text.PlainText
        text: "󰔟  " + root.step
        color: root.bar.foreground
        font.family: root.bar.fontFamily
        font.pixelSize: Style.font.body
        wrapMode: Text.WordWrap
      }
      Text {
        width: parent.width
        visible: !root.busy && root.note !== ""
        textFormat: Text.PlainText
        text: root.note
        color: Qt.darker(root.bar.foreground, 1.3)
        font.family: root.bar.fontFamily
        font.pixelSize: Style.font.caption
        wrapMode: Text.WordWrap
      }

      // Half way (PSP on, no display) offers both: on finishes, off undoes.
      Field {
        label: "PSP"
        Pair {
          Half { text: "On"; enabled: !root.busy && root.state !== "display"; onClicked: root.run(["on"]) }
          Half { text: "Off"; enabled: !root.busy && root.state !== "off"; onClicked: root.run(["off"]) }
        }
      }
      Field {
        label: "Relay"
        Pair {
          Half { text: "On"; enabled: !root.busy && root.relay !== "on"; onClicked: root.run(["relay", "on"]) }
          Half { text: "Off"; enabled: !root.busy && root.relay !== "off"; onClicked: root.run(["relay", "off"]) }
        }
      }
      Text {
        text: "Position · Drag the PSP"
        color: Qt.darker(root.bar.foreground, 1.3)
        font.family: root.bar.fontFamily
        font.pixelSize: Style.font.bodySmall
      }
      DisplayArrangement {
        width: parent.width
        height: Style.space(112)
        position: root.position
        foreground: root.bar.foreground
        fontFamily: root.bar.fontFamily
        textSize: Style.font.caption
        enabled: !root.busy
        onPlaced: value => {
          root.position = value
          root.run(["position", value])
        }
      }
      Choice {
        id: scaleField
        label: "Scale"
        what: "scale"
        chosen: root.scale
        options: [{ value: "0.25", label: "25%" }, { value: "0.5", label: "50%" },
                  { value: "0.8", label: "80%" }, { value: "1", label: "100%" }]
      }
      // The only workspace on the PSP.
      Choice {
        id: workspaceField
        label: "Workspace"
        what: "workspace"
        chosen: root.workspace
        options: ["1", "2", "3", "4", "5", "6", "7", "8", "9", "10"]
      }

      Text {
        width: parent.width
        visible: root.lastError !== ""
        textFormat: Text.PlainText
        text: root.lastError
        color: Color.urgent
        font.family: root.bar.fontFamily
        font.pixelSize: Style.font.caption
        wrapMode: Text.WordWrap
      }
    }
  }
}
