/****************************************************************************
 *
 * DhakshaGroundControl: owns the single active gimbal camera.
 *
 ****************************************************************************/

#include "DhakshaCameraManager.h"
#include "SiyiGimbalCamera.h"
#include "SkydroidGimbalCamera.h"

#ifndef DHAKSHA_CAMERA_NO_QGC
#include "Fact.h"
#include "SettingsManager.h"
#include "VideoManager.h"
#include "VideoSettings.h"
#endif

#include <QJSEngine>
#include <QSettings>

namespace {
    const char* kSettingsGroup  = "DhakshaCamera";
    const char* kConfiguredKey  = "configured";     // set once the operator applied a camera
    const char* kVendorKey      = "vendor";
    const char* kIpKey          = "ip";
    const char* kRtspKey        = "rtspUrl";
    const char* kNightRtspKey   = "nightRtspUrl";
    const char* kVideoModeKey   = "videoMode";
    // Read by GstSourceFactory when it opens the RTSP stream: TCP only (VideoReliable), keep late packets (VideoLowDelay)
    const char* kRtspTcpKey     = "DhakshaCamera/rtspTcp";
    const char* kRtspKeepLateKey = "DhakshaCamera/rtspKeepLate";
    // Day address saved by builds before the Skydroid's real one (port 554 stream=1) was known
    const char* kOldSkydroidRtsp = "rtsp://192.168.144.108:554/stream=0";
    const char* kGimbalSpeedKey = "DhakshaCamera/gimbalMaxSpeed";
    const char* kThermalPaletteKey = "DhakshaCamera/thermalPalette";
    // Renamed from "pipCamera" so every install starts with the map in the box: a second camera stream
    // shares the radio link and the decoder with the main one, so it only plays when the pilot asks
    const char* kPipCameraKey   = "pipCameraOn";

    QString vendorKey(int vendor)
    {
        return QStringLiteral("vendor%1").arg(vendor);
    }
}

DhakshaCameraManager* DhakshaCameraManager::instance()
{
    static DhakshaCameraManager* manager = new DhakshaCameraManager();
    return manager;
}

DhakshaCameraManager* DhakshaCameraManager::create(QQmlEngine* qmlEngine, QJSEngine* jsEngine)
{
    Q_UNUSED(qmlEngine);
    Q_UNUSED(jsEngine);
    DhakshaCameraManager* manager = instance();
    QJSEngine::setObjectOwnership(manager, QJSEngine::CppOwnership);
    return manager;
}

DhakshaCameraManager::DhakshaCameraManager(QObject* parent)
    : QObject(parent)
{
    QSettings settings;
    settings.beginGroup(kSettingsGroup);
    // The fleet flies the Skydroid camera, so a fresh install (or one whose settings were wiped by
    // reinstalling) starts with it; a vendor the operator applied is used as saved
    _vendor = VendorSkydroid;
    if (settings.value(kConfiguredKey, false).toBool()) {
        // Only Skydroid and the ZR10 remain; a camera type an earlier build saved (ViewPro, ZR10 over MAVLink) becomes Skydroid
        const int saved = settings.value(kVendorKey, VendorSkydroid).toInt();
        _vendor = saved == VendorZr10 ? VendorZr10 : VendorSkydroid;
    }
    _pipCamera = settings.value(kPipCameraKey, false).toBool();
    settings.endGroup();
    _loadVendorSettings();
}

QStringList DhakshaCameraManager::vendorNames() const
{
    return { tr("None"), QStringLiteral("Skydroid"), QStringLiteral("ZR10") };
}

QString DhakshaCameraManager::defaultCameraIp(int vendor)
{
    switch (vendor) {
    case VendorSkydroid:    return QStringLiteral("192.168.144.108");
    case VendorZr10:        return QStringLiteral("192.168.144.25");
    default:                return QString();
    }
}

QString DhakshaCameraManager::defaultRtspUrl(int vendor)
{
    switch (vendor) {
    case VendorSkydroid:    return QStringLiteral("rtsp://192.168.144.108:554/stream=1");
    case VendorZr10:        return QStringLiteral("rtsp://192.168.144.25:8554/main.264");
    default:                return QString();
    }
}

QString DhakshaCameraManager::defaultNightRtspUrl(int vendor)
{
    switch (vendor) {
    case VendorSkydroid:    return QStringLiteral("rtsp://192.168.144.108:555/stream=2");
    default:                return QString();
    }
}

