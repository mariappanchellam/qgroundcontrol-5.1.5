/****************************************************************************
 *
 * DhakshaGroundControl: Skydroid gimbal camera (C10, C13).
 *
 ****************************************************************************/

#include "SkydroidGimbalCamera.h"

#include <QSettings>
#include <QUdpSocket>
#include <QtGlobal>

namespace {
    // Command ids of the Topotek protocol used by Skydroid cameras
    const char* kIdGimbalMode   = "PTZ";    // 00 stop, 05 home/centre, 06 lock, 07 follow, 0A one-key down
    const char* kIdYawSpeed     = "GSY";    // signed speed -99..99, positive = right
    const char* kIdPitchSpeed   = "GSP";    // signed speed -99..99, positive = up
    const char* kIdRecord       = "REC";    // 00 stop, 01 start
    const char* kIdCapture      = "CAP";    // 01 take picture
    const char* kIdSdCard       = "SDC";    // read 00: remaining capacity; reply "NNNN" when there is no usable card
    const char* kIdZoom         = "ZMC";    // 00 stop, 01 out, 02 in
    const char* kIdDigitalZoom  = "DZM";    // as ZMC, for digital zoom
    const char* kIdFocus        = "FCC";    // 00 stop, 01 near, 02 far, 10 auto
    const char* kIdDayNight     = "IRC";    // 0A toggle
    const char* kIdPip          = "PIP";    // 00..03
    // Thermal pseudo colour (Topotek protocol): 00 white hot, 01 black hot, 02 ironbow, 03 rainbow, 04 glowbow,
    // 05 autumn, 06 winter, 07 hot metal; 0A next, 0B previous
    const char* kIdPalette      = "IMG";
    const char* kIdRates        = "YPR";    // yaw, pitch, roll speeds -99..99 as 2 hex each, positive pitch = down
    const char* kIdRange        = "LRF";    // 00 stop, 01 start, 02 single, 03 continuous; reply "12345.6" metres or "ERR"

    char hexChar(quint8 nibble)
    {
        return "0123456789ABCDEF"[nibble & 0x0F];
    }
}

