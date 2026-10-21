// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "headphonedevices.h"
#include "previewoutput.h"

#include <QIODevice>
#include <QPointer>

#include <memory>

namespace Fooyin::Headphones {

enum class PreviewSinkState
{
    Stopped,
    Active,
    Idle,
    Suspended
};

// Narrow QAudioSink boundary for deterministic backend/epoch tests. Construction,
// use and destruction all occur on the controller's thread.
class PreviewAudioSink
{
public:
    virtual ~PreviewAudioSink()                          = default;
    virtual QIODevice* start()                           = 0;
    virtual void reset()                                 = 0;
    virtual void setPaused(bool paused)                  = 0;
    virtual void setBufferSize(qsizetype bytes)          = 0;
    [[nodiscard]] virtual qsizetype bufferSize() const   = 0;
    virtual void setVolume(double gain)                  = 0;
    [[nodiscard]] virtual qint64 bytesFree() const       = 0;
    [[nodiscard]] virtual qint64 processedUSecs() const  = 0;
    [[nodiscard]] virtual PreviewSinkState state() const = 0;
    [[nodiscard]] virtual QString error() const          = 0; // Underrun is not fatal.
};

using PreviewSinkFactory = std::function<std::unique_ptr<PreviewAudioSink>(const HeadphoneDevice&, const AudioFormat&)>;

class QtAudioOutput final : public PreviewOutput
{
public:
    QtAudioOutput(HeadphoneDeviceCatalogue& devices, QByteArray selectedId, PreviewSinkFactory factory = {},
                  int firstChannel = 0);
    ~QtAudioOutput() override;

    [[nodiscard]] bool isAvailable() const override;
    [[nodiscard]] int channelPair() const override
    {
        return m_firstChannel;
    }
    std::optional<AudioFormat> configure(const AudioFormat& input, QString& error) override;
    bool start(QString& error) override;
    void setPaused(bool paused) override;
    void reset() override;
    void setVolume(double uiVolume) override;
    [[nodiscard]] qint64 bytesFree() const override;
    qint64 write(std::span<const std::byte> data) override;
    [[nodiscard]] bool drained() const override;
    [[nodiscard]] uint64_t processedMs() const override;
    [[nodiscard]] QString error() const override;
    [[nodiscard]] qsizetype bufferSize() const;

private:
    HeadphoneDeviceCatalogue& m_devices;
    const QByteArray m_selectedId;
    const int m_firstChannel;
    PreviewSinkFactory m_factory;
    std::unique_ptr<PreviewAudioSink> m_sink;
    QPointer<QIODevice> m_device;
    AudioFormat m_format;
    double m_volume {0.25};
    uint64_t m_writtenBytes {0};
    mutable uint64_t m_positionMs {0};
    bool m_available {false};
    bool m_started {false};
    bool m_paused {false};
};

[[nodiscard]] double previewVolumeGain(double uiVolume);

} // namespace Fooyin::Headphones
