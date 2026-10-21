// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "previewpcmbuffer.h"
#include "previewreplaygain.h"
#include "previewsource.h"

#include <atomic>
#include <thread>

namespace Fooyin::Headphones {

// Generation-owned cancellation, queue and output-configuration handshake.
// The abort mutex protects publication/lifetime, never a read/open/seek call.
struct PreviewSession
{
    PreviewSession(uint64_t generation_, Track track_, uint64_t position_, bool initialStart_ = false,
                   PreviewReplayGain replayGain_ = {});
    void cancel();
    void approve(const AudioFormat& outputFormat, int firstChannel = 0);
    std::optional<AudioFormat> waitForApproval();
    void installBuffer(std::shared_ptr<PreviewPcmBuffer> buffer);
    [[nodiscard]] std::shared_ptr<PreviewPcmBuffer> buffer() const;

    const uint64_t generation;
    const Track track;
    const uint64_t positionMs;
    const bool initialStart;
    const PreviewReplayGain replayGain;
    std::atomic<bool> cancelled {false};
    std::mutex abortMutex;
    PreviewSource* abortSource {nullptr};
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::optional<AudioFormat> outputFormat;
    int firstOutputChannel {0}; // Published under mutex along with outputFormat.
    std::shared_ptr<PreviewPcmBuffer> pcm;
};

class PreviewDecoderWorker
{
public:
    using Opened = std::function<void(std::shared_ptr<PreviewSession>, PreviewSourceInfo)>;
    using Failed = std::function<void(uint64_t, QString)>;

    PreviewDecoderWorker(PreviewSourceFactory factory, Opened opened, Failed failed);
    ~PreviewDecoderWorker();
    void submit(std::shared_ptr<PreviewSession> session);
    void shutdown();

private:
    void run();
    void decode(const std::shared_ptr<PreviewSession>& session);

    PreviewSourceFactory m_factory;
    Opened m_opened;
    Failed m_failed;
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::shared_ptr<PreviewSession> m_pending;
    std::shared_ptr<PreviewSession> m_active;
    bool m_closing {false};
    std::thread m_thread;
};

} // namespace Fooyin::Headphones
