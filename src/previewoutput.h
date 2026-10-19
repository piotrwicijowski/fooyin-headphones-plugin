// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <core/engine/audioformat.h>

#include <cstddef>
#include <span>

namespace Fooyin::Headphones {

// All calls, including destruction, belong to the controller's GUI thread.
// Phase 2 transports native PCM. Phase 3 extends configuration with negotiation
// and worker-side conversion before introducing a real QAudioSink adapter.
class PreviewOutput
{
public:
    virtual ~PreviewOutput()                                          = default;
    [[nodiscard]] virtual bool isAvailable() const                    = 0;
    virtual bool configure(const AudioFormat& format, QString& error) = 0;
    virtual bool start(QString& error)                                = 0;
    virtual void setPaused(bool paused)                               = 0;
    virtual void reset()                                  = 0; // Immediate discard, never synchronous draining.
    virtual void setVolume(double volume)                 = 0;
    [[nodiscard]] virtual qint64 bytesFree() const        = 0;
    virtual qint64 write(std::span<const std::byte> data) = 0;
    [[nodiscard]] virtual bool drained() const            = 0;
    [[nodiscard]] virtual uint64_t processedMs() const    = 0;
    [[nodiscard]] virtual QString error() const           = 0;
};

} // namespace Fooyin::Headphones
