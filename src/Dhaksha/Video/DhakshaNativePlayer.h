#pragma once

#include <QtCore/QElapsedTimer>
#include <QtCore/QLoggingCategory>
#include <QtCore/QMutex>
#include <QtCore/QObject>
#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <atomic>
#include <functional>
#include <memory>
#include <thread>

Q_DECLARE_LOGGING_CATEGORY(DhakshaNativeVideoLog)

struct ANativeWindow;
struct AMediaCodec;
typedef struct _GstElement GstElement;
typedef struct _GstBuffer GstBuffer;
typedef struct _GstPad GstPad;

/// Android hardware decoder drawing straight onto a GPU surface. Thread-safe: frames are fed from
/// the GStreamer streaming thread, the surface is attached from the Qt render thread, and a
/// thread of its own releases each decoded frame to the surface the moment it is ready.
class DhakshaDecoderCore
{
public:
    DhakshaDecoderCore() = default;
    ~DhakshaDecoderCore();

    DhakshaDecoderCore(const DhakshaDecoderCore&) = delete;
    DhakshaDecoderCore& operator=(const DhakshaDecoderCore&) = delete;

    /// The surface decoded frames are drawn on; nullptr detaches it and stops the decoder
    void setOutputWindow(ANativeWindow* window);

    /// One complete H.264/H.265 access unit in byte-stream format
    void queueAccessUnit(const char* mime, int width, int height, const uint8_t* data, size_t size, bool keyFrame);

    /// Drops the decoder so the next key frame starts a fresh one (new stream or stream error)
    void reset();

    /// Called from the decoder thread after each frame is drawn (cleared with nullptr)
    void setFrameCallback(std::function<void()> callback);

    quint64 framesRendered() const { return _framesRendered.load(std::memory_order_acquire); }

    QSize videoSize() const;
    QString decoderName() const;

private:
    bool _openLocked(const char* mime, int width, int height);
    void _closeLocked();
    void _outputLoop(AMediaCodec* codec);

    mutable QMutex _mutex;
    // Separate from _mutex: the output thread updates the size while _closeLocked() waits for it to finish
    mutable QMutex _sizeMutex;
    ANativeWindow* _window = nullptr;
    AMediaCodec* _codec = nullptr;
    QString _mime;
    QString _decoderName;
    QSize _videoSize;
    bool _needKeyFrame = true;
    std::thread _outputThread;
    std::atomic<bool> _outputRunning{false};
    std::atomic<quint64> _framesRendered{0};

    QMutex _callbackMutex;
    std::function<void()> _frameCallback;
};

/// Low-delay camera video for Android, played the way dedicated camera apps play it. GStreamer only
/// receives the RTSP stream and splits it into frames; DhakshaDecoderCore decodes each frame with
/// the hardware decoder straight onto a GPU surface that DhakshaVideoItem shows as soon as it is
/// decoded. No frame waits for a playout time and none is copied through the CPU.
class DhakshaNativePlayer : public QObject
{
    Q_OBJECT

public:
    explicit DhakshaNativePlayer(QObject* parent = nullptr);
    ~DhakshaNativePlayer() override;

    /// True on Android, where the hardware decoder can draw on a surface
    static bool supported();

    std::shared_ptr<DhakshaDecoderCore> decoder() const { return _decoder; }

    void start(const QString& url, int latencyMs, bool tcpOnly);
    void stop();

    bool running() const { return _pipeline != nullptr; }

private:
    void _createPipeline();
    void _destroyPipeline();
    void _pollBus();
    void _restartLater();

    static void _onPadAdded(GstElement* source, GstPad* pad, void* userData);
    static void _onHandoff(GstElement* sink, GstBuffer* buffer, GstPad* pad, void* userData);

    std::shared_ptr<DhakshaDecoderCore> _decoder = std::make_shared<DhakshaDecoderCore>();
    GstElement* _pipeline = nullptr;
    QString _url;
    int _latencyMs = 0;
    bool _tcpOnly = false;
    bool _wanted = false;
    QTimer _busTimer;
    QTimer _restartTimer;
    QElapsedTimer _sinceStart;
    std::atomic<qint64> _lastFrameMs{0};
};
