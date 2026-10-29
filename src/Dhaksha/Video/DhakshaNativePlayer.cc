#include "DhakshaNativePlayer.h"

#include <QtCore/QMutexLocker>
#include <chrono>
#include <cstring>
#include <utility>
#include <vector>

#ifdef Q_OS_ANDROID
#define DHAKSHA_NATIVE_VIDEO 1
#include <android/native_window.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#endif

Q_LOGGING_CATEGORY(DhakshaNativeVideoLog, "Dhaksha.NativeVideo")

namespace {
// As in the camera makers' players: a short queue that drops its oldest frame rather than delay the picture
constexpr size_t kMaxQueuedFrames = 20;
// A decoder that gives no picture this long while frames arrive is replaced; the camera connection is kept
constexpr qint64 kNoOutputResetMs = 2000;
constexpr std::chrono::milliseconds kControlInterval{100};
// Frames are shown as soon as they are decoded, so the presentation time only counts frames
constexpr qint64 kPtsStepUs = 33333;
#ifdef DHAKSHA_NATIVE_VIDEO
constexpr qint64 kOpenRetryMs = 1000;
constexpr int32_t kMinInputBytes = 1024 * 1024;
constexpr int kFallbackWidth = 1920;
constexpr int kFallbackHeight = 1080;
#endif
}  // namespace

// ----------------------------------------------------------------------------------------------
// DhakshaDecoderCore
// ----------------------------------------------------------------------------------------------

DhakshaDecoderCore::DhakshaDecoderCore()
{
    _clock.start();
#ifdef DHAKSHA_NATIVE_VIDEO
    // Only Android has a decoder to open and close
    _controlThread = std::thread(&DhakshaDecoderCore::_controlLoop, this);
#endif
}

DhakshaDecoderCore::~DhakshaDecoderCore()
{
    {
        QMutexLocker locker(&_mutex);
        _quit = true;
        _wake.notify_all();
    }
    if (_controlThread.joinable()) {
        _controlThread.join();
    }
    QMutexLocker lifecycle(&_lifecycleMutex);
    _closeCodec();
#ifdef DHAKSHA_NATIVE_VIDEO
    if (_window) {
        ANativeWindow_release(_window);
        _window = nullptr;
    }
#endif
}

void DhakshaDecoderCore::setOutputWindow(ANativeWindow* window)
{
#ifdef DHAKSHA_NATIVE_VIDEO
    QMutexLocker lifecycle(&_lifecycleMutex);
    {
        QMutexLocker locker(&_mutex);
        if (window == _window) {
            return;
        }
    }
    // The decoder is bound to the surface it was configured with
    _closeCodec();
    QMutexLocker locker(&_mutex);
    if (_window) {
        ANativeWindow_release(_window);
    }
    _window = window;
    if (_window) {
        ANativeWindow_acquire(_window);
    }
    _wake.notify_all();
#else
    _window = window;
#endif
}

void DhakshaDecoderCore::startStream(const DhakshaVideoFormat& format)
{
    QMutexLocker locker(&_mutex);
    if (!(format == _format)) {
        _format = format;
        ++_formatGeneration;
    }
    // Frames of the new session refer to pictures the decoder has not seen until the next key frame
    _queue.clear();
    _waitKeyFrame = true;
    _wake.notify_all();
}

void DhakshaDecoderCore::stopStream()
{
    QMutexLocker locker(&_mutex);
    _format = DhakshaVideoFormat();
    ++_formatGeneration;
    _queue.clear();
    _waitKeyFrame = true;
    _wake.notify_all();
}

void DhakshaDecoderCore::setFrameCallback(std::function<void()> callback)
{
    QMutexLocker locker(&_callbackMutex);
    _frameCallback = std::move(callback);
}

QSize DhakshaDecoderCore::videoSize() const
{
    QMutexLocker locker(&_mutex);
    return _videoSize;
}

QString DhakshaDecoderCore::decoderName() const
{
    QMutexLocker locker(&_mutex);
    return _decoderName;
}

void DhakshaDecoderCore::queueAccessUnit(QByteArray accessUnit, bool keyFrame)
{
    QMutexLocker locker(&_mutex);
    if (!_format.valid() || accessUnit.isEmpty()) {
        return;
    }
    _lastInputMs = _clock.elapsed();
    if (_waitKeyFrame) {
        if (!keyFrame) {
            _framesDropped.fetch_add(1, std::memory_order_acq_rel);
            return;
        }
        _waitKeyFrame = false;
    }
    _queue.push_back({std::move(accessUnit), keyFrame});
    _trimQueueLocked();
    _wake.notify_all();
}

