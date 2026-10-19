// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "previewcontroller.h"

#include <QHash>
#include <QSemaphore>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>

namespace Fooyin::Headphones::Tests {

class Gate
{
public:
    std::atomic<bool> entered {false};

    void wait(const std::atomic<bool>* aborted = nullptr)
    {
        std::unique_lock lock {m_mutex};
        entered = true;
        // Bound a failed test's cleanup too: an assertion must not hang shutdown.
        m_changed.wait_for(lock, std::chrono::seconds {3}, [&]() { return m_open || (aborted && aborted->load()); });
    }

    void release()
    {
        const std::lock_guard lock {m_mutex};
        m_open = true;
        m_changed.notify_all();
    }

    void wake()
    {
        const std::lock_guard lock {m_mutex};
        m_changed.notify_all();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_changed;
    bool m_open {false};
};

struct SourcePlan
{
    AudioFormat format {SampleFormat::S16, 1000, 1};
    uint64_t totalFrames {1000000};
    int blockFrames {64};
    int base {0};
    bool seekable {true};
    bool seekRoundsUp {false};
    bool openFails {false};
    bool seekFails {false};
    bool readFails {false};
    bool malformed {false};
    bool changedFormat {false};
    int starveReads {0}; // -1 means persistent starvation.
    std::shared_ptr<Gate> openGate;
    std::shared_ptr<Gate> readGate;
    std::atomic<int> opens {0};
    std::atomic<int> reads {0};
    std::atomic<int> stops {0};
    std::atomic<int> destroys {0};
    std::atomic<int> aborts {0};
    std::atomic<bool> wrongThread {false};
    std::atomic<uint64_t> lastSeek {0};
    QSemaphore stopped;
};

struct SourceLibrary
{
    QHash<QString, std::shared_ptr<SourcePlan>> plans;
};

class FakeSource final : public PreviewSource
{
public:
    explicit FakeSource(std::shared_ptr<SourceLibrary> library)
        : m_library {std::move(library)}
        , m_owner {std::this_thread::get_id()}
    {
    }

    ~FakeSource() override
    {
        if(m_plan) {
            checkThread();
            if(!m_stopped) {
                m_plan->wrongThread = true;
            }
            ++m_plan->destroys;
        }
    }

    std::optional<PreviewSourceInfo> open(const Track& track, QString& error) override
    {
        m_plan = m_library->plans.value(track.filepath());
        if(!m_plan) {
            error = QStringLiteral("Missing test source");
            return {};
        }
        checkThread();
        ++m_plan->opens;
        if(m_plan->openGate) {
            m_plan->openGate->wait();
        }
        if(m_plan->openFails) {
            error = QStringLiteral("Test open failure");
            return {};
        }
        return PreviewSourceInfo {m_plan->format, m_plan->seekable};
    }

    bool seek(uint64_t absoluteMs, QString& error) override
    {
        checkThread();
        m_plan->lastSeek = absoluteMs;
        m_frame = (absoluteMs * static_cast<uint64_t>(m_plan->format.sampleRate()) + (m_plan->seekRoundsUp ? 999 : 0))
                / 1000;
        if(m_plan->seekFails) {
            error = QStringLiteral("Test seek failure");
            return false;
        }
        return true;
    }

    AudioDecoder::ReadResult read(size_t maxBytes) override
    {
        checkThread();
        const auto call = ++m_plan->reads;
        if(m_plan->readGate && call == 1) {
            m_plan->readGate->wait(&m_aborted);
        }
        if(m_aborted || m_plan->readFails) {
            return AudioDecoder::ReadResult::errorResult(QStringLiteral("Test read failure"));
        }
        if(m_plan->starveReads < 0 || call <= m_plan->starveReads) {
            return AudioDecoder::ReadResult::needMoreInput();
        }
        if(m_frame >= m_plan->totalFrames) {
            return AudioDecoder::ReadResult::endOfStream();
        }
        if(m_plan->malformed) {
            return AudioDecoder::ReadResult::data({});
        }
        auto format = m_plan->format;
        if(m_plan->changedFormat) {
            format.setSampleRate(format.sampleRate() + 1);
        }
        const auto frames
            = std::min({static_cast<uint64_t>(m_plan->blockFrames),
                        static_cast<uint64_t>(maxBytes / format.bytesPerFrame()), m_plan->totalFrames - m_frame});
        AudioBuffer buffer {format, m_frame * 1000 / format.sampleRate()};
        buffer.resize(frames * format.bytesPerFrame());
        for(uint64_t i = 0; i < frames; ++i) {
            const auto value = static_cast<int16_t>((m_plan->base + m_frame + i) % 30000);
            for(int channel = 0; channel < format.channelCount(); ++channel) {
                std::memcpy(buffer.data() + (i * format.channelCount() + channel) * sizeof(value), &value,
                            sizeof(value));
            }
        }
        m_frame += frames;
        return AudioDecoder::ReadResult::data(std::move(buffer));
    }

