/****************************************************************************
 *
 * Dhaksha boot screen.
 *
 * Continues the static Android launch image (same colour, same logo size and
 * position) with a quadcopter flight animation and a simulated loading bar,
 * then fades out and emits finished(). Tap anywhere to skip.
 *
 ****************************************************************************/

import QtQuick

Rectangle {
    id:     root
    color:  "#DEE21B"

    signal finished()

    /// Total length of the simulated boot sequence (ms), excluding the fade out.
    property int duration: 6000

    property real   _progress:  0
    property bool   _finishing: false
    readonly property real _minDim: Math.min(width, height)

    function finish() {
        if (_finishing) {
            return
        }
        _finishing = true
        bootSequence.stop()
        fadeOut.start()
    }

    // Swallow all input while visible; a tap skips the animation.
    MouseArea {
        anchors.fill:   parent
        onClicked:      root.finish()
    }

    // Matches android/res/drawable-nodpi/splash_logo.png (420 x 364 px, centred)
    // so the hand-over from the static launch image is seamless.
    Image {
        id:                 logo
        anchors.centerIn:   parent
        width:              Math.min(420, root.width * 0.8, root.height * 0.55 * 420 / 364)
        height:             width * 364 / 420
        source:             "/res/DhakshaSplashLogo.svg"
        sourceSize.width:   width
        sourceSize.height:  height
        fillMode:           Image.PreserveAspectFit
        smooth:             true
        mipmap:             true
    }

    // ---------------------------------------------------------------- Drone
    Item {
        id:         shadow
        width:      drone.width
        height:     drone.height
        x:          drone.x + drone.width * 0.18 * drone.scale
        y:          drone.y + drone.height * 0.28 * drone.scale
        rotation:   drone.rotation
        scale:      drone.scale * 0.92
        opacity:    0.16
        visible:    drone.visible

        Image {
            anchors.fill:       parent
            source:             "/res/DhakshaDroneBody.svg"
            sourceSize.width:   width
            sourceSize.height:  height
            smooth:             true
        }
    }

    Item {
        id:         drone
        width:      Math.max(90, root._minDim * 0.17)
        height:     width
        x:          -width * 2
        y:          root.height
        scale:      0.7

        readonly property real _propSize: width * 0.40

        Image {
            anchors.fill:       parent
            source:             "/res/DhakshaDroneBody.svg"
            sourceSize.width:   width
            sourceSize.height:  height
            smooth:             true
            mipmap:             true
        }

        // Motor positions in DhakshaDroneBody.svg: 42 and 158 on a 200 grid.
        Repeater {
            model: [
                { px: 0.21, py: 0.21, dir:  1 },
                { px: 0.79, py: 0.21, dir: -1 },
                { px: 0.21, py: 0.79, dir: -1 },
                { px: 0.79, py: 0.79, dir:  1 }
            ]

            Image {
                width:              drone._propSize
                height:             width
                x:                  drone.width  * modelData.px - width  / 2
                y:                  drone.height * modelData.py - height / 2
                source:             "/res/DhakshaDronePropeller.svg"
                sourceSize.width:   width
                sourceSize.height:  height
                smooth:             true

                // Animators run on the render thread, so the props keep
                // spinning smoothly even while the UI thread is busy loading.
                RotationAnimator on rotation {
                    from:       0
                    to:         360 * modelData.dir
                    duration:   180
                    loops:      Animation.Infinite
                    running:    true
                }
            }
        }
    }

    // ---------------------------------------------------------- Loading bar
    Column {
        id:                         loadingColumn
        anchors.horizontalCenter:   parent.horizontalCenter
        anchors.bottom:             parent.bottom
        anchors.bottomMargin:       Math.max(24, root.height * 0.07)
        spacing:                    Math.max(6, root._minDim * 0.015)

        Rectangle {
            id:             track
            width:          Math.min(root.width * 0.5, 560)
            height:         Math.max(8, root._minDim * 0.014)
            radius:         height / 2
            color:          "transparent"
            border.color:   "#000000"
            border.width:   Math.max(1.5, height * 0.2)

            Rectangle {
                x:          track.border.width
                y:          track.border.width
                height:     track.height - 2 * track.border.width
                width:      Math.max(height, (track.width - 2 * track.border.width) * root._progress)
                radius:     height / 2
                color:      "#000000"
            }
        }

        Row {
            anchors.horizontalCenter:   parent.horizontalCenter
            spacing:                    track.height

            Text {
                text:           root._progress < 0.20 ? qsTr("Starting DhakshaGroundControl…")
                              : root._progress < 0.45 ? qsTr("Initializing communication links…")
                              : root._progress < 0.70 ? qsTr("Loading maps and flight tools…")
                              : root._progress < 0.95 ? qsTr("Preparing for takeoff…")
                              :                         qsTr("Ready")
                color:          "#000000"
                font.pixelSize: Math.max(12, root._minDim * 0.03)
            }

            Text {
                text:           Math.round(root._progress * 100) + "%"
                color:          "#000000"
                font.pixelSize: Math.max(12, root._minDim * 0.03)
                font.bold:      true
            }
        }
    }

    // ------------------------------------------------------------- Timeline
    readonly property real _landX: root.width  / 2
    readonly property real _landY: Math.max(drone.height * 0.6, logo.y - drone.height * 0.62)

    SequentialAnimation {
        id:         bootSequence
        running:    true

        // Hold the static frame briefly so the switch from the launch image is invisible.
        PauseAnimation { duration: 250 }

        ParallelAnimation {
            NumberAnimation {
                target:         root
                property:       "_progress"
                from:           0
                to:             1
                duration:       root.duration - 250
                easing.type:    Easing.InOutQuad
            }

            SequentialAnimation {
                // Take off, sweep across, loop around the logo and come in to land above it.
                ParallelAnimation {
                    PathAnimation {
                        target:                     drone
                        duration:                   root.duration * 0.72
                        easing.type:                Easing.InOutSine
                        anchorPoint:                Qt.point(drone.width / 2, drone.height / 2)
                        orientation:                PathAnimation.RightFirst
                        orientationEntryDuration:   200
                        orientationExitDuration:    500
                        endRotation:                -90
                        path: Path {
                            startX: -root.width * 0.12;  startY: root.height * 0.88
                            PathCubic {
                                x: root.width * 0.28;           y: root.height * 0.22
                                control1X: root.width * 0.02;   control1Y: root.height * 0.55
                                control2X: root.width * 0.10;   control2Y: root.height * 0.18
                            }
                            PathCubic {
                                x: root.width * 0.82;           y: root.height * 0.30
                                control1X: root.width * 0.48;   control1Y: root.height * 0.26
                                control2X: root.width * 0.68;   control2Y: root.height * 0.02
                            }
                            PathCubic {
                                x: root.width * 0.70;           y: root.height * 0.80
                                control1X: root.width * 1.00;   control1Y: root.height * 0.55
                                control2X: root.width * 0.92;   control2Y: root.height * 0.90
                            }
                            PathCubic {
                                x: root._landX;                 y: root._landY
                                control1X: root.width * 0.40;   control1Y: root.height * 0.70
                                control2X: root.width * 0.30;   control2Y: root._landY
                            }
                        }
                    }

                    // Altitude cue: climb after take-off, then descend on approach.
                    SequentialAnimation {
                        NumberAnimation { target: drone; property: "scale"; from: 0.7;  to: 1.15; duration: root.duration * 0.30; easing.type: Easing.OutQuad }
                        NumberAnimation { target: drone; property: "scale"; from: 1.15; to: 0.95; duration: root.duration * 0.42; easing.type: Easing.InOutQuad }
                    }
                }

                // Hover, then touch down above the logo.
                SequentialAnimation {
                    loops: 2
                    NumberAnimation { target: drone; property: "y"; to: root._landY - drone.height * 0.56; duration: root.duration * 0.05; easing.type: Easing.InOutSine }
                    NumberAnimation { target: drone; property: "y"; to: root._landY - drone.height * 0.50; duration: root.duration * 0.05; easing.type: Easing.InOutSine }
                }
                NumberAnimation { target: drone; property: "scale"; to: 0.82; duration: root.duration * 0.06; easing.type: Easing.OutQuad }
            }
        }

        PauseAnimation { duration: 250 }
        ScriptAction {
            script: {
                root._finishing = true
                fadeOut.start()
            }
        }
    }

    OpacityAnimator {
        id:         fadeOut
        target:     root
        from:       1
        to:         0
        duration:   450
        onFinished: root.finished()
    }
}
