// SPDX-License-Identifier: GPL-3.0-or-later
#include "fakes/audiosinkfakes.h"
#include "fakes/previewfakes.h"
#include "headphonessettings.h"

#include <utils/settings/settingsmanager.h>

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace Fooyin;
using namespace Fooyin::Headphones;
using namespace Fooyin::Headphones::Tests;

class PreviewOutputTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
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
