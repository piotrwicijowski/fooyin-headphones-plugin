// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "qtaudiooutput.h"

#include <QByteArray>

#include <algorithm>
#include <thread>

namespace Fooyin::Headphones::Tests {

class FakeDeviceCatalogue final : public HeadphoneDeviceCatalogue
{
public:
    std::vector<HeadphoneDevice> entries;
    std::vector<HeadphoneDevice> devices() const override
    {
        return entries;
    }
    void change(std::vector<HeadphoneDevice> next)
    {
        entries = std::move(next);
        Q_EMIT devicesChanged();
    }
};

inline HeadphoneDevice fakeDevice(const QByteArray& id,
                                  std::vector<AudioFormat> formats = {{SampleFormat::F32, 48000, 2}})
{
    return {id,
            QStringLiteral("Same device description"),
            formats.front(),
            [formats](const AudioFormat& format) { return std::ranges::find(formats, format) != formats.end(); },
            {}};
}

struct SinkState
{
    PreviewSinkState state {PreviewSinkState::Idle};
    std::vector<QByteArray> epochs;
    std::vector<QByteArray> openedIds;
    QPointer<QIODevice> lastDevice;
    qint64 processed {0};
    qint64 free {65536};
    qint64 maxWrite {65536};
    qsizetype requestedBuffer {0};
    double volume {0.0};
    int resets {0};
    int starts {0};
    bool startFails {false};
    bool writeFails {false};
    bool wrongThread {false};
    QString failure;
};

class FakeWritableDevice final : public QIODevice
{
public:
    explicit FakeWritableDevice(std::shared_ptr<SinkState> state)
        : m_state {std::move(state)}
        , m_epoch {m_state->epochs.size()}
    {
        m_state->epochs.emplace_back();
        open(QIODevice::WriteOnly | QIODevice::Unbuffered);
    }
    bool isSequential() const override
    {
        return true;
    }

protected:
    qint64 readData(char*, qint64) override
    {
        return -1;
    }
    qint64 writeData(const char* data, qint64 count) override
    {
        if(m_state->writeFails) {
            return -1;
        }
        const auto written = std::min(count, m_state->maxWrite);
        m_state->epochs[m_epoch].append(data, written);
        if(written > 0) {
            m_state->state = PreviewSinkState::Active;
        }
        return written;
    }

private:
    std::shared_ptr<SinkState> m_state;
    size_t m_epoch;
};

class FakeAudioSink final : public PreviewAudioSink
{
public:
    explicit FakeAudioSink(std::shared_ptr<SinkState> state)
        : m_state {std::move(state)}
        , m_owner {std::this_thread::get_id()}
    {
    }
    ~FakeAudioSink() override
    {
        checkThread();
    }
    QIODevice* start() override
    {
        checkThread();
        ++m_state->starts;
        if(m_state->startFails) {
            return nullptr;
        }
        m_device            = std::make_unique<FakeWritableDevice>(m_state);
        m_state->lastDevice = m_device.get();
        m_state->state      = PreviewSinkState::Idle;
        return m_device.get();
    }
    void reset() override
    {
        checkThread();
        m_device.reset();
        m_state->state     = PreviewSinkState::Stopped;
        m_state->processed = 0;
        ++m_state->resets;
    }
    void setPaused(bool paused) override
    {
        checkThread();
        m_state->state = paused ? PreviewSinkState::Suspended : PreviewSinkState::Active;
    }
    void setBufferSize(qsizetype bytes) override
    {
        checkThread();
        m_state->requestedBuffer = bytes;
    }
    qsizetype bufferSize() const override
    {
        checkThread();
        return m_state->requestedBuffer + 24;
    }
    void setVolume(double gain) override
    {
        checkThread();
        m_state->volume = gain;
    }
    qint64 bytesFree() const override
    {
        checkThread();
        return m_state->free;
    }
    qint64 processedUSecs() const override
    {
        checkThread();
        return m_state->processed;
    }
    PreviewSinkState state() const override
    {
        checkThread();
        return m_state->state;
    }
    QString error() const override
    {
        checkThread();
        return m_state->failure;
    }

private:
    void checkThread() const
    {
        if(std::this_thread::get_id() != m_owner) {
            m_state->wrongThread = true;
        }
    }
    std::shared_ptr<SinkState> m_state;
    std::unique_ptr<FakeWritableDevice> m_device;
    std::thread::id m_owner;
};

inline PreviewSinkFactory fakeSinkFactory(const std::shared_ptr<SinkState>& state)
{
    return [state](const HeadphoneDevice& device, const AudioFormat&) {
        state->openedIds.push_back(device.id);
        return std::make_unique<FakeAudioSink>(state);
    };
}

} // namespace Fooyin::Headphones::Tests
