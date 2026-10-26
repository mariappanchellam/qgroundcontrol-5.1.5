import QtQuick
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls

// DhakshaGroundControl: gimbal camera vendor, addresses, night palette and video mode
Rectangle {
    objectName:     "settingsPage_GimbalCamera"
    color:          qgcPal.window
    anchors.fill:   parent

    readonly property real _margins:    ScreenTools.defaultFontPixelHeight
    readonly property real _fieldWidth: ScreenTools.defaultFontPixelWidth * 28

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    QGCFlickable {
        anchors.margins:    _margins
        anchors.fill:       parent
        contentWidth:       grid.width
        contentHeight:      grid.height
        clip:               true

        GridLayout {
            id:             grid
            columns:        2
            rowSpacing:     _margins
            columnSpacing:  _margins

            QGCLabel { text: qsTr("Camera Vendor") }
            QGCComboBox {
                Layout.preferredWidth:  _fieldWidth
                // The fleet's two cameras: Skydroid (vendor 1) and SIYI ZR10 (vendor 2)
                model:                  [ "Skydroid", "ZR10" ]
                currentIndex:           DhakshaCameraManager.vendor - 1
                onActivated:            (index) => DhakshaCameraManager.vendor = index + 1
            }

            QGCLabel { text: qsTr("Camera IP") }
            QGCTextField {
                Layout.preferredWidth:  _fieldWidth
                text:                   DhakshaCameraManager.cameraIp
                enabled:                DhakshaCameraManager.vendor !== 0
                onEditingFinished:      DhakshaCameraManager.cameraIp = text
            }

            QGCLabel { text: qsTr("RTSP URL") }
            QGCTextField {
                Layout.preferredWidth:  _fieldWidth * 1.6
                text:                   DhakshaCameraManager.rtspUrl
                enabled:                DhakshaCameraManager.vendor !== 0
                onEditingFinished:      DhakshaCameraManager.rtspUrl = text
            }

            QGCLabel {
                text:       qsTr("Gimbal Command")
                visible:    DhakshaCameraManager.vendor === 1
            }
            QGCComboBox {
                Layout.preferredWidth:  _fieldWidth
                visible:                DhakshaCameraManager.vendor === 1
                model:                  [ qsTr("YPR (other Topotek cameras)"), qsTr("GSY / GSP (Skydroid C10 / C13)") ]
                currentIndex:           DhakshaCameraManager.skydroidGimbalStyle
                onActivated:            (index) => DhakshaCameraManager.skydroidGimbalStyle = index
            }

            QGCLabel {
                text:       qsTr("Night Palette")
                visible:    DhakshaCameraManager.vendor === 1
            }
            QGCComboBox {
                Layout.preferredWidth:  _fieldWidth
                visible:                DhakshaCameraManager.vendor === 1
                model:                  DhakshaCameraManager.thermalPaletteNames
                currentIndex:           DhakshaCameraManager.thermalPalette
                onActivated:            (index) => DhakshaCameraManager.thermalPalette = index
            }

            QGCLabel { text: qsTr("Video") }
            QGCComboBox {
                Layout.preferredWidth:  _fieldWidth * 1.6
                model:                  [ qsTr("Low delay (UDP, low latency mode)"), qsTr("Smooth (UDP, 150 ms buffer)"), qsTr("Reliable (TCP, 100 ms buffer)") ]
                currentIndex:           DhakshaCameraManager.videoMode
                onActivated:            (index) => DhakshaCameraManager.videoMode = index
            }

            QGCLabel { text: qsTr("Night RTSP URL") }
            QGCTextField {
                Layout.preferredWidth:  _fieldWidth * 1.6
                text:                   DhakshaCameraManager.nightRtspUrl
                placeholderText:        qsTr("Thermal camera, if any")
                enabled:                DhakshaCameraManager.vendor !== 0
                onEditingFinished:      DhakshaCameraManager.nightRtspUrl = text
            }

            QGCLabel { text: qsTr("Status") }
            QGCLabel {
                property var _camera: DhakshaCameraManager.camera
                text: !_camera ? qsTr("No camera selected")
                    : !_camera.controlSupported ? qsTr("%1: video stream only").arg(_camera.vendorName)
                    : _camera.connected ? qsTr("%1: connected").arg(_camera.vendorName)
                    : qsTr("%1: no reply from camera").arg(_camera.vendorName)
            }

            QGCButton {
                text:       qsTr("Restore Defaults")
                enabled:    DhakshaCameraManager.vendor !== 0
                onClicked:  DhakshaCameraManager.restoreVendorDefaults()
            }
            QGCButton {
                text:       qsTr("Apply and Start Video")
                primary:    true
                onClicked:  DhakshaCameraManager.apply()
            }
        }
    }
}