void DhakshaDecoderCore::_trimQueueLocked()
{
    if (_queue.size() > kMaxQueuedFrames) {
        // The decoder fell behind: skip ahead to the newest frames, starting at a key frame so the
        // picture never shows a frame decoded without the ones it depends on
        while (_queue.size() > kMaxQueuedFrames) {
            _queue.pop_front();
            _framesDropped.fetch_add(1, std::memory_order_acq_rel);
        }
        _skipToKeyFrameLocked();
    }
    // A fresh decoder must start at a key frame
    if (!_fedSinceOpen) {
        _skipToKeyFrameLocked();
    }
}

void DhakshaDecoderCore::_skipToKeyFrameLocked()
{
    while (!_queue.empty() && !_queue.front().keyFrame) {
        _queue.pop_front();
        _framesDropped.fetch_add(1, std::memory_order_acq_rel);
    }
    if (_queue.empty()) {
        _waitKeyFrame = true;
    }
}

bool DhakshaDecoderCore::_workPendingLocked() const
{
    if (_quit || _codecError || !_pendingOutputs.empty()) {
        return true;
    }
    if (_codec) {
        return (_codecFormatGeneration != _formatGeneration) || (!_freeInputs.empty() && !_queue.empty());
    }
    return _window && _format.valid() && !_queue.empty() && (_clock.elapsed() >= _retryAtMs);
}

bool DhakshaDecoderCore::_queueInput(AMediaCodec* codec, int32_t index, const QByteArray& data, qint64 ptsUs)
{
#ifdef DHAKSHA_NATIVE_VIDEO
    const size_t size = static_cast<size_t>(data.size());
    size_t capacity = 0;
    uint8_t* input = AMediaCodec_getInputBuffer(codec, static_cast<size_t>(index), &capacity);
    if (!input || (capacity < size)) {
        qCWarning(DhakshaNativeVideoLog) << "Frame of" << size << "bytes does not fit the decoder input of" << capacity;
        (void) AMediaCodec_queueInputBuffer(codec, static_cast<size_t>(index), 0, 0, 0, 0);
        return false;
    }
    memcpy(input, data.constData(), size);
    return AMediaCodec_queueInputBuffer(codec, static_cast<size_t>(index), 0, size, static_cast<uint64_t>(ptsUs), 0) ==
           AMEDIA_OK;
#else
    Q_UNUSED(codec)
    Q_UNUSED(index)
    Q_UNUSED(data)
    Q_UNUSED(ptsUs)
    return false;
#endif
}

void DhakshaDecoderCore::_serviceCodec()
{
#ifdef DHAKSHA_NATIVE_VIDEO
    // _lifecycleMutex keeps the codec alive while it is used here; _mutex is only held to collect the work
    std::lock_guard<std::mutex> lifecycle(_lifecycleMutex);
    AMediaCodec* codec = nullptr;
    std::deque<Output> outputs;
    std::vector<std::pair<int32_t, Frame>> inputs;
    qint64 ptsUs = 0;
    {
        std::lock_guard<std::mutex> locker(_mutex);
        codec = _codec;
        if (!codec) {
            return;
        }
        outputs.swap(_pendingOutputs);
        while (!_freeInputs.empty() && !_queue.empty()) {
            inputs.emplace_back(_freeInputs.front(), std::move(_queue.front()));
            _freeInputs.pop_front();
            _queue.pop_front();
        }
        ptsUs = _nextPtsUs;
    }

    // Older decoded frames first, then the newest: each is drawn the moment it is released
    quint64 rendered = 0;
    for (const Output& output : outputs) {
        (void) AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(output.index), output.render);
        if (output.render) {
            ++rendered;
            _lastRenderedNs.store(output.timestampNs, std::memory_order_release);
            _framesRendered.fetch_add(1, std::memory_order_acq_rel);
        }
    }

    quint64 fed = 0;
    bool lost = false;
    for (const auto& [index, frame] : inputs) {
        if (lost && !frame.keyFrame) {
            // After a lost frame, later frames refer to it: give their input buffers back empty
            (void) AMediaCodec_queueInputBuffer(codec, static_cast<size_t>(index), 0, 0, 0, 0);
            _framesDropped.fetch_add(1, std::memory_order_acq_rel);
            continue;
        }
        if (_queueInput(codec, index, frame.data, ptsUs)) {
            lost = false;
            ++fed;
            ptsUs += kPtsStepUs;
        } else {
            lost = true;
            _framesDropped.fetch_add(1, std::memory_order_acq_rel);
        }
    }

    {
        std::lock_guard<std::mutex> locker(_mutex);
        if (codec == _codec) {
            const qint64 now = _clock.elapsed();
            _nextPtsUs = ptsUs;
            if (rendered > 0) {
                _lastOutputMs = now;
            }
            if ((fed > 0) && !_fedSinceOpen) {
                _fedSinceOpen = true;
                _firstFedMs = now;
            }
            if (lost) {
                _skipToKeyFrameLocked();
            }
        }
    }

    if (rendered > 0) {
        std::lock_guard<std::mutex> locker(_callbackMutex);
        if (_frameCallback) {
            _frameCallback();
        }
    }
