// SPDX-License-Identifier: GPL-3.0-or-later
#include "previewcontroller.h"

#include "previewdecoderworker.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Qt::StringLiterals;

namespace Fooyin::Headphones {

PreviewController::PreviewController(PreviewSourceFactory factory, std::unique_ptr<PreviewOutput> output,
                                     QObject* parent)
    : QObject {parent}
    , m_output {std::move(output)}
{
    qRegisterMetaType<PreviewSnapshot>();
    m_snapshot.deviceAvailable = m_output && m_output->isAvailable();
    connectOutput();
    m_worker = std::make_unique<PreviewDecoderWorker>(
        std::move(factory),
        [this](std::shared_ptr<PreviewSession> session, PreviewSourceInfo info) {
            // Stale posted results must not retain whole generations of PCM.
            const std::weak_ptr<PreviewSession> weakSession {session};
            QMetaObject::invokeMethod(
                this,
                [this, weakSession, info]() {
                    if(auto locked = weakSession.lock()) {
                        opened(locked, info);
                    }
                },
                Qt::QueuedConnection);
        },
        [this](uint64_t generation, QString error) {
            QMetaObject::invokeMethod(
                this,
                [this, generation, error = std::move(error)]() {
                    if(!m_shutdown && m_session && generation == m_snapshot.generation) {
                        fail(error);
                    }
                },
                Qt::QueuedConnection);
        });
    m_pump.setInterval(10);
    QObject::connect(&m_pump, &QTimer::timeout, this, &PreviewController::pump);
    m_positionTimer.setInterval(100);
    QObject::connect(&m_positionTimer, &QTimer::timeout, this, &PreviewController::updatePosition);
}

PreviewController::~PreviewController()
{
    shutdown();
}

PreviewSnapshot PreviewController::snapshot() const
{
    return m_snapshot;
}

bool PreviewController::active() const
{
    return m_snapshot.state != PreviewState::Stopped && m_snapshot.state != PreviewState::Error;
}

void PreviewController::publish()
{
    Q_EMIT snapshotChanged(m_snapshot);
}

void PreviewController::invalidate()
{
    ++m_snapshot.generation;
    m_pump.stop();
    m_positionTimer.stop();
    if(m_output) {
        m_output->reset();
    }
    if(m_session) {
        m_session->cancel();
        m_session.reset();
    }
    m_pending.clear();
    m_pendingOffset    = 0;
    m_outputConfigured = false;
    m_outputStarted    = false;
}

void PreviewController::begin(const Track& track, uint64_t positionMs, bool paused, bool keepSeekable)
{
    invalidate();
    m_snapshot.track          = track;
    m_snapshot.durationMs     = track.duration();
    m_snapshot.pauseRequested = paused;
    m_snapshot.error.clear();
    if(!keepSeekable) {
        m_snapshot.seekable   = false;
        m_snapshot.positionMs = 0;
    }
    m_snapshot.deviceAvailable = m_output && m_output->isAvailable();
    const auto error           = previewTrackError(track);
    if(!error.isEmpty() || !m_snapshot.deviceAvailable) {
        m_snapshot.state = PreviewState::Error;
        m_snapshot.error = error.isEmpty() ? u"Select an available headphone output before previewing."_s : error;
        publish();
        return;
    }
    m_snapshot.state = PreviewState::Loading;
    m_session        = std::make_shared<PreviewSession>(m_snapshot.generation, track, positionMs);
    m_worker->submit(m_session);
    m_pump.start();
    m_positionTimer.start();
    publish();
}

void PreviewController::previewTrack(const Track& track)
{
    if(!m_shutdown) {
        begin(track, 0, false, false);
    }
}

void PreviewController::opened(const std::shared_ptr<PreviewSession>& session, const PreviewSourceInfo& info)
{
    if(m_shutdown || session != m_session || session->cancelled.load()) {
        return;
    }
    QString error;
    const auto outputFormat = m_output->configure(info.format, error);
    if(!outputFormat) {
        fail(error.isEmpty() ? u"Unable to configure the headphone output."_s : error);
        return;
    }
    m_output->setVolume(m_snapshot.volume);
    m_outputConfigured = true;
    m_prebufferBytes
        = static_cast<size_t>(std::max<uint64_t>(outputFormat->bytesPerFrame(), outputFormat->bytesForDuration(40)));
    m_snapshot.seekable   = info.seekable;
    m_snapshot.positionMs = session->positionMs;
    m_snapshot.state      = m_snapshot.pauseRequested ? PreviewState::Paused : PreviewState::Buffering;
    session->approve(*outputFormat);
    publish();
}

void PreviewController::playPause()
{
    if(m_shutdown) {
        return;
    }
    if(!active()) {
        if(m_snapshot.track.isValid() && m_output && m_output->isAvailable()) {
            begin(m_snapshot.track, 0, false, false);
        }
        return;
    }
    m_snapshot.pauseRequested = !m_snapshot.pauseRequested;
    if(m_outputStarted) {
        m_output->setPaused(m_snapshot.pauseRequested);
    }
    if(m_outputConfigured) {
        const auto queue = m_session->buffer();
        m_snapshot.state = m_snapshot.pauseRequested  ? PreviewState::Paused
                         : !m_outputStarted           ? PreviewState::Buffering
                         : queue && queue->finished() ? PreviewState::Draining
                                                      : PreviewState::Playing;
    }
    publish();
}

void PreviewController::stop()
{
    if(m_shutdown) {
        return;
    }
    invalidate();
    m_snapshot.state          = PreviewState::Stopped;
    m_snapshot.positionMs     = 0;
    m_snapshot.seekable       = false;
    m_snapshot.pauseRequested = false;
    m_snapshot.error.clear();
    publish();
}

bool PreviewController::seek(int64_t relativeMs)
{
    if(m_shutdown || !active() || !m_snapshot.seekable) {
        return false;
    }
    const auto maxTime = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    if(m_snapshot.track.offset() > maxTime) {
        return false;
    }
    auto target = static_cast<uint64_t>(std::max<int64_t>(0, relativeMs));
    target      = std::min(target, maxTime - m_snapshot.track.offset());
    if(m_snapshot.durationMs != 0) {
        target = std::min(target, m_snapshot.durationMs);
    }
    begin(m_snapshot.track, target, m_snapshot.pauseRequested, true);
    return true;
}

void PreviewController::setVolume(double volume)
{
    if(m_shutdown || !std::isfinite(volume)) {
        return;
    }
    m_snapshot.volume = std::clamp(volume, 0.0, 1.0);
    if(m_snapshot.volume > 0.0) {
        m_unmutedVolume = m_snapshot.volume;
    }
    if(m_output) {
        m_output->setVolume(m_snapshot.volume);
    }
    publish();
}

void PreviewController::mute()
{
    setVolume(0.0);
}

void PreviewController::unmute()
{
    if(m_snapshot.volume == 0.0) {
        setVolume(m_unmutedVolume);
    }
}

void PreviewController::toggleMute()
{
    if(m_snapshot.volume > 0.0) {
        mute();
    }
    else {
        unmute();
    }
}

void PreviewController::setOutput(std::unique_ptr<PreviewOutput> output)
{
    if(m_shutdown) {
        return;
    }
    stop();
    m_output = std::move(output);
    connectOutput();
    refreshOutput();
}

void PreviewController::connectOutput()
{
    if(m_output) {
        QObject::connect(m_output.get(), &PreviewOutput::availabilityChanged, this, &PreviewController::refreshOutput);
    }
}

void PreviewController::refreshOutput()
{
    if(m_shutdown) {
        return;
    }
    m_snapshot.deviceAvailable = m_output && m_output->isAvailable();
    if(active() && !m_snapshot.deviceAvailable) {
        fail(u"The selected headphone output is unavailable. Play explicitly after reconnecting."_s);
    }
    else {
        publish();
    }
}

void PreviewController::fail(const QString& error)
{
    invalidate();
    m_snapshot.state          = PreviewState::Error;
    m_snapshot.error          = error;
    m_snapshot.positionMs     = 0;
    m_snapshot.pauseRequested = false;
    m_snapshot.seekable       = false;
    publish();
}

void PreviewController::pump()
{
    if(m_shutdown || !m_session) {
        return;
    }
    if(!m_output->isAvailable()) {
        refreshOutput();
        return;
    }
    if(!m_output->error().isEmpty()) {
        fail(m_output->error());
        return;
    }
    if(!m_outputConfigured || m_snapshot.pauseRequested) {
        return;
    }
    auto queue = m_session->buffer();
    if(!queue) {
        return; // Worker is configuring conversion; the GUI never waits for it.
    }
    const auto previousState = m_snapshot.state;
    if(!m_outputStarted) {
        if(queue->size() < std::min(m_prebufferBytes, queue->capacity()) && !queue->finished()) {
            return;
        }
        if(queue->size() == 0 && queue->finished()) {
            stop();
            return;
        }
        QString error;
        if(!m_output->start(error)) {
            fail(error.isEmpty() ? u"Unable to start the headphone output."_s : error);
            return;
        }
        m_outputStarted  = true;
        m_snapshot.state = PreviewState::Playing;
    }
    // Bound GUI work and preserve arbitrary short writes, including partial frames.
    for(int i = 0; i < 4; ++i) {
        const auto free = m_output->bytesFree();
        if(free < 0) {
            fail(u"Unable to query headphone output capacity."_s);
            return;
        }
        if(free == 0) {
            break;
        }
        if(m_pendingOffset == m_pending.size()) {
            m_pending       = queue->pop(64 * 1024);
            m_pendingOffset = 0;
        }
        if(m_pending.empty()) {
            break;
        }
        const auto count = std::min<uint64_t>(static_cast<uint64_t>(free), m_pending.size() - m_pendingOffset);
        const auto written
            = m_output->write(std::span {m_pending}.subspan(m_pendingOffset, static_cast<size_t>(count)));
        if(written < 0 || static_cast<uint64_t>(written) > count) {
            fail(u"Unable to write headphone preview audio."_s);
            return;
        }
        m_pendingOffset += static_cast<size_t>(written);
        if(written == 0) {
            break;
        }
    }
    if(queue->finished()) {
        m_snapshot.state = PreviewState::Draining;
        if(queue->size() == 0 && m_pendingOffset == m_pending.size() && m_output->drained()) {
            stop();
            return;
        }
    }
    if(previousState != m_snapshot.state) {
        publish();
    }
}

void PreviewController::updatePosition()
{
    if(m_shutdown || !m_session || !m_outputStarted || m_snapshot.pauseRequested) {
        return;
    }
    const auto processed = m_output->processedMs();
    auto position
        = m_session->positionMs + std::min(processed, std::numeric_limits<uint64_t>::max() - m_session->positionMs);
    if(m_snapshot.track.isBoundedSegment()) {
        position = std::min(position, m_snapshot.durationMs);
    }
    if(position != m_snapshot.positionMs) {
        m_snapshot.positionMs = position;
        publish();
    }
}

void PreviewController::shutdown()
{
    if(m_shutdown) {
        return;
    }
    m_shutdown = true;
    invalidate();
    m_worker->shutdown(); // Does not require a GUI callback to finish.
    m_worker.reset();
    m_output.reset();
    m_snapshot.state           = PreviewState::Stopped;
    m_snapshot.positionMs      = 0;
    m_snapshot.seekable        = false;
    m_snapshot.pauseRequested  = false;
    m_snapshot.deviceAvailable = false;
    m_snapshot.error.clear();
    publish();
}

} // namespace Fooyin::Headphones