SkydroidGimbalCamera::SkydroidGimbalCamera(const QString& cameraIp, const QString& rtspUrl, QObject* parent)
    : DhakshaGimbalCamera   (QStringLiteral("Skydroid"), rtspUrl, parent)
    , _socket               (new QUdpSocket(this))
    , _cameraAddress        (cameraIp)
{
    // Always the same local port: a camera that remembers its controller's address and port keeps
    // answering after the app or the tablet restarts. Any free port if another program holds it.
    if (_socket->bind(QHostAddress(QHostAddress::AnyIPv4), kControlPort, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        _logCommand(tr("Camera control on UDP port %1").arg(kControlPort));
    } else {
        _socket->bind(QHostAddress(QHostAddress::AnyIPv4), 0);
        _logCommand(tr("UDP port %1 busy, camera control on port %2").arg(kControlPort).arg(_socket->localPort()));
    }
    connect(_socket, &QUdpSocket::readyRead, this, &SkydroidGimbalCamera::_readBytes);

    _pollTimer.setInterval(kPollIntervalMs);
    connect(&_pollTimer, &QTimer::timeout, this, &SkydroidGimbalCamera::_pollStatus);
    _pollTimer.start();

    _gimbalTimer.setInterval(kGimbalResendMs);
    connect(&_gimbalTimer, &QTimer::timeout, this, &SkydroidGimbalCamera::_resendGimbalSpeed);

    // Zoom is repeated while the button is held, like the gimbal speed
    _zoomTimer.setInterval(kZoomResendMs);
    connect(&_zoomTimer, &QTimer::timeout, this, [this]() {
        _sendZoom(_zoomDirection > 0 ? 0x02 : 0x01, false);
        _stepDigitalZoom();
    });

    _rangeTimer.setInterval(kRangeReadMs);

    _downTimer.setSingleShot(true);
    _downTimer.setInterval(kTiltDownMs);
    connect(&_downTimer, &QTimer::timeout, this, &SkydroidGimbalCamera::gimbalStop);
    connect(&_rangeTimer, &QTimer::timeout, this, &SkydroidGimbalCamera::_readRange);

    _gimbalStyle = QSettings().value(kGimbalStyleKey, kDefaultGimbalStyle).toInt() == GimbalStyleYpr ? GimbalStyleYpr : GimbalStyleGsy;

    _pollStatus();
}

QByteArray SkydroidGimbalCamera::buildFrame(char address, bool write, const char* id, quint8 data)
{
    return buildFrame(address, write, id, QByteArray(1, hexChar(data >> 4)) + hexChar(data));
}

QByteArray SkydroidGimbalCamera::buildFrame(char address, bool write, const char* id, const QByteArray& data)
{
    const QByteArray payload = data.left(15);
    QByteArray frame(payload.size() == 2 ? "#TP" : "#tp");
    frame.append(kSource);
    frame.append(address);
    frame.append(hexChar(static_cast<quint8>(payload.size())));
    frame.append(write ? 'w' : 'r');
    frame.append(id, 3);
    frame.append(payload);

    quint8 checksum = 0;
    for (const char c : frame) {
        checksum += static_cast<quint8>(c);
    }
    frame.append(hexChar(checksum >> 4));
    frame.append(hexChar(checksum));
    return frame;
}

void SkydroidGimbalCamera::_send(char address, bool write, const char* id, quint8 data, const QString& description)
{
    _sendFrame(buildFrame(address, write, id, data), description);
}

void SkydroidGimbalCamera::_sendFrame(const QByteArray& frame, const QString& description)
{
    const bool sent = !_cameraAddress.isNull() && _socket->writeDatagram(frame, _cameraAddress, kControlPort) == frame.size();

    if (!description.isEmpty()) {
        _logCommand(QStringLiteral("%1 %2  %3").arg(sent ? QStringLiteral("TX") : tr("TX FAILED"), QString::fromLatin1(frame), description));
    }
}

// ---------------------------------------------------------------------------- Camera

void SkydroidGimbalCamera::takePhoto()
{
    if (!sdCardProblem().isEmpty()) {
        emit notice(tr("Photo not possible: %1").arg(sdCardProblem()));
        return;
    }
    _send(kAddressImage, true, kIdCapture, 0x01, tr("Take photo"));
    emit notice(tr("Photo taken"));
}

void SkydroidGimbalCamera::startRecording()
{
    if (!sdCardProblem().isEmpty()) {
        emit notice(tr("Recording not possible: %1").arg(sdCardProblem()));
        return;
    }
    _send(kAddressImage, true, kIdRecord, 0x01, tr("Start recording"));
    // Shown as recording only once the camera says so: ask now and give up after 3 s
    _recordRequested = true;
    emit notice(tr("Starting recording..."));
    QTimer::singleShot(600, this, [this]() { _send(kAddressImage, false, kIdRecord, 0x00); });
    QTimer::singleShot(1500, this, [this]() { _send(kAddressImage, false, kIdRecord, 0x00); });
    QTimer::singleShot(3000, this, &SkydroidGimbalCamera::_checkRecordingStarted);
}

void SkydroidGimbalCamera::_checkRecordingStarted()
{
    if (_recordRequested && !recording()) {
        _recordRequested = false;
        emit notice(tr("Recording did not start (SD card missing or full?)"));
        _logCommand(tr("Recording not confirmed by the camera"));
    }
}

void SkydroidGimbalCamera::stopRecording()
{
    _recordRequested = false;
    _send(kAddressImage, true, kIdRecord, 0x00, tr("Stop recording"));
    _setRecording(false);
}

void SkydroidGimbalCamera::setThermalPalette(int palette)
{
    // Topotek documents the thermal camera at 'D' (SHD series) or 'E' (SMT series); the other one ignores it
    _send(kAddressImage,   true, kIdPalette, static_cast<quint8>(palette), tr("Night palette %1").arg(palette));
    _send(kAddressThermal, true, kIdPalette, static_cast<quint8>(palette));
}

void SkydroidGimbalCamera::toggleDayNight()
{
    _send(kAddressImage, true, kIdDayNight, 0x0A, tr("Day / night toggle"));
}

void SkydroidGimbalCamera::setPipMode(int mode)
{
    const int pipMode = qBound(0, mode, 3);
    _send(kAddressImage, true, kIdPip, static_cast<quint8>(pipMode), tr("PIP mode %1").arg(pipMode));
}

void SkydroidGimbalCamera::measureDistance()
{
    _send(kAddressLens, true, kIdRange, 0x02, tr("Laser range (single)"));
}

// The C13's laser address is not published (ArduPilot uses the lens address), so the laser commands go
// to the lens, image and gimbal addresses; a reply from any of them is shown.
void SkydroidGimbalCamera::_sendRange(bool write, quint8 value, const QString& description)
{
    _send(kAddressLens,   write, kIdRange, value, description);
    _send(kAddressImage,  write, kIdRange, value);
    _send(kAddressGimbal, write, kIdRange, value);
}

void SkydroidGimbalCamera::toggleRanging()
{
    if (rangingActive()) {
        _rangeTimer.stop();
        _sendRange(true, 0x00, tr("Laser range off"));
        _setRangingActive(false);
        _setDistanceText(QString());
    } else {
        // Continuous measurement, as ArduPilot enables it; the distance is also asked for every second
        _sendRange(true, 0x03, tr("Laser range on (continuous)"));
        _setRangingActive(true);
        _setDistanceText(tr("measuring..."));
        _rangeReads = 0;
        _rangeTimer.start();
    }
}

void SkydroidGimbalCamera::_readRange()
{
    // A single measurement each time as well, for cameras that do not measure continuously
    _sendRange(true, 0x02, QString());
    _sendRange(false, 0x00, QString());
    if (++_rangeReads == 3 && distanceText() == tr("measuring...")) {
        _setDistanceText(tr("no reply from laser"));
        _logCommand(tr("Laser: no reply in 3 s"));
    }
}

// ---------------------------------------------------------------------------- Lens

// Optical zoom (ZMC) runs while the button is held and stops on release. The C13 has digital zoom only:
// in the field DZM 02 zoomed one step and DZM 00 went back to 1x, so DZM takes the zoom level itself.
// Holding the button steps that level; releasing keeps it. Both go to the lens and the image address,
// as the camera ignores the ones it does not know.
void SkydroidGimbalCamera::_sendZoom(quint8 value, bool log)
{
    static const QString names[] = { tr("Zoom stop"), tr("Zoom out"), tr("Zoom in") };
    const QString description = log && value < 3 ? names[value] : QString();
    _send(kAddressLens,  true, kIdZoom, value, description);
    _send(kAddressImage, true, kIdZoom, value);
}

void SkydroidGimbalCamera::_stepDigitalZoom()
{
    const int level = qBound(1, _digitalZoomLevel + _zoomDirection, kMaxZoom);
    if (level == _digitalZoomLevel && _zoomDirection != 0) {
        if (_zoomDirection > 0 && !_maxZoomShown) {
            _maxZoomShown = true;
            emit notice(tr("Max zoom %1x").arg(kMaxZoom));
        }
        return;
    }
    _maxZoomShown = false;
    _digitalZoomLevel = level;

    // The camera zooms up to kMaxCameraZoom itself; above that the video is magnified on screen
    const int cameraLevel = qMin(level, kMaxCameraZoom);
    if (cameraLevel != _cameraZoomSent) {
        _cameraZoomSent = cameraLevel;
        _send(kAddressLens,  true, kIdDigitalZoom, static_cast<quint8>(cameraLevel), tr("Digital zoom %1x").arg(cameraLevel));
        _send(kAddressImage, true, kIdDigitalZoom, static_cast<quint8>(cameraLevel));
    }
    _setScreenZoom(static_cast<qreal>(level) / cameraLevel);
    emit notice(tr("Zoom %1x").arg(level));
}

void SkydroidGimbalCamera::zoomIn()
{
    _zoomDirection = 1;
    _sendZoom(0x02, true);
    _stepDigitalZoom();
    _zoomTimer.start();
}

void SkydroidGimbalCamera::zoomOut()
{
    _zoomDirection = -1;
    _sendZoom(0x01, true);
    _stepDigitalZoom();
    _zoomTimer.start();
}

void SkydroidGimbalCamera::zoomStop()
{
    _zoomTimer.stop();
    _zoomDirection = 0;
    _sendZoom(0x00, true);
}
void SkydroidGimbalCamera::focusIn()    { _send(kAddressLens, true, kIdFocus, 0x01, tr("Focus near")); }
void SkydroidGimbalCamera::focusOut()   { _send(kAddressLens, true, kIdFocus, 0x02, tr("Focus far")); }
void SkydroidGimbalCamera::focusStop()  { _send(kAddressLens, true, kIdFocus, 0x00, tr("Focus stop")); }
void SkydroidGimbalCamera::autoFocus()  { _send(kAddressLens, true, kIdFocus, 0x10, tr("Auto focus")); }

// ---------------------------------------------------------------------------- Gimbal

int SkydroidGimbalCamera::_percentToSpeed(int percent)
{
    return qBound(-99, (qBound(-100, percent, 100) * 99) / 100, 99);
}

void SkydroidGimbalCamera::gimbalSpeed(int yawPercent, int pitchPercent)
{
    _downTimer.stop();
    // The joystick calls this many times a second: only the start of a movement is logged
    const bool wasMoving = _yawSpeed != 0 || _pitchSpeed != 0;
    _yawSpeed   = _percentToSpeed(yawPercent);
    _pitchSpeed = _percentToSpeed(pitchPercent);

    if (_yawSpeed == 0 && _pitchSpeed == 0) {
        gimbalStop();
        return;
    }
    _sendGimbalSpeed(!wasMoving);
    if (!_gimbalTimer.isActive()) {
        _gimbalTimer.start();
    }
}

void SkydroidGimbalCamera::_sendGimbalSpeed(bool log)
{
    // One command style only: a camera obeying two styles at once moves in steps
    if (_gimbalStyle == GimbalStyleGsy) {
        _send(kAddressGimbal, true, kIdYawSpeed,   static_cast<quint8>(static_cast<qint8>(_yawSpeed)),   log ? tr("Gimbal yaw speed %1").arg(_yawSpeed) : QString());
        _send(kAddressGimbal, true, kIdPitchSpeed, static_cast<quint8>(static_cast<qint8>(_pitchSpeed)), log ? tr("Gimbal pitch speed %1").arg(_pitchSpeed) : QString());
        return;
    }

    QByteArray rates;
    for (const int speed : { _yawSpeed, -_pitchSpeed, 0 }) {
        const quint8 value = static_cast<quint8>(static_cast<qint8>(speed));
        rates.append(hexChar(value >> 4));
        rates.append(hexChar(value));
    }
    _sendFrame(buildFrame(kAddressGimbal, true, kIdRates, rates), log ? tr("Gimbal rates yaw %1 pitch %2").arg(_yawSpeed).arg(_pitchSpeed) : QString());
}

void SkydroidGimbalCamera::_resendGimbalSpeed()
{
    _sendGimbalSpeed(false);
}

void SkydroidGimbalCamera::gimbalStop()
{
    _downTimer.stop();
    _gimbalTimer.stop();
    _yawSpeed   = 0;
    _pitchSpeed = 0;
    _sendGimbalSpeed(true);
    _send(kAddressGimbal, true, kIdGimbalMode, 0x00, tr("Gimbal stop"));
}

void SkydroidGimbalCamera::gimbalCenter()
{
    _gimbalTimer.stop();
    _send(kAddressGimbal, true, kIdGimbalMode, 0x05, tr("Gimbal centre"));
}

void SkydroidGimbalCamera::gimbalLock()
{
    _send(kAddressGimbal, true, kIdGimbalMode, 0x06, tr("Gimbal lock"));
}

void SkydroidGimbalCamera::gimbalFollow()
{
    _send(kAddressGimbal, true, kIdGimbalMode, 0x07, tr("Gimbal follow"));
}

void SkydroidGimbalCamera::gimbalDown()
{
    // The C13 ignores the one-key-down command, so the gimbal is also driven down at full speed until it
    // reaches its lowest point (90 degrees down), then stopped
    _send(kAddressGimbal, true, kIdGimbalMode, 0x0A, tr("Gimbal tilt down 90"));
    _yawSpeed   = 0;
    _pitchSpeed = -99;
    _sendGimbalSpeed(false);
    _gimbalTimer.start();
    _downTimer.start();
    emit notice(tr("Tilting down 90°"));
}

// ---------------------------------------------------------------------------- Status

void SkydroidGimbalCamera::_pollStatus()
{
    // Recording status query doubles as a heartbeat: any valid reply marks the camera connected
    _send(kAddressImage, false, kIdRecord, 0x00);
    if (_pollCount++ % 5 == 0) {
        _send(kAddressImage, false, kIdSdCard, 0x00);
    }

    if (connected() && (!_lastReceive.isValid() || _lastReceive.elapsed() > kConnectionTimeoutMs)) {
        _setConnected(false);
    }
}

void SkydroidGimbalCamera::_readBytes()
{
    while (_socket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(static_cast<int>(_socket->pendingDatagramSize()));
        _socket->readDatagram(datagram.data(), datagram.size());
        _rxBuffer.append(datagram);
    }

    // Frame: "#TP" or "#tp" src dst len rw id(3) data(len) crc(2)
    static const int kHeaderLength = 10;
    while (true) {
        int start = -1;
        for (int i = _rxBuffer.indexOf('#'); i >= 0 && i + 2 < _rxBuffer.size(); i = _rxBuffer.indexOf('#', i + 1)) {
            if (_rxBuffer.mid(i + 1, 2).toUpper() == "TP") {
                start = i;
                break;
            }
        }
        if (start < 0) {
            // Keep a '#' at the very end: the rest of its header may come in the next datagram
            const int lastHash = _rxBuffer.lastIndexOf('#');
            _rxBuffer = (lastHash >= 0 && lastHash >= _rxBuffer.size() - 2) ? _rxBuffer.mid(lastHash) : QByteArray();
            return;
        }
        _rxBuffer.remove(0, start);
        if (_rxBuffer.size() < kHeaderLength) {
            return;
        }

        bool ok = false;
        const int dataLength = QByteArray(1, _rxBuffer.at(5)).toInt(&ok, 16);
        if (!ok) {
            _rxBuffer.remove(0, 3);
            continue;
        }
        const int frameLength = kHeaderLength + dataLength + 2;
        if (_rxBuffer.size() < frameLength) {
            return;
        }

        quint8 checksum = 0;
        for (int i = 0; i < frameLength - 2; i++) {
            checksum += static_cast<quint8>(_rxBuffer.at(i));
        }
        const QByteArray expected = QByteArray(1, hexChar(checksum >> 4)) + hexChar(checksum);
        if (_rxBuffer.mid(frameLength - 2, 2).toUpper() == expected) {
            _lastReceive.restart();
            _setConnected(true);
            _handleFrame(_rxBuffer.mid(7, 3), _rxBuffer.mid(kHeaderLength, dataLength));
        }
        _rxBuffer.remove(0, frameLength);
    }
}

void SkydroidGimbalCamera::_handleFrame(const QByteArray& id, const QByteArray& data)
{
    if (id == kIdRecord) {
        // The 2 s status poll is answered with REC, so it is not logged
        // '1' in either of the first two characters means recording (ArduPilot's Topotek driver)
        if (data.size() >= 2) {
            const bool recordingNow = data.at(0) == '1' || data.at(1) == '1';
            if (recordingNow) {
                _recordRequested = false;
            }
            _setRecording(recordingNow);
        }
        return;
    }

    if (id == kIdSdCard) {
        if (data.left(4).toUpper() == "NNNN") {
            _setSdCardStatus(QStringLiteral("none"));
            return;
        }
        bool ok = false;
        const int remaining = data.toInt(&ok, 16);
        _setSdCardStatus(ok && remaining == 0 ? QStringLiteral("full") : QStringLiteral("ok"));
        return;
    }

    _logCommand(QStringLiteral("RX %1 %2").arg(QString::fromLatin1(id), QString::fromLatin1(data)));

    if (id == kIdRange) {
        if (data.toUpper().startsWith("ERR")) {
            _setDistanceText(tr("no echo"));
            return;
        }
        bool ok = false;
        const double metres = data.trimmed().toDouble(&ok);
        if (ok && data.size() > 2) {
            _setDistanceText(tr("%1 m").arg(metres, 0, 'f', 1));
        }
    }
}