#endif
}

void DhakshaDecoderCore::_controlLoop()
{
    while (true) {
        enum class Action
        {
            Service,
            Open,
            Close,
        };
        Action action = Action::Service;
        {
            std::unique_lock<std::mutex> locker(_mutex);
            (void) _wake.wait_for(locker, kControlInterval, [this] { return _workPendingLocked(); });
            if (_quit) {
                return;
            }
            const qint64 now = _clock.elapsed();
            if (_codec) {
                const bool stalled = _fedSinceOpen && ((now - qMax(_lastOutputMs, _firstFedMs)) > kNoOutputResetMs) &&
                                     ((now - _lastInputMs) < kNoOutputResetMs);
                if (_codecError || stalled) {
                    qCWarning(DhakshaNativeVideoLog)
                        << "Restarting the decoder:" << (_codecError ? "decoder error" : "no picture for 2 s");
                }
                if (_codecError || stalled || (_codecFormatGeneration != _formatGeneration)) {
                    action = Action::Close;
                }
            } else if (_window && _format.valid() && !_queue.empty() && (now >= _retryAtMs)) {
                action = Action::Open;
            }
        }
        if (action == Action::Open) {
            std::lock_guard<std::mutex> lifecycle(_lifecycleMutex);
            _openCodec();
        } else if (action == Action::Close) {
            std::lock_guard<std::mutex> lifecycle(_lifecycleMutex);
            _closeCodec();
        } else {
            _serviceCodec();
        }
    }
}

