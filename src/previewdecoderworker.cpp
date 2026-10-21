// SPDX-License-Identifier: GPL-3.0-or-later
#include "previewdecoderworker.h"

#include "previewconverter.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <numeric>
#include <stdexcept>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace Fooyin::Headphones {
namespace {
uint64_t framesForMs(uint64_t ms, int rate)
{
    const auto r = static_cast<uint64_t>(rate);
    if(ms / 1000 > (std::numeric_limits<uint64_t>::max() - r) / r) {
        throw std::runtime_error {"Preview timeline exceeds supported frame range"};
    }
    return (ms / 1000) * r + ((ms % 1000) * r) / 1000;
}

size_t boundedBytes(uint64_t frames, size_t frameBytes)
{
    if(frameBytes == 0 || frames > PreviewMaxBufferBytes / frameBytes) {
        throw std::runtime_error {"Preview PCM exceeds the bounded buffer capacity"};
    }
    return static_cast<size_t>(frames) * frameBytes;
}

struct SourceLifetime
{
    std::shared_ptr<PreviewSession> session;
    std::unique_ptr<PreviewSource> source;

    ~SourceLifetime()
    {
        {
            const std::lock_guard lock {session->abortMutex};
            session->abortSource = nullptr;
        }
        if(source) {
            source->stop();
        }
    }
};
} // namespace

PreviewSession::PreviewSession(uint64_t generation_, Track track_, uint64_t position_, bool initialStart_,
                               PreviewReplayGain replayGain_)
    : generation {generation_}
    , track {std::move(track_)}
    , positionMs {position_}
    , initialStart {initialStart_}
    , replayGain {replayGain_}
{
}

void PreviewSession::cancel()
{
    cancelled.store(true);
    if(auto queue = buffer()) {
        queue->cancel();
    }
    changed.notify_all();
    const std::lock_guard lock {abortMutex};
    if(abortSource) {
        abortSource->requestAbort();
    }
}

void PreviewSession::approve(const AudioFormat& format)
{
    const std::lock_guard lock {mutex};
    outputFormat = format;
    changed.notify_all();
}

std::optional<AudioFormat> PreviewSession::waitForApproval()
{
    std::unique_lock lock {mutex};
    changed.wait(lock, [this]() { return outputFormat.has_value() || cancelled.load(); });
    return cancelled.load() ? std::optional<AudioFormat> {} : outputFormat;
}

void PreviewSession::installBuffer(std::shared_ptr<PreviewPcmBuffer> buffer)
{
    const std::lock_guard lock {mutex};
    pcm = std::move(buffer);
    if(cancelled.load()) {
        pcm->cancel();
    }
}

std::shared_ptr<PreviewPcmBuffer> PreviewSession::buffer() const
{
    const std::lock_guard lock {mutex};
    return pcm;
}

PreviewDecoderWorker::PreviewDecoderWorker(PreviewSourceFactory factory, Opened opened, Failed failed)
    : m_factory {std::move(factory)}
    , m_opened {std::move(opened)}
    , m_failed {std::move(failed)}
    , m_thread {[this]() {
        run();
    }}
{
}

PreviewDecoderWorker::~PreviewDecoderWorker()
{
    shutdown();
}

void PreviewDecoderWorker::submit(std::shared_ptr<PreviewSession> session)
{
    const std::lock_guard lock {m_mutex};
    if(m_closing) {
        session->cancel();
        return;
    }
    if(m_pending) {
        m_pending->cancel();
    }
    m_pending = std::move(session);
    m_changed.notify_one();
}

void PreviewDecoderWorker::shutdown()
{
    {
        const std::lock_guard lock {m_mutex};
        m_closing = true;
        if(m_pending) {
            m_pending->cancel();
            m_pending.reset();
        }
        if(m_active) {
            m_active->cancel();
        }
    }
    m_changed.notify_one();
    if(m_thread.joinable()) {
        m_thread.join();
    }
    m_factory = {}; // Release AudioLoader only after all sources have died.
}

void PreviewDecoderWorker::run()
{
    for(;;) {
        std::shared_ptr<PreviewSession> session;
        {
            std::unique_lock lock {m_mutex};
            m_changed.wait(lock, [this]() { return m_closing || m_pending; });
            if(m_closing) {
                return;
            }
            session  = std::move(m_pending);
            m_active = session;
        }
        try {
            if(!session->cancelled.load()) {
                decode(session);
            }
        }
        catch(const std::exception& exception) {
            if(!session->cancelled.load()) {
                m_failed(session->generation, QString::fromUtf8(exception.what()));
            }
        }
        catch(...) {
            if(!session->cancelled.load()) {
                m_failed(session->generation, u"Unexpected preview decoder failure."_s);
            }
        }
        const std::lock_guard lock {m_mutex};
        m_active.reset();
    }
}

