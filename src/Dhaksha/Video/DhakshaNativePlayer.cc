#include "DhakshaNativePlayer.h"

#include <QtCore/QMutexLocker>
#include <cstring>

#if defined(Q_OS_ANDROID) && defined(QGC_GST_STREAMING)
#define DHAKSHA_NATIVE_VIDEO 1
#include <android/native_window.h>
#include <gst/gst.h>
#include <gst/rtsp/gstrtsptransport.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#endif

Q_LOGGING_CATEGORY(DhakshaNativeVideoLog, "Dhaksha.NativeVideo")

namespace {
constexpr int kRestartDelayMs = 1000;
#ifdef DHAKSHA_NATIVE_VIDEO
// A stream that delivers nothing for this long is restarted; connecting gets longer
constexpr qint64 kStallRestartMs = 3000;
constexpr qint64 kConnectTimeoutMs = 10000;
// Waiting longer for a free decoder input would hold the network thread; the frame is skipped
// instead and decoding resumes at the next key frame so no broken picture is shown
constexpr int64_t kInputWaitUs = 20000;
constexpr int64_t kOutputWaitUs = 10000;
#endif
}  // namespace

// ----------------------------------------------------------------------------------------------
// DhakshaDecoderCore
// ----------------------------------------------------------------------------------------------

DhakshaDecoderCore::~DhakshaDecoderCore()
{
    QMutexLocker locker(&_mutex);
    _closeLocked();
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
    QMutexLocker locker(&_mutex);
    if (window == _window) {
        return;
    }
    // The decoder is bound to the surface it was configured with
    _closeLocked();
    if (_window) {
        ANativeWindow_release(_window);
    }
    _window = window;
    if (_window) {
        ANativeWindow_acquire(_window);
    }
#else
    _window = window;
#endif
}

void DhakshaDecoderCore::reset()
{
    QMutexLocker locker(&_mutex);
    _closeLocked();
}

void DhakshaDecoderCore::setFrameCallback(std::function<void()> callback)
{
    QMutexLocker locker(&_callbackMutex);
    _frameCallback = std::move(callback);
}

QSize DhakshaDecoderCore::videoSize() const
{
    QMutexLocker locker(&_sizeMutex);
    return _videoSize;
}

QString DhakshaDecoderCore::decoderName() const
{
    QMutexLocker locker(&_mutex);
    return _decoderName;
}

void DhakshaDecoderCore::queueAccessUnit(const char* mime, int width, int height, const uint8_t* data, size_t size,
                                         bool keyFrame)
{
#ifdef DHAKSHA_NATIVE_VIDEO
    QMutexLocker locker(&_mutex);
    if (!_window || !data || (size == 0)) {
        return;
    }
    if (_codec && (_mime != QLatin1String(mime))) {
        _closeLocked();
    }
    if (_needKeyFrame && !keyFrame) {
        return;
    }
    if (!_codec && !_openLocked(mime, width, height)) {
        return;
    }

    const ssize_t index = AMediaCodec_dequeueInputBuffer(_codec, kInputWaitUs);
    if (index < 0) {
        _needKeyFrame = true;
        return;
    }
    size_t capacity = 0;
    uint8_t* input = AMediaCodec_getInputBuffer(_codec, static_cast<size_t>(index), &capacity);
    if (!input || (capacity < size)) {
        (void) AMediaCodec_queueInputBuffer(_codec, static_cast<size_t>(index), 0, 0, 0, 0);
        _needKeyFrame = true;
        return;
    }
    memcpy(input, data, size);
    const uint64_t timeUs = static_cast<uint64_t>(g_get_monotonic_time());
    if (AMediaCodec_queueInputBuffer(_codec, static_cast<size_t>(index), 0, size, timeUs, 0) != AMEDIA_OK) {
        _needKeyFrame = true;
        return;
    }
    _needKeyFrame = false;
#else
    Q_UNUSED(mime);
    Q_UNUSED(width);
    Q_UNUSED(height);
    Q_UNUSED(data);
    Q_UNUSED(size);
    Q_UNUSED(keyFrame);
#endif
}