void DhakshaDecoderCore::_openCodec()
{
#ifdef DHAKSHA_NATIVE_VIDEO
    QMutexLocker locker(&_mutex);
    if (_codec || !_window || !_format.valid()) {
        return;
    }
    ANativeWindow* window = _window;
    ANativeWindow_acquire(window);
    const DhakshaVideoFormat format = _format;
    const quint64 generation = _formatGeneration;
    locker.unlock();

    AMediaCodec* codec = AMediaCodec_createDecoderByType(format.mime());
    if (!codec) {
        qCWarning(DhakshaNativeVideoLog) << "No hardware decoder for" << format.mime();
        locker.relock();
        _retryAtMs = _clock.elapsed() + kOpenRetryMs;
        locker.unlock();
        ANativeWindow_release(window);
        return;
    }

    const int width = (format.width > 0) ? format.width : kFallbackWidth;
    const int height = (format.height > 0) ? format.height : kFallbackHeight;
    AMediaFormat* mediaFormat = AMediaFormat_new();
    AMediaFormat_setString(mediaFormat, AMEDIAFORMAT_KEY_MIME, format.mime());
    AMediaFormat_setInt32(mediaFormat, AMEDIAFORMAT_KEY_WIDTH, width);
    AMediaFormat_setInt32(mediaFormat, AMEDIAFORMAT_KEY_HEIGHT, height);
    // The camera's own parameter sets, so the decoder is set up for the stream before its first frame
    AMediaFormat_setBuffer(mediaFormat, "csd-0", format.csd0.constData(), static_cast<size_t>(format.csd0.size()));
    if (!format.csd1.isEmpty()) {
        AMediaFormat_setBuffer(mediaFormat, "csd-1", format.csd1.constData(), static_cast<size_t>(format.csd1.size()));
    }
    // Room for the largest key frame: a frame that does not fit would be lost
    AMediaFormat_setInt32(mediaFormat, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, qMax(kMinInputBytes, width * height));
    // Ask for the decoder's low-latency, real-time behaviour; decoders that do not know a key ignore it
    AMediaFormat_setInt32(mediaFormat, "low-latency", 1);
    AMediaFormat_setInt32(mediaFormat, "priority", 0);
    AMediaFormat_setInt32(mediaFormat, "vendor.qti-ext-dec-low-latency.enable", 1);
    AMediaFormat_setInt32(mediaFormat, "vendor.rtc-ext-dec-low-latency.enable", 1);

    AMediaCodecOnAsyncNotifyCallback callbacks{};
    callbacks.onAsyncInputAvailable = &DhakshaDecoderCore::_onInputAvailable;
    callbacks.onAsyncOutputAvailable = &DhakshaDecoderCore::_onOutputAvailable;
    callbacks.onAsyncFormatChanged = &DhakshaDecoderCore::_onFormatChanged;
    callbacks.onAsyncError = [](AMediaCodec* errorCodec, void* userData, media_status_t error, int32_t actionCode,
                                const char* detail) {
        DhakshaDecoderCore::_onError(errorCodec, userData, static_cast<int>(error), actionCode, detail);
    };
    const bool configured = (AMediaCodec_setAsyncNotifyCallback(codec, callbacks, this) == AMEDIA_OK) &&
                            (AMediaCodec_configure(codec, mediaFormat, window, nullptr, 0) == AMEDIA_OK);
    AMediaFormat_delete(mediaFormat);
    QString name;
    char* codecName = nullptr;
    if (configured && (AMediaCodec_getName(codec, &codecName) == AMEDIA_OK) && codecName) {
        name = QString::fromUtf8(codecName);
        AMediaCodec_releaseName(codec, codecName);
    }

    locker.relock();
    if (!configured || (generation != _formatGeneration) || (window != _window)) {
        if (!configured) {
            qCWarning(DhakshaNativeVideoLog)
                << "Hardware decoder failed to configure for" << format.mime() << width << "x" << height;
            _retryAtMs = _clock.elapsed() + kOpenRetryMs;
        }
        locker.unlock();
        (void) AMediaCodec_delete(codec);
        ANativeWindow_release(window);
        return;
    }
    // Set before start(): the input callbacks begin as soon as it runs
    _codec = codec;
    _codecFormatGeneration = generation;
    _codecError = false;
    _freeInputs.clear();
    _pendingOutputs.clear();
    _fedSinceOpen = false;
    _lastOutputMs = 0;
    _decoderName = name;
    if ((format.width > 0) && (format.height > 0)) {
        _videoSize = QSize(format.width, format.height);
    }
    _trimQueueLocked();
    locker.unlock();

    if (AMediaCodec_start(codec) != AMEDIA_OK) {
        qCWarning(DhakshaNativeVideoLog) << "Hardware decoder failed to start for" << format.mime();
        _closeCodec();
        locker.relock();
        _retryAtMs = _clock.elapsed() + kOpenRetryMs;
        locker.unlock();
    } else {
        qCDebug(DhakshaNativeVideoLog) << "Decoding" << format.mime() << width << "x" << height << "with" << name;
    }
    ANativeWindow_release(window);
#endif
}

void DhakshaDecoderCore::_closeCodec()
{
    QMutexLocker locker(&_mutex);
    AMediaCodec* codec = _codec;
    _codec = nullptr;
    _freeInputs.clear();
    _pendingOutputs.clear();
    _fedSinceOpen = false;
    _codecError = false;
    _trimQueueLocked();
    locker.unlock();
#ifdef DHAKSHA_NATIVE_VIDEO
    // Outside the lock: callbacks still running take it, see the codec is gone and return
    if (codec) {
        (void) AMediaCodec_stop(codec);
        (void) AMediaCodec_delete(codec);
    }
#else
    Q_UNUSED(codec)
#endif
}

void DhakshaDecoderCore::_onInputAvailable(AMediaCodec* codec, void* userData, int32_t index)
{
    // No codec call here: the worker thread fills the buffer
    auto* self = static_cast<DhakshaDecoderCore*>(userData);
    std::lock_guard<std::mutex> locker(self->_mutex);
    if (codec != self->_codec) {
        return;
    }
    self->_freeInputs.push_back(index);
    self->_wake.notify_all();
}

