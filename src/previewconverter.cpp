// SPDX-License-Identifier: GPL-3.0-or-later
#include "previewconverter.h"

#include <core/engine/audioconverter.h>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#if LIBSWRESAMPLE_VERSION_MAJOR < 4 || LIBAVUTIL_VERSION_MAJOR < 57
#error "Headphones Preview requires FFmpeg 5+ (libswresample 4+, libavutil 57+)"
#endif

using namespace Qt::StringLiterals;

namespace Fooyin::Headphones {
namespace {
constexpr size_t MaxStagingBytes = 8 * 1024 * 1024;

AVSampleFormat outputSampleFormat(SampleFormat sample)
{
    switch(sample) {
        case SampleFormat::F32:
            return AV_SAMPLE_FMT_FLT;
        case SampleFormat::S16:
            return AV_SAMPLE_FMT_S16;
        case SampleFormat::S32:
            return AV_SAMPLE_FMT_S32;
        default:
            return AV_SAMPLE_FMT_NONE;
    }
}

QString conversionError(int code)
{
    char text[AV_ERROR_MAX_STRING_SIZE] {};
    av_strerror(code, text, sizeof(text));
    return u"Preview conversion failed: %1"_s.arg(QString::fromUtf8(text));
}
} // namespace

bool isPreviewFormat(const AudioFormat& format)
{
    return format.isValid() && format.sampleRate() <= 768000 && format.channelCount() <= 2
        && (!format.hasChannelLayout()
            || format.channelLayout() == AudioFormat::defaultChannelLayoutForChannelCount(format.channelCount()));
}

PreviewConverter::~PreviewConverter()
{
    reset();
}

void PreviewConverter::reset()
{
    swr_free(&m_context);
    m_input       = {};
    m_output      = {};
    m_passthrough = false;
    m_finished    = false;
    m_linearGain  = 1.0;
}

bool PreviewConverter::configure(const AudioFormat& input, const AudioFormat& output, QString& error, double linearGain)
{
    reset();
    if(!isPreviewFormat(input) || !isPreviewFormat(output)
       || outputSampleFormat(output.sampleFormat()) == AV_SAMPLE_FMT_NONE || !std::isfinite(linearGain)
       || linearGain < 0.0 || linearGain > std::numeric_limits<float>::max()) {
        error = u"Unsupported preview conversion format or channel layout."_s;
        return false;
    }
    m_input       = input;
    m_output      = output;
    m_linearGain  = linearGain;
    m_passthrough = input == output && linearGain == 1.0;
    if(m_passthrough) {
        return true;
    }
    AVChannelLayout inLayout {};
    AVChannelLayout outLayout {};
    av_channel_layout_default(&inLayout, input.channelCount());
    av_channel_layout_default(&outLayout, output.channelCount());
    int result = swr_alloc_set_opts2(&m_context, &outLayout, outputSampleFormat(output.sampleFormat()),
                                     output.sampleRate(), &inLayout, AV_SAMPLE_FMT_FLT, input.sampleRate(), 0, nullptr);
    av_channel_layout_uninit(&inLayout);
    av_channel_layout_uninit(&outLayout);
    // Matrix rows are output channels, columns are input channels. Mono is
    // duplicated at unity; stereo is averaged, never summed at double gain.
    const double monoToStereo[] {1.0, 1.0};
    const double stereoToMono[] {0.5, 0.5};
    if(result >= 0 && input.channelCount() != output.channelCount()) {
        result
            = swr_set_matrix(m_context, input.channelCount() == 1 ? monoToStereo : stereoToMono, input.channelCount());
    }
    if(result >= 0) {
        result = swr_init(m_context);
    }
    if(result < 0) {
        error = conversionError(result);
        reset();
        return false;
    }
    return true;
}

std::optional<std::vector<std::byte>> PreviewConverter::convert(const AudioBuffer& input, QString& error)
{
    if(m_finished || !m_input.isValid() || !input.isValid() || input.format() != m_input || input.frameCount() <= 0
       || input.frameCount() > 4096) {
        error = u"Invalid, oversized or discontinuous preview conversion input."_s;
        return {};
    }
    if(m_passthrough) {
        return std::vector<std::byte> {input.constData().begin(), input.constData().end()};
    }
    auto floatFormat = m_input;
    floatFormat.setSampleFormat(SampleFormat::F32);
    // Fooyin's S24In32 is a left-aligned S32 lane, matching its public helper.
    auto normalised = Audio::convert(input, floatFormat);
    if(!normalised.isValid()) {
        error = u"Unable to normalise preview PCM to float."_s;
        return {};
    }
    if(m_linearGain != 1.0) {
        // Gain precedes channel mixing/resampling and final integer quantisation.
        // Source peaks cannot guarantee absence of later resampling overshoot.
        constexpr double Limit = std::numeric_limits<float>::max();
        for(size_t offset = 0; offset < normalised.constData().size(); offset += sizeof(float)) {
            float value;
            std::memcpy(&value, normalised.constData().data() + offset, sizeof(value));
            value = std::isfinite(value) ? static_cast<float>(std::clamp(value * m_linearGain, -Limit, Limit)) : 0.0F;
            std::memcpy(normalised.data() + offset, &value, sizeof(value));
        }
    }
    return process(reinterpret_cast<const uint8_t*>(normalised.data()), normalised.frameCount(), error);
}

std::optional<std::vector<std::byte>> PreviewConverter::process(const uint8_t* input, int frames, QString& error)
{
    const int capacity      = swr_get_out_samples(m_context, frames);
    const size_t frameBytes = static_cast<size_t>(m_output.bytesPerFrame());
    if(capacity < 0 || static_cast<uint64_t>(capacity) * frameBytes > MaxStagingBytes) {
        error = u"Preview resampling exceeds the bounded staging capacity."_s;
        return {};
    }
    std::vector<std::byte> output(static_cast<size_t>(std::max(1, capacity)) * frameBytes);
    auto* destination = reinterpret_cast<uint8_t*>(output.data());
    const int written = swr_convert(m_context, &destination, std::max(1, capacity), input ? &input : nullptr, frames);
    if(written < 0) {
        error = conversionError(written);
        return {};
    }
    output.resize(static_cast<size_t>(written) * frameBytes);
    return output;
}

std::optional<std::vector<std::byte>> PreviewConverter::finish(QString& error)
{
    if(m_finished) {
        return std::vector<std::byte> {};
    }
    m_finished = true;
    if(m_passthrough) {
        return std::vector<std::byte> {};
    }
    if(!m_context) {
        error = u"Preview converter is not configured."_s;
        return {};
    }
    std::vector<std::byte> tail;
    for(int attempt = 0; attempt < 8; ++attempt) {
        auto data = process(nullptr, 0, error);
        if(!data) {
            return {};
        }
        if(data->empty()) {
            return tail;
        }
        if(tail.size() + data->size() > MaxStagingBytes) {
            break;
        }
        tail.insert(tail.end(), data->begin(), data->end());
    }
    error = u"Preview resampler tail exceeded the bounded drain limit."_s;
    return {};
}

} // namespace Fooyin::Headphones
