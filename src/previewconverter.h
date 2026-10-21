// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <core/engine/audiobuffer.h>

#include <array>
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

    bool configure(const AudioFormat& input, const AudioFormat& output, QString& error, double linearGain = 1.0,
                   int firstChannel = 0);
    std::optional<std::vector<std::byte>> convert(const AudioBuffer& input, QString& error);
    std::optional<std::vector<std::byte>> finish(QString& error);
    void reset();

private:
    std::optional<std::vector<std::byte>> process(const uint8_t* input, int frames, QString& error);
    std::optional<std::vector<std::byte>> route(std::vector<std::byte> stereo, QString& error) const;
    AudioFormat m_input;
    AudioFormat m_output;
    AudioFormat m_physicalOutput;
    int m_firstChannel {0};
    SwrContext* m_context {nullptr};
    std::array<int, 8> m_channelMap {}; // libswresample borrows this mapping for the context lifetime.
    double m_linearGain {1.0};
    bool m_passthrough {false};
    bool m_finished {false};
};

inline constexpr size_t PreviewBlockFrames    = 4096;
inline constexpr size_t PreviewMaxBufferBytes = 8 * 1024 * 1024;

[[nodiscard]] bool isPreviewInputFormat(const AudioFormat& format, QString* error = nullptr);
[[nodiscard]] bool isPreviewOutputFormat(const AudioFormat& format);

} // namespace Fooyin::Headphones