bool DhakshaDecoderCore::_openLocked(const char* mime, int width, int height)
{
#ifdef DHAKSHA_NATIVE_VIDEO
    AMediaCodec* codec = AMediaCodec_createDecoderByType(mime);
    if (!codec) {
        qCWarning(DhakshaNativeVideoLog) << "No hardware decoder for" << mime;
        return false;
    }

    AMediaFormat* format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, (width > 0) ? width : 1920);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, (height > 0) ? height : 1080);
    // Ask for the decoder's low-latency, real-time behaviour; decoders that do not know a key ignore it
    AMediaFormat_setInt32(format, "low-latency", 1);
    AMediaFormat_setInt32(format, "priority", 0);
    AMediaFormat_setInt32(format, "vendor.qti-ext-dec-low-latency.enable", 1);
    AMediaFormat_setInt32(format, "vendor.rtc-ext-dec-low-latency.enable", 1);

    const media_status_t configured = AMediaCodec_configure(codec, format, _window, nullptr, 0);
    AMediaFormat_delete(format);
    if ((configured != AMEDIA_OK) || (AMediaCodec_start(codec) != AMEDIA_OK)) {
        qCWarning(DhakshaNativeVideoLog) << "Hardware decoder failed to start for" << mime << width << "x" << height;
        AMediaCodec_delete(codec);
        return false;
    }

    char* name = nullptr;
    if ((AMediaCodec_getName(codec, &name) == AMEDIA_OK) && name) {
        _decoderName = QString::fromUtf8(name);
        AMediaCodec_releaseName(codec, name);
    }
    _codec = codec;
    _mime = QLatin1String(mime);
    {
        QMutexLocker sizeLocker(&_sizeMutex);
        _videoSize = QSize(width, height);
    }
    _needKeyFrame = true;
    _outputRunning.store(true);
    _outputThread = std::thread(&DhakshaDecoderCore::_outputLoop, this, codec);
    qCDebug(DhakshaNativeVideoLog) << "Decoding" << mime << width << "x" << height << "with" << _decoderName;
    return true;
#else
    Q_UNUSED(mime);
    Q_UNUSED(width);
    Q_UNUSED(height);
    return false;
#endif
}

void DhakshaDecoderCore::_closeLocked()
{
    _needKeyFrame = true;
    _outputRunning.store(false);
    if (!_codec) {
        return;
    }
#ifdef DHAKSHA_NATIVE_VIDEO
    if (_outputThread.joinable()) {
        _outputThread.join();
    }
    (void) AMediaCodec_stop(_codec);
    (void) AMediaCodec_delete(_codec);
    _codec = nullptr;
    _mime.clear();
#endif
}

void DhakshaDecoderCore::_outputLoop(AMediaCodec* codec)
{
#ifdef DHAKSHA_NATIVE_VIDEO
    while (_outputRunning.load()) {
        AMediaCodecBufferInfo info;
        const ssize_t index = AMediaCodec_dequeueOutputBuffer(codec, &info, kOutputWaitUs);
        if (index >= 0) {
            // Drawn now, not at a presentation time: the newest frame is always the one shown
            (void) AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(index), info.size > 0);
            if (info.size > 0) {
                _framesRendered.fetch_add(1, std::memory_order_acq_rel);
                QMutexLocker locker(&_callbackMutex);
                if (_frameCallback) {
                    _frameCallback();
                }
            }
        } else if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* format = AMediaCodec_getOutputFormat(codec);
            if (format) {
                int32_t left = 0, top = 0, right = -1, bottom = -1;
                int32_t width = 0, height = 0;
                QSize size;
                if (AMediaFormat_getRect(format, AMEDIAFORMAT_KEY_DISPLAY_CROP, &left, &top, &right, &bottom) &&
                    (right > left) && (bottom > top)) {
                    size = QSize(right - left + 1, bottom - top + 1);
                } else if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &width) &&
                           AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &height)) {
                    size = QSize(width, height);
                }
                AMediaFormat_delete(format);
                if (size.isValid()) {
                    QMutexLocker locker(&_sizeMutex);
                    _videoSize = size;
                }
            }
        }
    }
#else
    Q_UNUSED(codec)
#endif
}

// ----------------------------------------------------------------------------------------------
// DhakshaNativePlayer
// ----------------------------------------------------------------------------------------------

DhakshaNativePlayer::DhakshaNativePlayer(QObject* parent) : QObject(parent)
{
    _busTimer.setInterval(200);
    (void) connect(&_busTimer, &QTimer::timeout, this, &DhakshaNativePlayer::_pollBus);
    _restartTimer.setSingleShot(true);
    _restartTimer.setInterval(kRestartDelayMs);
    (void) connect(&_restartTimer, &QTimer::timeout, this, [this]() {
        if (_wanted) {
            _destroyPipeline();
            _createPipeline();
        }
    });
}

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

void DhakshaNativePlayer::start(const QString& url, int latencyMs, bool tcpOnly)
{
    stop();
    if (url.isEmpty() || !supported()) {
        return;
    }
    _url = url;
    _latencyMs = qMax(0, latencyMs);
    _tcpOnly = tcpOnly;
    _wanted = true;
    _createPipeline();
    _busTimer.start();
}

void DhakshaNativePlayer::stop()
{
    _wanted = false;
    _lastFrameMs.store(0);
    _busTimer.stop();
    _restartTimer.stop();
    _destroyPipeline();
    _decoder->reset();
}

