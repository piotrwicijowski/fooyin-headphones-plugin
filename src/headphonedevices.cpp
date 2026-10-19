// SPDX-License-Identifier: GPL-3.0-or-later
#include "headphonedevices.h"

#include "previewconverter.h"

#include <QAudioFormat>

namespace Fooyin::Headphones {
namespace {
QAudioFormat qtFormat(const AudioFormat& format)
{
    QAudioFormat result;
    result.setSampleRate(format.sampleRate());
    result.setChannelCount(format.channelCount());
    switch(format.sampleFormat()) {
        case SampleFormat::F32:
            result.setSampleFormat(QAudioFormat::Float);
            break;
        case SampleFormat::S16:
            result.setSampleFormat(QAudioFormat::Int16);
            break;
        case SampleFormat::S32:
            result.setSampleFormat(QAudioFormat::Int32);
            break;
        default:
            break;
    }
    return result;
}

AudioFormat fooyinFormat(const QAudioFormat& format)
{
    SampleFormat sample {SampleFormat::Unknown};
    switch(format.sampleFormat()) {
        case QAudioFormat::Float:
            sample = SampleFormat::F32;
            break;
        case QAudioFormat::Int16:
            sample = SampleFormat::S16;
            break;
        case QAudioFormat::Int32:
            sample = SampleFormat::S32;
            break;
        default:
            break;
    }
    return {sample, format.sampleRate(), format.channelCount()};
}
} // namespace

std::optional<HeadphoneDevice> HeadphoneDeviceCatalogue::resolve(const QByteArray& id) const
{
    if(id.isEmpty()) {
        return {};
    }
    for(const auto& device : devices()) {
        if(device.id == id) {
            return device;
        }
    }
    return {};
}

HeadphoneDevices::HeadphoneDevices(QObject* parent)
    : HeadphoneDeviceCatalogue {parent}
{
    QObject::connect(&m_mediaDevices, &QMediaDevices::audioOutputsChanged, this, &HeadphoneDevices::devicesChanged);
}

std::vector<HeadphoneDevice> HeadphoneDevices::devices() const
{
    std::vector<HeadphoneDevice> result;
    for(const auto& device : QMediaDevices::audioOutputs()) {
        if(device.isNull() || device.id().isEmpty()) {
            continue;
        }
        result.push_back({device.id(), device.description(), fooyinFormat(device.preferredFormat()),
                          [device](const AudioFormat& format) { return device.isFormatSupported(qtFormat(format)); },
                          device});
    }
    return result;
}

QString encodeDeviceId(const QByteArray& id)
{
    return QString::fromLatin1(id.toBase64());
}

QByteArray decodeDeviceId(const QString& encoded)
{
    const auto decoded = QByteArray::fromBase64Encoding(encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    return decoded ? decoded.decoded : QByteArray {};
}

std::optional<AudioFormat> negotiatePreviewFormat(const HeadphoneDevice& device, const AudioFormat& input)
{
    if(device.id.isEmpty() || !device.supportsFormat || !isPreviewFormat(input)) {
        return {};
    }
    for(const int channels : {2, 1}) {
        std::vector<AudioFormat> candidates {{SampleFormat::F32, input.sampleRate(), channels}};
        if(device.preferredFormat.channelCount() == channels) {
            candidates.push_back(device.preferredFormat);
        }
        for(const int rate : {device.preferredFormat.sampleRate(), input.sampleRate(), 48000, 44100}) {
            for(const auto sample : {SampleFormat::F32, SampleFormat::S16, SampleFormat::S32}) {
                candidates.emplace_back(sample, rate, channels);
            }
        }
        for(const auto& candidate : candidates) {
            const auto sample = candidate.sampleFormat();
            if(isPreviewFormat(candidate)
               && (sample == SampleFormat::F32 || sample == SampleFormat::S16 || sample == SampleFormat::S32)
               && device.supportsFormat(candidate)) {
                return candidate;
            }
        }
    }
    return {};
}

} // namespace Fooyin::Headphones
