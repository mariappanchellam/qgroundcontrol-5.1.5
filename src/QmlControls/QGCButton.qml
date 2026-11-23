import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls

/// Standard push button control:
///     If there is both an icon and text the icon will be to the left of the text
///     If icon only, icon will be centered
Button {
    property bool primary: false
    property bool showBorder: qgcPal.globalTheme === QGCPalette.Light
    property real backRadius: ScreenTools.defaultBorderRadius
    property real heightFactor: 0.5
    property string iconSource: ""
    property real fontWeight: Font.Normal // default for qml Text
    property real pointSize: ScreenTools.defaultFontPointSize

    property alias wrapMode: text.wrapMode
    property alias horizontalAlignment: text.horizontalAlignment
    property color backgroundColor: primary ? qgcPal.primaryButton : qgcPal.button
    property alias textColor: text.color

    id: control
    hoverEnabled: !ScreenTools.isMobile
    topPadding: _verticalPadding
    bottomPadding: _verticalPadding
    leftPadding: _horizontalPadding
    rightPadding: _horizontalPadding
    focusPolicy: Qt.ClickFocus
    font.family: ScreenTools.normalFontFamily
    text: ""

    property bool _showHighlight: enabled && (pressed | checked)
    property int _horizontalPadding: ScreenTools.defaultFontPixelWidth * 2
    property int _verticalPadding: Math.round(ScreenTools.defaultFontPixelHeight * heightFactor) - (iconSource === "" ? 0 : (_iconHeight - ScreenTools.defaultFontPixelHeight)  / 2)
    property real _iconHeight: text.height * 1.5

    // Dhaksha skin: a raised, 3D button. The face is lit from above and stands on a dark edge;
    // pressing it pushes the face down onto the edge.
    readonly property real _depth: Math.max(2, Math.round(ScreenTools.defaultFontPixelHeight * 0.18))
    readonly property bool _down: pressed || checked
    readonly property color _faceColor: _showHighlight ? qgcPal.buttonHighlight : backgroundColor

    transform: Translate { y: control._down ? control._depth : 0 }

    QGCPalette { id: qgcPal; colorGroupEnabled: control.enabled }

    background: Item {
        implicitWidth: ScreenTools.implicitButtonWidth
        implicitHeight: ScreenTools.implicitButtonHeight

        // The edge the face stands on; hidden under the face while the button is down
        Rectangle {
            anchors.fill: parent
            anchors.topMargin: control._depth
            anchors.bottomMargin: control._down ? 0 : -control._depth
            radius: backRect.radius
            color: Qt.darker(control._faceColor, 2.4)
            visible: control.enabled
        }

        Rectangle {
            id: backRect
            anchors.fill: parent
            radius: backRadius
            border.width: 1
            border.color: showBorder ? qgcPal.buttonBorder : Qt.darker(control._faceColor, 1.7)
            gradient: Gradient {
                GradientStop { position: 0.0; color: Qt.lighter(control._faceColor, control._down ? 1.05 : 1.45) }
                GradientStop { position: 0.12; color: Qt.lighter(control._faceColor, control._down ? 1.0 : 1.15) }
                GradientStop { position: 0.6; color: control._faceColor }
                GradientStop { position: 1.0; color: Qt.darker(control._faceColor, control._down ? 1.05 : 1.3) }
            }

            Rectangle {
                anchors.fill: parent
                color: qgcPal.buttonHighlight
                opacity: !_showHighlight && control.enabled && control.hovered ? .2 : 0
                radius: parent.radius
            }
        }
    }

    contentItem: RowLayout {
        spacing: ScreenTools.defaultFontPixelWidth

        QGCColoredImage {
            id: icon
            Layout.alignment: Qt.AlignHCenter
            source: control.iconSource
            height: _iconHeight
            width: height
            color: text.color
            fillMode: Image.PreserveAspectFit
            sourceSize.height: height
            visible: control.iconSource !== ""
        }

        QGCLabel {
            id: text
            Layout.alignment: Qt.AlignHCenter
            text: control.text
            font.pointSize: control.pointSize
            font.family: control.font.family
            font.weight: fontWeight
            color: _showHighlight ? qgcPal.buttonHighlightText : (primary ? qgcPal.primaryButtonText : qgcPal.buttonText)
            visible: control.text !== ""
        }
    }
}
