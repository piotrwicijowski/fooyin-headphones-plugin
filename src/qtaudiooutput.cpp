// SPDX-License-Identifier: GPL-3.0-or-later
#include "qtaudiooutput.h"

#include <QAudioSink>

#include <algorithm>
#include <cmath>
#include <limits>

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
namespace AudioApi = QtAudio;
#else
namespace AudioApi = QAudio;
#endif

using namespace Qt::StringLiterals;

namespace Fooyin::Headphones {
namespace {
class QtSink final : public PreviewAudioSink
{
public:
    QtSink(const QAudioDevice& device, const AudioFormat& format)
    {
        const auto qtFormat = qtPreviewFormat(format);
        // This is deliberately the explicit-device overload, never the default.
        m_sink = std::make_unique<QAudioSink>(device, qtFormat);
    }
    QIODevice* start() override
    {
        return m_sink->start();
    }
    void reset() override
    {
        m_sink->reset();
    }
    void setPaused(bool paused) override
    {
        paused ? m_sink->suspend() : m_sink->resume();
    }
    void setBufferSize(qsizetype bytes) override
    {
        m_sink->setBufferSize(bytes);
    }
    qsizetype bufferSize() const override
    {
        return m_sink->bufferSize();
    }
    void setVolume(double gain) override
    {
        m_sink->setVolume(gain);
    }
    qint64 bytesFree() const override
    {
        return m_sink->bytesFree();
    }
    qint64 processedUSecs() const override
    {
        return m_sink->processedUSecs();
    }
    PreviewSinkState state() const override
    {
        switch(m_sink->state()) {
            case AudioApi::ActiveState:
                return PreviewSinkState::Active;
            case AudioApi::IdleState:
                return PreviewSinkState::Idle;
            case AudioApi::SuspendedState:
                return PreviewSinkState::Suspended;
            default:
                return PreviewSinkState::Stopped;
        }
    }
    QString error() const override
    {
        switch(m_sink->error()) {
            case AudioApi::NoError:
            case AudioApi::UnderrunError:
                return {};
            case AudioApi::OpenError:
                return u"Unable to open the selected headphone device."_s;
            case AudioApi::IOError:
                return u"Headphone device I/O failed."_s;
            default:
                return u"Headphone audio backend failed."_s;
        }
    }

private:
    std::unique_ptr<QAudioSink> m_sink;
};
} // namespace

double previewVolumeGain(double uiVolume)
{
    const auto clamped = std::clamp(std::isfinite(uiVolume) ? uiVolume : 0.0, 0.0, 1.0);
    return AudioApi::convertVolume(static_cast<float>(clamped), AudioApi::LogarithmicVolumeScale,
                                   AudioApi::LinearVolumeScale);
}

QtAudioOutput::QtAudioOutput(HeadphoneDeviceCatalogue& devices, QByteArray selectedId, PreviewSinkFactory factory,
                             int firstChannel)
    : m_devices {devices}
    , m_selectedId {std::move(selectedId)}
    , m_firstChannel {firstChannel}
    , m_factory {std::move(factory)}
{
    if(!m_factory) {
        m_factory = [](const HeadphoneDevice& device, const AudioFormat& format) -> std::unique_ptr<PreviewAudioSink> {
            if(device.qtDevice.isNull() || device.qtDevice.id() != device.id) {
                return {};
            }
            return std::make_unique<QtSink>(device.qtDevice, format);
        };
    }
    m_available = isAvailable();
    QObject::connect(&m_devices, &HeadphoneDeviceCatalogue::devicesChanged, this, [this]() {
        const bool available = isAvailable();
        if(m_available && !available) {
            const auto configured = m_format;
            reset(); // Discard before notifying the controller, even while paused.
            // Keep the lost format long enough for the controller to observe
            // invalidation even if a different format still exposes this pair.
            m_format = configured;
        }
        if(m_available != available) {
            m_available = available;
            Q_EMIT availabilityChanged();
            // A controller may reset the invalidated session in response. If
            // the pair still has a different supported format, make explicit
            // Play available again without reopening or restarting anything.
            const bool refreshed = isAvailable();
            if(m_available != refreshed) {
                m_available = refreshed;
                Q_EMIT availabilityChanged();
            }
        }
    });
}

QtAudioOutput::~QtAudioOutput()
{
    reset();
}

bool QtAudioOutput::isAvailable() const
{
    const auto device = m_devices.resolve(m_selectedId);
    if(!device) {
        return false;
    }
    if(m_firstChannel == 0) {
        return true;
    }
    const auto pairs = previewChannelPairs(*device);
    return std::ranges::find(pairs, m_firstChannel) != pairs.end()
        && (!m_format.isValid()
            || (std::ranges::find(device->pairFormats, m_format) != device->pairFormats.end()
                && device->supportsFormat(m_format)));
}

std::optional<AudioFormat> QtAudioOutput::configure(const AudioFormat& input, QString& error)
{
    reset();
    const auto device = m_devices.resolve(m_selectedId);
    const auto format = device ? negotiatePreviewFormat(*device, input, m_firstChannel) : std::optional<AudioFormat> {};
    if(!format) {
        error = device ? (m_firstChannel != 0
                              ? u"The selected headphone channel pair is unavailable. Choose another output."_s
                              : u"The selected headphone device has no supported mono/stereo PCM format."_s)
                       : u"The explicitly selected headphone device is missing or not configured."_s;
        return {};
    }
    m_format = *format;
    return format;
}

bool QtAudioOutput::start(QString& error)
{
    const auto device = m_devices.resolve(m_selectedId);
    if(!device || !isAvailable() || !m_format.isValid() || !device->supportsFormat
       || !device->supportsFormat(m_format)) {
        error = u"The selected headphone device or format is no longer available."_s;
        return false;
    }
    m_sink = m_factory(*device, m_format);
    if(!m_sink) {
        error = u"Unable to create an output for the explicit headphone device."_s;
        return false;
    }
    m_sink->setBufferSize(static_cast<qsizetype>(m_format.bytesForDuration(100)));
    m_sink->setVolume(previewVolumeGain(m_volume));
    m_device = m_sink->start();
    if(!m_device || !m_sink->error().isEmpty() || m_sink->state() == PreviewSinkState::Stopped) {
        error = m_sink->error().isEmpty() ? u"Unable to start the headphone audio stream."_s : m_sink->error();
        reset();
        return false;
    }
    m_started = true;
    return true;
}

void QtAudioOutput::setPaused(bool paused)
{
    if(m_sink && m_started) {
        if(paused) {
            static_cast<void>(processedMs());
        }
        m_paused = paused;
        m_sink->setPaused(paused);
    }
}

void QtAudioOutput::reset()
{
    m_device  = nullptr; // A reset invalidates the writable device epoch.
    m_started = false;
    m_paused  = false;
    if(m_sink) {
        m_sink->reset();
        m_sink.reset();
    }
    m_format       = {};
    m_writtenBytes = 0;
    m_positionMs   = 0;
}

void QtAudioOutput::setVolume(double uiVolume)
{
    if(!std::isfinite(uiVolume)) {
        return;
    }
    m_volume = std::clamp(uiVolume, 0.0, 1.0);
    if(m_sink) {
        m_sink->setVolume(previewVolumeGain(m_volume));
    }
}

qint64 QtAudioOutput::bytesFree() const
{
    return m_sink && m_started && !m_paused && error().isEmpty() ? m_sink->bytesFree() : 0;
}

qint64 QtAudioOutput::write(std::span<const std::byte> data)
{
    if(!m_device || !m_started || !error().isEmpty()) {
        return -1;
    }
    if(m_paused) {
        return 0;
    }
    const auto count = m_device->write(reinterpret_cast<const char*>(data.data()), static_cast<qint64>(data.size()));
    if(count > 0 && static_cast<size_t>(count) <= data.size()) {
        m_writtenBytes += static_cast<uint64_t>(count);
    }
    return count;
}

bool QtAudioOutput::drained() const
{
    return m_sink && m_started && !m_paused && m_writtenBytes > 0 && error().isEmpty()
        && m_sink->state() == PreviewSinkState::Idle;
}

uint64_t QtAudioOutput::processedMs() const
{
    if(!m_sink || !m_started || m_paused) {
        return m_positionMs;
    }
    const auto processed = static_cast<uint64_t>(std::max<qint64>(0, m_sink->processedUSecs())) / 1000;
    const auto writtenMs = m_format.durationForBytes(m_writtenBytes);
    m_positionMs         = std::max(m_positionMs, std::min(processed, writtenMs));
    return m_positionMs;
}

QString QtAudioOutput::error() const
{
    if(!isAvailable()) {
        return m_firstChannel > 0
                 ? u"Channels %1/%2 are unavailable on the selected headphone device. Choose another output."_s
                       .arg(m_firstChannel)
                       .arg(m_firstChannel + 1)
             : m_firstChannel < 0 ? u"The saved headphone channel selection is invalid. Choose another output."_s
                                  : u"The explicitly selected headphone device is missing or not configured."_s;
    }
    if(!m_sink || !m_started) {
        return {};
    }
    const auto failure = m_sink->error();
    if(!failure.isEmpty()) {
        return failure;
    }
    if(m_sink->state() == PreviewSinkState::Stopped || !m_device) {
        return u"The headphone audio stream stopped unexpectedly."_s;
    }
    return {};
}

qsizetype QtAudioOutput::bufferSize() const
{
    return m_sink ? m_sink->bufferSize() : 0;
}

} // namespace Fooyin::Headphones