    void requestAbort() override
    {
        m_aborted = true;
        ++m_plan->aborts;
        if(m_plan->readGate) {
            m_plan->readGate->wake();
        }
    }

    void stop() override
    {
        if(m_plan && !m_stopped) {
            checkThread();
            m_stopped = true;
            ++m_plan->stops;
            m_plan->stopped.release();
        }
    }

private:
    void checkThread()
    {
        if(m_owner != std::this_thread::get_id()) {
            m_plan->wrongThread = true;
        }
    }

    std::shared_ptr<SourceLibrary> m_library;
    std::shared_ptr<SourcePlan> m_plan;
    std::thread::id m_owner;
    std::atomic<bool> m_aborted {false};
    uint64_t m_frame {0};
    bool m_stopped {false};
};

struct OutputState
{
    bool available {true};
    bool configureFails {false};
    bool startFails {false};
    bool writeFails {false};
    bool zeroWrites {false};
    bool canDrain {false};
    bool paused {false};
    bool wrongThread {false};
    qint64 freeBytes {4096};
    qint64 maxWrite {std::numeric_limits<qint64>::max()};
    QString failure;
    AudioFormat format;
    std::optional<AudioFormat> negotiatedFormat;
    std::vector<std::byte> stream;
    int configurations {0};
    int starts {0};
    int resets {0};
    uint64_t processed {0};
    double volume {0.0};
};

class FakeOutput final : public PreviewOutput
{
public:
    explicit FakeOutput(std::shared_ptr<OutputState> state)
        : m_state {std::move(state)}
        , m_owner {std::this_thread::get_id()}
    {
    }

    ~FakeOutput() override
    {
        checkThread();
    }
    bool isAvailable() const override
    {
        checkThread();
        return m_state->available;
    }
    std::optional<AudioFormat> configure(const AudioFormat& format, QString&) override
    {
        checkThread();
        m_state->format = m_state->negotiatedFormat.value_or(format);
        ++m_state->configurations;
        return m_state->configureFails ? std::optional<AudioFormat> {} : m_state->format;
    }
    bool start(QString&) override
    {
        checkThread();
        ++m_state->starts;
        return !m_state->startFails;
    }
    void setPaused(bool paused) override
    {
        checkThread();
        m_state->paused = paused;
    }
    void reset() override
    {
        checkThread();
        ++m_state->resets;
        m_state->stream.clear();
        m_state->processed = 0;
        m_state->paused    = false;
    }
    void setVolume(double volume) override
    {
        checkThread();
        m_state->volume = volume;
    }
    qint64 bytesFree() const override
    {
        checkThread();
        return m_state->freeBytes;
    }
    qint64 write(std::span<const std::byte> data) override
    {
        checkThread();
        if(m_state->writeFails) {
            return -1;
        }
        if(m_state->zeroWrites) {
            return 0;
        }
        const auto count = std::min<size_t>(data.size(), static_cast<size_t>(m_state->maxWrite));
        m_state->stream.insert(m_state->stream.end(), data.begin(), data.begin() + static_cast<ptrdiff_t>(count));
        return static_cast<qint64>(count);
    }
    bool drained() const override
    {
        checkThread();
        return m_state->canDrain;
    }
    uint64_t processedMs() const override
    {
        checkThread();
        return m_state->processed;
    }
    QString error() const override
    {
        checkThread();
        return m_state->failure;
    }

private:
    void checkThread() const
    {
        if(m_owner != std::this_thread::get_id()) {
            m_state->wrongThread = true;
        }
    }
    std::shared_ptr<OutputState> m_state;
    std::thread::id m_owner;
};

struct PreviewHarness
{
    std::shared_ptr<SourceLibrary> library {std::make_shared<SourceLibrary>()};
    std::shared_ptr<OutputState> output {std::make_shared<OutputState>()};
    std::unique_ptr<PreviewController> controller;

    void create()
    {
        controller = std::make_unique<PreviewController>(
            [library = library]() { return std::make_unique<FakeSource>(library); },
            std::make_unique<FakeOutput>(output));
    }
    std::shared_ptr<SourcePlan> add(const QString& path)
    {
        auto plan = std::make_shared<SourcePlan>();
        library->plans.insert(path, plan);
        return plan;
    }
};

inline int16_t sampleAt(const std::vector<std::byte>& bytes, size_t index)
{
    int16_t sample {0};
    std::memcpy(&sample, bytes.data() + index * sizeof(sample), sizeof(sample));
    return sample;
}

} // namespace Fooyin::Headphones::Tests
