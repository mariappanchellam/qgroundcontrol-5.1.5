/****************************************************************************
 *
 * DhakshaGroundControl: vendor-neutral gimbal camera interface.
 *
 ****************************************************************************/

#include "DhakshaGimbalCamera.h"

#include <QTime>

DhakshaGimbalCamera::DhakshaGimbalCamera(const QString& vendorName, const QString& rtspUrl, QObject* parent)
    : QObject       (parent)
    , _vendorName   (vendorName)
    , _rtspUrl      (rtspUrl)
{
}

void DhakshaGimbalCamera::_setConnected(bool connected)
{
    if (connected != _connected) {
        _connected = connected;
        emit connectedChanged();
        _logCommand(connected ? tr("Camera connected") : tr("Camera not responding"));
    }
}

void DhakshaGimbalCamera::_setRecording(bool recording)
{
    if (recording != _recording) {
        _recording = recording;
        emit recordingChanged();
        _logCommand(recording ? tr("Recording ON") : tr("Recording OFF"));
    }
}

void DhakshaGimbalCamera::_logCommand(const QString& entry)
{
    _commandLog.prepend(QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz ")) + entry);
    while (_commandLog.size() > kMaxLogEntries) {
        _commandLog.removeLast();
    }
    emit commandLogChanged();
}

void DhakshaGimbalCamera::_setDistanceText(const QString& text)
{
    if (text != _distanceText) {
        _distanceText = text;
        emit distanceTextChanged();
    }
}

void DhakshaGimbalCamera::_setRangingActive(bool active)
{
    if (active != _rangingActive) {
        _rangingActive = active;
        emit rangingActiveChanged();
    }
}

void DhakshaGimbalCamera::_setSdCardStatus(const QString& status)
{
    if (status != _sdCardStatus) {
        _sdCardStatus = status;
        emit sdCardStatusChanged();
        if (!sdCardProblem().isEmpty()) {
            _logCommand(sdCardProblem());
        }
    }
}

void DhakshaGimbalCamera::_setScreenZoom(qreal zoom)
{
    if (!qFuzzyCompare(zoom, _screenZoom)) {
        _screenZoom = zoom;
        emit screenZoomChanged();
    }
}

QString DhakshaGimbalCamera::sdCardProblem() const
{
    if (_sdCardStatus == QLatin1String("none")) {
        return tr("No SD card in the camera");
    }
    if (_sdCardStatus == QLatin1String("full")) {
        return tr("Camera SD card is full");
    }
    return QString();
}

void DhakshaGimbalCamera::clearCommandLog()
{
    _commandLog.clear();
    emit commandLogChanged();
}
