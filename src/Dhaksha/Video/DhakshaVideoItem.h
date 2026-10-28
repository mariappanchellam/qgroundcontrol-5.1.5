#pragma once

#include <QtCore/QPointer>
#include <QtCore/QSize>
#include <QtCore/QTimer>
#include <QtQmlIntegration/QtQmlIntegration>
#include <QtQuick/QQuickItem>
#include <atomic>

class DhakshaNativePlayer;

/// DhakshaGroundControl's fast camera player for the Fly View. Plays an RTSP camera with
/// DhakshaNativePlayer and draws each decoded frame straight from the hardware decoder's GPU
/// surface, inside the Qt Quick scene so the map and controls stay on top of it.
/// Android only; elsewhere `supported` is false and the item draws nothing.
class DhakshaVideoItem : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(int latencyMs READ latencyMs WRITE setLatencyMs NOTIFY latencyMsChanged)  ///< Network buffer, 0 for none
    Q_PROPERTY(bool tcpOnly READ tcpOnly WRITE setTcpOnly NOTIFY tcpOnlyChanged)
    Q_PROPERTY(bool cropToFill READ cropToFill WRITE setCropToFill NOTIFY
                   cropToFillChanged)  ///< Fill the item, cropping the video, instead of fitting it
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)  ///< Frames are arriving
    Q_PROPERTY(QSize videoSize READ videoSize NOTIFY videoSizeChanged)
    Q_PROPERTY(QString decoderName READ decoderName NOTIFY videoSizeChanged)
    Q_PROPERTY(bool supported READ supported CONSTANT)

public:
    explicit DhakshaVideoItem(QQuickItem* parent = nullptr);
    ~DhakshaVideoItem() override;

    QString source() const { return _source; }

    int latencyMs() const { return _latencyMs; }

    bool tcpOnly() const { return _tcpOnly; }

    bool cropToFill() const { return _cropToFill; }

    bool playing() const { return _playing; }

    QSize videoSize() const { return _videoSize; }

    QString decoderName() const { return _decoderName; }

    bool supported() const;

    void setSource(const QString& source);
    void setLatencyMs(int latencyMs);
    void setTcpOnly(bool tcpOnly);
    void setCropToFill(bool cropToFill);

    /// Reconnects to the camera
    Q_INVOKABLE void restart();

signals:
    void sourceChanged();
    void latencyMsChanged();
    void tcpOnlyChanged();
    void cropToFillChanged();
    void playingChanged();
    void videoSizeChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;

private:
    void _scheduleRestart();
    void _updateStatus();

    DhakshaNativePlayer* _player = nullptr;
    QString _source;
    int _latencyMs = 0;
    bool _tcpOnly = false;
    bool _cropToFill = false;
    bool _playing = false;
    QSize _videoSize;
    QString _decoderName;
    quint64 _lastFrames = 0;
    QTimer _restartTimer;
    QTimer _statusTimer;
    std::atomic<bool> _updateQueued{false};
};