void DhakshaCameraManager::_loadVendorSettings()
{
    QSettings settings;
    settings.beginGroup(kSettingsGroup);
    settings.beginGroup(vendorKey(_vendor));
    setCameraIp(settings.value(kIpKey, defaultCameraIp(_vendor)).toString());
    QString rtspUrl = settings.value(kRtspKey, defaultRtspUrl(_vendor)).toString();
    if (_vendor == VendorSkydroid && rtspUrl == QLatin1String(kOldSkydroidRtsp)) {
        rtspUrl = defaultRtspUrl(_vendor);
    }
    setRtspUrl(rtspUrl);
    setNightRtspUrl(settings.value(kNightRtspKey, defaultNightRtspUrl(_vendor)).toString());
    setVideoMode(settings.value(kVideoModeKey, defaultVideoMode(_vendor)).toInt());
}

void DhakshaCameraManager::_saveSettings()
{
    QSettings settings;
    settings.beginGroup(kSettingsGroup);
    settings.setValue(kConfiguredKey, true);
    settings.setValue(kVendorKey, _vendor);
    settings.beginGroup(vendorKey(_vendor));
    settings.setValue(kIpKey, _cameraIp);
    settings.setValue(kRtspKey, _rtspUrl);
    settings.setValue(kNightRtspKey, _nightRtspUrl);
    settings.setValue(kVideoModeKey, _videoMode);
}

void DhakshaCameraManager::setVendor(int vendor)
{
    vendor = qBound(0, vendor, VendorCount - 1);
    if (vendor != _vendor) {
        _vendor = vendor;
        emit vendorChanged();
        emit pipAvailableChanged();
        _loadVendorSettings();
    }
}

void DhakshaCameraManager::setCameraIp(const QString& cameraIp)
{
    const QString trimmed = cameraIp.trimmed();
    if (trimmed != _cameraIp) {
        _cameraIp = trimmed;
        emit cameraIpChanged();
    }
}

void DhakshaCameraManager::setRtspUrl(const QString& rtspUrl)
{
    const QString trimmed = rtspUrl.trimmed();
    if (trimmed != _rtspUrl) {
        _rtspUrl = trimmed;
        emit rtspUrlChanged();
        emit pipAvailableChanged();
    }
}

void DhakshaCameraManager::setNightRtspUrl(const QString& rtspUrl)
{
    const QString trimmed = rtspUrl.trimmed();
    if (trimmed != _nightRtspUrl) {
        _nightRtspUrl = trimmed;
        emit nightRtspUrlChanged();
        emit pipAvailableChanged();
    }
}

int DhakshaCameraManager::skydroidGimbalStyle() const
{
    return QSettings().value(SkydroidGimbalCamera::kGimbalStyleKey, SkydroidGimbalCamera::kDefaultGimbalStyle).toInt();
}

void DhakshaCameraManager::setSkydroidGimbalStyle(int style)
{
    if (style != skydroidGimbalStyle()) {
        QSettings().setValue(SkydroidGimbalCamera::kGimbalStyleKey, style);
        emit skydroidGimbalStyleChanged();
    }
}

int DhakshaCameraManager::defaultVideoMode(int vendor)
{
    // The ZR10 over the SIYI link froze with QGC's 17 ms buffer; Skydroid plays well with it
    return vendor == VendorZr10 ? VideoSmooth : VideoLowDelay;
}

void DhakshaCameraManager::setVideoMode(int mode)
{
    mode = qBound(static_cast<int>(VideoLowDelay), mode, static_cast<int>(VideoReliable));
    if (mode != _videoMode) {
        _videoMode = mode;
        emit videoModeChanged();
    }
}

int DhakshaCameraManager::gimbalMaxSpeed() const
{
    return qBound(kMinGimbalSpeed, QSettings().value(kGimbalSpeedKey, kMaxGimbalSpeed).toInt(), kMaxGimbalSpeed);
}

void DhakshaCameraManager::setGimbalMaxSpeed(int degreesPerSecond)
{
    degreesPerSecond = qBound(kMinGimbalSpeed, degreesPerSecond, kMaxGimbalSpeed);
    if (degreesPerSecond != gimbalMaxSpeed()) {
        QSettings().setValue(kGimbalSpeedKey, degreesPerSecond);
        emit gimbalMaxSpeedChanged();
    }
}

QStringList DhakshaCameraManager::thermalPaletteNames()
{
    // In the order of the camera's palette numbers (Topotek IMG command 00..07)
    return { tr("White hot"), tr("Black hot"), tr("Ironbow"), tr("Rainbow"),
             tr("Glowbow"), tr("Autumn"), tr("Winter"), tr("Hot metal") };
}

int DhakshaCameraManager::thermalPalette() const
{
    const int palette = QSettings().value(kThermalPaletteKey, 0).toInt();
    return (palette >= 0 && palette < thermalPaletteNames().size()) ? palette : 0;
}

void DhakshaCameraManager::setThermalPalette(int palette)
{
    if (palette < 0 || palette >= thermalPaletteNames().size()) {
        return;
    }
    QSettings().setValue(kThermalPaletteKey, palette);
    emit thermalPaletteChanged();
    // Sent even when unchanged, so choosing it again fixes a camera that was changed elsewhere
    if (_camera) {
        _camera->setThermalPalette(palette);
    }
}

