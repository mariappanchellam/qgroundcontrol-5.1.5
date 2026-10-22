/****************************************************************************
 *
 * Dhaksha: password prompt guarding the Vehicle Configuration
 * Parameters page.
 *
 ****************************************************************************/

import QtQuick
import QtQuick.Controls

import QGroundControl
import QGroundControl.Controls

Popup {
    id:             root
    parent:         Overlay.overlay
    modal:          true
    focus:          true
    closePolicy:    Popup.CloseOnEscape
    padding:        ScreenTools.defaultFontPixelWidth * 2
    x:              Math.round((parent.width  - width)  / 2)
    // Upper third so the on-screen keyboard does not cover the dialog on Android
    y:              Math.round((parent.height - height) / 3)

    /// Emitted after the correct password is entered and OK is pressed.
    signal passwordAccepted()

    property string password: "123"

    function _tryAccept() {
        if (passwordField.text === password) {
            close()
            passwordAccepted()
        } else {
            errorLabel.visible = true
            passwordField.text = ""
            passwordField.forceActiveFocus()
        }
    }

    onOpened: {
        passwordField.text = ""
        errorLabel.visible = false
        passwordField.forceActiveFocus()
    }

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    background: Rectangle {
        color:          qgcPal.window
        border.color:   qgcPal.text
        border.width:   1
        radius:         ScreenTools.defaultFontPixelWidth / 2
    }

    contentItem: Column {
        spacing: ScreenTools.defaultFontPixelHeight / 2

        QGCLabel {
            text:       qsTr("Parameters")
            font.bold:  true
        }

        QGCLabel {
            text: qsTr("Enter password to access vehicle parameters:")
        }

        QGCTextField {
            id:                 passwordField
            width:              ScreenTools.defaultFontPixelWidth * 36
            echoMode:           TextInput.Password
            inputMethodHints:   Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
            onAccepted:         root._tryAccept()
            onTextChanged:      if (text !== "") errorLabel.visible = false
        }

        QGCLabel {
            id:         errorLabel
            visible:    false
            color:      qgcPal.warningText
            text:       qsTr("Incorrect password")
        }

        // Cancel on the left, OK on the right
        Item {
            width:  passwordField.width
            height: okButton.height

            QGCButton {
                anchors.left:   parent.left
                text:           qsTr("Cancel")
                onClicked:      root.close()
            }

            QGCButton {
                id:             okButton
                anchors.right:  parent.right
                text:           qsTr("OK")
                primary:        true
                onClicked:      root._tryAccept()
            }
        }
    }
}
