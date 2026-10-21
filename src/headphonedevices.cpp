// SPDX-License-Identifier: GPL-3.0-or-later
#include "headphonedevices.h"

#include "pipewirechannelmaps.h"
#include "previewconverter.h"

#include <QAudioFormat>
#include <QVersionNumber>

#include <algorithm>
#include <array>

using namespace Qt::StringLiterals;

namespace Fooyin::Headphones {
namespace {
using Position = AudioFormat::ChannelPosition;
constexpr std::array Positions {
    std::pair {Position::FrontLeft, QAudioFormat::FrontLeft},
    std::pair {Position::FrontRight, QAudioFormat::FrontRight},
    std::pair {Position::FrontCenter, QAudioFormat::FrontCenter},
    std::pair {Position::LFE, QAudioFormat::LFE},
    std::pair {Position::BackLeft, QAudioFormat::BackLeft},
    std::pair {Position::BackRight, QAudioFormat::BackRight},
    std::pair {Position::FrontLeftOfCenter, QAudioFormat::FrontLeftOfCenter},
    std::pair {Position::FrontRightOfCenter, QAudioFormat::FrontRightOfCenter},
    std::pair {Position::BackCenter, QAudioFormat::BackCenter},
    std::pair {Position::SideLeft, QAudioFormat::SideLeft},
    std::pair {Position::SideRight, QAudioFormat::SideRight},
};

bool isNativePipewire()
{
#ifdef Q_OS_LINUX
    const auto backend = qgetenv("QT_AUDIO_BACKEND");
    // A plugin compiled against older Qt can run on a newer Qt runtime.
    return QVersionNumber::fromString(QString::fromLatin1(qVersion())) >= QVersionNumber {6, 10}
        && (backend.isNull() || backend == "pipewire");
#else
    return false;
#endif
}

bool isDevicePairFormat(const HeadphoneDevice& device, const AudioFormat& format, int first)
{
    return device.discreteChannelMap ? !format.hasChannelLayout() && isPreviewPairPcmFormat(format, first)
                                     : isExplicitPairFormat(format, first);
}
} // namespace

QAudioFormat qtPreviewFormat(const AudioFormat& format)
{
    QAudioFormat result;
    result.setSampleRate(format.sampleRate());
    result.setChannelCount(format.channelCount());
    if(format.hasChannelLayout()) {
        quint32 config = 0;
        int previous   = 0;
        for(const auto position : format.channelLayoutView()) {
            const auto found = std::ranges::find(Positions, position, &decltype(Positions)::value_type::first);
            if(found == Positions.end() || static_cast<int>(found->second) <= previous) {
                return {}; // Qt config masks cannot preserve permuted/unknown channels.
            }
            previous = static_cast<int>(found->second);
            config |= 1U << previous;
        }
        result.setChannelConfig(static_cast<QAudioFormat::ChannelConfig>(config));
    }
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

namespace {
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
    AudioFormat result {sample, format.sampleRate(), format.channelCount()};
    result.clearChannelLayout();
    if(format.channelConfig() != QAudioFormat::ChannelConfigUnknown) {
        AudioFormat::ChannelLayout layout;
        quint32 known = 0;
        for(const auto& [position, qtPosition] : Positions) {
            const auto bit = 1U << qtPosition;
            if(format.channelConfig() & bit) {
                layout.push_back(position);
                known |= bit;
            }
        }
        if(known == format.channelConfig() && static_cast<int>(layout.size()) == format.channelCount()) {
            result.setChannelLayout(layout);
        }
    }
    return result;
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
    if(isNativePipewire()) {
        m_pipewireMaps = std::make_unique<PipewireChannelMaps>();
        if(!m_pipewireMaps->isConnected()) {
            // Qt falls back to PulseAudio when no native PipeWire server exists.
            m_pipewireMaps.reset();
            return;
        }
        QObject::connect(m_pipewireMaps.get(), &PipewireChannelMaps::mapsChanged, this,
                         &HeadphoneDevices::devicesChanged);
    }
}

HeadphoneDevices::~HeadphoneDevices() = default;

std::vector<HeadphoneDevice> HeadphoneDevices::devices() const
{
    std::vector<HeadphoneDevice> result;
    for(const auto& device : QMediaDevices::audioOutputs()) {
        if(device.isNull() || device.id().isEmpty()) {
            continue;
        }
        HeadphoneDevice entry {device.id(),
                               device.description(),
                               fooyinFormat(device.preferredFormat()),
                               [device](const AudioFormat& format) {
                                   const auto candidate = qtPreviewFormat(format);
                                   return candidate.isValid() && device.isFormatSupported(candidate);
                               },
                               device,
                               {}};
        auto mapped = device.preferredFormat();
        // Qt supplies a concrete PCM candidate. Its mask can be guessed on
        // native PipeWire, so verify the actual endpoint map below.
        if(device.channelConfiguration() != QAudioFormat::ChannelConfigUnknown) {
            mapped.setChannelConfig(device.channelConfiguration());
            auto candidate = std::optional {fooyinFormat(mapped)};
            if(m_pipewireMaps) {
                const auto positions     = m_pipewireMaps->positions(device.id());
                candidate                = positions ? pipewirePairFormat(*candidate, *positions) : std::nullopt;
                entry.discreteChannelMap = candidate && !candidate->hasChannelLayout();
            }
            if(candidate && candidate->channelCount() > 2 && isDevicePairFormat(entry, *candidate, 1)
               && entry.supportsFormat(*candidate)) {
                entry.pairFormats.push_back(*candidate);
            }
        }
        result.push_back(std::move(entry));
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

bool isExplicitPairFormat(const AudioFormat& format, int firstChannel)
{
    return isPreviewPairPcmFormat(format, firstChannel) && format.hasChannelLayout()
        && qtPreviewFormat(format).isValid();
}

bool isPreviewPairPcmFormat(const AudioFormat& format, int firstChannel)
{
    return format.isValid() && format.sampleRate() <= 768000 && format.channelCount() <= 8 && firstChannel > 0
        && firstChannel % 2 == 1 && firstChannel < format.channelCount()
        && (format.sampleFormat() == SampleFormat::F32 || format.sampleFormat() == SampleFormat::S16
            || format.sampleFormat() == SampleFormat::S32);
}

std::optional<AudioFormat> pipewirePairFormat(const AudioFormat& preferred, const QStringList& nativePositions)
{
    if(nativePositions.size() != preferred.channelCount() || !isPreviewPairPcmFormat(preferred, 1)) {
        return {};
    }
    bool auxiliary = true;
    for(int channel = 0; channel < nativePositions.size(); ++channel) {
        auxiliary = auxiliary && nativePositions[channel] == QStringLiteral("AUX%1").arg(channel);
    }
    if(auxiliary) {
        auto discrete = preferred;
        discrete.clearChannelLayout();
        // Native Qt PipeWire maps ChannelConfigUnknown to AUX0..N. Retaining
        // Qt's guessed 5.1 config instead downmixes this stream onto AUX0/1.
        return discrete;
    }
    // These are the speaker configurations Qt's native PipeWire backend
    // actually transmits; other masks are translated to AUX by that backend.
    static const QList<QStringList> speakerMaps {
        {u"FL"_s, u"FR"_s},
        {u"FL"_s, u"FR"_s, u"LFE"_s},
        {u"FL"_s, u"FR"_s, u"FC"_s},
        {u"FL"_s, u"FR"_s, u"FC"_s, u"LFE"_s},
        {u"FL"_s, u"FR"_s, u"FC"_s, u"RL"_s, u"RR"_s},
        {u"FL"_s, u"FR"_s, u"FC"_s, u"LFE"_s, u"RL"_s, u"RR"_s},
        {u"FL"_s, u"FR"_s, u"FC"_s, u"RL"_s, u"RR"_s, u"SL"_s, u"SR"_s},
        {u"FL"_s, u"FR"_s, u"FC"_s, u"LFE"_s, u"RL"_s, u"RR"_s, u"SL"_s, u"SR"_s},
    };
    static const std::array<const char*, Positions.size()> names {"FL",  "FR",  "FC", "LFE", "RL", "RR",
                                                                  "FLC", "FRC", "RC", "SL",  "SR"};
    QStringList transmitted;
    for(const auto position : preferred.channelLayoutView()) {
        const auto found = std::ranges::find(Positions, position, &decltype(Positions)::value_type::first);
        if(found == Positions.end()) {
            return {};
        }
        transmitted.push_back(QString::fromLatin1(names[static_cast<size_t>(found - Positions.begin())]));
    }
    return speakerMaps.contains(nativePositions) && transmitted == nativePositions ? std::optional {preferred}
                                                                                   : std::nullopt;
}

std::vector<int> previewChannelPairs(const HeadphoneDevice& device)
{
    std::vector<int> pairs;
    for(int first = 1; first < 8; first += 2) {
        if(device.supportsFormat && std::ranges::any_of(device.pairFormats, [&](const auto& format) {
               return isDevicePairFormat(device, format, first) && device.supportsFormat(format);
           })) {
            pairs.push_back(first);
        }
    }
    return pairs;
}

std::optional<AudioFormat> negotiatePreviewFormat(const HeadphoneDevice& device, const AudioFormat& input,
                                                  int firstChannel)
{
    if(device.id.isEmpty() || !device.supportsFormat || !isPreviewInputFormat(input)) {
        return {};
    }
    if(firstChannel != 0) {
        for(const auto& candidate : device.pairFormats) {
            if(isDevicePairFormat(device, candidate, firstChannel) && device.supportsFormat(candidate)) {
                return candidate;
            }
        }
        return {}; // An explicit destination must never negotiate automatic stereo.
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
            if(isPreviewOutputFormat(candidate) && device.supportsFormat(candidate)) {
                return candidate;
            }
        }
    }
    return {};
}

} // namespace Fooyin::Headphones
