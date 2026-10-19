// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <core/engine/audioinput.h>

#include <functional>

namespace Fooyin {
class AudioLoader;
}

namespace Fooyin::Headphones {

struct PreviewSourceInfo
{
    AudioFormat format;
    bool seekable {false};
};

// Created, opened, read, sought, stopped and destroyed on the decoder worker.
// requestAbort() is the only cross-thread method, and must return promptly.
class PreviewSource
{
public:
    virtual ~PreviewSource()                                                          = default;
    virtual std::optional<PreviewSourceInfo> open(const Track& track, QString& error) = 0;
    virtual bool seek(uint64_t absoluteMs, QString& error)                            = 0;
    virtual AudioDecoder::ReadResult read(size_t maxBytes)                            = 0;
    virtual void requestAbort()                                                       = 0;
    virtual void stop()                                                               = 0;
};

using PreviewSourceFactory = std::function<std::unique_ptr<PreviewSource>()>;

[[nodiscard]] QString previewTrackError(const Track& track);
[[nodiscard]] PreviewSourceFactory makePreviewSourceFactory(std::shared_ptr<AudioLoader> loader);

} // namespace Fooyin::Headphones
