// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "previewoutput.h"
#include "previewsource.h"
#include "previewtypes.h"

#include <QObject>
#include <QTimer>

#include <vector>

namespace Fooyin::Headphones {
class PreviewDecoderWorker;
struct PreviewSession;

// One instance is owned by the plugin, independently of all layout widgets.
// This dependency surface intentionally contains no main-player/queue APIs.
class PreviewController : public QObject
{
    Q_OBJECT

public:
    PreviewController(PreviewSourceFactory factory, std::unique_ptr<PreviewOutput> output, QObject* parent = nullptr);
    ~PreviewController() override;

    [[nodiscard]] PreviewSnapshot snapshot() const;
    void previewTrack(const Track& track);
    void playPause();
    void stop();
    bool seek(int64_t relativeMs);
    void setVolume(double volume);
    void setOutput(std::unique_ptr<PreviewOutput> output);
    void refreshOutput();
    void shutdown();

Q_SIGNALS:
    void snapshotChanged(const Fooyin::Headphones::PreviewSnapshot& snapshot);

private:
    void begin(const Track& track, uint64_t positionMs, bool paused, bool keepSeekable);
    void invalidate();
    void fail(const QString& error);
    void opened(const std::shared_ptr<PreviewSession>& session, const PreviewSourceInfo& info);
    void pump();
    void publish();
    [[nodiscard]] bool active() const;

    PreviewSnapshot m_snapshot;
    std::unique_ptr<PreviewOutput> m_output;
    std::unique_ptr<PreviewDecoderWorker> m_worker;
    std::shared_ptr<PreviewSession> m_session;
    QTimer m_pump;
    std::vector<std::byte> m_pending;
    size_t m_pendingOffset {0};
    size_t m_prebufferBytes {0};
    bool m_outputConfigured {false};
    bool m_outputStarted {false};
    bool m_shutdown {false};
};

} // namespace Fooyin::Headphones
