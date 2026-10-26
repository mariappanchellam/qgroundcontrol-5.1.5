/****************************************************************************
 *
 * DhakshaGroundControl: Skydroid gimbal camera (C10, C13).
 *
 * Skydroid cameras speak the Topotek ASCII protocol over UDP (default
 * 192.168.144.108:5000). Fixed-length frames:
 *
 *   "#TP" <source> <address> <data length> <'w'|'r'> <3-char id> <2 hex data> <2 hex checksum>
 *
 * Variable-length frames start with "#tp" and carry up to 15 data characters
 * (gimbal rates "YPR", laser range replies "LRF").
 *
 * source 'U', address 'G' gimbal / 'D' system and image / 'M' lens, and the
 * checksum is the sum of all preceding bytes modulo 256, e.g. "#TPUG2wPTZ056F"
 * centres the gimbal.
 *
 ****************************************************************************/

#pragma once

#include "DhakshaGimbalCamera.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QTimer>

class QUdpSocket;

class SkydroidGimbalCamera : public DhakshaGimbalCamera
{
    Q_OBJECT

public:
    SkydroidGimbalCamera(const QString& cameraIp, const QString& rtspUrl, QObject* parent = nullptr);

    static constexpr quint16 kControlPort = 5000;

    bool controlSupported() const override { return true; }
    bool rangefinderSupported() const override { return true; }
    bool thermalPaletteSupported() const override { return true; }

    void takePhoto          () override;
    void startRecording     () override;
    void stopRecording      () override;
    void toggleDayNight     () override;
    void setPipMode         (int mode) override;
    void measureDistance    () override;
    void toggleRanging      () override;
    void setThermalPalette  (int palette) override;

    void zoomIn             () override;
    void zoomOut            () override;
    void zoomStop           () override;
    void focusIn            () override;
    void focusOut           () override;
    void focusStop          () override;
    void autoFocus          () override;

    void gimbalSpeed        (int yawPercent, int pitchPercent) override;
    void gimbalStop         () override;
    void gimbalCenter       () override;
    void gimbalLock         () override;
    void gimbalFollow       () override;
    void gimbalDown         () override;

    /// How gimbal speeds are sent: one YPR frame (ArduPilot's Topotek driver) or GSY + GSP.
    enum GimbalStyle { GimbalStyleYpr = 0, GimbalStyleGsy = 1 };
    /// The fleet's Skydroid C13 moves with GSY + GSP and ignores YPR (field test 2026-10-13)
    static constexpr int kDefaultGimbalStyle = GimbalStyleGsy;
    static constexpr const char* kGimbalStyleKey = "DhakshaCamera/skydroidGimbalStyle";

    /// Builds a fixed-length Topotek frame including checksum (exposed for tests).
    static QByteArray buildFrame(char address, bool write, const char* id, quint8 data);
    /// Builds a frame with the given ASCII data: "#TP" for exactly two characters, otherwise "#tp".
    static QByteArray buildFrame(char address, bool write, const char* id, const QByteArray& data);

private slots:
    void _readBytes         ();
    void _pollStatus        ();
    void _resendGimbalSpeed ();
    void _readRange         ();
    void _checkRecordingStarted();

private:
    /// Sends one frame; a non-empty description is added to the command log (polls and resends are not logged)
    void _send              (char address, bool write, const char* id, quint8 data, const QString& description = QString());
    void _sendFrame         (const QByteArray& frame, const QString& description);
    void _sendZoom          (quint8 value, bool log);
    void _stepDigitalZoom   ();
    void _sendRange         (bool write, quint8 value, const QString& description);
    void _sendGimbalSpeed   (bool log);
    void _handleFrame       (const QByteArray& id, const QByteArray& data);

    static int _percentToSpeed(int percent);

    QUdpSocket*     _socket         = nullptr;
    QHostAddress    _cameraAddress;
    QTimer          _pollTimer;
    QTimer          _gimbalTimer;
    QTimer          _rangeTimer;
    QTimer          _zoomTimer;
    int             _zoomDirection  = 0;    ///< 1 zooming in, -1 out, 0 not zooming
    int             _digitalZoomLevel = 1;     ///< total zoom shown to the operator
    int             _cameraZoomSent = 1;       ///< the part the camera does itself
    bool            _maxZoomShown   = false;
    QTimer          _downTimer;
    int             _rangeReads     = 0;
    int             _gimbalStyle    = kDefaultGimbalStyle;
    int             _pollCount      = 0;
    bool            _recordRequested = false;  ///< start sent, camera has not confirmed yet
    QElapsedTimer   _lastReceive;
    QByteArray      _rxBuffer;
    int             _yawSpeed       = 0;    ///< -99..99
    int             _pitchSpeed     = 0;    ///< -99..99

    static constexpr char kSource           = 'U';
    static constexpr char kAddressGimbal    = 'G';
    static constexpr char kAddressImage     = 'D';
    static constexpr char kAddressLens      = 'M';
    static constexpr char kAddressThermal   = 'E';  ///< Auxiliary equipment (thermal camera) on Topotek SMT gimbals

    static constexpr int kPollIntervalMs        = 2000;
    static constexpr int kGimbalResendMs        = 100;    ///< as ArduPilot; the camera stops between slower resends
    static constexpr int kRangeReadMs           = 1000;
    static constexpr int kZoomResendMs          = 400;    ///< also one digital zoom step
    static constexpr int kMaxCameraZoom         = 4;      ///< the C13 accepted DZM 01..04 in the field; 05 did nothing
    static constexpr int kMaxZoom               = 30;     ///< camera 4x, then the video is magnified on screen up to 7.5x
    static constexpr int kTiltDownMs            = 3000;   ///< full speed down this long reaches the lowest point
    static constexpr int kConnectionTimeoutMs   = 5000;
};