void DhakshaNativePlayer::_createPipeline()
{
#ifdef DHAKSHA_NATIVE_VIDEO
    if (!gst_is_initialized()) {
        // QGC finishes starting GStreamer in the background; try again shortly
        _restartLater();
        return;
    }

    GstElement* pipeline = gst_pipeline_new("dhaksha-native-video");
    GstElement* source = gst_element_factory_make("rtspsrc", "source");
    if (!pipeline || !source) {
        qCWarning(DhakshaNativeVideoLog) << "rtspsrc not available";
        if (pipeline) {
            gst_object_unref(pipeline);
        }
        if (source) {
            gst_object_unref(source);
        }
        return;
    }

    const QByteArray location = _url.toUtf8();
    const GstRTSPLowerTrans protocols =
        _tcpOnly ? GST_RTSP_LOWER_TRANS_TCP
                 : static_cast<GstRTSPLowerTrans>(GST_RTSP_LOWER_TRANS_UDP | GST_RTSP_LOWER_TRANS_TCP);
    // Over TCP nothing is lost, so nothing is dropped for arriving late either: a dropped packet would
    // smear the picture just like a lost one
    const gboolean dropOnLatency = (!_tcpOnly && (_latencyMs > 0)) ? TRUE : FALSE;
    g_object_set(source, "location", location.constData(), "latency", static_cast<guint>(_latencyMs), "drop-on-latency",
                 dropOnLatency, "do-retransmission", FALSE, "udp-reconnect", TRUE, "tcp-timeout",
                 static_cast<guint64>(2000000), "protocols", protocols, nullptr);
    (void) g_signal_connect(source, "pad-added", G_CALLBACK(&DhakshaNativePlayer::_onPadAdded), this);

    gst_bin_add(GST_BIN(pipeline), source);
    _pipeline = pipeline;
    _sinceStart.start();
    _lastFrameMs.store(0);
    _decoder->reset();

    if (gst_element_set_state(_pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        qCWarning(DhakshaNativeVideoLog) << "Could not start" << _url;
        _restartLater();
    } else {
        qCDebug(DhakshaNativeVideoLog) << "Playing" << _url << "latency" << _latencyMs << "ms"
                                       << (_tcpOnly ? "TCP" : "UDP");
    }
#endif
}

void DhakshaNativePlayer::_destroyPipeline()
{
#ifdef DHAKSHA_NATIVE_VIDEO
    if (!_pipeline) {
        return;
    }
    GstElement* pipeline = _pipeline;
    _pipeline = nullptr;
    (void) gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
#endif
}

void DhakshaNativePlayer::_restartLater()
{
    if (_wanted && !_restartTimer.isActive()) {
        _restartTimer.start();
    }
}

void DhakshaNativePlayer::_pollBus()
{
#ifdef DHAKSHA_NATIVE_VIDEO
    if (!_pipeline) {
        return;
    }
    GstBus* bus = gst_element_get_bus(_pipeline);
    bool restart = false;
    while (GstMessage* message =
               gst_bus_pop_filtered(bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS))) {
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            GError* error = nullptr;
            gst_message_parse_error(message, &error, nullptr);
            qCWarning(DhakshaNativeVideoLog) << "Stream error:" << (error ? error->message : "unknown");
            g_clear_error(&error);
        }
        gst_message_unref(message);
        restart = true;
    }
    gst_object_unref(bus);

    const qint64 lastFrameMs = _lastFrameMs.load();
    const qint64 nowMs = _sinceStart.elapsed();
    const bool stalled = (lastFrameMs == 0) ? (nowMs > kConnectTimeoutMs) : ((nowMs - lastFrameMs) > kStallRestartMs);
    if (stalled) {
        restart = true;
    }
    if (restart) {
        _destroyPipeline();
        _decoder->reset();
        _restartLater();
    }
#endif
}

