// SPDX-License-Identifier: GPL-3.0-or-later
#include "previewsource.h"

#include <core/engine/audioloader.h>

#include <QFileInfo>

using namespace Qt::StringLiterals;

namespace Fooyin::Headphones {
namespace {
class LoaderPreviewSource final : public PreviewSource
{
public:
    explicit LoaderPreviewSource(std::shared_ptr<AudioLoader> loader)
        : m_loader {std::move(loader)}
    {
    }

    ~LoaderPreviewSource() override
    {
        stop();
    }

    std::optional<PreviewSourceInfo> open(const Track& track, QString& error) override
    {
        stop();
        error = previewTrackError(track);
        if(!error.isEmpty()) {
            return {};
        }
        if(!m_loader || !QFileInfo {track.filepath()}.isFile()) {
            error = u"Preview requires an available decoder loader and an ordinary local file."_s;
            return {};
        }
        m_loaded = m_loader->loadDecoderForTrack(track, AudioDecoder::NoLooping);
        m_loaded.input.rebind();
        if(!m_loaded.decoder || !m_loaded.format || !m_loaded.format->isValid()) {
            error = u"Unable to open a decoder for headphone preview."_s;
            return {};
        }
        if(!m_loaded.decoder->allowsConcurrentDecoding()) {
            error = u"This decoder does not allow independent concurrent preview."_s;
            return {};
        }
        if(!m_loaded.isDecoding) {
            m_loaded.decoder->start();
            m_loaded.isDecoding = true;
        }
        return PreviewSourceInfo {*m_loaded.format, m_loaded.decoder->isSeekable()};
    }

    bool seek(uint64_t absoluteMs, QString& error) override
    {
        if(!m_loaded.decoder || !m_loaded.decoder->isSeekable()) {
            error = u"This preview source is not seekable."_s;
            return false;
        }
        // Public decoder seek uses the source timeline. Its void return does not
        // report backend seek failures; this adapter must not invent a result.
        m_loaded.decoder->seek(absoluteMs);
        return true;
    }

    AudioDecoder::ReadResult read(size_t maxBytes) override
    {
        return m_loaded.decoder->readAudio(maxBytes);
    }

    void requestAbort() override
    {
        m_loaded.decoder->requestAbort();
    }

    void stop() override
    {
        if(m_loaded.decoder) {
            m_loaded.decoder->stop();
            // Destroy the decoder before releasing its owning input bundle.
            m_loaded.decoder.reset();
        }
        m_loaded.input = {};
        m_loaded.format.reset();
        m_loaded.isDecoding = false;
    }

private:
    std::shared_ptr<AudioLoader> m_loader;
    LoadedDecoder m_loaded;
};
} // namespace

QString previewTrackError(const Track& track)
{
    if(!track.isValid() || track.isRemote() || track.isVirtual() || track.isInArchive()) {
        return u"Headphone preview supports ordinary local files only (no remote, virtual or archive sources)."_s;
    }
    return {};
}

PreviewSourceFactory makePreviewSourceFactory(std::shared_ptr<AudioLoader> loader)
{
    return [loader = std::move(loader)]() {
        return std::make_unique<LoaderPreviewSource>(loader);
    };
}

} // namespace Fooyin::Headphones
