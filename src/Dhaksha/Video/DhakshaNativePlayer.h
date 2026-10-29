#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QElapsedTimer>
#include <QtCore/QLoggingCategory>
#include <QtCore/QObject>
#include <QtCore/QSize>
#include <QtCore/QString>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "DhakshaRtspClient.h"

Q_DECLARE_LOGGING_CATEGORY(DhakshaNativeVideoLog)

struct ANativeWindow;
struct AMediaCodec;
struct AMediaFormat;
struct AMediaCodecBufferInfo;

/// Android hardware decoder (NDK MediaCodec in asynchronous mode) drawing straight onto a GPU surface.
///
/// Frames come from the network thread and never wait for the decoder: they go into a short queue that
/// skips ahead to a key frame when the decoder falls behind. Every codec call is made by one worker thread:
/// the codec's callbacks and the network thread only note what is ready and wake it. MediaCodec delivers
/// its callbacks on the looper that also runs its own calls, so a codec call made while holding a lock a
/// callback needs, or from inside a callback, hangs the decoder and everything that waits on it. Each
/// decoded frame is released to the surface the moment the worker sees it. A decoder that reports an error,
/// or gives no picture for 2 s while frames arrive, is replaced; the camera connection is kept.
class DhakshaDecoderCore
{
public:
    DhakshaDecoderCore();
    ~DhakshaDecoderCore();

    DhakshaDecoderCore(const DhakshaDecoderCore&) = delete;
    DhakshaDecoderCore& operator=(const DhakshaDecoderCore&) = delete;

    /// The surface decoded frames are drawn on; nullptr detaches it and stops the decoder
    void setOutputWindow(ANativeWindow* window);

    /// A new camera session: decoding restarts at its next key frame, with a new decoder if the format changed
    void startStream(const DhakshaVideoFormat& format);

    /// Stops decoding until the next startStream()
    void stopStream();

    /// One complete H.264/H.265 access unit in byte-stream format
    void queueAccessUnit(QByteArray accessUnit, bool keyFrame);

    /// Called from the decoder's thread after each frame is released to the surface (cleared with nullptr)
    void setFrameCallback(std::function<void()> callback);

    quint64 framesRendered() const { return _framesRendered.load(std::memory_order_acquire); }

    /// Frames not decoded: dropped when the decoder fell behind, or while waiting for a key frame
    quint64 framesDropped() const { return _framesDropped.load(std::memory_order_acquire); }

    /// Presentation time of the newest frame released to the surface; the surface reports it per frame
    qint64 lastRenderedTimestampNs() const { return _lastRenderedNs.load(std::memory_order_acquire); }

    QSize videoSize() const;
    QString decoderName() const;

private:
    struct Frame
    {
        QByteArray data;
        bool keyFrame = false;
    };

    struct Output
    {
        int32_t index = 0;
        bool render = false;
        qint64 timestampNs = 0;
    };

    static void _onInputAvailable(AMediaCodec* codec, void* userData, int32_t index);
    static void _onOutputAvailable(AMediaCodec* codec, void* userData, int32_t index, AMediaCodecBufferInfo* info);
    static void _onFormatChanged(AMediaCodec* codec, void* userData, AMediaFormat* format);
    static void _onError(AMediaCodec* codec, void* userData, int error, int32_t actionCode, const char* detail);

    void _controlLoop();
    void _openCodec();
    void _closeCodec();
    void _serviceCodec();
    bool _workPendingLocked() const;
    bool _queueInput(AMediaCodec* codec, int32_t index, const QByteArray& data, qint64 ptsUs);
    void _trimQueueLocked();
    void _skipToKeyFrameLocked();

    // Held while a codec is used, opened or closed; taken before _mutex, never by the codec callbacks or the
    // network thread. _mutex is only held for bookkeeping, never across a codec call.
    std::mutex _lifecycleMutex;
    mutable std::mutex _mutex;
    std::condition_variable _wake;
    std::thread _controlThread;
    bool _quit = false;
    QElapsedTimer _clock;

    ANativeWindow* _window = nullptr;
    DhakshaVideoFormat _format;
    quint64 _formatGeneration = 0;
    quint64 _codecFormatGeneration = 0;
    AMediaCodec* _codec = nullptr;
    bool _codecError = false;
    qint64 _retryAtMs = 0;
    std::deque<Frame> _queue;
    std::deque<int32_t> _freeInputs;
    std::deque<Output> _pendingOutputs;
    bool _waitKeyFrame = true;
    bool _fedSinceOpen = false;
    qint64 _firstFedMs = 0;
    qint64 _lastInputMs = -1;
    qint64 _lastOutputMs = 0;
    qint64 _nextPtsUs = 0;
    QSize _videoSize;
    QString _decoderName;

    std::atomic<quint64> _framesRendered{0};
    std::atomic<quint64> _framesDropped{0};
    std::atomic<qint64> _lastRenderedNs{0};

    std::mutex _callbackMutex;
    std::function<void()> _frameCallback;
};

/// Low-delay camera video for Android, played the way the camera makers' apps play it: DhakshaRtspClient
/// receives the RTSP stream on its own thread and hands each frame to DhakshaDecoderCore, which decodes it
/// with the hardware decoder straight onto a GPU surface that DhakshaVideoItem shows. No GStreamer, no
/// jitter buffer, no CPU copy of the picture.
class DhakshaNativePlayer : public QObject
{
    Q_OBJECT

public:
    explicit DhakshaNativePlayer(QObject* parent = nullptr);
    ~DhakshaNativePlayer() override;

    /// True on Android, where the hardware decoder can draw on a surface
    static bool supported();

    std::shared_ptr<DhakshaDecoderCore> decoder() const { return _decoder; }

    void start(const QString& url, bool tcp);
    void stop();

    bool running() const { return _running; }

    bool connected() const { return _client.connected(); }

    QString codecName() const;
    QString transportName() const;

private:
    std::shared_ptr<DhakshaDecoderCore> _decoder = std::make_shared<DhakshaDecoderCore>();
    DhakshaRtspClient _client;
    bool _running = false;
};
