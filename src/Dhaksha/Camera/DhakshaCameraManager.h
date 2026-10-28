/****************************************************************************
 *
 * DhakshaGroundControl: owns the single active gimbal camera.
 *
 * The operator picks the camera (Skydroid or SIYI ZR10) plus its IP and
 * RTSP URL once; the choice is stored when applied and, on every start-up,
 * the manager creates the matching DhakshaGimbalCamera and points QGC's video
 * stream at the camera's RTSP URL so video starts automatically. Until a
 * camera has been applied the vendor is None and QGC's own video settings are
 * left untouched.
 *
 ****************************************************************************/

#pragma once

#include "DhakshaGimbalCamera.h"

#include <QObject>
#include <QStringList>
#include <QtQmlIntegration/QtQmlIntegration>

class QQmlEngine;
class QJSEngine;

class DhakshaCameraManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    enum Vendor {
        VendorNone = 0,
        VendorSkydroid,
        VendorZr10,             ///< SIYI ZR10 on the RC's Ethernet, SIYI's own protocol
        VendorCount
    };
    Q_ENUM(Vendor)

    /// How the RTSP video is received
    enum VideoMode {
        VideoLowDelay = 0,      ///< QGC's Low Latency Mode with a 17 ms buffer; late packets are kept
        VideoSmooth,            ///< 150 ms jitter buffer that absorbs radio jitter; older video is dropped
        VideoReliable,          ///< TCP only, 100 ms buffer: nothing lost, a little more delay when the link is poor
    };
    Q_ENUM(VideoMode)

    Q_PROPERTY(QStringList          vendorNames READ vendorNames                    CONSTANT)
    Q_PROPERTY(int                  vendor      READ vendor     WRITE setVendor     NOTIFY vendorChanged)
    Q_PROPERTY(QString              cameraIp    READ cameraIp   WRITE setCameraIp   NOTIFY cameraIpChanged)
    Q_PROPERTY(QString              rtspUrl     READ rtspUrl    WRITE setRtspUrl    NOTIFY rtspUrlChanged)
    Q_PROPERTY(QString              nightRtspUrl READ nightRtspUrl WRITE setNightRtspUrl NOTIFY nightRtspUrlChanged)   ///< Thermal camera; empty when there is none
    Q_PROPERTY(bool                 nightView   READ nightView                      NOTIFY nightViewChanged)    ///< Night video on screen
    Q_PROPERTY(bool     pipAvailable    READ pipAvailable                       NOTIFY pipAvailableChanged)     ///< The camera has a second (night / day) stream for the PiP box
    Q_PROPERTY(bool     pipCamera       READ pipCamera      WRITE setPipCamera  NOTIFY pipCameraChanged)        ///< PiP box shows the other camera (true) or the map (false)
    Q_PROPERTY(bool     pipBoxShown     READ pipBoxShown    WRITE setPipBoxShown NOTIFY pipBoxShownChanged)     ///< The PiP box is on screen; the second stream only plays while it is
    Q_PROPERTY(int  skydroidGimbalStyle READ skydroidGimbalStyle WRITE setSkydroidGimbalStyle NOTIFY skydroidGimbalStyleChanged) ///< 0 YPR (C13), 1 GSY/GSP (C10); used from the next apply()
    Q_PROPERTY(DhakshaGimbalCamera* camera      READ camera                         NOTIFY cameraChanged)
    Q_PROPERTY(int  gimbalMaxSpeed  READ gimbalMaxSpeed WRITE setGimbalMaxSpeed NOTIFY gimbalMaxSpeedChanged)  ///< Degrees per second at full joystick, 10..60
    Q_PROPERTY(QStringList thermalPaletteNames READ thermalPaletteNames CONSTANT)
    Q_PROPERTY(int  thermalPalette  READ thermalPalette WRITE setThermalPalette NOTIFY thermalPaletteChanged)  ///< Night camera palette, saved; sent when chosen and when the camera connects
    Q_PROPERTY(int  videoMode       READ videoMode      WRITE setVideoMode      NOTIFY videoModeChanged)       ///< VideoMode, saved per camera; used from the next apply()
    Q_PROPERTY(bool nativeVideoSupported READ nativeVideoSupported CONSTANT)                                    ///< The fast player works on this platform (Android)
    Q_PROPERTY(bool nativeVideo     READ nativeVideo    WRITE setNativeVideo    NOTIFY nativeVideoChanged)     ///< Fast player chosen in the settings, saved
    Q_PROPERTY(bool nativeVideoActive READ nativeVideoActive                    NOTIFY nativeVideoChanged)     ///< The Fly View plays the main camera with the fast player
    Q_PROPERTY(QString mainVideoUrl READ mainVideoUrl                           NOTIFY mainVideoUrlChanged)    ///< Camera stream on the main screen (day or night)
    Q_PROPERTY(int  nativeLatencyMs READ nativeLatencyMs                        NOTIFY activeVideoModeChanged) ///< Fast player network buffer for the applied video mode
    Q_PROPERTY(bool nativeTcpOnly   READ nativeTcpOnly                          NOTIFY activeVideoModeChanged)
    Q_PROPERTY(QString nativeVideoStatus READ nativeVideoStatus WRITE setNativeVideoStatus NOTIFY nativeVideoStatusChanged) ///< What the fast player receives, set by the Fly View

    static DhakshaCameraManager* instance();
    static DhakshaCameraManager* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    QStringList             vendorNames () const;
    int                     vendor      () const { return _vendor; }
    QString                 cameraIp    () const { return _cameraIp; }
    QString                 rtspUrl     () const { return _rtspUrl; }
    QString                 nightRtspUrl() const { return _nightRtspUrl; }
    bool                    nightView   () const { return _nightView; }
    bool                    pipAvailable() const;
    bool                    pipCamera   () const { return _pipCamera; }
    void                    setPipCamera(bool camera);
    bool                    pipBoxShown () const { return _pipBoxShown; }
    void                    setPipBoxShown(bool shown);
    int                     skydroidGimbalStyle() const;
    void                    setSkydroidGimbalStyle(int style);
    int                     videoMode() const { return _videoMode; }
    void                    setVideoMode(int mode);
    static bool             nativeVideoSupported();
    bool                    nativeVideo () const { return _nativeVideo; }
    void                    setNativeVideo(bool native);
    bool                    nativeVideoActive() const;
    QString                 mainVideoUrl() const { return _mainVideoUrl; }
    int                     nativeLatencyMs() const;
    /// The fast player always receives over TCP, as the camera makers' apps do: a lost UDP packet smears the
    /// picture until the next key frame, which fast gimbal moves (large frames, bursts of packets) made frequent
    bool                    nativeTcpOnly() const { return true; }
    QString                 nativeVideoStatus() const { return _nativeVideoStatus; }
    void                    setNativeVideoStatus(const QString& status);
    static int              defaultVideoMode(int vendor);
    int                     gimbalMaxSpeed() const;
    void                    setGimbalMaxSpeed(int degreesPerSecond);
    static QStringList      thermalPaletteNames();
    int                     thermalPalette() const;
    void                    setThermalPalette(int palette);
    static constexpr int    kMinGimbalSpeed = 10;
    static constexpr int    kMaxGimbalSpeed = 60;
    DhakshaGimbalCamera*    camera      () const { return _camera; }

    /// Changing the vendor loads that vendor's saved IP / RTSP URL; call apply() to use them.
    void setVendor      (int vendor);
    void setCameraIp    (const QString& cameraIp);
    void setRtspUrl     (const QString& rtspUrl);
    void setNightRtspUrl(const QString& rtspUrl);

    /// Switches the video on screen between the day and the night (thermal) stream; apply() goes back to day.
    Q_INVOKABLE void toggleNightView();

    /// Saves the settings, (re)creates the camera instance and starts its RTSP video stream.
    Q_INVOKABLE void apply();

    /// Start-up: same as apply() with the stored settings, without saving them.
    void start();

    /// Restores the selected vendor's default IP and RTSP URL (not saved until apply()).
    Q_INVOKABLE void restoreVendorDefaults();

    static QString defaultCameraIp  (int vendor);
    static QString defaultRtspUrl   (int vendor);
    static QString defaultNightRtspUrl(int vendor);