void DhakshaDecoderCore::_onOutputAvailable(AMediaCodec* codec, void* userData, int32_t index,
                                            AMediaCodecBufferInfo* info)
{
#ifdef DHAKSHA_NATIVE_VIDEO
    // No codec call here: the worker thread releases the frame to the surface straight away
    auto* self = static_cast<DhakshaDecoderCore*>(userData);
    std::lock_guard<std::mutex> locker(self->_mutex);
    if (codec != self->_codec) {
        return;
    }
    const bool render = info && (info->size > 0);
    self->_pendingOutputs.push_back({index, render, render ? info->presentationTimeUs * 1000 : 0});
    self->_wake.notify_all();
#else
    Q_UNUSED(codec)
    Q_UNUSED(userData)
    Q_UNUSED(index)
    Q_UNUSED(info)
#endif
}

void DhakshaDecoderCore::_onFormatChanged(AMediaCodec* codec, void* userData, AMediaFormat* format)
{
#ifdef DHAKSHA_NATIVE_VIDEO
    // The format belongs to MediaCodec, which deletes it after the callback
    if (!format) {
        return;
    }
    int32_t left = 0, top = 0, right = -1, bottom = -1;
    int32_t width = 0, height = 0;
    QSize size;
    if (AMediaFormat_getRect(format, AMEDIAFORMAT_KEY_DISPLAY_CROP, &left, &top, &right, &bottom) && (right > left) &&
        (bottom > top)) {
        size = QSize(right - left + 1, bottom - top + 1);
    } else if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &width) &&
               AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &height)) {
        size = QSize(width, height);
    }
    auto* self = static_cast<DhakshaDecoderCore*>(userData);
    QMutexLocker locker(&self->_mutex);
    if ((codec == self->_codec) && size.isValid()) {
        self->_videoSize = size;
    }
#else
    Q_UNUSED(codec)
    Q_UNUSED(userData)
    Q_UNUSED(format)
#endif
}

void DhakshaDecoderCore::_onError(AMediaCodec* codec, void* userData, int error, int32_t actionCode, const char* detail)
{
    qCWarning(DhakshaNativeVideoLog) << "Decoder error" << error << "action" << actionCode << (detail ? detail : "");
    auto* self = static_cast<DhakshaDecoderCore*>(userData);
    QMutexLocker locker(&self->_mutex);
    if (codec == self->_codec) {
        // Replaced from the control thread: a codec must not be deleted from its own callback
        self->_codecError = true;
        self->_wake.notify_all();
    }
}

// ----------------------------------------------------------------------------------------------
// DhakshaNativePlayer
// ----------------------------------------------------------------------------------------------

DhakshaNativePlayer::DhakshaNativePlayer(QObject* parent) : QObject(parent) {}

DhakshaNativePlayer::~DhakshaNativePlayer()
{
    stop();
}

bool DhakshaNativePlayer::supported()
{
#ifdef DHAKSHA_NATIVE_VIDEO
    return true;
#else
    return false;
#endif
}

void DhakshaNativePlayer::start(const QString& url, bool tcp)
{
    stop();
    if (url.isEmpty() || !supported()) {
        return;
    }
    const std::shared_ptr<DhakshaDecoderCore> decoder = _decoder;
    DhakshaRtspClient::Callbacks callbacks;
    callbacks.format = [decoder](const DhakshaVideoFormat& format) { decoder->startStream(format); };
    callbacks.accessUnit = [decoder](QByteArray accessUnit, bool keyFrame) {
        decoder->queueAccessUnit(std::move(accessUnit), keyFrame);
    };
    _client.start(url, tcp ? DhakshaRtspClient::Transport::Tcp : DhakshaRtspClient::Transport::Udp,
                  std::move(callbacks));
    _running = true;
    qCDebug(DhakshaNativeVideoLog) << "Playing" << url << (tcp ? "over TCP" : "over UDP");
}

void DhakshaNativePlayer::stop()
{
    _client.stop();
    _decoder->stopStream();
    _running = false;
}

QString DhakshaNativePlayer::codecName() const
{
    return _client.h265() ? QStringLiteral("H.265") : QStringLiteral("H.264");
}

QString DhakshaNativePlayer::transportName() const
{
    return (_client.activeTransport() == DhakshaRtspClient::Transport::Tcp) ? QStringLiteral("TCP")
                                                                            : QStringLiteral("UDP");
}
