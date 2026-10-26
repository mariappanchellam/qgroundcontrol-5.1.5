/****************************************************************************
 *
 * DhakshaGroundControl: second camera in the bottom-left picture-in-picture box.
 *
 * For a camera with day and night streams (Skydroid) the box shows the one
 * not on the main screen: night on the main screen puts day in the box and
 * day puts night in it. Tap the box to swap them. The small map button puts
 * the map back in the box (the second stream then stops); the camera button
 * on the map brings the camera back. Lies exactly over QGC's PiP box.
 *
 * The VideoOutput named "dhakshaPipVideo" is played by VideoManager's
 * picture-in-picture receiver, so it must exist when the window starts.
 *
 ****************************************************************************/

import QtQuick
import QtMultimedia

import QGroundControl
import QGroundControl.Controls

Item {
    id: root

    /// QGC's PiP box is showing the small window (not hidden, not expanded to the main screen)
    property bool boxShown: false

    readonly property bool _available:  DhakshaCameraManager.pipAvailable && boxShown
    readonly property real _button:     ScreenTools.defaultFontPixelHeight * 1.6
    readonly property string _iconPath: "/res/DhakshaCamera/"

    component SmallButton: Rectangle {
        id:             button
        width:          _button
        height:         width
        radius:         width / 2
        color:          "#C0202020"
        border.color:   "#80FFFFFF"

        property string icon
        property var    clickAction

        QGCColoredImage {
            anchors.centerIn:   parent
            width:              parent.width * 0.6
            height:             width
            source:             _iconPath + button.icon + ".svg"
            sourceSize.height:  height
            fillMode:           Image.PreserveAspectFit
            color:              "white"
        }
        MouseArea {
            anchors.fill:   parent
            onClicked:      button.clickAction()
        }
    }

    // The other camera, played by the picture-in-picture video receiver
    Rectangle {
        anchors.fill:   parent
        visible:        _available && DhakshaCameraManager.pipCamera
        color:          "black"
        border.color:   "#80FFFFFF"

        VideoOutput {
            objectName:         "dhakshaPipVideo"
            anchors.fill:       parent
            anchors.margins:    1
            fillMode:           VideoOutput.PreserveAspectFit
        }

        QGCLabel {
            anchors.left:       parent.left
            anchors.top:        parent.top
            anchors.margins:    ScreenTools.defaultFontPixelHeight / 4
            text:               DhakshaCameraManager.nightView ? qsTr("DAY") : qsTr("NIGHT")
            color:              "white"
            font.bold:          true
            font.pointSize:     ScreenTools.smallFontPointSize
            style:              Text.Outline
            styleColor:         "black"
        }

        // Tap anywhere on the box: swap day and night between the main screen and the box
        MouseArea {
            anchors.fill:   parent
            onClicked:      DhakshaCameraManager.toggleNightView()
        }

        SmallButton {
            anchors.right:      parent.right
            anchors.top:        parent.top
            anchors.margins:    ScreenTools.defaultFontPixelHeight / 4
            icon:               "map"
            clickAction:        function() { DhakshaCameraManager.pipCamera = false }
        }
    }

    // Map in the box: one button brings the camera back
    SmallButton {
        anchors.right:      parent.right
        anchors.top:        parent.top
        anchors.margins:    ScreenTools.defaultFontPixelHeight / 4
        visible:            _available && !DhakshaCameraManager.pipCamera
        icon:               DhakshaCameraManager.nightView ? "day" : "night"
        clickAction:        function() { DhakshaCameraManager.pipCamera = true }
    }
}
