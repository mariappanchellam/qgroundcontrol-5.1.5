import QtQuick

import QGroundControl
import QGroundControl.Controls

Item {
    id: _root

    property Item pipView
    property Item pipState: videoPipState

    PipState {
        id:         videoPipState
        pipView:    _root.pipView
        isDark:     true

        onWindowAboutToOpen: {
            QGroundControl.videoManager.stopVideo()
            videoStartDelay.start()
        }

        onWindowAboutToClose: {
            QGroundControl.videoManager.stopVideo()
            videoStartDelay.start()
        }

        onStateChanged: {
            if (pipState.state !== pipState.fullState) {
                QGroundControl.videoManager.fullScreen = false
            }
        }
    }

    Timer {
        id:           videoStartDelay
        interval:     2000;
        running:      false
        repeat:       false
        onTriggered:  QGroundControl.videoManager.startVideo()
    }

    //-- Video Streaming
    FlightDisplayViewVideo {
        id:             videoStreaming
        anchors.fill:   parent
        useSmallFont:   _root.pipState.state !== _root.pipState.fullState
        visible:        QGroundControl.videoManager.isStreamSource || QGroundControl.videoManager.isUvc
    }

    QGCLabel {
        text: qsTr("Double-click to exit full screen")
        font.pointSize: ScreenTools.largeFontPointSize
        visible: QGroundControl.videoManager.fullScreen
        anchors.centerIn: parent

        onVisibleChanged: {
            if (visible) {
                labelAnimation.start()
            }
        }

        PropertyAnimation on opacity {
            id: labelAnimation
            duration: 10000
            from: 1.0
            to: 0.0
            easing.type: Easing.InExpo
        }
    }

    OnScreenGimbalController {
        id:                      onScreenGimbalController
        anchors.fill:            parent
        cameraTrackingEnabled:   !!(videoStreaming._camera && videoStreaming._camera.trackingEnabled)
    }

    OnScreenCameraTrackingController {
        id:                      cameraTrackingController
        anchors.fill:            parent
        camera:                  videoStreaming._camera
        videoWidth:              videoStreaming.getWidth()
        videoHeight:             videoStreaming.getHeight()
    }

    MouseArea {
        id:                         flyViewVideoMouseArea
        anchors.fill:               parent
        enabled:                    pipState.state === pipState.fullState

        property real _pressX:      0
        property real _pressY:      0
        property bool _dragging:    false
        property bool _doubleClicked: false
        readonly property real _dragThreshold: 10

        // DhakshaGroundControl: with the on-screen gimbal joystick hidden, dragging on the video turns the
        // gimbal. The press point is the centre of an invisible joystick: the further the finger moves from
        // it, the faster the gimbal turns, and lifting the finger stops it.
        readonly property var  _dhakshaCamera:      DhakshaCameraManager.camera
        readonly property bool _dhakshaGesture:     _dhakshaCamera && _dhakshaCamera.controlSupported && !DhakshaCameraManager.gimbalControlsShown
        readonly property real _dhakshaFullDrag:    Math.max(ScreenTools.defaultFontPixelHeight * 6, height * 0.3)
        property bool _dhakshaTurning:  false
        property int  _dhakshaYawSent:  0
        property int  _dhakshaPitchSent: 0

        preventStealing: _dhakshaGesture

        function _dhakshaSpeed(distance) {
            var fraction = Math.max(-1, Math.min(1, distance / _dhakshaFullDrag))
            return Math.round((fraction < 0 ? -1 : 1) * fraction * fraction * 100)
        }

        function _dhakshaTurn(x, y) {
            var limit = DhakshaCameraManager.gimbalMaxSpeed / 60
            var yaw   = Math.round(_dhakshaSpeed(x - _pressX) * limit)
            var pitch = Math.round(_dhakshaSpeed(_pressY - y) * limit)
            _dhakshaTurning = true
            gestureMark.fingerX = x
            gestureMark.fingerY = y
            if (yaw !== _dhakshaYawSent || pitch !== _dhakshaPitchSent) {
                _dhakshaYawSent   = yaw
                _dhakshaPitchSent = pitch
                _dhakshaCamera.gimbalSpeed(yaw, pitch)
            }
        }

        function _dhakshaStop() {
            if (_dhakshaTurning) {
                _dhakshaTurning   = false
                _dhakshaYawSent   = 0
                _dhakshaPitchSent = 0
                if (_dhakshaCamera) {
                    _dhakshaCamera.gimbalStop()
                }
            }
        }

        on_DhakshaGestureChanged:   if (!_dhakshaGesture) _dhakshaStop()
        onCanceled:                 { _dhakshaStop(); _dragging = false }

        // Defer single-click handling so a double-click (fullscreen toggle) doesn't also
        // fire an unintended gimbal click-to-point/tracking command on its first click.
        Timer {
            id:         singleClickTimer
            interval:   Qt.styleHints.mouseDoubleClickInterval
            repeat:     false

            property real clickX: 0
            property real clickY: 0

            onTriggered: {
                onScreenGimbalController.mouseClicked(clickX, clickY)
                cameraTrackingController.mouseClicked(clickX, clickY)
            }
        }

        onDoubleClicked: {
            // Fires on the second press of a double-click. The second release still emits
            // onReleased, so flag it to prevent re-arming the single-click timer.
            _doubleClicked = true
            singleClickTimer.stop()
            QGroundControl.videoManager.fullScreen = !QGroundControl.videoManager.fullScreen
        }

        onPressed: (mouse) => {
            _pressX = mouse.x
            _pressY = mouse.y
            _dragging = false
            // Clear any stale flag (e.g. double-click followed by drag releases through the
            // drag branch without consuming it). Safe: pressed is emitted before doubleClicked.
            _doubleClicked = false
        }

        onPositionChanged: (mouse) => {
            if (!_dragging && (Math.abs(mouse.x - _pressX) >= _dragThreshold || Math.abs(mouse.y - _pressY) >= _dragThreshold)) {
                _dragging = true
                onScreenGimbalController.mouseDragStart(_pressX, _pressY)
                cameraTrackingController.mouseDragStart(_pressX, _pressY)
            }
            if (_dragging) {
                onScreenGimbalController.mouseDragPositionChanged(mouse.x, mouse.y)
                cameraTrackingController.mouseDragPositionChanged(mouse.x, mouse.y)
                if (_dhakshaGesture) {
                    _dhakshaTurn(mouse.x, mouse.y)
                }
            }
        }

        onReleased: (mouse) => {
            _dhakshaStop()
            if (_dragging) {
                onScreenGimbalController.mouseDragEnd()
                cameraTrackingController.mouseDragEnd(mouse.x, mouse.y)
            } else if (_doubleClicked) {
                // Second release of a double-click - fullscreen toggle already handled
                _doubleClicked = false
            } else {
                singleClickTimer.clickX = mouse.x
                singleClickTimer.clickY = mouse.y
                singleClickTimer.restart()
            }
            _dragging = false
        }
    }

    // Where the gimbal drag started and where the finger is now
    Item {
        id:             gestureMark
        anchors.fill:   parent
        visible:        flyViewVideoMouseArea._dhakshaTurning

        property real fingerX: 0
        property real fingerY: 0
        readonly property real _ring: ScreenTools.defaultFontPixelHeight * 2
        readonly property real _dot:  ScreenTools.defaultFontPixelHeight * 0.9

        Rectangle {
            x:              flyViewVideoMouseArea._pressX - width / 2
            y:              flyViewVideoMouseArea._pressY - height / 2
            width:          gestureMark._ring
            height:         width
            radius:         width / 2
            color:          "transparent"
            border.color:   "white"
            border.width:   2
            opacity:        0.7
        }
        Rectangle {
            x:          gestureMark.fingerX - width / 2
            y:          gestureMark.fingerY - height / 2
            width:      gestureMark._dot
            height:     width
            radius:     width / 2
            color:      "#F26522"
        }
    }

    ProximityRadarVideoView{
        anchors.fill:   parent
        vehicle:        QGroundControl.multiVehicleManager.activeVehicle
    }

    ObstacleDistanceOverlayVideo {
        id: obstacleDistance
        showText: pipState.state === pipState.fullState
    }
}