signals:
    void vendorChanged  ();
    void cameraIpChanged();
    void rtspUrlChanged ();
    void nightRtspUrlChanged();
    void nightViewChanged();
    void pipAvailableChanged();
    void pipCameraChanged();
    void pipBoxShownChanged();
    void skydroidGimbalStyleChanged();
    void gimbalMaxSpeedChanged();
    void thermalPaletteChanged();
    void videoModeChanged();
    void cameraChanged  ();
    void nativeVideoChanged();
    void mainVideoUrlChanged();
    void activeVideoModeChanged();
    void nativeVideoStatusChanged();
    /// apply(): the fast player reconnects to the camera
    void videoRestartRequested();

private:
    explicit DhakshaCameraManager(QObject* parent = nullptr);

    void _loadVendorSettings    ();
    void _saveSettings          ();
    void _createCamera          ();
    void _applyNativeVideo      ();
    void _startVideoStream      ();
    void _showVideo             (const QString& rtspUrl);
    void _applyVideoMode        ();
    void _updatePipVideo        ();

    int                     _vendor     = VendorNone;
    QString                 _cameraIp;
    QString                 _rtspUrl;
    QString                 _nightRtspUrl;
    int                     _videoMode  = VideoLowDelay;
    bool                    _nightView  = false;
    bool                    _pipCamera  = false;
    bool                    _pipBoxShown = false;
    bool                    _nativeVideo = true;
    QString                 _mainVideoUrl;
    int                     _activeVideoMode = VideoLowDelay;
    QString                 _nativeVideoStatus;
    DhakshaGimbalCamera*    _camera     = nullptr;
};
