// SPDX-License-Identifier: GPL-3.0-or-later
#include "previewconverter.h"

#include <QTest>

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
        QTest::newRow("44100-to-48000") << 44100 << 48000;
        QTest::newRow("96000-to-48000") << 96000 << 48000;
        QTest::newRow("48000-to-44100") << 48000 << 44100;
    }

    void streamingRateChanges()
    {
        QFETCH(int, inputRate);
        QFETCH(int, outputRate);
        const AudioFormat input {SampleFormat::F32, inputRate, 1};
        PreviewConverter converter;
        QString error;
        QVERIFY(converter.configure(input, {SampleFormat::F32, outputRate, 1}, error));
        std::vector<std::byte> result;
        const size_t inputFrames = inputRate / 10;
        for(size_t start = 0; start < inputFrames;) {
            const auto count = std::min<size_t>(211, inputFrames - start);
            std::vector<float> signal(count);
            for(size_t i = 0; i < count; ++i) {
                signal[i] = static_cast<float>(0.25 * std::sin(2 * std::numbers::pi * 1000 * (start + i) / inputRate));
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
        const auto frames = result.size() / sizeof(float);
        QVERIFY(std::abs(static_cast<int>(frames) - outputRate / 10) <= 1);
        // Verify pitch, gain and continuity across the many input block boundaries.
        for(size_t i = 128; i + 128 < frames; ++i) {
            const auto expected = 0.25 * std::sin(2 * std::numbers::pi * 1000 * i / outputRate);
            QVERIFY(std::abs(floatAt(result, i) - expected) < 0.005);
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
        QVERIFY(!converter.configure({SampleFormat::F32, 48000, 6}, output, error));
        QVERIFY(!converter.configure(input, {SampleFormat::U8, 48000, 1}, error));
        if(converter.configure({SampleFormat::F32, 1, 1}, {SampleFormat::F32, 768000, 2}, error)) {
            QVERIFY(!converter.convert(samples<float>(std::vector<float>(4096), {SampleFormat::F32, 1, 1}), error));
        }
    }
};

QTEST_GUILESS_MAIN(PreviewConverterTest)
#include "previewconvertertest.moc"