void PreviewDecoderWorker::decode(const std::shared_ptr<PreviewSession>& session)
{
    SourceLifetime lifetime {session, m_factory()};
    if(!lifetime.source) {
        throw std::runtime_error {"No preview source is available"};
    }
    auto& source  = *lifetime.source;
    QString error = previewTrackError(session->track);
    if(!error.isEmpty()) {
        m_failed(session->generation, error);
        return;
    }
    auto info = source.open(session->track, error);
    if(session->cancelled.load()) {
        return; // Loader open cannot be aborted through the public API.
    }
    if(!info) {
        m_failed(session->generation, error.isEmpty() ? u"Unable to open preview source."_s : error);
        return;
    }
    {
        const std::lock_guard lock {session->abortMutex};
        session->abortSource = &source;
    }
    if(session->cancelled.load()) {
        return;
    }
    const auto& format = info->format;
    if(!isPreviewInputFormat(format, &error)) {
        m_failed(session->generation, error);
        return;
    }
    const auto maxTime = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    auto positionMs = session->positionMs;
    if(session->initialStart
       && (!info->seekable || (session->track.duration() != 0 && positionMs >= session->track.duration()))) {
        // A configured offset is optional. Never decode-and-discard a long
        // unseekable prefix; a nonzero segment source offset still errors below.
        positionMs = 0;
    }
    if(session->track.offset() > maxTime || positionMs > maxTime - session->track.offset()
       || (session->track.isBoundedSegment() && session->track.duration() > maxTime - session->track.offset())) {
        throw std::runtime_error {"Preview segment timeline is out of range"};
    }
    const uint64_t absoluteMs = session->track.offset() + positionMs;
    // A direct millisecond seek can round to a different sample than PCM frame
    // arithmetic (e.g. FFmpeg rounds 27 ms at 44.1 kHz up). Seek to a preceding
    // millisecond whose frame coordinate is integral, then discard exact frames.
    // This adds < 1000 ms of bounded worker-side preroll, never GUI-side I/O.
    const uint64_t anchorPeriod = 1000 / std::gcd(format.sampleRate(), 1000);
    const uint64_t anchorMs     = absoluteMs - absoluteMs % anchorPeriod;
    if(absoluteMs != 0 && !info->seekable) {
        m_failed(session->generation, u"This source cannot seek to the preview segment."_s);
        return;
    }
    if(anchorMs != 0 && !source.seek(anchorMs, error)) {
        m_failed(session->generation, error.isEmpty() ? u"This source cannot seek to the preview segment."_s : error);
        return;
    }
    uint64_t skipFrames = framesForMs(absoluteMs, format.sampleRate()) - framesForMs(anchorMs, format.sampleRate());
    if(session->cancelled.load()) {
        return;
    }
    info->positionMs = positionMs;
    m_opened(session, *info);
    const auto outputFormat = session->waitForApproval();
    if(!outputFormat) {
        return;
    }
    PreviewConverter converter;
    if(!converter.configure(format, *outputFormat, error, session->replayGain.linearGain(session->track))) {
        m_failed(session->generation, error);
        return;
    }
    const size_t frameBytes       = static_cast<size_t>(format.bytesPerFrame());
    const size_t outputFrameBytes = static_cast<size_t>(outputFormat->bytesPerFrame());
    const size_t capacity
        = boundedBytes(std::max<uint64_t>(1, framesForMs(500, outputFormat->sampleRate())), outputFrameBytes);
    auto queue = std::make_shared<PreviewPcmBuffer>(capacity, outputFrameBytes);
    session->installBuffer(queue);

    uint64_t remaining = std::numeric_limits<uint64_t>::max();
    if(session->track.isBoundedSegment()) {
        const auto duration = session->track.duration();
        // Subtract frame coordinates, not rounded per-block millisecond durations.
        remaining = framesForMs(session->track.offset() + duration, format.sampleRate())
                  - framesForMs(session->track.offset() + std::min(duration, positionMs), format.sampleRate());
    }
    const size_t requestBytes   = boundedBytes(PreviewBlockFrames, frameBytes);
    auto starvedSince           = std::chrono::steady_clock::now();
    while(!session->cancelled.load() && remaining > 0) {
        auto result = source.read(requestBytes);
        if(session->cancelled.load()) {
            return;
        }
        if(result.status == AudioDecoder::ReadStatus::EndOfStream) {
            break;
        }
        if(result.status == AudioDecoder::ReadStatus::Error) {
            m_failed(session->generation, result.error.isEmpty() ? u"Preview decoding failed."_s : result.error);
            return;
        }
        if(result.status == AudioDecoder::ReadStatus::NeedMoreInput) {
            if(std::chrono::steady_clock::now() - starvedSince >= 2s) {
                m_failed(session->generation, u"Local preview source remained starved for input."_s);
                return;
            }
            std::unique_lock lock {session->mutex};
            session->changed.wait_for(lock, 10ms, [&]() { return session->cancelled.load(); });
            continue;
        }
        auto& buffer = result.buffer;
        if(!buffer.isValid() || buffer.frameCount() <= 0 || buffer.format() != format
           || static_cast<size_t>(buffer.byteCount()) > requestBytes) {
            m_failed(session->generation, u"Preview decoder returned malformed PCM or changed format."_s);
            return;
        }
        starvedSince       = std::chrono::steady_clock::now();
        const auto skipped = std::min<uint64_t>(skipFrames, static_cast<uint64_t>(buffer.frameCount()));
        skipFrames -= skipped;
        const auto frames = std::min<uint64_t>(remaining, static_cast<uint64_t>(buffer.frameCount()) - skipped);
        if(frames == 0) {
            continue;
        }
        const auto payload = buffer.constData().subspan(static_cast<size_t>(skipped) * frameBytes,
                                                        static_cast<size_t>(frames) * frameBytes);
        const AudioBuffer clipped {payload, format, buffer.startTime()};
        auto converted = converter.convert(clipped, error);
        if(!converted) {
            m_failed(session->generation, error);
            return;
        }
        if(!queue->push(*converted)) {
            return;
        }
        remaining -= frames;
        starvedSince = std::chrono::steady_clock::now();
    }
    if(!session->cancelled.load()) {
        auto tail = converter.finish(error);
        if(!tail) {
            m_failed(session->generation, error);
            return;
        }
        if(!queue->push(*tail)) {
            return;
        }
        queue->finish();
    }
}

} // namespace Fooyin::Headphones
