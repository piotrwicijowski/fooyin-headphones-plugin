// SPDX-License-Identifier: GPL-3.0-or-later
#include "previewconverter.h"

#include <QTest>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

using namespace Fooyin;
using namespace Fooyin::Headphones;

namespace {
template <typename T>
AudioBuffer samples(const std::vector<T>& values, const AudioFormat& format)
{
    return {reinterpret_cast<const uint8_t*>(values.data()), values.size() * sizeof(T), format, 0};
}

float floatAt(const std::vector<std::byte>& data, size_t index)
{
    float value {};
    std::memcpy(&value, data.data() + index * sizeof(value), sizeof(value));
    return value;
}
} // namespace

class PreviewConverterTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void explicitOutputPairsPreserveStereoAndSilenceAcrossConversion_data()
    {
        QTest::addColumn<int>("physicalChannels");
        QTest::addColumn<bool>("discrete");
        QTest::newRow("7.1-speakers") << 8 << false;
        QTest::newRow("six-aux-channels") << 6 << true;
    }

    void explicitOutputPairsPreserveStereoAndSilenceAcrossConversion()
    {
        QFETCH(int, physicalChannels);
        QFETCH(bool, discrete);
        for(const auto sample : {SampleFormat::F32, SampleFormat::S16, SampleFormat::S32}) {
            for(const int inputChannels : {1, 2, 6}) {
                for(const int rate : {44100, 48000}) {
                    const AudioFormat input {SampleFormat::F32, 48000, inputChannels};
                    const AudioFormat stereo {sample, rate, 2};
                    AudioFormat physical {sample, rate, physicalChannels};
                    if(discrete) {
                        physical.clearChannelLayout();
                    }
                    std::vector<float> signal(1024 * inputChannels);
                    for(size_t i = 0; i < signal.size(); ++i) {
                        signal[i] = 0.1F * std::sin(static_cast<float>(i) * 0.07F);
                    }
                    for(int pair = 1; pair < physicalChannels; pair += 2) {
                        for(const double gain : {1.0, 0.5}) {
                            PreviewConverter logical, routed;
                            QString error;
                            QVERIFY2(logical.configure(input, stereo, error, gain), qPrintable(error));
                            QVERIFY2(routed.configure(input, physical, error, gain, pair), qPrintable(error));
                            auto expected = logical.convert(samples(signal, input), error);
                            auto actual   = routed.convert(samples(signal, input), error);
                            QVERIFY2(expected && actual, qPrintable(error));
                            const auto verify = [&](const auto& reference, const auto& output) {
                                const size_t sampleBytes = physical.bytesPerSample();
                                const size_t frames      = reference.size() / stereo.bytesPerFrame();
                                QCOMPARE(output.size(), frames * physical.bytesPerFrame());
                                for(size_t frame = 0; frame < frames; ++frame) {
                                    for(int channel = 0; channel < physicalChannels; ++channel) {
                                        const size_t offset = (frame * physicalChannels + channel) * sampleBytes;
                                        if(channel == pair - 1 || channel == pair) {
                                            const size_t source = (frame * 2 + channel - pair + 1) * sampleBytes;
                                            QVERIFY(std::equal(output.begin() + offset,
                                                               output.begin() + offset + sampleBytes,
                                                               reference.begin() + source));
                                        }
                                        else {
                                            QVERIFY(std::all_of(output.begin() + offset,
                                                                output.begin() + offset + sampleBytes,
                                                                [](std::byte byte) { return byte == std::byte {0}; }));
                                        }
                                    }
                                }
                            };
                            verify(*expected, *actual);
                            const auto expectedTail = logical.finish(error);
                            const auto actualTail   = routed.finish(error);
                            QVERIFY2(expectedTail && actualTail, qPrintable(error));
                            verify(*expectedTail, *actualTail);
                            QVERIFY(routed.finish(error)->empty());
                            // Reconfiguration discards every previous route and resampler tail.
                            QVERIFY(routed.configure(input, stereo, error));
                            QCOMPARE(routed.convert(samples(signal, input), error)->size(), expected->size());
                        }
                    }
                }
            }
        }
    }

    void explicitOutputDoesNotBroadenInputOrAutomaticOutputValidation()
    {
        PreviewConverter converter;
        QString error;
        const AudioFormat wide {SampleFormat::F32, 48000, 8};
        const AudioFormat stereo {SampleFormat::F32, 48000, 2};
        QVERIFY(!converter.configure(stereo, wide, error));
        for(const int pair : {-1, 2, 8, 9}) {
            QVERIFY(!converter.configure(stereo, wide, error, 1.0, pair));
        }
        auto unknown = wide;
        unknown.clearChannelLayout();
        // Output approval can describe raw numbered AUX slots. This does not
        // permit unlabelled multichannel *input* or automatic wide output.
        QVERIFY(converter.configure(stereo, unknown, error, 1.0, 3));
        QVERIFY(!converter.configure(unknown, wide, error, 1.0, 3));
    }

    void routedExpansionEnforcesPhysicalStagingLimit()
    {
        PreviewConverter converter;
        QString error;
        const AudioFormat input {SampleFormat::F32, 8000, 1};
        QVERIFY(converter.configure(input, {SampleFormat::F32, 768000, 8}, error, 1.0, 7));
        const std::vector<float> signal(PreviewBlockFrames, 0.25F);
        QVERIFY(!converter.convert(samples(signal, input), error));
        QVERIFY(error.contains(QStringLiteral("bounded staging")));
    }

    void multichannelRouting_data()
    {
        using P      = AudioFormat::ChannelPosition;
        using Layout = AudioFormat::ChannelLayout;
        QTest::addColumn<AudioFormat>("input");
        QTest::addColumn<std::vector<double>>("left");
        QTest::addColumn<std::vector<double>>("right");
        const double c = 1.0 / std::sqrt(2.0);
        const auto row = [&](const char* name, Layout layout, std::vector<double> left, std::vector<double> right,
                             double divisor) {
            for(auto& value : left) {
                value /= divisor;
            }
            for(auto& value : right) {
                value /= divisor;
            }
            for(bool reordered : {false, true}) {
                if(reordered) {
                    std::reverse(layout.begin(), layout.end());
                    std::reverse(left.begin(), left.end());
                    std::reverse(right.begin(), right.end());
                    std::rotate(layout.begin(), layout.begin() + 1, layout.end());
                    std::rotate(left.begin(), left.begin() + 1, left.end());
                    std::rotate(right.begin(), right.begin() + 1, right.end());
                }
                AudioFormat format {SampleFormat::F32, 48000, static_cast<int>(layout.size())};
                QVERIFY(format.setChannelLayout(layout));
                const auto label = QByteArray {name} + (reordered ? "-permuted" : "-native");
                QTest::newRow(label) << format << left << right;
            }
        };
        row("3.0", {P::FrontLeft, P::FrontRight, P::FrontCenter}, {1, 0, c}, {0, 1, c}, 1 + c);
        row("quad-back", {P::FrontLeft, P::FrontRight, P::BackLeft, P::BackRight}, {1, 0, c, 0}, {0, 1, 0, c}, 1 + c);
        row("quad-side", {P::FrontLeft, P::FrontRight, P::SideLeft, P::SideRight}, {1, 0, c, 0}, {0, 1, 0, c}, 1 + c);
        row("5.0-back", {P::FrontLeft, P::FrontRight, P::FrontCenter, P::BackLeft, P::BackRight}, {1, 0, c, c, 0},
            {0, 1, c, 0, c}, 1 + 2 * c);
        row("5.0-side", {P::FrontLeft, P::FrontRight, P::FrontCenter, P::SideLeft, P::SideRight}, {1, 0, c, c, 0},
            {0, 1, c, 0, c}, 1 + 2 * c);
        row("5.1-back", {P::FrontLeft, P::FrontRight, P::FrontCenter, P::LFE, P::BackLeft, P::BackRight},
            {1, 0, c, 0, c, 0}, {0, 1, c, 0, 0, c}, 1 + 2 * c);
        row("5.1-side", {P::FrontLeft, P::FrontRight, P::FrontCenter, P::LFE, P::SideLeft, P::SideRight},
            {1, 0, c, 0, c, 0}, {0, 1, c, 0, 0, c}, 1 + 2 * c);
        row("7.1",
            {P::FrontLeft, P::FrontRight, P::FrontCenter, P::LFE, P::BackLeft, P::BackRight, P::SideLeft, P::SideRight},
            {1, 0, c, 0, c, 0, c, 0}, {0, 1, c, 0, 0, c, 0, c}, 1 + 3 * c);
        row("7.1-wide-side",
            {P::FrontLeft, P::FrontRight, P::FrontCenter, P::LFE, P::FrontLeftOfCenter, P::FrontRightOfCenter,
             P::SideLeft, P::SideRight},
            {1, 0, c, 0, c, 0, c, 0}, {0, 1, c, 0, 0, c, 0, c}, 1 + 3 * c);
        row("7.1-wide-back",
            {P::FrontLeft, P::FrontRight, P::FrontCenter, P::LFE, P::BackLeft, P::BackRight, P::FrontLeftOfCenter,
             P::FrontRightOfCenter},
            {1, 0, c, 0, c, 0, c, 0}, {0, 1, c, 0, 0, c, 0, c}, 1 + 3 * c);
    }

    void multichannelRouting()
    {
        QFETCH(AudioFormat, input);
        QFETCH(std::vector<double>, left);
        QFETCH(std::vector<double>, right);
        const auto count = static_cast<size_t>(input.channelCount());
        // One isolated impulse per input lane, followed by correlated +/- full
        // scale. Integer normalisation must retain a non-native source layout.
        std::vector<float> signal((count + 2) * count, 0.0F);
        for(size_t i = 0; i < count; ++i) {
            signal[i * count + i] = 0.5F;
        }
        std::fill_n(signal.begin() + count * count, count, 0.5F);
        std::fill_n(signal.begin() + (count + 1) * count, count, -0.5F);
        for(const auto sample : {SampleFormat::F32, SampleFormat::S16}) {
            input.setSampleFormat(sample);
            std::vector<int16_t> integer(signal.size());
            std::ranges::transform(signal, integer.begin(),
                                   [](float value) { return static_cast<int16_t>(value * 32768); });
            const auto buffer = sample == SampleFormat::F32 ? samples(signal, input) : samples(integer, input);
            for(int channels : {2, 1}) {
                PreviewConverter converter;
                QString error;
                QVERIFY2(converter.configure(input, {SampleFormat::F32, 48000, channels}, error, 2.0),
                         qPrintable(error));
                auto output = converter.convert(buffer, error);
                QVERIFY2(output.has_value(), qPrintable(error));
                QCOMPARE(output->size(), (count + 2) * channels * sizeof(float));
                for(size_t i = 0; i < count; ++i) {
                    if(channels == 2) {
                        QVERIFY(std::abs(floatAt(*output, i * 2) - left[i]) < 0.000001);
                        QVERIFY(std::abs(floatAt(*output, i * 2 + 1) - right[i]) < 0.000001);
                    }
                    else {
                        QVERIFY(std::abs(floatAt(*output, i) - (left[i] + right[i]) / 2) < 0.000001);
                    }
                }
                for(int side = 0; side < channels; ++side) {
                    const auto positive = floatAt(*output, count * channels + side);
                    const auto negative = floatAt(*output, (count + 1) * channels + side);
                    QVERIFY(std::abs(positive - 1.0F) < 0.000001F);
                    QVERIFY(std::abs(negative + 1.0F) < 0.000001F);
                    QVERIFY(std::abs(positive) <= 1.000001F && std::abs(negative) <= 1.000001F);
                }
                QVERIFY(converter.finish(error)->empty());
            }
        }
    }

    void rejectsUnknownAndUnsupportedLayouts()
    {
        using P = AudioFormat::ChannelPosition;
        std::vector<AudioFormat> formats {{SampleFormat::F32, 48000, 7},
                                          {SampleFormat::F32, 48000, 32},
                                          {SampleFormat::F32, 48000, 33},
                                          {SampleFormat::F32, 768001, 6}};
        AudioFormat missing {SampleFormat::F32, 48000, 6};
        missing.clearChannelLayout();
        formats.push_back(missing);
        for(const auto& layout : std::vector<AudioFormat::ChannelLayout> {
                {P::FrontLeft, P::FrontRight, P::FrontCenter, P::LFE, P::SideLeft, P::SideLeft},
                {P::FrontLeft, P::FrontRight, P::FrontCenter, P::LFE, P::SideLeft, P::UnknownPosition},
                {P::FrontLeft, P::FrontRight, P::FrontCenter, P::LFE, P::TopFrontLeft, P::TopFrontRight},
                {P::FrontLeft, P::FrontRight, P::FrontCenter, P::LFE, P::BackCenter, P::SideRight}}) {
            AudioFormat format {SampleFormat::F32, 48000, 6};
            QVERIFY(format.setChannelLayout(layout));
            formats.push_back(format);
        }
        for(const auto& format : formats) {
            PreviewConverter converter;
            QString error;
            QVERIFY(!converter.configure(format, {SampleFormat::F32, 48000, 2}, error));
            QVERIFY(!error.isEmpty());
            QVERIFY(!converter.finish(error));
        }
        PreviewConverter converter;
        QString error;
        AudioFormat input {SampleFormat::F32, 48000, 6};
        QVERIFY(converter.configure(input, {SampleFormat::F32, 48000, 2}, error));
        auto changed = input.channelLayout();
        std::swap(changed[0], changed[1]);
        QVERIFY(input.setChannelLayout(changed));
        QVERIFY(!converter.convert(samples<float>(std::vector<float>(6), input), error));
        input = {SampleFormat::F32, 48000, 6};
        AudioBuffer malformed {input, 0};
        malformed.resize(25);
        QVERIFY(!converter.convert(malformed, error));
        QVERIFY(!converter.convert(samples<float>(std::vector<float>(4097 * 6), input), error));
        QVERIFY(!converter.configure(input, input, error)); // Outputs remain mono/stereo.
    }

    void normalisesSampleContainers_data()
    {
        QTest::addColumn<int>("sample");
        for(auto type : {SampleFormat::U8, SampleFormat::S16, SampleFormat::S24In32, SampleFormat::S32,
                         SampleFormat::F32, SampleFormat::F64}) {
            QTest::newRow(qPrintable(QString::number(static_cast<int>(type)))) << static_cast<int>(type);
        }
    }

    void normalisesSampleContainers()
    {
        QFETCH(int, sample);
        const auto type = static_cast<SampleFormat>(sample);
        const AudioFormat input {type, 48000, 1};
        AudioBuffer data;
        switch(type) {
            case SampleFormat::U8:
                data = samples<uint8_t>({64, 128, 192}, input);
                break;
            case SampleFormat::S16:
                data = samples<int16_t>({-16384, 0, 16384}, input);
                break;
            case SampleFormat::S24In32:
            case SampleFormat::S32:
                data = samples<int32_t>({-1073741824, 0, 1073741824}, input);
                break;
            case SampleFormat::F32:
                data = samples<float>({-0.5F, 0.0F, 0.5F}, input);
                break;
            case SampleFormat::F64:
                data = samples<double>({-0.5, 0.0, 0.5}, input);
                break;
            default:
                QFAIL("Unknown fixture sample type");
        }
        PreviewConverter converter;
        QString error;
        QVERIFY2(converter.configure(input, {SampleFormat::F32, 48000, 1}, error), qPrintable(error));
        auto output = converter.convert(data, error);
        QVERIFY2(output.has_value(), qPrintable(error));
        QCOMPARE(output->size(), size_t {12});
        QCOMPARE(floatAt(*output, 0), -0.5F);
        QCOMPARE(floatAt(*output, 1), 0.0F);
        QCOMPARE(floatAt(*output, 2), 0.5F);
        auto tail = converter.finish(error);
        QVERIFY(tail);
        QVERIFY(tail->empty());
        QVERIFY(converter.finish(error)->empty());
    }

    void unityDuplicationAndAveragedDownmix()
    {
        PreviewConverter converter;
        QString error;
        const AudioFormat mono {SampleFormat::F32, 48000, 1};
        const AudioFormat stereo {SampleFormat::F32, 48000, 2};
        QVERIFY(converter.configure(mono, stereo, error));
        auto duplicated = converter.convert(samples<float>({0.25F, -0.5F}, mono), error);
        QVERIFY(duplicated);
        QCOMPARE(duplicated->size(), size_t {16});
        QCOMPARE(floatAt(*duplicated, 0), 0.25F);
        QCOMPARE(floatAt(*duplicated, 1), 0.25F);
        QCOMPARE(floatAt(*duplicated, 2), -0.5F);
        QCOMPARE(floatAt(*duplicated, 3), -0.5F);
        QVERIFY(converter.configure(stereo, mono, error));
        auto averaged = converter.convert(samples<float>({1.0F, -1.0F, 0.8F, 0.4F}, stereo), error);
        QVERIFY(averaged);
        QCOMPARE(floatAt(*averaged, 0), 0.0F);
        QVERIFY(std::abs(floatAt(*averaged, 1) - 0.6F) < 0.00001F);
    }

    void integerOutputScaling()
    {
        const AudioFormat input {SampleFormat::F32, 48000, 1};
        for(const auto type : {SampleFormat::S16, SampleFormat::S32}) {
            PreviewConverter converter;
            QString error;
            QVERIFY(converter.configure(input, {type, 48000, 1}, error));
            auto output = converter.convert(samples<float>({-0.5F, 0.0F, 0.5F}, input), error);
            QVERIFY(output);
            if(type == SampleFormat::S16) {
                int16_t values[3] {};
                std::memcpy(values, output->data(), sizeof(values));
                QCOMPARE(values[0], int16_t {-16384});
                QCOMPARE(values[2], int16_t {16384});
            }
            else {
                int32_t values[3] {};
                std::memcpy(values, output->data(), sizeof(values));
                QCOMPARE(values[0], int32_t {-1073741824});
                QCOMPARE(values[2], int32_t {1073741824});
            }
        }
    }

    void streamingRateChanges_data()
    {
        QTest::addColumn<int>("inputRate");
        QTest::addColumn<int>("outputRate");
        QTest::addColumn<int>("inputChannels");
        QTest::addColumn<int>("outputChannels");
        QTest::addColumn<int>("outputSample");
        for(const auto rates : {std::pair {44100, 48000}, std::pair {96000, 48000}, std::pair {48000, 44100}}) {
            for(int input : {1, 6, 8}) {
                for(int output : {1, 2}) {
                    for(auto sample : {SampleFormat::F32, SampleFormat::S16, SampleFormat::S32}) {
                        const auto label = QStringLiteral("%1-to-%2-%3ch-to-%4ch-%5")
                                               .arg(rates.first)
                                               .arg(rates.second)
                                               .arg(input)
                                               .arg(output)
                                               .arg(static_cast<int>(sample));
                        QTest::newRow(qPrintable(label))
                            << rates.first << rates.second << input << output << static_cast<int>(sample);
                    }
                }
            }
        }
    }

    void streamingRateChanges()
    {
        QFETCH(int, inputRate);
        QFETCH(int, outputRate);
        QFETCH(int, inputChannels);
        QFETCH(int, outputChannels);
        QFETCH(int, outputSample);
        const AudioFormat input {SampleFormat::F32, inputRate, inputChannels};
        const auto sample = static_cast<SampleFormat>(outputSample);
        const AudioFormat output {sample, outputRate, outputChannels};
        PreviewConverter converter;
        QString error;
        QVERIFY(converter.configure(input, output, error));
        std::vector<std::byte> result;
        const size_t inputFrames = inputRate / 10;
        for(size_t start = 0; start < inputFrames;) {
            const auto count = std::min<size_t>(211, inputFrames - start);
            std::vector<float> signal(count * inputChannels);
            for(size_t i = 0; i < count; ++i) {
                const auto value
                    = static_cast<float>(0.25 * std::sin(2 * std::numbers::pi * 1000 * (start + i) / inputRate));
                std::fill_n(signal.begin() + i * inputChannels, inputChannels, value);
            }
            auto block = converter.convert(samples(signal, input), error);
            QVERIFY2(block.has_value(), qPrintable(error));
            QVERIFY(block->size() < 8 * 1024 * 1024);
            result.insert(result.end(), block->begin(), block->end());
            start += count;
        }
        const auto beforeTail = result.size();
        auto tail             = converter.finish(error);
        QVERIFY2(tail.has_value(), qPrintable(error));
        QVERIFY(!tail->empty());
        result.insert(result.end(), tail->begin(), tail->end());
        QVERIFY(result.size() > beforeTail);
        QVERIFY(converter.finish(error)->empty());
        const auto frames = result.size() / output.bytesPerFrame();
        QVERIFY(std::abs(static_cast<int>(frames) - outputRate / 10) <= 1);
        // Verify pitch, gain and continuity across the many input block boundaries.
        for(size_t i = 128; i + 128 < frames; ++i) {
            const auto expected = 0.25 * std::sin(2 * std::numbers::pi * 1000 * i / outputRate);
            for(int channel = 0; channel < outputChannels; ++channel) {
                const auto index = i * outputChannels + channel;
                double actual;
                if(sample == SampleFormat::F32) {
                    actual = floatAt(result, index);
                }
                else if(sample == SampleFormat::S16) {
                    int16_t value;
                    std::memcpy(&value, result.data() + index * sizeof(value), sizeof(value));
                    actual = value / 32768.0;
                }
                else {
                    int32_t value;
                    std::memcpy(&value, result.data() + index * sizeof(value), sizeof(value));
                    actual = value / 2147483648.0;
                }
                QVERIFY(std::abs(actual - expected) < 0.005);
            }
        }
    }

    void resetDiscardsTailAndInvalidInputIsBounded()
    {
        PreviewConverter converter;
        QString error;
        const AudioFormat input {SampleFormat::F32, 44100, 1};
        const AudioFormat output {SampleFormat::F32, 48000, 1};
        QVERIFY(converter.configure(input, output, error));
        QVERIFY(converter.convert(samples<float>(std::vector<float>(200, 0.75F), input), error));
        converter.reset();
        QVERIFY(converter.configure(input, output, error));
        auto block = converter.convert(samples<float>(std::vector<float>(200, 0.0F), input), error);
        auto tail  = converter.finish(error);
        QVERIFY(block && tail);
        for(const auto& data : {*block, *tail}) {
            for(size_t i = 0; i < data.size() / sizeof(float); ++i) {
                QCOMPARE(floatAt(data, i), 0.0F);
            }
        }
        QVERIFY(!converter.convert(samples<float>({0.0F}, input), error));
        QVERIFY(converter.configure(input, output, error));
        QVERIFY(!converter.convert(samples<float>(std::vector<float>(4097), input), error));
        auto missingLayout = AudioFormat {SampleFormat::F32, 48000, 6};
        missingLayout.clearChannelLayout();
        QVERIFY(!converter.configure(missingLayout, output, error));
        QVERIFY(!converter.configure(input, {SampleFormat::U8, 48000, 1}, error));
        if(converter.configure({SampleFormat::F32, 1, 1}, {SampleFormat::F32, 768000, 2}, error)) {
            QVERIFY(!converter.convert(samples<float>(std::vector<float>(4096), {SampleFormat::F32, 1, 1}), error));
        }
    }
};

QTEST_GUILESS_MAIN(PreviewConverterTest)
#include "previewconvertertest.moc"
