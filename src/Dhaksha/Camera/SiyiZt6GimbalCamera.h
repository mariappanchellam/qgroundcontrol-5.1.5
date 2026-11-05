/****************************************************************************
 *
 * DhakshaGroundControl: SIYI ZT6 dual-sensor (zoom + thermal) gimbal camera on the RC's Ethernet.
 *
 * Same SIYI protocol, UDP port and commands as the ZR10 (photo, record, zoom, gimbal, centre, down),
 * following SIYI's "ZT6 Optical Pod External SDK Guide" v0.1.0, plus:
 *
 *   0x11  video stitching mode: set to 3 each time the camera connects, so the main stream is the zoom
 *         camera and the sub stream the thermal camera. The day / night switch and the picture-in-picture
 *         box then work as with the Skydroid: day = main stream URL, night = sub stream URL.
 *   0x1B  thermal pseudo-colour (night palette), one of the camera's 11 palettes.
 *
 ****************************************************************************/

#pragma once

#include <QStringList>

#include "SiyiGimbalCamera.h"

class SiyiZt6GimbalCamera : public SiyiGimbalCamera
{
    Q_OBJECT

public:
    SiyiZt6GimbalCamera(const QString& cameraIp, const QString& rtspUrl, QObject* parent = nullptr);

    bool thermalPaletteSupported() const override { return true; }

    /// An index into paletteNames()
    void setThermalPalette(int palette) override;

    /// The ZT6 palettes in the order shown to the operator
    static QStringList paletteNames();

    /// The camera's own number for paletteNames()[index] (number 1 is reserved and skipped)
    static int paletteCode(int index);

    static constexpr quint8 kCmdSetStitching = 0x11;        ///< uint8 mode; reply: the mode now in use
    static constexpr quint8 kCmdSetPalette = 0x1B;          ///< uint8 palette; reply: the palette now in use
    static constexpr quint8 kStitchZoomMainThermalSub = 3;  ///< main stream: zoom, sub stream: thermal

protected:
    void _handleFrame(quint8 command, const QByteArray& data) override;

private:
    void _sendStitchingMode();
};
