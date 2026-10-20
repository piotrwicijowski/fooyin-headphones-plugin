// SPDX-License-Identifier: GPL-3.0-or-later
#include "fakes/previewfakes.h"
#include "previewconverter.h"
#include "previewreplaygain.h"

#include <core/constants.h>
#include <core/coresettings.h>
#include <core/engine/audioloader.h>
#include <core/playlist/playlist.h>
#include <utils/settings/settingsmanager.h>

#include <QFile>
#include <QPluginLoader>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;
using namespace Fooyin;
using namespace Fooyin::Headphones;
using namespace Fooyin::Headphones::Tests;

namespace {
// The host owns registration in production. Register its public keys for this fixture.
void registerSettings(SettingsManager& settings)
{
    settings.createSetting<Settings::Core::RGMode>(0, u"Engine/ReplayGainMode"_s);
    settings.createSetting<Settings::Core::RGType>(0, u"Engine/ReplayGainType"_s);
    settings.createSetting<Settings::Core::RGPreAmp>(0.0F, u"Engine/ReplayGainPreAmp"_s);
    settings.createSetting<Settings::Core::NonRGPreAmp>(0.0F, u"Engine/NonReplayGainPreAmp"_s);
    settings.createSetting<Settings::Core::PlayMode>(0, u"Playlist/PlayMode"_s);
}

template <typename T>
T valueAt(const std::vector<std::byte>& bytes, size_t index)
{
    T value;
    std::memcpy(&value, bytes.data() + index * sizeof(value), sizeof(value));
    return value;
}

// Change only the first Ogg page's Opus output gain, repairing its CRC.
bool writeOpusWithGain(const QByteArray& original, const QString& path, int16_t gain)
{
    auto data = original;
    const auto head = data.indexOf("OpusHead");
    if(!data.startsWith("OggS") || data.size() < 27 || head < 0 || head + 19 > data.size()) {
        return false;
    }
    const auto segments = static_cast<uint8_t>(data[26]);
    qsizetype pageSize = 27 + segments;
    if(data.size() < pageSize) {
        return false;
    }
    for(int i = 0; i < segments; ++i) {
        pageSize += static_cast<uint8_t>(data[27 + i]);
    }
    if(pageSize > data.size() || head + 19 > pageSize) {
        return false;
    }
    qToLittleEndian(gain, data.data() + head + 16);
    std::fill_n(data.data() + 22, 4, 0);
    uint32_t crc = 0;
    for(qsizetype i = 0; i < pageSize; ++i) {
        crc ^= static_cast<uint32_t>(static_cast<uint8_t>(data[i])) << 24;
        for(int bit = 0; bit < 8; ++bit) {
            crc = (crc << 1) ^ ((crc & 0x80000000U) ? 0x04c11db7U : 0U);
        }
    }
    qToLittleEndian(crc, data.data() + 22);
    QFile file {path};
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}
} // namespace

class PreviewReplayGainTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void sharedPolicyPcm_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<int>("type");
        QTest::addColumn<int>("playMode");
        QTest::addColumn<float>("trackGain");
        QTest::addColumn<float>("albumGain");
        QTest::addColumn<float>("trackPeak");
        QTest::addColumn<float>("albumPeak");
        QTest::addColumn<float>("preamp");
        QTest::addColumn<float>("withoutInfo");
        QTest::addColumn<double>("expectedGain");
        constexpr float Missing = Constants::InvalidGain;
        constexpr float NoPeak = Constants::InvalidPeak;
        const auto row = [](const char* name, int mode, int type, float track, float album, float peak,
                            float albumPeak, float preamp, float withoutInfo, double expected, int playMode = 0) {
            QTest::newRow(name) << mode << type << playMode << track << album << peak << albumPeak << preamp
                                << withoutInfo << expected;
        };
        const float twiceDb = static_cast<float>(20.0 * std::log10(2.0));
        row("disabled", 0, 0, twiceDb, -twiceDb, 2, 4, 10, 10, 1);
        row("track-positive", 1, 0, twiceDb, -twiceDb, NoPeak, NoPeak, 0, 0, 2);
        row("album-negative", 1, 1, twiceDb, -twiceDb, NoPeak, NoPeak, 0, 0, 0.5);
        row("track-fallback", 1, 0, Missing, -twiceDb, NoPeak, NoPeak, 0, 0, 0.5);
        row("album-fallback", 1, 1, twiceDb, Missing, NoPeak, NoPeak, 0, 0, 2);
        row("tagged-preamp", 1, 0, twiceDb, Missing, NoPeak, NoPeak, -twiceDb, 12, 1);
        row("fallback-is-tagged", 1, 0, Missing, twiceDb, NoPeak, NoPeak, -twiceDb, 12, 1);
        row("untagged-preamp", 1, 1, Missing, Missing, NoPeak, NoPeak, 12, -twiceDb, 0.5);
        row("untagged-amplification", 1, 1, Missing, Missing, NoPeak, NoPeak, -12, twiceDb, 2);
        row("apply-without-clipping", 1, 0, twiceDb, Missing, 2, NoPeak, 0, 0, 2);
        row("gain-and-peak", 3, 0, twiceDb, Missing, 1, NoPeak, 0, 0, 1);
        row("album-peak-preference", 3, 1, twiceDb, twiceDb, 1, 2, 0, 0, 0.5);
        row("track-peak-fallback", 3, 0, twiceDb, Missing, NoPeak, 2, 0, 0, 0.5);
        row("album-peak-fallback", 3, 1, twiceDb, twiceDb, 2, NoPeak, 0, 0, 0.5);
        row("peak-fallback-independent-of-gain", 3, 0, Missing, twiceDb, 4, 2, 0, 0, 0.25);
        row("clipping-only", 2, 0, twiceDb, Missing, 2, NoPeak, 12, 12, 0.5);
        row("clipping-only-does-not-boost", 2, 0, twiceDb, Missing, 0.25F, NoPeak, 12, 12, 1);
        row("clipping-only-untagged", 2, 0, Missing, Missing, 2, NoPeak, 12, 12, 0.5);
        row("no-peak", 3, 0, twiceDb, Missing, NoPeak, NoPeak, 0, 0, 2);
        row("zero-peak-fallback", 3, 0, twiceDb, Missing, 0, 2, 0, 0, 0.5);
        row("negative-peaks", 3, 0, twiceDb, Missing, -2, -3, 0, 0, 2);
        row("shuffle-tracks", 1, 2, twiceDb, -twiceDb, NoPeak, NoPeak, 0, 0, 2, Playlist::ShuffleTracks);
        row("normal-order", 1, 2, twiceDb, -twiceDb, NoPeak, NoPeak, 0, 0, 0.5);
        row("shuffle-albums", 1, 2, twiceDb, -twiceDb, NoPeak, NoPeak, 0, 0, 0.5, Playlist::ShuffleAlbums);
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        row("nonfinite-gain-fallback", 1, 0, nan, twiceDb, NoPeak, NoPeak, 0, 0, 2);
        row("nonfinite-peak-fallback", 3, 0, twiceDb, Missing, nan, 2, 0, 0, 0.5);
        row("nonfinite-metadata", 3, 0, nan, inf, nan, inf, 0, -twiceDb, 0.5);
        row("nonfinite-preamps", 1, 0, Missing, Missing, NoPeak, NoPeak, nan, inf, 1);
        row("preamp-bounded", 1, 0, -20, Missing, NoPeak, NoPeak, 100, 0, 1);
        row("extreme-gain", 1, 0, std::numeric_limits<float>::max(), Missing, NoPeak, NoPeak, 0, 0, 1);
        row("invalid-mode", 99, 0, twiceDb, Missing, NoPeak, NoPeak, 0, 0, 1);
    }

    void sharedPolicyPcm()
    {
        QFETCH(int, mode);
        QFETCH(int, type);
        QFETCH(int, playMode);
        QFETCH(float, trackGain);
        QFETCH(float, albumGain);
        QFETCH(float, trackPeak);
        QFETCH(float, albumPeak);
        QFETCH(float, preamp);
        QFETCH(float, withoutInfo);
        QFETCH(double, expectedGain);
        QTemporaryDir dir;
        SettingsManager settings {dir.filePath(u"settings.ini"_s)};
        registerSettings(settings);
        settings.set<Settings::Core::RGMode>(mode);
        settings.set<Settings::Core::RGType>(type);
        settings.set<Settings::Core::PlayMode>(playMode);
        settings.set<Settings::Core::RGPreAmp>(preamp);
        settings.set<Settings::Core::NonRGPreAmp>(withoutInfo);
        Track track {u"gain.wav"_s};
        track.setRGTrackGain(trackGain);
        track.setRGAlbumGain(albumGain);
        track.setRGTrackPeak(trackPeak);
        track.setRGAlbumPeak(albumPeak);
        // Effective Opus metadata adds this again. Processing must use raw RG
        // metadata, leaving source/header-gain policy to the selected decoder.
        track.setOpusHeaderGainQ78(6 * 256);
        const auto policy = PreviewReplayGain::capture(&settings);
        const AudioFormat input {SampleFormat::F32, 48000, 2};
        const std::vector<float> signal {-0.125F, 0.125F, -0.25F, 0.25F};
        const AudioBuffer buffer {reinterpret_cast<const uint8_t*>(signal.data()), signal.size() * sizeof(float), input, 0};
        for(const auto sample : {SampleFormat::F32, SampleFormat::S16, SampleFormat::S32}) {
            PreviewConverter converter;
            QString error;
            QVERIFY2(converter.configure(input, {sample, 48000, 2}, error, policy.linearGain(track)), qPrintable(error));
            auto output = converter.convert(buffer, error);
            QVERIFY2(output.has_value(), qPrintable(error));
            const double scale = sample == SampleFormat::S16 ? 32768.0 : 2147483648.0;
            for(size_t i = 0; i < signal.size(); ++i) {
                const double actual = sample == SampleFormat::F32 ? valueAt<float>(*output, i)
                                    : sample == SampleFormat::S16 ? valueAt<int16_t>(*output, i) / scale
                                                                  : valueAt<int32_t>(*output, i) / scale;
                QVERIFY(std::isfinite(actual));
                QVERIFY2(std::abs(actual - signal[i] * expectedGain) < 0.00004, qPrintable(QString::number(actual, 'g', 12)));
            }
            QVERIFY(converter.finish(error)->empty());
        }
        // Capturing/processing does not write back sanitised values.
        QCOMPARE(settings.value<Settings::Core::RGMode>(), mode);
        QCOMPARE(settings.value<Settings::Core::RGType>(), type);
        QCOMPARE(settings.value<Settings::Core::PlayMode>(), playMode);
        QCOMPARE(settings.value<Settings::Core::RGPreAmp>(), preamp);
        QCOMPARE(settings.value<Settings::Core::NonRGPreAmp>(), withoutInfo);
    }

    void snapshotAcrossOpenPauseSeekAndRestart()
    {
        QTemporaryDir dir;
        SettingsManager settings {dir.filePath(u"settings.ini"_s)};
        registerSettings(settings);
        settings.set<Settings::Core::RGMode>(1);
        settings.set<Settings::Core::RGType>(2); // Playback order, initially Album.
        PreviewHarness h;
        auto plan = h.add(u"session.wav"_s);
        plan->base = 1000;
        plan->totalFrames = 1000;
        plan->openGate = std::make_shared<Gate>();
        h.create();
        h.controller->setReplayGainSettingsManager(&settings);
        Track track {u"session.wav"_s};
        track.setRGTrackGain(static_cast<float>(20 * std::log10(2.0)));
        track.setRGAlbumGain(static_cast<float>(20 * std::log10(0.5)));
        h.controller->previewTrack(track);
        QTRY_VERIFY(plan->openGate->entered.load());
        settings.set<Settings::Core::PlayMode>(static_cast<int>(Playlist::ShuffleTracks));
        plan->openGate->release();
        QTRY_COMPARE(h.output->stream.size(), size_t {2000});
        QCOMPARE(sampleAt(h.output->stream, 0), int16_t {500}); // Captured before open.
        const auto generation = h.controller->snapshot().generation;
        h.controller->playPause();
        settings.set<Settings::Core::RGPreAmp>(6.0F);
        QCoreApplication::processEvents();
        QCOMPARE(h.controller->snapshot().generation, generation);
        QVERIFY(h.controller->seek(400));
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Paused);
        QVERIFY(h.output->stream.empty());
        h.controller->playPause();
        QTRY_COMPARE(h.output->stream.size(), size_t {1200});
        QCOMPARE(sampleAt(h.output->stream, 0), int16_t {700}); // Seek retains Album and old preamp.
        settings.set<Settings::Core::RGPreAmp>(0.0F);
        h.controller->previewTrack(track); // Explicit restart adopts Track.
        QTRY_COMPARE(h.output->stream.size(), size_t {2000});
        QCOMPARE(sampleAt(h.output->stream, 0), int16_t {2000});
        h.controller->stop();
        settings.set<Settings::Core::RGMode>(0);
        h.controller->playPause();
        QTRY_COMPARE(h.output->stream.size(), size_t {2000});
        QCOMPARE(sampleAt(h.output->stream, 0), int16_t {1000});
        h.output->canDrain = true;
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Stopped);
        h.output->canDrain = false;
        settings.set<Settings::Core::RGMode>(1);
        h.controller->mute();
        h.controller->playPause(); // EOF restart captures latest, volume is separate.
        QTRY_COMPARE(h.output->stream.size(), size_t {2000});
        QCOMPARE(sampleAt(h.output->stream, 0), int16_t {2000});
        QCOMPARE(h.output->volume, 0.0);
        h.controller->unmute();
        QCOMPARE(h.output->volume, 0.25);
        h.controller->shutdown();
        QCOMPARE(settings.value<Settings::Core::RGMode>(), 1);
        settings.storeAllSettings();
        QSettings saved {dir.filePath(u"settings.ini"_s), QSettings::IniFormat};
        QVERIFY(!saved.childGroups().contains(u"Headphones"_s));
        QVERIFY(!plan->wrongThread);
    }

    void disabledNativePcmIsBitExact()
    {
        QTemporaryDir dir;
        SettingsManager settings {dir.filePath(u"settings.ini"_s)};
        registerSettings(settings);
        QVERIFY(!settings.settingsHaveChanged());
        const auto policy = PreviewReplayGain::capture(&settings);
        Track track {u"disabled.wav"_s};
        track.setRGTrackGain(12);
        track.setRGTrackPeak(8);
        track.setOpusHeaderGainQ78(6 * 256);
        for(const auto sample : {SampleFormat::S16, SampleFormat::S32, SampleFormat::F32}) {
            const AudioFormat format {sample, 48000, 1};
            // Includes arbitrary integer low bits and negative zero in F32.
            const std::vector<uint32_t> bits {0, 0x80000000U, 0x3e123457U, 0xbeabcdefU};
            const AudioBuffer buffer {reinterpret_cast<const uint8_t*>(bits.data()), bits.size() * 4, format, 0};
            PreviewConverter converter;
            QString error;
            QVERIFY(converter.configure(format, format, error, policy.linearGain(track)));
            auto output = converter.convert(buffer, error);
            QVERIFY(output);
            QCOMPARE(*output, (std::vector<std::byte> {buffer.constData().begin(), buffer.constData().end()}));
        }
        QVERIFY(!settings.settingsHaveChanged());
    }

    void nativeOpusHeaderGain()
    {
        // Optional real-backend check, using the matching host's public plugin
        // interface. No private decoder headers or link dependencies are needed.
        const auto pluginPath = qEnvironmentVariable("HEADPHONES_TEST_OPUS_PLUGIN");
        const auto fixturePath = qEnvironmentVariable("HEADPHONES_TEST_OPUS_FILE");
        if(pluginPath.isEmpty() || fixturePath.isEmpty()) {
            QSKIP("Set HEADPHONES_TEST_OPUS_PLUGIN and HEADPHONES_TEST_OPUS_FILE for native Opus validation");
        }
        QPluginLoader plugin {pluginPath};
        auto* input = qobject_cast<InputPlugin*>(plugin.instance());
        QVERIFY2(input, qPrintable(plugin.errorString()));
        const auto creator = input->inputCreator();
        QVERIFY(creator.decoder);
        auto loader = std::make_shared<AudioLoader>();
        loader->addDecoder(input->inputName(), creator.decoder);
        QFile fixture {fixturePath};
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        const auto original = fixture.readAll();
        QTemporaryDir dir;
        const auto zeroPath = dir.filePath(u"zero.opus"_s);
        const auto headerPath = dir.filePath(u"header.opus"_s);
        QVERIFY(writeOpusWithGain(original, zeroPath, 0));
        QVERIFY(writeOpusWithGain(original, headerPath, 6 * 256));
        SettingsManager settings {dir.filePath(u"settings.ini"_s)};
        registerSettings(settings);
        auto output = std::make_shared<Tests::OutputState>();
        output->negotiatedFormat = AudioFormat {SampleFormat::F32, 48000, 2};
        PreviewController controller {makePreviewSourceFactory(loader), std::make_unique<FakeOutput>(output)};
        controller.setReplayGainSettingsManager(&settings);
        Track zero {zeroPath};
        zero.setDuration(50);
        zero.setIsChapter(true); // Keep the fixture bounded regardless of original duration.
        controller.previewTrack(zero);
        QTRY_COMPARE(output->stream.size(), size_t {2400 * 8});
        const auto baseline = output->stream;
        QVERIFY(std::ranges::any_of(baseline, [](std::byte value) { return value != std::byte {0}; }));
        Track header {headerPath};
        header.setDuration(50);
        header.setIsChapter(true);
        header.setOpusHeaderGainQ78(6 * 256);
        header.setRGTrackGain(static_cast<float>(20 * std::log10(0.5)));
        header.setRGAlbumGain(static_cast<float>(20 * std::log10(0.25)));
        header.setRGTrackPeak(2);
        header.setRGAlbumPeak(4);
        for(int mode : {0, 1, 2, 3}) {
            for(int type : {0, 1, 2}) {
                settings.set<Settings::Core::RGMode>(mode);
                settings.set<Settings::Core::RGType>(type);
                controller.previewTrack(header);
                QTRY_COMPARE(output->stream.size(), baseline.size());
                // Native fooyin Opus uses OP_ABSOLUTE_GAIN: it suppresses header
                // gain at decode time, including Disabled. Match host behaviour
                // rather than applying effectiveRG* on top of decoded samples.
                const double gain = mode == 0 ? 1.0 : type == 0 ? 0.5 : 0.25;
                for(size_t i = 0; i < baseline.size() / 4; ++i) {
                    QVERIFY(std::abs(valueAt<float>(output->stream, i) - gain * valueAt<float>(baseline, i)) < 0.000001);
                }
            }
        }
        controller.shutdown();
    }

    void gainBeforeQuantisationAndAveraging()
    {
        const AudioFormat input {SampleFormat::F32, 48000, 2};
        const std::vector<float> signal {1.5F, -0.5F, -1.5F, 0.5F, 1.5F, 1.5F, -1.5F, -1.5F};
        const AudioBuffer buffer {reinterpret_cast<const uint8_t*>(signal.data()), signal.size() * 4, input, 0};
        for(const auto sample : {SampleFormat::F32, SampleFormat::S16, SampleFormat::S32}) {
            PreviewConverter converter;
            QString error;
            QVERIFY(converter.configure(input, {sample, 48000, 1}, error, 0.5));
            auto output = converter.convert(buffer, error);
            QVERIFY(output);
            const double expected[] {0.25, -0.25, 0.75, -0.75};
            for(size_t i = 0; i < 4; ++i) {
                const double value = sample == SampleFormat::F32 ? valueAt<float>(*output, i)
                                   : sample == SampleFormat::S16 ? valueAt<int16_t>(*output, i) / 32768.0
                                                                 : valueAt<int32_t>(*output, i) / 2147483648.0;
                QCOMPARE(value, expected[i]);
            }
        }
    }

    void sourceReplacementDuringBlockedGainProcessing()
    {
        QTemporaryDir dir;
        SettingsManager settings {dir.filePath(u"settings.ini"_s)};
        registerSettings(settings);
        settings.set<Settings::Core::RGMode>(1);
        settings.set<Settings::Core::NonRGPreAmp>(static_cast<float>(20 * std::log10(0.5)));
        PreviewHarness h;
        auto old = h.add(u"blocked.wav"_s);
        auto next = h.add(u"replacement.wav"_s);
        next->totalFrames = 100;
        next->base = 1000;
        h.create();
        h.controller->setReplayGainSettingsManager(&settings);
        h.controller->previewTrack(Track {u"blocked.wav"_s});
        h.controller->playPause();
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Paused);
        QTRY_VERIFY(old->reads.load() >= 8); // Gain-enabled producer blocked on its full queue.
        settings.set<Settings::Core::NonRGPreAmp>(0.0F);
        h.controller->previewTrack(Track {u"replacement.wav"_s});
        QTRY_COMPARE(h.output->stream.size(), size_t {200});
        QCOMPARE(sampleAt(h.output->stream, 0), int16_t {1000});
        QCOMPARE(sampleAt(h.output->stream, 99), int16_t {1099});
        h.controller->shutdown();
        QCOMPARE(old->destroys.load(), 1);
        QVERIFY(!old->wrongThread);
    }

    void replacementAndResampledTail()
    {
        QTemporaryDir dir;
        SettingsManager settings {dir.filePath(u"settings.ini"_s)};
        registerSettings(settings);
        settings.set<Settings::Core::RGMode>(1);
        PreviewHarness h;
        auto old = h.add(u"old.wav"_s);
        old->openGate = std::make_shared<Gate>();
        auto next = h.add(u"next.wav"_s);
        next->format.setSampleRate(44100);
        next->base = 8000;
        next->totalFrames = 4410;
        h.output->negotiatedFormat = AudioFormat {SampleFormat::F32, 48000, 2};
        h.create();
        h.controller->setReplayGainSettingsManager(&settings);
        Track first {u"old.wav"_s};
        first.setRGTrackGain(20);
        h.controller->previewTrack(first);
        QTRY_VERIFY(old->openGate->entered.load());
        settings.set<Settings::Core::RGPreAmp>(-20.0F);
        h.controller->previewTrack(Track {u"next.wav"_s}); // Untagged uses non-RG preamp, not -20.
        settings.set<Settings::Core::NonRGPreAmp>(20.0F); // Too late for this session.
        old->openGate->release();
        QTRY_COMPARE(h.output->stream.size(), size_t {4800 * 8});
        const auto baseline = h.output->stream;
        settings.set<Settings::Core::NonRGPreAmp>(static_cast<float>(20 * std::log10(0.5)));
        h.controller->previewTrack(Track {u"next.wav"_s});
        QTRY_COMPARE(h.output->stream.size(), baseline.size());
        // Includes the flushed tail and both channels. No stale generation audio.
        for(size_t i = 0; i < baseline.size() / sizeof(float); ++i) {
            QVERIFY(std::abs(valueAt<float>(h.output->stream, i) - 0.5 * valueAt<float>(baseline, i)) < 0.000001);
        }
        h.controller->shutdown();
        QCOMPARE(old->destroys.load(), 1);
        QCOMPARE(next->destroys.load(), 2);
    }
};

QTEST_GUILESS_MAIN(PreviewReplayGainTest)
#include "previewreplaygaintest.moc"
