/****************************************************************************
 *
 * DhakshaGroundControl: SIYI ZT6 dual-sensor (zoom + thermal) gimbal camera on the RC's Ethernet.
 *
 ****************************************************************************/

#include "SiyiZt6GimbalCamera.h"

#include <array>

namespace {
// SDK guide, 0x1A / 0x1B: 0 White_Hot, 1 Reserved, 2 Sepia, 3 Ironbow, 4 Rainbow, 5 Night, 6 Aurora,
// 7 Red_Hot, 8 Jungle, 9 Medical, 10 Black_Hot, 11 Glory_Hot
constexpr std::array<int, 11> kPaletteCodes = {0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
}  // namespace

SiyiZt6GimbalCamera::SiyiZt6GimbalCamera(const QString& cameraIp, const QString& rtspUrl, QObject* parent)
    : SiyiGimbalCamera(QStringLiteral("ZT6"), cameraIp, rtspUrl, parent)
{
    // Each time the camera (re)connects, e.g. after it was powered off, it gets the stream layout the
    // day / night URLs expect
    connect(this, &DhakshaGimbalCamera::connectedChanged, this, [this]() {
        if (connected()) {
            _sendStitchingMode();
        }
    });
}

QStringList SiyiZt6GimbalCamera::paletteNames()
{
    return {tr("White hot"), tr("Sepia"),  tr("Ironbow"), tr("Rainbow"),   tr("Night"),    tr("Aurora"),
            tr("Red hot"),   tr("Jungle"), tr("Medical"), tr("Black hot"), tr("Glory hot")};
}

int SiyiZt6GimbalCamera::paletteCode(int index)
{
    return (index >= 0 && index < static_cast<int>(kPaletteCodes.size())) ? kPaletteCodes[static_cast<size_t>(index)]
                                                                          : -1;
}

void SiyiZt6GimbalCamera::setThermalPalette(int palette)
{
    const int code = paletteCode(palette);
    if (code < 0) {
        return;
    }
    _send(kCmdSetPalette, QByteArray(1, static_cast<char>(code)),
          tr("Night palette %1").arg(paletteNames().at(palette)));
}

void SiyiZt6GimbalCamera::_sendStitchingMode()
{
    _send(kCmdSetStitching, QByteArray(1, static_cast<char>(kStitchZoomMainThermalSub)),
          tr("Video streams: zoom on main, thermal on sub"));
}

void SiyiZt6GimbalCamera::_handleFrame(quint8 command, const QByteArray& data)
{
    switch (command) {
        case kCmdSetStitching:
            if (!data.isEmpty() && static_cast<quint8>(data.at(0)) != kStitchZoomMainThermalSub) {
                emit notice(
                    tr("ZT6 kept video mode %1: day / night may be swapped").arg(static_cast<quint8>(data.at(0))));
            }
            break;
        case kCmdSetPalette:
            break;
        default:
            SiyiGimbalCamera::_handleFrame(command, data);
            break;
    }
}
