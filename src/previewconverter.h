// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <core/engine/audiobuffer.h>

#include <optional>

struct SwrContext;

namespace Fooyin::Headphones {

// One streaming converter per generation, used exclusively by the decoder worker.
class PreviewConverter
{
public:
    PreviewConverter() = default;
    ~PreviewConverter();
    PreviewConverter(const PreviewConverter&)            = delete;
    PreviewConverter& operator=(const PreviewConverter&) = delete;

    bool configure(const AudioFormat& input, const AudioFormat& output, QString& error, double linearGain = 1.0);
    std::optional<std::vector<std::byte>> convert(const AudioBuffer& input, QString& error);
    std::optional<std::vector<std::byte>> finish(QString& error);
    void reset();

private:
    std::optional<std::vector<std::byte>> process(const uint8_t* input, int frames, QString& error);
    AudioFormat m_input;
    AudioFormat m_output;
    SwrContext* m_context {nullptr};
    double m_linearGain {1.0};
    bool m_passthrough {false};
    bool m_finished {false};
};

[[nodiscard]] bool isPreviewFormat(const AudioFormat& format);

} // namespace Fooyin::Headphones
