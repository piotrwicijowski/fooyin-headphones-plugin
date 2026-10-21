// SPDX-License-Identifier: GPL-3.0-or-later
#include "previewconverter.h"

#include "headphonedevices.h"

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
#include <numbers>

#if LIBSWRESAMPLE_VERSION_MAJOR < 4 || LIBAVUTIL_VERSION_MAJOR < 57
#error "Headphones Preview requires FFmpeg 5+ (libswresample 4+, libavutil 57+)"
#endif

using namespace Qt::StringLiterals;

namespace Fooyin::Headphones {
namespace {
using Position = AudioFormat::ChannelPosition;

AVChannel avChannel(Position position)
{
    switch(position) {
        case Position::FrontLeft:
            return AV_CHAN_FRONT_LEFT;
        case Position::FrontRight:
            return AV_CHAN_FRONT_RIGHT;
        case Position::FrontCenter:
            return AV_CHAN_FRONT_CENTER;
        case Position::LFE:
            return AV_CHAN_LOW_FREQUENCY;
        case Position::BackLeft:
            return AV_CHAN_BACK_LEFT;
        case Position::BackRight:
            return AV_CHAN_BACK_RIGHT;
        case Position::SideLeft:
            return AV_CHAN_SIDE_LEFT;
        case Position::SideRight:
            return AV_CHAN_SIDE_RIGHT;
        case Position::FrontLeftOfCenter:
            return AV_CHAN_FRONT_LEFT_OF_CENTER;
        case Position::FrontRightOfCenter:
            return AV_CHAN_FRONT_RIGHT_OF_CENTER;
        default:
            return AV_CHAN_NONE;
    }
}

uint64_t inputLayoutMask(const AudioFormat& format)
{
    if(format.channelCount() <= 2) {
        if(!format.hasChannelLayout()
           || format.channelLayout() == AudioFormat::defaultChannelLayoutForChannelCount(format.channelCount())) {
            return format.channelCount() == 1 ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
        }
        return 0;
    }
    if(!format.hasChannelLayout()) {
        return 0; // Never manufacture multichannel roles from the channel count.
    }
    uint64_t mask = 0;
    for(const auto position : format.channelLayoutView()) {
        const auto channel = avChannel(position);
        if(channel == AV_CHAN_NONE) {
            return 0;
        }
        const uint64_t bit = uint64_t {1} << channel;
        if(mask & bit) {
            return 0;
        }
        mask |= bit;
    }
    for(const uint64_t supported : {AV_CH_LAYOUT_SURROUND, AV_CH_LAYOUT_QUAD, AV_CH_LAYOUT_2_2, AV_CH_LAYOUT_5POINT0,
                                    AV_CH_LAYOUT_5POINT0_BACK, AV_CH_LAYOUT_5POINT1, AV_CH_LAYOUT_5POINT1_BACK,
                                    AV_CH_LAYOUT_7POINT1, AV_CH_LAYOUT_7POINT1_WIDE, AV_CH_LAYOUT_7POINT1_WIDE_BACK}) {
        if(mask == supported) {
            return mask;
        }
    }
    return 0;
}

// Rows are stereo output, columns are FFmpeg's native channel order. A separate
// input map preserves the decoder's actual interleaving, even for permuted roles.
std::vector<double> downmixMatrix(const AVChannelLayout& layout, int outputChannels)
{
    const auto channels = static_cast<size_t>(layout.nb_channels);
    std::vector<double> stereo(2 * channels, 0.0);
    constexpr double Mix = 1.0 / std::numbers::sqrt2;
    for(size_t i = 0; i < channels; ++i) {
        const auto channel = av_channel_layout_channel_from_index(&layout, i);
        switch(channel) {
            case AV_CHAN_FRONT_LEFT:
                stereo[i] = 1.0;
                break;
            case AV_CHAN_FRONT_RIGHT:
                stereo[channels + i] = 1.0;
                break;
            case AV_CHAN_FRONT_CENTER:
                stereo[i] = stereo[channels + i] = Mix;
                break;
            case AV_CHAN_BACK_LEFT:
            case AV_CHAN_SIDE_LEFT:
            case AV_CHAN_FRONT_LEFT_OF_CENTER:
                stereo[i] = Mix;
                break;
            case AV_CHAN_BACK_RIGHT:
            case AV_CHAN_SIDE_RIGHT:
            case AV_CHAN_FRONT_RIGHT_OF_CENTER:
                stereo[channels + i] = Mix;
                break;
            default:
                break; // LFE deliberately omitted.
        }
    }
    double left = 0.0, right = 0.0;
    for(size_t i = 0; i < channels; ++i) {
        left += std::abs(stereo[i]);
        right += std::abs(stereo[channels + i]);
    }
    const double divisor = std::max({1.0, left, right});
    for(auto& gain : stereo) {
        gain /= divisor;
    }
    if(outputChannels == 1) {
        for(size_t i = 0; i < channels; ++i) {
            stereo[i] = 0.5 * (stereo[i] + stereo[channels + i]);
        }
        stereo.resize(channels);
    }
    return stereo;
}

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

bool isPreviewInputFormat(const AudioFormat& format, QString* error)
{
    QString reason;
    if(!format.isValid() || format.sampleRate() > 768000 || format.bytesPerSample() <= 0) {
        reason = u"Preview requires valid PCM at a sample rate no higher than 768 kHz."_s;
    }
    else if(format.channelCount() > 8 || inputLayoutMask(format) == 0) {
        reason = u"Unsupported preview input channel layout (%1). Multichannel preview requires identified, unique "
                 "speaker roles in a supported 3.0, quad, 5.0, 5.1 or 7.1 layout."_s.arg(format.prettyChannelLayout());
    }
    if(error) {
        *error = reason;
    }
    return reason.isEmpty();
}

bool isPreviewOutputFormat(const AudioFormat& format)
{
    return format.isValid() && format.sampleRate() <= 768000 && format.channelCount() <= 2
        && outputSampleFormat(format.sampleFormat()) != AV_SAMPLE_FMT_NONE
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
    m_input          = {};
    m_output         = {};
    m_physicalOutput = {};
    m_firstChannel   = 0;
    m_passthrough    = false;
    m_finished       = false;
    m_linearGain     = 1.0;
}

bool PreviewConverter::configure(const AudioFormat& input, const AudioFormat& physicalOutput, QString& error,
                                 double linearGain, int firstChannel)
{
    reset();
    auto output = physicalOutput;
    if(firstChannel != 0) {
        if(!isExplicitPairFormat(physicalOutput, firstChannel)) {
            error = u"The selected headphone channel pair or output layout is unavailable."_s;
            return false;
        }
        output = AudioFormat {physicalOutput.sampleFormat(), physicalOutput.sampleRate(), 2};
    }
    if(!isPreviewInputFormat(input, &error)) {
        return false;
    }
    if(!isPreviewOutputFormat(output) || !std::isfinite(linearGain) || linearGain < 0.0
       || linearGain > std::numeric_limits<float>::max()) {
        error = u"Unsupported preview conversion format or channel layout."_s;
        return false;
    }
    m_input          = input;
    m_output         = output;
    m_physicalOutput = physicalOutput;
    m_firstChannel   = firstChannel;
    m_linearGain     = linearGain;
    m_passthrough    = input == output && linearGain == 1.0;
    if(m_passthrough) {
        return true;
    }
    AVChannelLayout inLayout {};
    AVChannelLayout outLayout {};
    av_channel_layout_from_mask(&inLayout, inputLayoutMask(input));
    av_channel_layout_default(&outLayout, output.channelCount());
    int result = swr_alloc_set_opts2(&m_context, &outLayout, outputSampleFormat(output.sampleFormat()),
                                     output.sampleRate(), &inLayout, AV_SAMPLE_FMT_FLT, input.sampleRate(), 0, nullptr);
    // Matrix rows are output channels, columns are input channels. Mono is
    // duplicated at unity; stereo is averaged, never summed at double gain.
    const double monoToStereo[] {1.0, 1.0};
    const double stereoToMono[] {0.5, 0.5};
    if(result >= 0 && input.channelCount() > 2) {
        for(int i = 0; i < input.channelCount(); ++i) {
            const auto channel = av_channel_layout_channel_from_index(&inLayout, i);
            for(int source = 0; source < input.channelCount(); ++source) {
                if(avChannel(input.channelPosition(source)) == channel) {
                    m_channelMap[i] = source;
                    break;
                }
            }
        }
        result            = swr_set_channel_mapping(m_context, m_channelMap.data());
        const auto matrix = downmixMatrix(inLayout, output.channelCount());
        if(result >= 0) {
            result = swr_set_matrix(m_context, matrix.data(), input.channelCount());
        }
    }
    else if(result >= 0 && input.channelCount() != output.channelCount()) {
        result
            = swr_set_matrix(m_context, input.channelCount() == 1 ? monoToStereo : stereoToMono, input.channelCount());
    }
    if(result >= 0) {
        result = swr_init(m_context);
    }
    av_channel_layout_uninit(&inLayout);
    av_channel_layout_uninit(&outLayout);
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
       || static_cast<size_t>(input.frameCount()) > PreviewBlockFrames
       || input.constData().size() > PreviewMaxBufferBytes) {
        error = u"Invalid, oversized or discontinuous preview conversion input."_s;
        return {};
    }
    if(m_passthrough) {
        return route(std::vector<std::byte> {input.constData().begin(), input.constData().end()}, error);
    }
    auto floatFormat = m_input;
    floatFormat.setSampleFormat(SampleFormat::F32);
    if(static_cast<size_t>(input.frameCount()) > PreviewMaxBufferBytes / floatFormat.bytesPerFrame()) {
        error = u"Preview float PCM exceeds the bounded staging capacity."_s;
        return {};
    }
    // Fooyin's S24In32 is a left-aligned S32 lane, matching its public helper.
    // Copying the layout is essential: a count-only format can remap the roles.
    auto normalised = Audio::convert(input, floatFormat);
    if(!normalised.isValid() || normalised.format() != floatFormat || normalised.frameCount() != input.frameCount()) {
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
    auto converted = process(reinterpret_cast<const uint8_t*>(normalised.data()), normalised.frameCount(), error);
    return converted ? route(std::move(*converted), error) : std::nullopt;
}

std::optional<std::vector<std::byte>> PreviewConverter::route(std::vector<std::byte> stereo, QString& error) const
{
    if(m_firstChannel == 0) {
        return stereo;
    }
    const size_t stereoFrame   = static_cast<size_t>(m_output.bytesPerFrame());
    const size_t physicalFrame = static_cast<size_t>(m_physicalOutput.bytesPerFrame());
    const size_t frames        = stereo.size() / stereoFrame;
    if(stereo.size() % stereoFrame != 0 || frames > PreviewMaxBufferBytes / physicalFrame) {
        error = u"Routed preview PCM exceeds the bounded staging capacity."_s;
        return {};
    }
    std::vector<std::byte> output(frames * physicalFrame, std::byte {0});
    const auto offset = static_cast<size_t>(m_firstChannel - 1) * m_physicalOutput.bytesPerSample();
    for(size_t frame = 0; frame < frames; ++frame) {
        std::memcpy(output.data() + frame * physicalFrame + offset, stereo.data() + frame * stereoFrame, stereoFrame);
    }
    return output;
}

std::optional<std::vector<std::byte>> PreviewConverter::process(const uint8_t* input, int frames, QString& error)
{
    const int capacity      = swr_get_out_samples(m_context, frames);
    const size_t frameBytes = static_cast<size_t>(m_output.bytesPerFrame());
    if(capacity < 0 || static_cast<size_t>(std::max(1, capacity)) > PreviewMaxBufferBytes / frameBytes) {
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
            return route(std::move(tail), error);
        }
        if(data->size() > PreviewMaxBufferBytes - tail.size()) {
            break;
        }
        tail.insert(tail.end(), data->begin(), data->end());
    }
    error = u"Preview resampler tail exceeded the bounded drain limit."_s;
    return {};
}

} // namespace Fooyin::Headphones