void DhakshaNativePlayer::_onPadAdded(GstElement* source, GstPad* pad, void* userData)
{
#ifdef DHAKSHA_NATIVE_VIDEO
    auto* self = static_cast<DhakshaNativePlayer*>(userData);
    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) {
        caps = gst_pad_query_caps(pad, nullptr);
    }
    if (!caps) {
        return;
    }
    const GstStructure* structure = gst_caps_get_structure(caps, 0);
    const gchar* media = gst_structure_get_string(structure, "media");
    const gchar* encoding = gst_structure_get_string(structure, "encoding-name");
    const bool video = media && (g_strcmp0(media, "video") == 0);
    const bool h265 =
        encoding && ((g_ascii_strcasecmp(encoding, "H265") == 0) || (g_ascii_strcasecmp(encoding, "HEVC") == 0));
    const bool h264 = encoding && (g_ascii_strcasecmp(encoding, "H264") == 0);
    gst_caps_unref(caps);
    GstElement* pipeline = GST_ELEMENT(gst_element_get_parent(source));
    if (!pipeline) {
        return;
    }
    if (!video || (!h264 && !h265)) {
        // Audio or other streams go to a sink of their own: an unlinked stream stops rtspsrc with an error
        qCDebug(DhakshaNativeVideoLog) << "Ignoring stream" << (media ? media : "?") << (encoding ? encoding : "?");
        GstElement* discard = gst_element_factory_make("fakesink", nullptr);
        if (discard) {
            g_object_set(discard, "sync", FALSE, "async", FALSE, nullptr);
            gst_bin_add(GST_BIN(pipeline), discard);
            (void) gst_element_sync_state_with_parent(discard);
            GstPad* discardPad = gst_element_get_static_pad(discard, "sink");
            (void) gst_pad_link(pad, discardPad);
            gst_object_unref(discardPad);
        }
        gst_object_unref(pipeline);
        return;
    }
    GstElement* depay = gst_element_factory_make(h265 ? "rtph265depay" : "rtph264depay", nullptr);
    GstElement* parse = gst_element_factory_make(h265 ? "h265parse" : "h264parse", nullptr);
    GstElement* filter = gst_element_factory_make("capsfilter", nullptr);
    GstElement* sink = gst_element_factory_make("fakesink", nullptr);
    if (!depay || !parse || !filter || !sink) {
        qCWarning(DhakshaNativeVideoLog) << "Missing depayloader or parser for" << encoding;
        for (GstElement* element : {depay, parse, filter, sink}) {
            if (element) {
                gst_object_unref(element);
            }
        }
        gst_object_unref(pipeline);
        return;
    }

    // Parameter sets before every key frame, whole frames per buffer: what the hardware decoder expects
    g_object_set(parse, "config-interval", -1, nullptr);
    GstCaps* filterCaps = gst_caps_from_string(h265 ? "video/x-h265,stream-format=byte-stream,alignment=au"
                                                    : "video/x-h264,stream-format=byte-stream,alignment=au");
    g_object_set(filter, "caps", filterCaps, nullptr);
    gst_caps_unref(filterCaps);
    g_object_set(sink, "sync", FALSE, "async", FALSE, "signal-handoffs", TRUE, "enable-last-sample", FALSE, nullptr);
    (void) g_signal_connect(sink, "handoff", G_CALLBACK(&DhakshaNativePlayer::_onHandoff), self);

    gst_bin_add_many(GST_BIN(pipeline), depay, parse, filter, sink, nullptr);
    if (!gst_element_link_many(depay, parse, filter, sink, nullptr)) {
        qCWarning(DhakshaNativeVideoLog) << "Could not link the video branch";
    }
    for (GstElement* element : {sink, filter, parse, depay}) {
        (void) gst_element_sync_state_with_parent(element);
    }
    GstPad* depaySink = gst_element_get_static_pad(depay, "sink");
    if (gst_pad_link(pad, depaySink) != GST_PAD_LINK_OK) {
        qCWarning(DhakshaNativeVideoLog) << "Could not link the camera stream";
    }
    gst_object_unref(depaySink);
    gst_object_unref(pipeline);
#else
    Q_UNUSED(source);
    Q_UNUSED(pad);
    Q_UNUSED(userData);
#endif
}

void DhakshaNativePlayer::_onHandoff(GstElement* sink, GstBuffer* buffer, GstPad* pad, void* userData)
{
#ifdef DHAKSHA_NATIVE_VIDEO
    Q_UNUSED(sink)
    auto* self = static_cast<DhakshaNativePlayer*>(userData);
    self->_lastFrameMs.store(self->_sinceStart.elapsed());

    int width = 0;
    int height = 0;
    const char* mime = "video/avc";
    if (GstCaps* caps = gst_pad_get_current_caps(pad)) {
        const GstStructure* structure = gst_caps_get_structure(caps, 0);
        (void) gst_structure_get_int(structure, "width", &width);
        (void) gst_structure_get_int(structure, "height", &height);
        if (gst_structure_has_name(structure, "video/x-h265")) {
            mime = "video/hevc";
        }
        gst_caps_unref(caps);
    }

    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        return;
    }
    const bool keyFrame = !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
    self->_decoder->queueAccessUnit(mime, width, height, map.data, map.size, keyFrame);
    gst_buffer_unmap(buffer, &map);
#else
    Q_UNUSED(sink);
    Q_UNUSED(buffer);
    Q_UNUSED(pad);
    Q_UNUSED(userData);
#endif
}
