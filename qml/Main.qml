import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Round-1 main screen (T9, issue #10): variant A "Synoptic" from the T6 mockup
// (design/round-1/index.html?variant=a). Custom window chrome carries the
// identity; international orange is reserved for corrector actions.
//
// Even scaling (T9 requirement): one design-unit factor `s` derived from the
// 1100x720 reference. Every size, gap, and type ramp multiplies by it via u();
// the layout itself fills the real window, so off-ratio windows stretch through
// spacers instead of letterboxing.
ApplicationWindow {
    id: root
    // A -200 is a -200 exactly while its centre tank has fuel (T14, #15): the
    // slot only resolves on a non-zero read, so capacity is the honest test.
    readonly property bool sixTank: (Sim.tanks[4] && Sim.tanks[4].capacity > 0) === true
    // Settings is a full-window page (T34 variant A, T36 · #55): the body under
    // the titlebar swaps out, the titlebar and the event line stay.
    property bool settingsOpen: false

    width: 1100
    height: 810
    minimumWidth: 700
    minimumHeight: 515
    visible: true
    title: qsTr("MaxWarp")
    flags: Qt.Window | Qt.FramelessWindowHint
    color: "transparent"
    background: null

    readonly property bool maximized: root.visibility === Window.Maximized
    readonly property real s: Math.min(width / 1100, height / 810)
    function u(x) { return Math.round(x * s) }
    readonly property int hairW: Math.max(1, Math.round(s))

    // ---------- identity tokens (T6 variant A) ----------
    QtObject {
        id: ui
        readonly property bool dark: Theme.darkActive
        readonly property color bg: dark ? "#14120E" : "#F6F4EE"
        readonly property color surface: dark ? "#1B1914" : "#FDFCF8"
        readonly property color surface2: dark ? "#221F18" : "#EFEDE5"
        readonly property color ink: dark ? "#EFEBE0" : "#1C1A16"
        readonly property color ink2: dark ? "#B5AE9C" : "#55503F"
        readonly property color muted: dark ? "#7C7666" : "#8B8574"
        readonly property color hairline: dark ? "#2B2820" : "#DDD9CC"
        readonly property color hairline2: dark ? "#3A362B" : "#CBC6B6"
        readonly property color accent: dark ? "#FF6B2B" : "#E14E00"
        readonly property color accentInk: dark ? "#FF7E45" : "#B23E00"
        readonly property color ok: dark ? "#4CD98B" : "#177A48"
        readonly property color warn: dark ? "#E8B554" : "#9A6B00"
        readonly property color warnSoft: dark ? "#2E2410" : "#F3E8CB"
        readonly property color accentSoft: dark ? "#33200F" : "#F7E3D6"
        readonly property color meterFill: dark ? "#CFC8B4" : "#4C4738"
        readonly property color meterTrack: dark ? "#262319" : "#E7E4D9"
        readonly property string fontUi: "Instrument Sans"
        readonly property string fontNum: "Martian Mono"
    }

    // ---------- derived state ----------
    readonly property string linkWord: Sim.status === "connected" ? "CONNECTED"
                                     : Sim.status === "stalled" ? "STALLED"
                                     : "CONNECTING"
    readonly property bool correcting: Sim.correctorState === "CORRECTING"

    // ---------- formatting ----------
    function fmtGroup(n) {
        var v = Math.round(Math.abs(n));
        var str = String(v);
        var out = "";
        while (str.length > 3) {
            out = "," + str.slice(-3) + out;
            str = str.slice(0, -3);
        }
        return (n < 0 && v > 0 ? "−" : "") + str + out;
    }
    function fmtRate(r) {
        if (r >= 1)
            return Math.abs(r - Math.round(r)) < 1e-6 ? String(Math.round(r)) : r.toFixed(2);
        return r > 0 ? String(parseFloat(r.toFixed(4))) : "0";
    }
    function fmtDelta(v) {
        var r = Math.abs(v) < 0.005 ? 0 : v;
        return (r < 0 ? "−" : "") + Math.abs(r).toFixed(2);
    }
    function fmtHMS(sec) {
        var t = Math.max(0, Math.floor(sec));
        var h = Math.floor(t / 3600), m = Math.floor((t % 3600) / 60), x = t % 60;
        function p(n) { return (n < 10 ? "0" : "") + n }
        return p(h) + ":" + p(m) + ":" + p(x);
    }
    // Write fidelity (T19): realised over the commanded total for exactly the
    // intervals realised could be measured across. Measuring it against the whole
    // commanded figure instead would read a voided interval — a refuel, a pause,
    // a long frame — as fuel that failed to leave.
    function fmtRealisedPct() {
        if (Sim.removedMeasuredKg < 0.5)
            return "—";
        return Math.round(100 * Sim.removedRealisedKg / Sim.removedMeasuredKg) + "%";
    }
    function fmtTph(kg) {
        if (Sim.elapsedSeconds < 30)
            return "0.0";
        return (kg / 1000 / (Sim.elapsedSeconds / 3600)).toFixed(1);
    }

    // ---------- shared components ----------
    component Micro: Text {
        font.family: ui.fontUi
        font.pixelSize: u(10)
        font.weight: Font.DemiBold
        font.letterSpacing: 0.18 * u(10)
        font.capitalization: Font.AllUppercase
        color: ui.muted
    }

    // Mono numeral + small unit. Martian Mono's line box is ~1.6x the glyph
    // size; the design runs at line-height 1.1, so the layout height is pinned
    // and the glyph centered (it may paint outside its box, which is fine).
    component NumUnit: Item {
        property string value
        property string unit: ""
        property int size: 22
        property int unitSize: 11
        property color valueColor: ui.ink
        implicitWidth: nv.implicitWidth + (nu.visible ? nu.implicitWidth + u(4) : 0)
        implicitHeight: Math.round(u(size) * 1.1)
        Text {
            id: nv
            anchors.verticalCenter: parent.verticalCenter
            text: parent.value
            font.family: ui.fontNum
            font.pixelSize: u(parent.size)
            font.weight: Font.Medium
            font.letterSpacing: -0.02 * u(parent.size)
            color: parent.valueColor
        }
        Text {
            id: nu
            visible: parent.unit.length > 0
            anchors.baseline: nv.baseline
            anchors.left: nv.right
            anchors.leftMargin: u(4)
            text: parent.unit
            font.family: ui.fontNum
            font.pixelSize: u(parent.unitSize)
            color: ui.muted
        }
    }

    component Stage: Column {
        property string label
        property string word
        property string sub: ""
        property color dotColor: ui.muted
        spacing: u(8)
        Micro { text: parent.label }
        Row {
            spacing: u(10)
            Rectangle {
                width: u(7); height: u(7); radius: width / 2
                color: parent.parent.dotColor
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                text: parent.parent.word
                font.family: ui.fontUi
                font.pixelSize: u(16)
                font.weight: Font.DemiBold
                color: ui.ink
            }
            Text {
                visible: parent.parent.sub.length > 0
                text: parent.parent.sub
                font.family: ui.fontNum
                font.pixelSize: u(14)
                font.weight: Font.Medium
                color: ui.muted
                anchors.verticalCenter: parent.verticalCenter
            }
        }
    }

    component StepBtn: Rectangle {
        property alias glyph: stepGlyph.text
        signal stepped()
        property bool locked: false // T41: no press above the turn rate while a window is open
        opacity: locked ? 0.35 : 1
        width: u(44); height: u(44); radius: u(10)
        color: stepMa.containsMouse && !locked ? ui.surface2 : ui.surface
        border.color: stepMa.containsMouse && !locked ? ui.ink2 : ui.hairline2
        border.width: hairW
        Text {
            id: stepGlyph
            anchors.centerIn: parent
            font.family: ui.fontNum
            font.pixelSize: u(19)
            font.weight: Font.Medium
            color: ui.ink
        }
        MouseArea {
            id: stepMa
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: parent.locked ? Qt.ArrowCursor : Qt.PointingHandCursor
            onClicked: if (!parent.locked) parent.stepped()
        }
    }

    component TankCard: Rectangle {
        id: card
        property string label
        property int slot
        property bool showTag: false
        readonly property var t: Sim.tanks[slot] || ({})
        readonly property real pct: Math.max(0, Math.min(1, t.pct || 0))
        readonly property real obsK: t.obsKgS || 0
        readonly property real cmdK: t.cmdKgS || 0
        readonly property bool amplifying: correcting && Math.abs(cmdK - obsK) > 0.02
        // Warm means one thing on this card: MaxWarp is pulling this tank down
        // harder than the aircraft would (T29, #44). Not participation — a
        // transfer's *destination* is amplified just as hard, and painting its
        // gain in the corrector's action colour said "removing" about a tank
        // filling up. Measured on T26's -200: 86 ticks of both outers feeding
        // both inners at 8x, where L INNER read `+2.00 -> +16.00` in orange.
        //
        // Since R_eff > 1 whenever engaged, the write carries the same sign as
        // the natural delta, so observed, commanded and written all agree on
        // what "draining" means and there is no reading to choose between.
        //
        // No hysteresis, deliberately: across 3,775 captured ticks the drain
        // sign crosses zero 15 times and 13 of those are one clean event, a
        // transfer ending. What prevents oscillation is the dead zone formed by
        // T16's deadband and the 0.02 threshold above -- a tank near balance
        // stops being amplified rather than flipping sign. If those constants
        // move, re-check this.
        readonly property bool draining: amplifying && obsK < 0
        // Card height is driven by the gauge, so this is the one knob that
        // makes a box read as a bigger or a smaller tank (T22, #30).
        property int gaugeH: 86
        color: ui.surface
        border.color: ui.hairline
        border.width: hairW
        radius: u(10)
        implicitHeight: cardCol.implicitHeight + u(26)

        ColumnLayout {
            id: cardCol
            anchors.fill: parent
            anchors.topMargin: u(14)
            anchors.bottomMargin: u(12)
            anchors.leftMargin: u(16)
            anchors.rightMargin: u(16)
            spacing: u(10)
            RowLayout {
                Layout.fillWidth: true
                Micro { text: card.label }
                Item { Layout.fillWidth: true }
                // The words are a *name* — trim is the CG-transfer tank, which is
                // the one thing the rate row below cannot say — so they are
                // always rendered and never keyed on anything (T29, #44). It is
                // the colour that reports. Quiescent, the tag sits in the same
                // muted register as the card label beside it and reads as part
                // of the label; amber is earned only by drain.
                Rectangle {
                    visible: card.showTag
                    color: card.draining ? ui.warnSoft : ui.surface2
                    radius: u(4)
                    implicitWidth: tagText.implicitWidth + u(12)
                    implicitHeight: tagText.implicitHeight + u(6)
                    Text {
                        id: tagText
                        anchors.centerIn: parent
                        text: "CG XFR"
                        font.family: ui.fontNum
                        font.pixelSize: u(9)
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.1 * u(9)
                        color: card.draining ? ui.warn : ui.muted
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: u(14)
                Rectangle {
                    Layout.preferredWidth: u(26)
                    Layout.preferredHeight: u(card.gaugeH)
                    radius: u(5)
                    color: ui.meterTrack
                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: parent.height * card.pct
                        radius: u(4)
                        color: ui.meterFill
                    }
                }
                ColumnLayout {
                    Layout.alignment: Qt.AlignVCenter
                    spacing: u(7)
                    NumUnit {
                        value: fmtGroup(card.t.kg || 0)
                        unit: "kg"
                        size: 22
                        unitSize: 11
                    }
                    Text {
                        text: fmtGroup(card.t.gallons || 0) + " gal · "
                              + Math.round(card.pct * 100) + "%"
                        font.family: ui.fontNum
                        font.pixelSize: u(10)
                        color: ui.muted
                    }
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: u(9)
                Rectangle { Layout.fillWidth: true; height: hairW; color: ui.hairline }
                RowLayout {
                    spacing: u(7)
                    Text {
                        text: fmtDelta(card.obsK)
                        font.family: ui.fontNum
                        font.pixelSize: u(11)
                        font.weight: Font.Medium
                        color: ui.ink2
                    }
                    Text {
                        text: "→"
                        font.family: ui.fontNum
                        font.pixelSize: u(11)
                        color: ui.muted
                    }
                    // Which figure is shown, and its weight, still key on
                    // `amplifying` — that is the honesty cue T9 built the
                    // obs->cmd row for, marking this number as MaxWarp's rather
                    // than the aircraft's, and a destination's commanded gain is
                    // just as much ours. Only the *hue* moved to `draining`
                    // (T29, #44), so the cue survives as weight where it used to
                    // ride on colour and warm is left saying one thing.
                    Text {
                        text: fmtDelta(card.amplifying ? card.cmdK : card.obsK)
                        font.family: ui.fontNum
                        font.pixelSize: u(11)
                        font.weight: card.amplifying ? Font.DemiBold : Font.Medium
                        color: card.draining ? ui.accentInk : ui.ink2
                    }
                    Text {
                        text: "kg/s"
                        font.family: ui.fontNum
                        font.pixelSize: u(11)
                        color: ui.muted
                    }
                }
            }
        }
    }

    component Stat: Column {
        property string label
        property string value
        property string unit: ""
        // Muted second line, in the tank cards' "gal · %" idiom. Always rendered,
        // empty or not, so the rail never reflows when a figure first appears —
        // T11 fixed the 1 Hz repaint as the tick made visible, and a layout that
        // jumps under it is a different thing entirely.
        property string sub: ""
        property color valueColor: ui.ink
        spacing: u(7)
        Micro { text: parent.label }
        NumUnit {
            value: parent.value
            unit: parent.unit
            size: 26
            unitSize: 13
            valueColor: parent.valueColor
        }
        Text {
            text: parent.sub
            font.family: ui.fontNum
            font.pixelSize: u(10)
            color: ui.muted
        }
    }

    component TitleGlyph: Text {
        signal activated()
        font.family: ui.fontNum
        font.pixelSize: u(15)
        color: glyphMa.containsMouse ? ui.ink : ui.muted
        MouseArea {
            id: glyphMa
            anchors.fill: parent
            anchors.margins: -u(6)
            hoverEnabled: true
            onClicked: parent.activated()
        }
    }

    // ---------- settings controls (T36, #55 — sizes from the T34 mockup) ----------
    // A group: micro title over a hairline2 rule, then its rows.
    component SetGroup: ColumnLayout {
        property string title
        default property alias rows: rowsCol.data
        spacing: 0
        Micro { text: parent.title }
        Rectangle {
            Layout.fillWidth: true
            Layout.topMargin: u(10)
            height: hairW
            color: ui.hairline2
        }
        ColumnLayout {
            id: rowsCol
            Layout.fillWidth: true
            spacing: 0
        }
    }

    // A row: name + help at the left, the control(s) at the right, hairline
    // between rows (never above the first — the group rule is already there).
    component SetRow: Item {
        id: setRow
        property string name
        property string help
        default property alias controls: ctlRow.data
        readonly property bool first: parent && parent.children[0] === setRow
        Layout.fillWidth: true
        implicitHeight: Math.max(txtCol.implicitHeight, ctlRow.implicitHeight) + u(32)
        Rectangle {
            visible: !setRow.first
            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
            height: hairW
            color: ui.hairline
        }
        Column {
            id: txtCol
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.topMargin: u(16)
            width: Math.min(u(300), parent.width - ctlRow.implicitWidth - u(24))
            spacing: u(5)
            Text {
                text: setRow.name
                width: parent.width
                font.family: ui.fontUi
                font.pixelSize: u(13)
                font.weight: Font.DemiBold
                color: ui.ink
            }
            Text {
                text: setRow.help
                width: parent.width
                wrapMode: Text.WordWrap
                font.family: ui.fontUi
                font.pixelSize: u(11)
                lineHeight: 1.08 // ≈ CSS 1.45 on the 11px size: Qt scales the natural line box
                color: ui.muted
            }
        }
        Row {
            id: ctlRow
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.topMargin: u(17)
            spacing: u(10)
        }
    }

    // Segmented control: uppercase UI labels, or mono when numeric; the chosen
    // segment sits on surface2 with a 2-unit accent underline.
    component Seg: Rectangle {
        id: seg
        property var labels: []
        property int current: 0
        property bool numeric: false
        signal picked(int index)
        implicitWidth: segRow.implicitWidth
        implicitHeight: segRow.implicitHeight
        radius: u(8)
        color: ui.surface
        border.color: ui.hairline2
        border.width: hairW
        clip: true
        Row {
            id: segRow
            Repeater {
                model: seg.labels
                delegate: Rectangle {
                    id: segItem
                    required property int index
                    required property string modelData
                    readonly property bool on: index === seg.current
                    implicitWidth: segText.implicitWidth + u(28)
                    implicitHeight: segText.implicitHeight + u(18)
                    color: on || segMa.containsMouse ? ui.surface2 : "transparent"
                    Rectangle {
                        visible: segItem.index > 0
                        anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                        width: hairW
                        color: ui.hairline2
                    }
                    Rectangle {
                        visible: segItem.on
                        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                        height: Math.max(2, u(2))
                        color: ui.accent
                    }
                    Text {
                        id: segText
                        anchors.centerIn: parent
                        text: segItem.modelData
                        font.family: seg.numeric ? ui.fontNum : ui.fontUi
                        font.pixelSize: u(11)
                        font.weight: Font.DemiBold
                        font.letterSpacing: seg.numeric ? 0 : 0.08 * u(11)
                        font.capitalization: seg.numeric ? Font.MixedCase : Font.AllUppercase
                        color: segItem.on ? ui.ink : ui.ink2
                    }
                    MouseArea {
                        id: segMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: seg.picked(segItem.index)
                    }
                }
            }
        }
    }

    // On/off switch: accent when on — these are the governor's own switches,
    // and the accent is what the map reserves for its actions.
    component Toggle: Rectangle {
        id: sw
        property bool on: false
        signal toggled()
        implicitWidth: u(46)
        implicitHeight: u(26)
        radius: height / 2
        color: on ? ui.accentSoft : ui.meterTrack
        border.color: on ? ui.accent : ui.hairline2
        border.width: hairW
        Rectangle {
            width: u(20); height: u(20); radius: width / 2
            anchors.verticalCenter: parent.verticalCenter
            x: sw.on ? sw.width - width - u(3) : u(3)
            color: sw.on ? ui.accent : ui.muted
            Behavior on x { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: sw.toggled()
        }
    }

    // Stepper end button: shared by both ends of a Stepper.
    component StpBtn: Rectangle {
        property alias glyph: stpGlyph.text
        signal hit()
        width: u(30); height: u(32)
        color: stpMa.containsMouse ? ui.surface2 : "transparent"
        Text {
            id: stpGlyph
            anchors.centerIn: parent
            font.family: ui.fontNum
            font.pixelSize: u(14)
            font.weight: Font.Medium
            color: stpMa.containsMouse ? ui.ink : ui.ink2
        }
        MouseArea {
            id: stpMa
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: parent.hit()
        }
    }

    // − value unit + : the value is the caller's already-formatted string; the
    // clamping lives in the setter behind it.
    component Stepper: Rectangle {
        id: stp
        property string value
        property string unit: ""
        signal minus()
        signal plus()
        implicitWidth: stpRow.implicitWidth
        implicitHeight: u(32)
        radius: u(8)
        color: ui.surface
        border.color: ui.hairline2
        border.width: hairW
        clip: true
        Row {
            id: stpRow
            StpBtn { glyph: "−"; onHit: stp.minus() }
            Item {
                width: Math.max(u(74), stpVal.implicitWidth + u(16)); height: stp.height
                Rectangle { anchors.left: parent.left; width: hairW; height: parent.height; color: ui.hairline2 }
                Rectangle { anchors.right: parent.right; width: hairW; height: parent.height; color: ui.hairline2 }
                Row {
                    id: stpVal
                    anchors.centerIn: parent
                    spacing: u(3)
                    Text {
                        id: stpValText
                        text: stp.value
                        font.family: ui.fontNum
                        font.pixelSize: u(14)
                        font.weight: Font.Medium
                        color: ui.ink
                    }
                    Text {
                        text: stp.unit
                        anchors.baseline: stpValText.baseline
                        font.family: ui.fontNum
                        font.pixelSize: u(10)
                        color: ui.muted
                    }
                }
            }
            StpBtn { glyph: "+"; onHit: stp.plus() }
        }
    }

    // One-line text field in the mono face; commits on Enter or focus-out.
    component Field: Rectangle {
        id: fld
        property alias text: fldInput.text
        property string placeholder: ""
        signal committed(string text)
        implicitWidth: u(200)
        implicitHeight: u(34)
        radius: u(8)
        color: ui.surface
        border.color: fldInput.activeFocus ? ui.ink2 : ui.hairline2
        border.width: hairW
        TextInput {
            id: fldInput
            anchors.fill: parent
            anchors.leftMargin: u(12)
            anchors.rightMargin: u(12)
            verticalAlignment: TextInput.AlignVCenter
            font.family: ui.fontNum
            font.pixelSize: u(13)
            font.weight: Font.Medium
            color: ui.ink
            selectionColor: ui.accentSoft
            selectedTextColor: ui.ink
            clip: true
            onEditingFinished: fld.committed(text)
            Text {
                visible: !fldInput.text.length && !fldInput.activeFocus
                anchors.verticalCenter: parent.verticalCenter
                text: fld.placeholder
                width: parent.width
                elide: Text.ElideRight
                font: fldInput.font
                color: ui.muted
            }
        }
    }

    // Small outline button in the AUTO-chip register: uppercase, letterspaced.
    component OutlineBtn: Rectangle {
        id: obtn
        property string label
        signal activated()
        implicitWidth: obtnText.implicitWidth + u(28)
        implicitHeight: obtnText.implicitHeight + u(20)
        radius: u(8)
        color: ui.surface
        border.color: obtnMa.containsMouse ? ui.ink2 : ui.hairline2
        border.width: hairW
        Text {
            id: obtnText
            anchors.centerIn: parent
            text: obtn.label
            font.family: ui.fontUi
            font.pixelSize: u(10)
            font.weight: Font.DemiBold
            font.letterSpacing: 0.14 * u(10)
            font.capitalization: Font.AllUppercase
            color: obtnMa.containsMouse ? ui.ink : ui.ink2
        }
        MouseArea {
            id: obtnMa
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: obtn.activated()
        }
    }

    // The flight-plan status box (T38): route as the headline when a plan is
    // held, otherwise the plan word — NO IDENTITY · NOT FETCHED · FETCHING or
    // one of the failure words (NO SUCH USER · NO PLAN FILED · NO NETWORK ·
    // NO FIXES). The second line is always Sim.planDetail: count-and-time,
    // or what would change the word.
    component PlanBox: Rectangle {
        id: pb
        readonly property bool havePlan: Sim.planRoute.length > 0
        implicitWidth: Math.max(u(260), pbCol.implicitWidth + u(28))
        implicitHeight: pbCol.implicitHeight + u(24)
        radius: u(10)
        color: ui.surface
        border.color: ui.hairline
        border.width: hairW
        Column {
            id: pbCol
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.leftMargin: u(14)
            anchors.topMargin: u(12)
            spacing: u(7)
            Text {
                text: pb.havePlan ? Sim.planRoute : Sim.planWord
                font.family: ui.fontNum
                font.pixelSize: u(14)
                font.weight: Font.Medium
                color: pb.havePlan ? ui.ink : ui.muted
            }
            Text {
                text: Sim.planDetail
                font.family: ui.fontNum
                font.pixelSize: u(10)
                color: ui.muted
            }
        }
    }

    Shortcut { sequence: "Escape"; enabled: settingsOpen; onActivated: settingsOpen = false }

    // ---------- the window ----------
    Rectangle {
        id: chrome
        anchors.fill: parent
        color: ui.bg
        border.color: ui.hairline2
        border.width: hairW
        radius: maximized ? 0 : u(14)
        clip: true

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: hairW
            spacing: 0

            // ---------- titlebar ----------
            Item {
                id: titlebar
                Layout.fillWidth: true
                Layout.preferredHeight: u(56)

                DragHandler {
                    target: null
                    onActiveChanged: if (active) root.startSystemMove()
                }
                TapHandler {
                    onDoubleTapped: maximized ? root.showNormal() : root.showMaximized()
                }

                Row {
                    anchors.left: parent.left
                    anchors.leftMargin: u(20)
                    anchors.verticalCenter: parent.verticalCenter
                    Text {
                        text: "MAX"
                        font.family: ui.fontNum
                        font.pixelSize: u(16)
                        font.weight: Font.Bold
                        font.letterSpacing: 0.22 * u(16)
                        color: ui.ink
                    }
                    Text {
                        text: "WARP"
                        font.family: ui.fontNum
                        font.pixelSize: u(16)
                        font.weight: Font.Bold
                        font.letterSpacing: 0.22 * u(16)
                        color: ui.accent
                    }
                    // ← Back: the only way out of the settings page besides Esc.
                    Item { width: u(28); height: 1; visible: settingsOpen }
                    Item {
                        visible: settingsOpen
                        width: backRow.implicitWidth
                        height: backRow.implicitHeight
                        anchors.verticalCenter: parent.verticalCenter
                        Row {
                            id: backRow
                            spacing: u(8)
                            Text {
                                text: "←"
                                font.family: ui.fontNum
                                font.pixelSize: u(14)
                                color: backMa.containsMouse ? ui.ink : ui.ink2
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                text: "BACK"
                                font.family: ui.fontUi
                                font.pixelSize: u(11)
                                font.weight: Font.DemiBold
                                font.letterSpacing: 0.14 * u(11)
                                color: backMa.containsMouse ? ui.ink : ui.ink2
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }
                        MouseArea {
                            id: backMa
                            anchors.fill: parent
                            anchors.margins: -u(8)
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: settingsOpen = false
                        }
                    }
                }

                Row {
                    anchors.right: parent.right
                    anchors.rightMargin: u(20)
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: u(24)
                    // ⚙ sits where the theme glyph was (T34): theme moved into
                    // the page as a segmented control.
                    // Segoe UI Symbol carries a monochrome U+2699; the default fallback is
                    // the colour-emoji face, which is not this window.
                    TitleGlyph { text: "⚙"; font.family: "Segoe UI Symbol"; visible: !settingsOpen; onActivated: settingsOpen = true }
                    TitleGlyph { text: "─"; onActivated: root.showMinimized() }
                    TitleGlyph {
                        text: "□"
                        onActivated: maximized ? root.showNormal() : root.showMaximized()
                    }
                    TitleGlyph { text: "✕"; onActivated: Qt.quit() }
                }

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: hairW
                    color: ui.hairline
                }
            }

            // ---------- stage rail ----------
            Item {
                visible: !settingsOpen
                Layout.fillWidth: true
                implicitHeight: railRow.implicitHeight + u(40)

                RowLayout {
                    id: railRow
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: u(28)
                    anchors.rightMargin: u(28)
                    anchors.verticalCenter: parent.verticalCenter
                    // T39: the governor's fifth stage spends the rail's slack
                    // (T34) — gaps tighten 22→14 and the connectors 30→18.
                    spacing: u(14)

                    Stage {
                        label: "Sim link"
                        word: linkWord
                        dotColor: linkWord === "CONNECTED" ? ui.ok
                                : linkWord === "STALLED" ? ui.warn : ui.muted
                    }
                    Rectangle {
                        Layout.preferredWidth: u(18)
                        Layout.preferredHeight: hairW
                        Layout.alignment: Qt.AlignBottom
                        Layout.bottomMargin: u(12)
                        color: ui.hairline2
                    }
                    // The bridge is a prerequisite, not a feature (T18): with no
                    // in-sim module there is no core state to read or write, so it
                    // earns a stage of its own between the link and the aircraft.
                    // NO MODULE and NO FUEL VARS stay distinct — different problems,
                    // different fixes.
                    Stage {
                        label: "Bridge"
                        word: Sim.bridgeWord
                        dotColor: Sim.bridgeUp ? ui.ok
                                : Sim.bridgeWord === "LINKING" ? ui.muted : ui.warn
                    }
                    Rectangle {
                        Layout.preferredWidth: u(18)
                        Layout.preferredHeight: hairW
                        Layout.alignment: Qt.AlignBottom
                        Layout.bottomMargin: u(12)
                        color: ui.hairline2
                    }
                    Stage {
                        label: "Aircraft"
                        word: Sim.sessionActive ? Sim.aircraftVariant : "SEARCHING"
                        dotColor: Sim.sessionActive ? ui.ok : ui.muted
                    }
                    Rectangle {
                        Layout.preferredWidth: u(18)
                        Layout.preferredHeight: hairW
                        Layout.alignment: Qt.AlignBottom
                        Layout.bottomMargin: u(12)
                        color: ui.hairline2
                    }
                    Stage {
                        // The R sub moved to the hero's R eff row (T34): the
                        // fifth stage takes the width it freed.
                        label: "Corrector"
                        word: Sim.correctorState
                        dotColor: correcting ? ui.ok
                                : Sim.correctorState === "SUSPENDED" ? ui.warn : ui.muted
                    }
                    Rectangle {
                        Layout.preferredWidth: u(18)
                        Layout.preferredHeight: hairW
                        Layout.alignment: Qt.AlignBottom
                        Layout.bottomMargin: u(12)
                        color: ui.hairline2
                    }
                    // The governor's stage (T39, T34 variant A): a readout, not
                    // a control — the switch lives only in settings. Fixed words
                    // from the core; the fix and its angle ride the sub.
                    Stage {
                        label: "Governor"
                        word: Sim.govWord
                        sub: Sim.govSub
                        dotColor: Sim.govWord === "TURN" ? ui.accent
                                : Sim.govWord === "ON PLAN" ? ui.ok : ui.muted
                    }

                    Item { Layout.fillWidth: true }

                    // The AUTO chip *is* the master switch (T10): dashed-idle in
                    // AUTO, orange invitation to re-arm when OFF.
                    Rectangle {
                        id: autoChip
                        implicitWidth: chipText.implicitWidth + u(34)
                        implicitHeight: chipText.implicitHeight + u(20)
                        radius: height / 2
                        color: chipMa.containsMouse ? ui.surface2 : ui.surface
                        border.width: Sim.masterAuto ? 0 : hairW
                        border.color: ui.accent
                        Canvas {
                            id: chipDash
                            anchors.fill: parent
                            visible: Sim.masterAuto
                            property color line: ui.hairline2
                            onLineChanged: requestPaint()
                            onWidthChanged: requestPaint()
                            onHeightChanged: requestPaint()
                            onPaint: {
                                var ctx = getContext("2d");
                                ctx.reset();
                                ctx.strokeStyle = String(line);
                                ctx.lineWidth = hairW;
                                ctx.setLineDash([4 * s, 4 * s]);
                                ctx.beginPath();
                                ctx.roundedRect(hairW / 2, hairW / 2,
                                                width - hairW, height - hairW,
                                                height / 2, height / 2);
                                ctx.stroke();
                            }
                        }
                        Text {
                            id: chipText
                            anchors.centerIn: parent
                            text: Sim.masterAuto ? "AUTO" : "OFF"
                            font.family: ui.fontUi
                            font.pixelSize: u(13)
                            font.weight: Font.DemiBold
                            font.letterSpacing: 0.08 * u(13)
                            color: Sim.masterAuto ? ui.muted : ui.accentInk
                        }
                        MouseArea {
                            id: chipMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Sim.masterAuto ? Qt.ArrowCursor : Qt.PointingHandCursor
                            onClicked: Sim.arm()
                        }
                    }

                    // The master-switch button mirrors the switch it acts on: it
                    // offers the action *available*, never the state you are in.
                    // Armed → orange DISENGAGE; OFF → green ENGAGE (T11). Re-arming
                    // stays a deliberate human click, per T10's sticky-OFF rule.
                    Rectangle {
                        id: kill
                        readonly property color tone: Sim.masterAuto ? ui.accent : ui.ok
                        readonly property color toneInk: Sim.masterAuto ? ui.accentInk : ui.ok
                        implicitWidth: killText.implicitWidth + u(48)
                        implicitHeight: killText.implicitHeight + u(26)
                        radius: u(8)
                        color: killMa.containsMouse ? tone : "transparent"
                        border.color: tone
                        border.width: Math.max(1, Math.round(1.5 * s))
                        Text {
                            id: killText
                            anchors.centerIn: parent
                            text: Sim.masterAuto ? "DISENGAGE" : "ENGAGE"
                            font.family: ui.fontUi
                            font.pixelSize: u(13)
                            font.weight: Font.DemiBold
                            font.letterSpacing: 0.16 * u(13)
                            color: killMa.containsMouse ? "#ffffff" : kill.toneInk
                        }
                        MouseArea {
                            id: killMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: Sim.masterAuto ? Sim.disengage() : Sim.arm()
                        }
                    }
                }

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: hairW
                    color: ui.hairline
                }
            }

            // ---------- hero: sim rate ----------
            Item {
                visible: !settingsOpen
                Layout.fillWidth: true
                implicitHeight: heroRow.implicitHeight + u(36)

                RowLayout {
                    id: heroRow
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: parent.top
                    anchors.topMargin: u(30)
                    spacing: u(24)

                    // T41: the ceiling is the governor's turn rate, not the
                    // window itself. Below the ceiling a + press is legal —
                    // the driver only steps back down while the expectation
                    // exceeds the ceiling — so only lock at or above it.
                    // Residual: Sim.simRate is a one-tick-late readback (it
                    // refreshes on the 1 Hz correctorUpdate), so two fast
                    // presses from below can momentarily land above the
                    // ceiling. RateDriver::drive() re-lowers on the next tick,
                    // so the invariant holds; the cost is a one-tick overshoot
                    // in the numeral, not a broken ceiling.
                    readonly property bool plusLocked:
                        Sim.govWindowOpen && Sim.simRate >= Sim.turnRate - 1e-9

                    StepBtn { glyph: "−"; onStepped: Sim.rateDown() }

                    Item {
                        implicitWidth: rateText.implicitWidth + rateX.implicitWidth + u(4)
                        implicitHeight: u(96) // line-height 1: the glyph box, not the font box
                        Text {
                            id: rateText
                            anchors.verticalCenter: parent.verticalCenter
                            text: fmtRate(Sim.simRate)
                            font.family: ui.fontNum
                            font.pixelSize: u(96)
                            font.weight: Font.Light
                            font.letterSpacing: -0.04 * u(96)
                            // Held for a turn (T39): the numeral takes the
                            // action colour for exactly the window's life.
                            color: Sim.govWindowOpen ? ui.accent : ui.ink
                        }
                        Text {
                            id: rateX
                            anchors.baseline: rateText.baseline
                            anchors.left: rateText.right
                            anchors.leftMargin: u(4)
                            text: "×"
                            font.family: ui.fontNum
                            font.pixelSize: u(40)
                            font.weight: Font.Light
                            color: ui.muted
                        }
                    }

                    StepBtn { glyph: "+"; locked: heroRow.plusLocked; onStepped: Sim.rateUp() }

                    ColumnLayout {
                        Layout.alignment: Qt.AlignVCenter
                        Layout.leftMargin: u(12)
                        spacing: u(8)
                        Micro { text: "Sim rate" }
                        Row {
                            spacing: u(5)
                            Repeater {
                                model: 8
                                Rectangle {
                                    // While a window holds the rate down, the
                                    // cruise rate's pip stays as a ghost outline
                                    // — the rung the restore returns to (T34).
                                    readonly property bool lit:
                                        Sim.simRate >= Math.pow(2, index) - 1e-9
                                    readonly property bool ghost:
                                        Sim.govWindowOpen && !lit
                                        && Math.abs(Sim.govCruiseRate
                                                    - Math.pow(2, index)) < 1e-6
                                    width: u(14); height: u(4); radius: u(2)
                                    anchors.bottom: parent.bottom
                                    color: lit ? ui.accent
                                         : ghost ? "transparent" : ui.meterTrack
                                    border.width: ghost ? hairW : 0
                                    border.color: ui.accent
                                }
                            }
                            Text {
                                text: "1–128"
                                font.family: ui.fontNum
                                font.pixelSize: u(9)
                                font.weight: Font.Medium
                                color: ui.muted
                                anchors.bottom: parent.bottom
                            }
                        }
                        Row {
                            spacing: u(6)
                            Text {
                                text: "R eff"
                                font.family: ui.fontUi
                                font.pixelSize: u(10)
                                font.weight: Font.DemiBold
                                font.letterSpacing: 0.08 * u(10)
                                color: ui.muted
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                text: Sim.effectiveRate > 0 ? Sim.effectiveRate.toFixed(2) : "—"
                                font.family: ui.fontNum
                                font.pixelSize: u(11)
                                font.weight: Font.DemiBold
                                color: ui.ink
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }
                        // The held line (T34/T39): what the rate is held for and
                        // what it returns to. Always rendered so the hero never
                        // reflows when a window opens (the Stat.sub precedent).
                        Row {
                            spacing: u(6)
                            opacity: Sim.govWindowOpen ? 1 : 0
                            Text {
                                text: Sim.govFix.length > 0
                                      ? "HELD FOR " + Sim.govFix : "HELD"
                                font.family: ui.fontUi
                                font.pixelSize: u(10)
                                font.weight: Font.DemiBold
                                font.letterSpacing: 0.08 * u(10)
                                color: ui.accent
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                // Only claim "+ locked" when it actually is.
                                text: "returns to " + fmtRate(Sim.govCruiseRate) + "×"
                                      + (heroRow.plusLocked ? " · + locked" : "")
                                font.family: ui.fontNum
                                font.pixelSize: u(11)
                                font.weight: Font.Medium
                                color: ui.accentInk
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }
                    }
                }
            }

            // ---------- planform ----------
            Item {
                visible: !settingsOpen
                id: planform
                Layout.fillWidth: true
                implicitHeight: tankRow.implicitHeight + u(18) + spine.implicitHeight + u(36)

                // T22 (#30): the centre tank is drawn where it physically is —
                // in the wing box, between the inners — so on a -200 the
                // fuselage column widens from a hairline into a real card and
                // the wing cards give up ~12% of their width to it. That
                // collapses the centreline stack back to Trim alone, which is
                // what makes the six-tank screen fit the same window the
                // five-tank one already fits (T14 found it clipped by ~187
                // units when Centre and Trim stacked). The -300 populates no
                // centre slot, so it renders exactly as it did before.
                readonly property bool centreInBox: root.sixTank
                readonly property real midUnits: centreInBox ? 1.35 : 0.6
                // The outer boxes hold the shortest numbers but the same
                // furniture, so at five columns they were the first to crowd —
                // widened until a brim-full "942 gal · 100%" clears the edge.
                readonly property real outerUnits: centreInBox ? 1.2 : 1.0
                readonly property real avail: width - 2 * u(40) - 4 * u(18)
                function cw(w) {
                    return Math.floor(avail * w / (2 * outerUnits + 2.7 + midUnits))
                }
                // Centre stands proud of the wing cards rather than sitting
                // flush, so the wing cards must stop stretching to the row —
                // otherwise they would all grow with it.
                readonly property int wingAlign: Qt.AlignVCenter

                RowLayout {
                    id: tankRow
                    anchors.top: parent.top
                    anchors.topMargin: u(26)
                    anchors.left: parent.left
                    anchors.leftMargin: u(40)
                    anchors.right: parent.right
                    anchors.rightMargin: u(40)
                    spacing: u(18)

                    TankCard {
                        label: "L Outer"; slot: 2
                        Layout.preferredWidth: planform.cw(planform.outerUnits)
                        Layout.fillHeight: !planform.centreInBox
                        Layout.alignment: planform.wingAlign
                    }
                    TankCard {
                        label: "L Inner"; slot: 0
                        Layout.preferredWidth: planform.cw(1.35)
                        Layout.fillHeight: !planform.centreInBox
                        Layout.alignment: planform.wingAlign
                    }
                    ColumnLayout {
                        Layout.preferredWidth: planform.cw(planform.midUnits)
                        Layout.fillHeight: true
                        spacing: u(8)
                        TankCard {
                            visible: planform.centreInBox
                            label: "Center"; slot: 4
                            // The largest tank aboard (10,735 gal), so it stands
                            // proud of the wing cards rather than sitting flush.
                            gaugeH: 106
                            Layout.fillWidth: true
                            Layout.alignment: planform.wingAlign
                        }
                        Rectangle {
                            visible: !planform.centreInBox
                            Layout.alignment: Qt.AlignHCenter
                            Layout.fillHeight: true
                            width: hairW
                            color: ui.hairline2
                        }
                        Micro {
                            // With a card in the wing box the card *is* the
                            // fuselage marker, and the stage rail already names
                            // the variant — so the centreline label goes, and
                            // the wing row keeps exactly the -300's height.
                            visible: !planform.centreInBox
                            Layout.alignment: Qt.AlignHCenter
                            Layout.bottomMargin: u(6)
                            text: Sim.aircraftVariant.length > 0 ? Sim.aircraftVariant : "A330"
                        }
                    }
                    TankCard {
                        label: "R Inner"; slot: 1
                        Layout.preferredWidth: planform.cw(1.35)
                        Layout.fillHeight: !planform.centreInBox
                        Layout.alignment: planform.wingAlign
                    }
                    TankCard {
                        label: "R Outer"; slot: 3
                        Layout.preferredWidth: planform.cw(planform.outerUnits)
                        Layout.fillHeight: !planform.centreInBox
                        Layout.alignment: planform.wingAlign
                    }
                }

                // Fuselage axis (T11, refined by T22). Trim fuel sits on the
                // aircraft's centreline, aft in the tail, so it hangs below the
                // wing box on that axis. Centre used to stack here too, forward
                // of Trim — but the centre tank is physically *in* the wing box,
                // so T22 moved it into the row above, where it belongs and where
                // it costs no height. On a -200 this axis is Trim alone; on a
                // -300, which populates no centre slot, it always was.
                ColumnLayout {
                    id: spine
                    anchors.top: tankRow.bottom
                    anchors.topMargin: u(18)
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: u(12)

                    TankCard {
                        label: "Trim"; slot: 5; showTag: true
                        // 1,609 gal against the mains' 10,848 — the smallest
                        // tank aboard, so the shortest box. Only worth saying on
                        // a -200, where there is a bigger box to say it against.
                        gaugeH: planform.centreInBox ? 52 : 86
                        Layout.preferredWidth: u(300)
                        Layout.alignment: Qt.AlignHCenter
                    }
                }
            }

            Item { Layout.fillHeight: true; visible: !settingsOpen }

            // ---------- bottom rail ----------
            Item {
                visible: !settingsOpen
                Layout.fillWidth: true
                implicitHeight: statRow.implicitHeight + u(44)

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    height: hairW
                    color: ui.hairline
                }

                RowLayout {
                    id: statRow
                    anchors.left: parent.left
                    anchors.leftMargin: u(32)
                    anchors.right: parent.right
                    anchors.rightMargin: u(32)
                    anchors.top: parent.top
                    anchors.topMargin: u(20)
                    spacing: u(46)

                    Stat {
                        label: "Fuel on board"
                        value: fmtGroup(Sim.totalKg)
                        unit: "kg"
                    }
                    // T19: the headline is what the tanks actually gave up beyond
                    // their natural drain — measured, never commanded. Commanded
                    // rides underneath rather than being dropped, because a gap
                    // between the two is the single most diagnostic thing on this
                    // screen: for the whole of T13's cruise it was the difference
                    // between "531 gal removed" and none.
                    Stat {
                        label: "Session removed"
                        value: Sim.removedRealisedKg >= 0.5
                               ? fmtGroup(-Sim.removedRealisedKg) : "0"
                        unit: "kg"
                        valueColor: ui.accentInk
                        sub: Sim.removedCommandedKg >= 0.5
                             ? "cmd " + fmtGroup(Sim.removedCommandedKg)
                               + " · " + fmtRealisedPct()
                             : ""
                    }
                    Stat {
                        label: "Burn · observed"
                        value: fmtTph(Sim.burnObservedKg)
                        unit: "t/h"
                    }
                    Stat {
                        label: "Burn · effective"
                        value: fmtTph(Sim.burnEffectiveKg)
                        unit: "t/h"
                    }
                    Item { Layout.fillWidth: true }
                    Stat {
                        label: "Elapsed"
                        value: fmtHMS(Sim.elapsedSeconds)
                    }
                }
            }

            // ---------- settings page (T34 variant A · T36, #55) ----------
            // The whole window becomes the page: one 640-unit column, three
            // groups, the event line still visible at its foot. Every control
            // writes straight through to Sim's persisted setting or Theme.mode.
            Flickable {
                id: settingsPage
                visible: settingsOpen
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: width
                contentHeight: setCol.implicitHeight + u(60)
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                ColumnLayout {
                    id: setCol
                    width: Math.min(u(640), parent.width - u(64))
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: u(40)
                    spacing: u(34)

                    SetGroup {
                        title: "Appearance"
                        SetRow {
                            name: "Theme"
                            help: "System follows Windows. The window chrome is MaxWarp’s own either way."
                            Seg {
                                labels: ["System", "Light", "Dark"]
                                current: Theme.mode === "light" ? 1 : Theme.mode === "dark" ? 2 : 0
                                onPicked: (i) => Theme.mode = ["system", "light", "dark"][i]
                            }
                        }
                    }

                    SetGroup {
                        title: "Turn governor"
                        SetRow {
                            name: "Turn governor"
                            help: "Slow the sim to the turn rate ahead of each planned turn, and hand your rate back once the turn is flown. Independent of the corrector’s master switch."
                            Toggle {
                                on: Sim.governorEnabled
                                onToggled: Sim.governorEnabled = !Sim.governorEnabled
                            }
                        }
                        SetRow {
                            name: "Turn rate"
                            help: "The rate held through a turn. The governor only ever lowers to it, and never raises above the rate you chose."
                            Seg {
                                labels: ["1×", "2×", "4×"]
                                numeric: true
                                current: Sim.turnRate >= 4 ? 2 : Sim.turnRate >= 2 ? 1 : 0
                                onPicked: (i) => Sim.turnRate = [1, 2, 4][i]
                            }
                        }
                        SetRow {
                            name: "Angle threshold"
                            help: "A fix whose course change is smaller than this is not a turn: no window is predicted for it and no bank at it opens one. 0–180°."
                            Stepper {
                                value: String(Math.round(Sim.angleThresholdDeg))
                                unit: "°"
                                onMinus: Sim.angleThresholdDeg = Sim.angleThresholdDeg - 1
                                onPlus: Sim.angleThresholdDeg = Sim.angleThresholdDeg + 1
                            }
                        }
                        SetRow {
                            name: "Lead margin"
                            help: "Added ahead of the predicted roll-in point, on top of the allowance for the ticks the sim rate compresses into real time."
                            Stepper {
                                value: Sim.marginNm.toFixed(1)
                                unit: "nm"
                                onMinus: Sim.marginNm = Sim.marginNm - 0.5
                                onPlus: Sim.marginNm = Sim.marginNm + 0.5
                            }
                        }
                        SetRow {
                            name: "Sensed turn without a plan"
                            help: "With no plan loaded, or off plan, open a window when the aircraft banks. There is no lead — by the time the bank is seen the turn has begun."
                            Toggle {
                                on: Sim.sensedTurnSource
                                onToggled: Sim.sensedTurnSource = !Sim.sensedTurnSource
                            }
                        }
                    }

                    SetGroup {
                        title: "SimBrief"
                        SetRow {
                            name: "SimBrief identity"
                            help: "Your SimBrief username, or your numeric pilot ID. All digits is read as an ID; anything else as a username."
                            Field {
                                text: Sim.simbriefIdentity
                                placeholder: "username or ID"
                                onCommitted: (t) => Sim.simbriefIdentity = t
                            }
                        }
                        SetRow {
                            name: "Flight plan"
                            help: "Fetched when a pairing locks and whenever you press refresh. Never polled — MaxWarp does not watch SimBrief in the background."
                            PlanBox {}
                            OutlineBtn { label: "Refresh"; onActivated: Sim.refreshPlan() }
                        }
                    }
                }
            }

            // ---------- last-event line (T10; styled properly in round 2) ----------
            Item {
                Layout.fillWidth: true
                implicitHeight: u(36)

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    height: hairW
                    color: ui.hairline
                }

                RowLayout {
                    anchors.left: parent.left
                    anchors.leftMargin: u(32)
                    anchors.right: parent.right
                    anchors.rightMargin: u(32)
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: u(12)

                    Micro { text: "Last event" }
                    Text {
                        visible: Sim.lastEventTime.length > 0
                        text: Sim.lastEventTime
                        font.family: ui.fontNum
                        font.pixelSize: u(10)
                        color: ui.muted
                    }
                    Text {
                        Layout.fillWidth: true
                        text: Sim.lastEvent.length > 0 ? Sim.lastEvent : "—"
                        font.family: ui.fontUi
                        font.pixelSize: u(12)
                        color: ui.ink2
                        elide: Text.ElideRight
                    }
                }
            }
        }
    }

    // ---------- frameless-window resize grips ----------
    component Grip: MouseArea {
        property int edges
        visible: !maximized
        z: 100
        acceptedButtons: Qt.LeftButton
        onPressed: root.startSystemResize(edges)
    }
    Grip { edges: Qt.LeftEdge; cursorShape: Qt.SizeHorCursor
           anchors { left: parent.left; top: parent.top; bottom: parent.bottom
                     topMargin: 12; bottomMargin: 12 } width: 6 }
    Grip { edges: Qt.RightEdge; cursorShape: Qt.SizeHorCursor
           anchors { right: parent.right; top: parent.top; bottom: parent.bottom
                     topMargin: 12; bottomMargin: 12 } width: 6 }
    Grip { edges: Qt.TopEdge; cursorShape: Qt.SizeVerCursor
           anchors { top: parent.top; left: parent.left; right: parent.right
                     leftMargin: 12; rightMargin: 12 } height: 6 }
    Grip { edges: Qt.BottomEdge; cursorShape: Qt.SizeVerCursor
           anchors { bottom: parent.bottom; left: parent.left; right: parent.right
                     leftMargin: 12; rightMargin: 12 } height: 6 }
    Grip { edges: Qt.TopEdge | Qt.LeftEdge; cursorShape: Qt.SizeFDiagCursor
           anchors { top: parent.top; left: parent.left } width: 12; height: 12 }
    Grip { edges: Qt.TopEdge | Qt.RightEdge; cursorShape: Qt.SizeBDiagCursor
           anchors { top: parent.top; right: parent.right } width: 12; height: 12 }
    Grip { edges: Qt.BottomEdge | Qt.LeftEdge; cursorShape: Qt.SizeBDiagCursor
           anchors { bottom: parent.bottom; left: parent.left } width: 12; height: 12 }
    Grip { edges: Qt.BottomEdge | Qt.RightEdge; cursorShape: Qt.SizeFDiagCursor
           anchors { bottom: parent.bottom; right: parent.right } width: 12; height: 12 }
}