void DhakshaCameraManager::restoreVendorDefaults()
{
    setCameraIp(defaultCameraIp(_vendor));
    setRtspUrl(defaultRtspUrl(_vendor));
    setNightRtspUrl(defaultNightRtspUrl(_vendor));
}

void DhakshaCameraManager::toggleNightView()
{
    if (_vendor == VendorNone || (!_nightView && _nightRtspUrl.isEmpty())) {
        return;
    }
    _nightView = !_nightView;
    emit nightViewChanged();
    _showVideo(_nightView ? _nightRtspUrl : _rtspUrl);
    _updatePipVideo();
}

bool DhakshaCameraManager::pipAvailable() const
{
    return _vendor != VendorNone && !_nightRtspUrl.isEmpty() && !_rtspUrl.isEmpty();
}

void DhakshaCameraManager::setPipCamera(bool camera)
{
    if (camera != _pipCamera) {
        _pipCamera = camera;
        QSettings settings;
        settings.beginGroup(kSettingsGroup);
        settings.setValue(kPipCameraKey, camera);
        emit pipCameraChanged();
        _updatePipVideo();
    }
}

void DhakshaCameraManager::setPipBoxShown(bool shown)
{
    if (shown != _pipBoxShown) {
        _pipBoxShown = shown;
        emit pipBoxShownChanged();
        _updatePipVideo();
    }
}

void DhakshaCameraManager::_updatePipVideo()
{
    // The box shows whichever camera is not on the main screen. The second stream stops with the map in
    // the box or the box hidden, so the radio and the decoder only carry it while it is on screen
    const QString url = _pipCamera && _pipBoxShown && pipAvailable() ? (_nightView ? _rtspUrl : _nightRtspUrl) : QString();
#ifndef DHAKSHA_CAMERA_NO_QGC
    VideoManager::instance()->setSecondaryRtspUrl(url);
#else
    Q_UNUSED(url)
#endif
}

void DhakshaCameraManager::apply()
{
    _saveSettings();
    start();
}

void DhakshaCameraManager::start()
{
    _createCamera();
    _applyVideoMode();
    _startVideoStream();
    _updatePipVideo();
}

void DhakshaCameraManager::_createCamera()
{
    if (_camera) {
        _camera->deleteLater();
        _camera = nullptr;
    }

    switch (_vendor) {
    case VendorSkydroid:
        _camera = new SkydroidGimbalCamera(_cameraIp, _rtspUrl, this);
        // The saved night palette, each time the camera (re)connects, e.g. after it was powered off
        connect(_camera, &DhakshaGimbalCamera::connectedChanged, this, [this, camera = _camera]() {
            if (camera == _camera && camera->connected()) {
                camera->setThermalPalette(thermalPalette());
            }
        });
        break;
    case VendorZr10:
        _camera = new SiyiGimbalCamera(_cameraIp, _rtspUrl, this);
        break;
    default:
        break;
    }
    emit cameraChanged();
}

void DhakshaCameraManager::_startVideoStream()
{
    if (_nightView) {
        _nightView = false;
        emit nightViewChanged();
    }
    if (_vendor != VendorNone) {
        _showVideo(_rtspUrl);
    }
}

void DhakshaCameraManager::_showVideo(const QString& rtspUrl)
{
#ifndef DHAKSHA_CAMERA_NO_QGC
    if (rtspUrl.isEmpty()) {
        return;
    }
    VideoSettings* videoSettings = SettingsManager::instance()->videoSettings();
    videoSettings->rtspUrl()->setRawValue(rtspUrl);
    videoSettings->videoSource()->setRawValue(VideoSettings::videoSourceRTSP);
#else
    Q_UNUSED(rtspUrl)
#endif
}

void DhakshaCameraManager::_applyVideoMode()
{
    // QGC restarts the stream itself when these settings change
    const bool tcp = _videoMode == VideoReliable;
    QSettings settings;
    settings.setValue(kRtspTcpKey, tcp);
    // Low delay shows late video instead of dropping it, as in 4.2: a dropped packet breaks the picture
    // (smears, blocks) until the camera's next key frame
    settings.setValue(kRtspKeepLateKey, _videoMode == VideoLowDelay);
#ifndef DHAKSHA_CAMERA_NO_QGC
    VideoSettings* videoSettings = SettingsManager::instance()->videoSettings();
    // Low delay holds packets 17 ms at most, as the 4.2 build did; otherwise it kept whatever buffer an earlier mode set
    const uint latencyMs = _videoMode == VideoSmooth ? 150 : _videoMode == VideoReliable ? 100 : 17;
    videoSettings->lowLatencyMode()->setRawValue(_videoMode == VideoLowDelay);
    videoSettings->rtpJitterLatencyMs()->setRawValue(latencyMs);
#endif
}
