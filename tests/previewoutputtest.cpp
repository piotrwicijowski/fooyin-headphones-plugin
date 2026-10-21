// SPDX-License-Identifier: GPL-3.0-or-later
#include "fakes/audiosinkfakes.h"
#include "fakes/previewfakes.h"
#include "headphonessettings.h"

#include <utils/settings/settingsmanager.h>

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <cstring>

using namespace Qt::StringLiterals;
using namespace Fooyin;
using namespace Fooyin::Headphones;
using namespace Fooyin::Headphones::Tests;

class PreviewOutputTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void explicitPairsRequireConcreteKnownFormats()
    {
        const AudioFormat input {SampleFormat::F32, 44100, 2};
        auto device = fakePairDevice("interface");
        QCOMPARE(previewChannelPairs(device), (std::vector<int> {1, 3}));
        QCOMPARE(negotiatePreviewFormat(device, input, 3), std::optional<AudioFormat>(device.pairFormats.front()));
        QVERIFY(!negotiatePreviewFormat(device, input, 5));
        QVERIFY(!negotiatePreviewFormat(device, input, -1));
        QVERIFY(!negotiatePreviewFormat(device, input, 2));
        device.pairFormats.clear();
        QVERIFY(previewChannelPairs(device).empty()); // Count-only support is insufficient.
        QVERIFY(!negotiatePreviewFormat(device, input, 3));
        device = fakePairDevice("interface");
        device.pairFormats.front().clearChannelLayout();
        QVERIFY(previewChannelPairs(device).empty());
        device      = fakePairDevice("interface");
        auto layout = device.pairFormats.front().channelLayout();
        std::swap(layout[0], layout[1]);
        device.pairFormats.front().setChannelLayout(layout);
        QVERIFY(previewChannelPairs(device).empty()); // A mask cannot represent permutations.
        device                = fakePairDevice("interface");
        device.supportsFormat = [](const AudioFormat&) {
            return false;
        };
        QVERIFY(previewChannelPairs(device).empty());
    }

    void explicitPairRevalidationAndPhysicalClock()
    {
        FakeDeviceCatalogue catalogue;
        catalogue.entries = {fakePairDevice("interface")};
        auto sink         = std::make_shared<SinkState>();
        QtAudioOutput output {catalogue, "interface", fakeSinkFactory(sink), 3};
        QString error;
        QVERIFY(output.isAvailable());
        const auto physical = output.configure({SampleFormat::F32, 48000, 2}, error);
        QVERIFY(physical);
        QCOMPARE(physical->channelCount(), 4);
        QVERIFY(output.start(error));
        QCOMPARE(sink->requestedBuffer, qsizetype {76800});
        std::vector<std::byte> data(physical->bytesForDuration(100));
        sink->maxWrite = 3;
        QCOMPARE(output.write(data), qint64 {3});
        sink->maxWrite = static_cast<qint64>(data.size());
        QCOMPARE(output.write(std::span {data}.subspan(3)), static_cast<qint64>(data.size() - 3));
        sink->processed = 200000;
        QCOMPARE(output.processedMs(), uint64_t {100}); // Physical frames, not logical stereo.
        QSignalSpy changes {&output, &PreviewOutput::availabilityChanged};
        catalogue.change({fakeDevice("interface")});
        QVERIFY(!output.isAvailable());
        QVERIFY(sink->lastDevice.isNull());
        QCOMPARE(changes.size(), 1);
        QVERIFY(!output.configure({SampleFormat::F32, 48000, 2}, error));
        QVERIFY(error.contains(u"channel pair"));
        catalogue.change({fakePairDevice("same-name-other-id")});
        QVERIFY(!output.isAvailable());
        catalogue.change({fakePairDevice("interface")});
        QVERIFY(output.isAvailable());
        QCOMPARE(sink->starts, 1); // No reconnection autoplay.
        QVERIFY(output.configure({SampleFormat::F32, 48000, 2}, error));
        auto replacement = fakePairDevice("interface", {SampleFormat::S16, 44100, 4});
        catalogue.change({replacement});
        QVERIFY(!output.start(error)); // Revalidate the approved format, not just pair membership.
    }

    void pairSettingsAreAtomicBoundAndPersistent()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath(u"settings.ini"_s);
        {
            SettingsManager manager {path};
            HeadphonesSettings settings {&manager};
            QCOMPARE(settings.outputChannelPair(), 0);
            QSignalSpy changes {&settings, &HeadphonesSettings::deviceChanged};
            connect(&settings, &HeadphonesSettings::deviceChanged, &settings, [&] {
                QVERIFY(settings.deviceId() == QByteArray {"interface"});
                QCOMPARE(settings.outputChannelPair(), 3);
            });
            settings.setDestination("interface", u"USB Interface"_s, 3);
            QCOMPARE(changes.size(), 1);
            settings.setDestination("interface", u"Renamed interface"_s, 3);
            QCOMPARE(changes.size(), 1);
            manager.storeAllSettings();
        }
        SettingsManager manager {path};
        HeadphonesSettings settings {&manager};
        QCOMPARE(settings.deviceId(), QByteArray {"interface"});
        QCOMPARE(settings.outputChannelPair(), 3);
        manager.set(u"Headphones/DeviceId", encodeDeviceId("same-name-other-id"));
        QCOMPARE(settings.outputChannelPair(), -1); // Never reuse the old pair on a different device.
        settings.setDevice("different", u"Other"_s);
        QCOMPARE(settings.outputChannelPair(), 0);
        settings.setDestination("different", u"Other"_s, 2);
        QCOMPARE(settings.outputChannelPair(), -1);
    }

    void routedWorkerPreservesIsolationAcrossShortWritesPauseSeekAndReplacement()
    {
        FakeDeviceCatalogue catalogue;
        catalogue.entries = {fakePairDevice("interface", {SampleFormat::S16, 1000, 4})};
        auto library      = std::make_shared<SourceLibrary>();
        auto plan         = std::make_shared<SourcePlan>();
        plan->format      = {SampleFormat::S16, 1000, 2};
        plan->base        = 123;
        library->plans.insert(u"track.wav"_s, plan);
        auto sink      = std::make_shared<SinkState>();
        sink->maxWrite = 4093; // Deliberately splits physical frames and sample containers.
        PreviewController controller {
            [library]() { return std::make_unique<FakeSource>(library); },
            std::make_unique<QtAudioOutput>(catalogue, "interface", fakeSinkFactory(sink), 3)};
        controller.previewTrack(Track {u"track.wav"_s});
        QTRY_VERIFY(!sink->epochs.empty() && sink->epochs.back().size() >= 8);
        controller.playPause();
        QCOMPARE(controller.snapshot().state, PreviewState::Paused);
        const auto bytes = sink->epochs.back().size();
        QTest::qWait(30);
        QCOMPARE(sink->epochs.back().size(), bytes);
        QVERIFY(controller.seek(1000));
        QTRY_COMPARE(controller.snapshot().state, PreviewState::Paused);
        const auto starts = sink->starts;
        controller.playPause();
        QTRY_COMPARE(sink->starts, starts + 1);
        QTRY_VERIFY(sink->epochs.back().size() >= 8);
        controller.stop();
        controller.previewTrack(Track {u"track.wav"_s});
        QTRY_COMPARE(sink->starts, starts + 2);
        QTRY_VERIFY(sink->epochs.back().size() >= 8);
        controller.playPause();
        catalogue.change({fakeDevice("interface")});
        QCOMPARE(controller.snapshot().state, PreviewState::Error);
        QVERIFY(!controller.snapshot().deviceAvailable);
        catalogue.change({fakePairDevice("interface", {SampleFormat::S16, 1000, 4})});
        QVERIFY(controller.snapshot().deviceAvailable);
        QCOMPARE(controller.snapshot().state, PreviewState::Error);
        QCOMPARE(sink->starts, starts + 2);
        QCOMPARE(sink->epochs.size(), size_t {3});
        for(size_t epoch = 0; epoch < sink->epochs.size(); ++epoch) {
            const auto& data = sink->epochs[epoch];
            for(qsizetype frame = 0; frame < data.size() / 8; ++frame) {
                QCOMPARE(data[frame * 8], char {0});
                QCOMPARE(data[frame * 8 + 1], char {0});
                QCOMPARE(data[frame * 8 + 2], char {0});
                QCOMPARE(data[frame * 8 + 3], char {0});
                int16_t left, right;
                std::memcpy(&left, data.constData() + frame * 8 + 4, 2);
                std::memcpy(&right, data.constData() + frame * 8 + 6, 2);
                const auto expected = static_cast<int16_t>((123 + (epoch == 1 ? 1000 : 0) + frame) % 30000);
                QCOMPARE(left, expected);
                QCOMPARE(right, expected);
            }
        }
        QVERIFY(!sink->wrongThread);
    }

    void routedQueueEnforcesPhysicalMemoryLimit()
    {
        FakeDeviceCatalogue catalogue;
        catalogue.entries = {fakePairDevice("interface", {SampleFormat::F32, 768000, 8})};
        auto library      = std::make_shared<SourceLibrary>();
        library->plans.insert(u"track.wav"_s, std::make_shared<SourcePlan>());
        auto sink = std::make_shared<SinkState>();
        PreviewController controller {
            [library]() { return std::make_unique<FakeSource>(library); },
            std::make_unique<QtAudioOutput>(catalogue, "interface", fakeSinkFactory(sink), 7)};
        controller.previewTrack(Track {u"track.wav"_s});
        QTRY_COMPARE(controller.snapshot().state, PreviewState::Error);
        QVERIFY(controller.snapshot().error.contains(u"bounded buffer capacity"));
        QCOMPARE(sink->starts, 0);
    }

    void changedPhysicalFormatStopsButAllowsExplicitRecoveryOnSamePair()
    {
        FakeDeviceCatalogue catalogue;
        catalogue.entries = {fakePairDevice("interface", {SampleFormat::S16, 1000, 4})};
        auto library = std::make_shared<SourceLibrary>();
        library->plans.insert(u"track.wav"_s, std::make_shared<SourcePlan>());
        auto sink = std::make_shared<SinkState>();
        PreviewController controller {[library]() { return std::make_unique<FakeSource>(library); },
                                      std::make_unique<QtAudioOutput>(catalogue, "interface", fakeSinkFactory(sink), 3)};
        controller.previewTrack(Track {u"track.wav"_s});
        QTRY_COMPARE(controller.snapshot().state, PreviewState::Playing);
        controller.playPause();
        const auto starts = sink->starts;
        catalogue.change({fakePairDevice("interface", {SampleFormat::S32, 2000, 4})});
        QCOMPARE(controller.snapshot().state, PreviewState::Error);
        QVERIFY(controller.snapshot().deviceAvailable);
        QCOMPARE(sink->starts, starts);
        controller.playPause();
        QTRY_COMPARE(controller.snapshot().state, PreviewState::Playing);
        QCOMPARE(sink->starts, starts + 1);
        QCOMPARE(sink->requestedBuffer, qsizetype {3200});
    }

    void formatNegotiationTestsConcreteCandidates()
    {
        const AudioFormat input {SampleFormat::S24In32, 96000, 1};
        auto device = fakeDevice("headphones", {{SampleFormat::S16, 48000, 2}});
        QCOMPARE(negotiatePreviewFormat(device, input), std::optional<AudioFormat>({SampleFormat::S16, 48000, 2}));
        device = fakeDevice("headphones", {{SampleFormat::F32, 96000, 2}, {SampleFormat::S16, 48000, 2}});
        QCOMPARE(negotiatePreviewFormat(device, input), std::optional<AudioFormat>({SampleFormat::F32, 96000, 2}));
        device = fakeDevice("headphones", {{SampleFormat::S32, 44100, 1}});
        QCOMPARE(negotiatePreviewFormat(device, input), std::optional<AudioFormat>({SampleFormat::S32, 44100, 1}));
        device = fakeDevice("headphones", {{SampleFormat::F32, 48000, 6}});
        QVERIFY(!negotiatePreviewFormat(device, input));
        QVERIFY(!negotiatePreviewFormat(device, {SampleFormat::F32, 48000, 6}));
        const AudioFormat surround {SampleFormat::S24In32, 96000, 6};
        device = fakeDevice("headphones", {{SampleFormat::S16, 48000, 2}});
        QCOMPARE(negotiatePreviewFormat(device, surround), std::optional<AudioFormat>({SampleFormat::S16, 48000, 2}));
        device = fakeDevice("headphones", {{SampleFormat::S32, 44100, 1}});
        QCOMPARE(negotiatePreviewFormat(device, surround), std::optional<AudioFormat>({SampleFormat::S32, 44100, 1}));
        auto unknown = surround;
        unknown.clearChannelLayout();
        QVERIFY(!negotiatePreviewFormat(device, unknown));
    }

    void explicitIdsAndOpenTimeResolution()
    {
        FakeDeviceCatalogue catalogue;
        catalogue.entries = {fakeDevice("a"), fakeDevice("b")};
        auto sink         = std::make_shared<SinkState>();
        QString error;
        for(const auto& id : {QByteArray {}, QByteArray {"missing"}}) {
            QtAudioOutput missing {catalogue, id, fakeSinkFactory(sink)};
            QVERIFY(!missing.isAvailable());
            QVERIFY(!missing.configure({SampleFormat::F32, 44100, 2}, error));
            QVERIFY(!missing.start(error));
        }
        QVERIFY(sink->openedIds.empty());
        QtAudioOutput output {catalogue, "b", fakeSinkFactory(sink)};
        QVERIFY(output.configure({SampleFormat::F32, 44100, 2}, error));
        catalogue.change({fakeDevice("a")});
        QVERIFY(!output.start(error));
        QVERIFY(sink->openedIds.empty());
        catalogue.change({fakeDevice("b"), fakeDevice("a")});
        QCOMPARE(sink->starts, 0); // Reconnect does not open anything.
        QVERIFY(output.configure({SampleFormat::F32, 44100, 2}, error));
        QVERIFY(output.start(error));
        QCOMPARE(sink->openedIds, std::vector<QByteArray> {"b"});
        const auto resets = sink->resets;
        catalogue.change({fakeDevice("a"), fakeDevice("b")}); // A new default/order is unrelated.
        QCOMPARE(sink->resets, resets);
        // A fake descriptor with a null Qt device cannot reach the real factory.
        QtAudioOutput realFactory {catalogue, "a"};
        QVERIFY(realFactory.configure({SampleFormat::F32, 48000, 2}, error));
        QVERIFY(!realFactory.start(error));
    }

    void writableEpochResetPauseDrainAndClock()
    {
        FakeDeviceCatalogue catalogue;
        catalogue.entries = {fakeDevice("headphones", {{SampleFormat::F32, 1000, 2}})};
        auto sink         = std::make_shared<SinkState>();
        QtAudioOutput output {catalogue, "headphones", fakeSinkFactory(sink)};
        QString error;
        QVERIFY(output.configure({SampleFormat::F32, 1000, 2}, error));
        QVERIFY(output.start(error));
        QCOMPARE(sink->requestedBuffer, qsizetype {800});
        QCOMPARE(output.bufferSize(), qsizetype {824});
        QVERIFY(!output.drained()); // Initially Idle without submitted audio is not EOF.
        std::vector<std::byte> data(800);
        sink->maxWrite = 3;
        QCOMPARE(output.write(data), qint64 {3});
        sink->maxWrite = 0;
        QCOMPARE(output.write(std::span {data}.subspan(3)), qint64 {0});
        sink->maxWrite = 800;
        QCOMPARE(output.write(std::span {data}.subspan(3)), qint64 {797});
        sink->processed = 50000;
        QCOMPARE(output.processedMs(), uint64_t {50});
        output.setPaused(true);
        sink->processed = 200000;
        QCOMPARE(output.processedMs(), uint64_t {50});
        QCOMPARE(output.bytesFree(), qint64 {0});
        output.setPaused(false);
        sink->processed = 70000;
        QCOMPARE(output.processedMs(), uint64_t {70});
        sink->state = PreviewSinkState::Idle;
        QVERIFY(output.drained());
        output.reset();
        QVERIFY(sink->lastDevice.isNull());
        QCOMPARE(output.processedMs(), uint64_t {0});
        QCOMPARE(output.write(data), qint64 {-1});
        QVERIFY(output.configure({SampleFormat::F32, 1000, 2}, error));
        QVERIFY(output.start(error));
        data[0] = std::byte {42};
        QCOMPARE(output.write(data), qint64 {800});
        QCOMPARE(sink->epochs.size(), size_t {2});
        QCOMPARE(sink->epochs[0][0], char {0});
        QCOMPARE(sink->epochs[1][0], char {42});
        sink->failure = u"Backend error"_s;
        QVERIFY(!output.error().isEmpty());
        QCOMPARE(output.write(data), qint64 {-1});
        QVERIFY(!sink->wrongThread);
    }

    void hotplugStopsEveryActiveState_data()
    {
        QTest::addColumn<QString>("stage");
        for(const auto* stage : {"loading", "buffering", "playing", "paused", "draining"}) {
            QTest::newRow(stage) << QString::fromLatin1(stage);
        }
    }

    void hotplugStopsEveryActiveState()
    {
        QFETCH(QString, stage);
        FakeDeviceCatalogue catalogue;
        catalogue.entries = {fakeDevice("selected", {{SampleFormat::F32, 1000, 2}})};
        auto library      = std::make_shared<SourceLibrary>();
        auto plan         = std::make_shared<SourcePlan>();
        library->plans.insert(u"track.wav"_s, plan);
        if(stage == u"loading") {
            plan->openGate = std::make_shared<Gate>();
        }
        if(stage == u"buffering") {
            plan->readGate = std::make_shared<Gate>();
        }
        if(stage == u"draining") {
            plan->totalFrames = 71;
        }
        auto sink = std::make_shared<SinkState>();
        PreviewController controller {[library]() { return std::make_unique<FakeSource>(library); },
                                      std::make_unique<QtAudioOutput>(catalogue, "selected", fakeSinkFactory(sink))};
        controller.previewTrack(Track {u"track.wav"_s});
        if(stage == u"loading") {
            QTRY_VERIFY(plan->openGate->entered.load());
        }
        else if(stage == u"buffering") {
            QTRY_VERIFY(plan->readGate->entered.load());
        }
        else if(stage == u"draining") {
            QTRY_COMPARE(controller.snapshot().state, PreviewState::Draining);
        }
        else {
            QTRY_COMPARE(controller.snapshot().state, PreviewState::Playing);
            if(stage == u"paused") {
                controller.playPause();
            }
        }
        catalogue.change({fakeDevice("other")});
        QCOMPARE(controller.snapshot().state, PreviewState::Error);
        QVERIFY(!controller.snapshot().deviceAvailable);
        QVERIFY(sink->lastDevice.isNull());
        const auto starts = sink->starts;
        catalogue.change({fakeDevice("selected", {{SampleFormat::F32, 1000, 2}})});
        QCOMPARE(controller.snapshot().state, PreviewState::Error);
        QVERIFY(controller.snapshot().deviceAvailable);
        QCOMPARE(sink->starts, starts);
        if(plan->openGate) {
            plan->openGate->release();
        }
        controller.shutdown();
    }

    void underrunIsNotEofAndDrainWaitsForSink()
    {
        FakeDeviceCatalogue catalogue;
        catalogue.entries = {fakeDevice("selected", {{SampleFormat::F32, 1000, 2}})};
        auto library      = std::make_shared<SourceLibrary>();
        auto plan         = std::make_shared<SourcePlan>();
        library->plans.insert(u"track.wav"_s, plan);
        auto sink = std::make_shared<SinkState>();
        PreviewController controller {[library]() { return std::make_unique<FakeSource>(library); },
                                      std::make_unique<QtAudioOutput>(catalogue, "selected", fakeSinkFactory(sink))};
        controller.previewTrack(Track {u"track.wav"_s});
        QTRY_COMPARE(controller.snapshot().state, PreviewState::Playing);
        sink->free  = 0;
        sink->state = PreviewSinkState::Idle;
        QCoreApplication::processEvents();
        QCOMPARE(controller.snapshot().state, PreviewState::Playing);
        controller.stop();
        // Changing source config is safe after stop has joined its source generation.
        controller.shutdown();
        plan->totalFrames = 71;
        sink->free        = 65536;
        PreviewController finite {[library]() { return std::make_unique<FakeSource>(library); },
                                  std::make_unique<QtAudioOutput>(catalogue, "selected", fakeSinkFactory(sink))};
        finite.previewTrack(Track {u"track.wav"_s});
        QTRY_COMPARE(finite.snapshot().state, PreviewState::Draining);
        QCOMPARE(sink->state, PreviewSinkState::Active);
        sink->state = PreviewSinkState::Idle;
        QTRY_COMPARE(finite.snapshot().state, PreviewState::Stopped);
    }

    void backendFailureResetsEpochAndExplicitPlayRecovers()
    {
        FakeDeviceCatalogue catalogue;
        catalogue.entries = {fakeDevice("selected")};
        auto library      = std::make_shared<SourceLibrary>();
        library->plans.insert(u"track.wav"_s, std::make_shared<SourcePlan>());
        auto sink = std::make_shared<SinkState>();
        PreviewController controller {[library]() { return std::make_unique<FakeSource>(library); },
                                      std::make_unique<QtAudioOutput>(catalogue, "selected", fakeSinkFactory(sink))};
        controller.previewTrack(Track {u"track.wav"_s});
        QTRY_COMPARE(controller.snapshot().state, PreviewState::Playing);
        sink->failure = u"Backend failed"_s;
        QTRY_COMPARE(controller.snapshot().state, PreviewState::Error);
        QVERIFY(sink->lastDevice.isNull());
        QCOMPARE(sink->starts, 1);
        sink->failure.clear();
        controller.playPause();
        QTRY_COMPARE(controller.snapshot().state, PreviewState::Playing);
        QCOMPARE(sink->starts, 2);
    }

    void settingsRoundTripOpaqueIdsAndLogarithmicVolume()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath(u"settings.ini"_s);
        const QByteArray id {"\0headphones\xff/id", 15};
        QCOMPARE(decodeDeviceId(encodeDeviceId(id)), id);
        QVERIFY(decodeDeviceId(u"not base64!!"_s).isEmpty());
        {
            SettingsManager manager {path};
            HeadphonesSettings settings {&manager};
            QSignalSpy changes {&settings, &HeadphonesSettings::deviceChanged};
            QVERIFY(settings.deviceId().isEmpty());
            QCOMPARE(settings.volume(), 0.25);
            QCOMPARE(settings.previewStartOffsetSeconds(), 0);
            settings.setDevice(id, u"Missing saved device"_s);
            QCOMPARE(changes.size(), 1);
            settings.setDevice(id, u"Updated description"_s);
            QCOMPARE(changes.size(), 1); // Same ID never triggers output replacement.
            settings.setVolume(0.4);
            settings.setPreviewStartOffsetSeconds(42);
            manager.storeAllSettings();
        }
        SettingsManager restored {path};
        HeadphonesSettings settings {&restored};
        QCOMPARE(settings.deviceId(), id);
        QCOMPARE(settings.deviceDescription(), u"Updated description"_s);
        QCOMPARE(settings.volume(), 0.4);
        QCOMPARE(settings.previewStartOffsetSeconds(), 42);
        restored.set(u"Headphones/DeviceId", u"malformed!"_s);
        QVERIFY(settings.deviceId().isEmpty());
        settings.setVolume(-5.0);
        QCOMPARE(settings.volume(), 0.0);
        settings.setVolume(5.0);
        QCOMPARE(settings.volume(), 1.0);
        QCOMPARE(previewVolumeGain(0.0), 0.0);
        QCOMPARE(previewVolumeGain(1.0), 1.0);
        QVERIFY(previewVolumeGain(0.25) > 0.0 && previewVolumeGain(0.25) < 0.25);
        QVERIFY(previewVolumeGain(0.5) > previewVolumeGain(0.25));
    }

    void invalidPersistedStartOffsets_data()
    {
        QTest::addColumn<QVariant>("stored");
        QTest::addColumn<int>("expected");
        QTest::newRow("negative") << QVariant {-1} << 0;
        QTest::newRow("text") << QVariant {u"invalid"_s} << 0;
        QTest::newRow("fractional") << QVariant {1.5} << 0;
        QTest::newRow("boolean") << QVariant {true} << 0;
        QTest::newRow("overflow-ui") << QVariant {qint64(HeadphonesSettings::MaxPreviewStartOffsetSeconds) + 1} << 0;
        QTest::newRow("overflow-signed") << QVariant {u"9223372036854775808"_s} << 0;
        QTest::newRow("overflow-unsigned") << QVariant {std::numeric_limits<quint64>::max()} << 0;
        QTest::newRow("overflow-text") << QVariant {u"18446744073709551616"_s} << 0;
        QTest::newRow("integer-text") << QVariant {u"37"_s} << 37;
        QTest::newRow("ui-maximum") << QVariant {HeadphonesSettings::MaxPreviewStartOffsetSeconds}
                                  << HeadphonesSettings::MaxPreviewStartOffsetSeconds;
    }

    void invalidPersistedStartOffsets()
    {
        QFETCH(QVariant, stored);
        QFETCH(int, expected);
        QTemporaryDir directory;
        SettingsManager manager {directory.filePath(u"settings.ini"_s)};
        manager.fileSet(u"Headphones/PreviewStartOffsetSeconds", stored);
        HeadphonesSettings settings {&manager};
        QCOMPARE(settings.previewStartOffsetSeconds(), expected);
        settings.setPreviewStartOffsetSeconds(-1);
        QCOMPARE(settings.previewStartOffsetSeconds(), 0);
        settings.setPreviewStartOffsetSeconds(HeadphonesSettings::MaxPreviewStartOffsetSeconds);
        QCOMPARE(settings.previewStartOffsetSeconds(), HeadphonesSettings::MaxPreviewStartOffsetSeconds);
        HeadphonesSettings unavailable {nullptr};
        QCOMPARE(unavailable.previewStartOffsetSeconds(), 0);
    }
};

QTEST_GUILESS_MAIN(PreviewOutputTest)
#include "previewoutputtest.moc"
