// SPDX-License-Identifier: GPL-3.0-or-later
#include "fakes/previewfakes.h"
#include "previewdecoderworker.h"
#include "previewpcmbuffer.h"

#include <QSignalSpy>
#include <QTest>

#include <future>
#include <limits>
#include <numeric>

using namespace Qt::StringLiterals;
using namespace Fooyin;
using namespace Fooyin::Headphones;
using namespace Fooyin::Headphones::Tests;

class PreviewEngineTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void boundedQueueAndCancellation()
    {
        PreviewPcmBuffer queue {8, 2};
        const std::vector<std::byte> data {std::byte {1}, std::byte {2},  std::byte {3},  std::byte {4},
                                           std::byte {5}, std::byte {6},  std::byte {7},  std::byte {8},
                                           std::byte {9}, std::byte {10}, std::byte {11}, std::byte {12}};
        auto producer = std::async(std::launch::async, [&]() { return queue.push(data); });
        // Cleanup cannot leave the future waiting if an assertion fails.
        struct Cancel
        {
            PreviewPcmBuffer& queue;
            ~Cancel()
            {
                queue.cancel();
            }
        } cancel {queue};
        QTRY_COMPARE(queue.size(), size_t {8});
        QCOMPARE(queue.capacity(), size_t {8});
        auto received = queue.pop(5); // Never pop a partial frame.
        QCOMPARE(received.size(), size_t {4});
        QTRY_VERIFY(producer.wait_for(std::chrono::seconds {0}) == std::future_status::ready);
        QVERIFY(producer.get());
        auto rest = queue.pop(100);
        received.insert(received.end(), rest.begin(), rest.end());
        QCOMPARE(received, data);
        QVERIFY(queue.push(std::span {data}.first(8)));
        auto blocked = std::async(std::launch::async, [&]() { return queue.push(data); });
        queue.cancel();
        QVERIFY(!blocked.get());
        QCOMPARE(queue.size(), size_t {0});
        QVERIFY_EXCEPTION_THROWN(queue.push(std::span {data}.first(1)), std::invalid_argument);
    }

    void transportAndProcessedPosition()
    {
        PreviewHarness h;
        auto plan = h.add(u"track.wav"_s);
        h.create();
        QCOMPARE(h.controller->snapshot().state, PreviewState::Stopped);
        QCOMPARE(plan->opens.load(), 0);
        h.controller->previewTrack(Track {u"track.wav"_s});
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Playing);
        QTRY_VERIFY(!h.output->stream.empty());
        h.output->processed = 123;
        QTRY_COMPARE(h.controller->snapshot().positionMs, uint64_t {123});
        h.controller->playPause();
        QCOMPARE(h.controller->snapshot().state, PreviewState::Paused);
        QVERIFY(h.output->paused);
        h.controller->playPause();
        QVERIFY(!h.output->paused);
        QCOMPARE(h.controller->snapshot().state, PreviewState::Playing);
        h.controller->stop();
        QCOMPARE(h.controller->snapshot().state, PreviewState::Stopped);
        QCOMPARE(h.controller->snapshot().positionMs, uint64_t {0});
        QCOMPARE(h.controller->snapshot().track.filepath(), u"track.wav"_s);
        QVERIFY(h.output->stream.empty());
        h.controller->stop();
        h.controller->playPause();
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Playing);
        h.controller->shutdown();
        QCOMPARE(plan->opens.load(), 2);
        QCOMPARE(plan->stops.load(), 2);
        QCOMPARE(plan->destroys.load(), 2);
        QVERIFY(!plan->wrongThread);
        QVERIFY(!h.output->wrongThread);
    }

    void pauseDuringOpenAndFullQueueShutdown()
    {
        PreviewHarness h;
        auto plan      = h.add(u"slow.wav"_s);
        plan->openGate = std::make_shared<Gate>();
        h.create();
        h.controller->previewTrack(Track {u"slow.wav"_s});
        QTRY_VERIFY(plan->openGate->entered.load());
        h.controller->playPause();
        QVERIFY(h.controller->snapshot().pauseRequested);
        plan->openGate->release();
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Paused);
        QTRY_VERIFY(plan->reads.load() >= 8); // Producer fills 500 frames, then blocks.
        QCOMPARE(h.output->starts, 0);
        h.controller->shutdown(); // Wakes full-queue producer and joins without GUI delivery.
        h.controller->shutdown();
        QCOMPARE(plan->stops.load(), 1);
        QCOMPARE(plan->destroys.load(), 1);
        QVERIFY(!plan->wrongThread);
    }

    void latestReplacementWinsAfterDelayedOpen()
    {
        PreviewHarness h;
        auto a      = h.add(u"a.wav"_s);
        auto b      = h.add(u"b.wav"_s);
        auto c      = h.add(u"c.wav"_s);
        a->openGate = std::make_shared<Gate>();
        c->base     = 9000;
        h.create();
        h.controller->previewTrack(Track {u"a.wav"_s});
        QTRY_VERIFY(a->openGate->entered.load());
        const auto resets = h.output->resets;
        h.controller->stop();
        QCOMPARE(h.output->resets, resets + 1);
        QCOMPARE(h.controller->snapshot().state, PreviewState::Stopped);
        bool heartbeat {false};
        QTimer::singleShot(0, [&]() { heartbeat = true; });
        QTRY_VERIFY(heartbeat); // GUI remains responsive with the open still held.
        h.controller->previewTrack(Track {u"b.wav"_s});
        h.controller->previewTrack(Track {u"c.wav"_s});
        a->openGate->release();
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Playing);
        QTRY_VERIFY(!h.output->stream.empty());
        QCOMPARE(sampleAt(h.output->stream, 0), int16_t {9000});
        QCOMPARE(b->opens.load(), 0);
        QCOMPARE(a->stops.load(), 1);
        QCOMPARE(h.controller->snapshot().track.filepath(), u"c.wav"_s);
    }

    void abortBlockingReadAndIgnoreQueuedFailure()
    {
        PreviewHarness h;
        auto plan      = h.add(u"read.wav"_s);
        plan->readGate = std::make_shared<Gate>();
        h.create();
        h.controller->previewTrack(Track {u"read.wav"_s});
        QTRY_VERIFY(plan->readGate->entered.load());
        h.controller->stop();
        QCOMPARE(h.controller->snapshot().state, PreviewState::Stopped);
        QVERIFY(h.output->stream.empty());
        QVERIFY(plan->aborts.load() > 0);
        h.controller->shutdown();
        QCOMPARE(plan->stops.load(), 1);
        QVERIFY(!plan->wrongThread);

        PreviewHarness failed;
        auto bad       = failed.add(u"bad.wav"_s);
        bad->openFails = true;
        failed.create();
        failed.controller->previewTrack(Track {u"bad.wav"_s});
        // stop() occurs after the worker queued an error, before GUI delivery.
        QVERIFY(bad->stopped.tryAcquire(1, 2000));
        failed.controller->stop();
        QCoreApplication::processEvents();
        QCOMPARE(failed.controller->snapshot().state, PreviewState::Stopped);
        QCOMPARE(failed.output->starts, 0);
    }

    void shutdownDuringOpenWithoutGuiCallbacks()
    {
        PreviewHarness h;
        auto plan      = h.add(u"open.wav"_s);
        plan->openGate = std::make_shared<Gate>();
        h.create();
        h.controller->previewTrack(Track {u"open.wav"_s});
        QTRY_VERIFY(plan->openGate->entered.load());
        std::thread release {[gate = plan->openGate]() {
            gate->release();
        }};
        h.controller->shutdown();
        release.join();
        QCOMPARE(plan->destroys.load(), 1);
        QCOMPARE(plan->stops.load(), 1);
        QCOMPARE(h.output->starts, 0);
        QCoreApplication::processEvents();
        QCOMPARE(h.controller->snapshot().state, PreviewState::Stopped);
        h.controller->previewTrack(Track {u"open.wav"_s});
        QCOMPARE(plan->opens.load(), 1);
    }

    void boundedSegments_data()
    {
        QTest::addColumn<int>("rate");
        QTest::addColumn<quint64>("offset");
        QTest::addColumn<quint64>("duration");
        QTest::addColumn<bool>("bounded");
        QTest::newRow("cue-straddling-block") << 1000 << quint64 {100} << quint64 {123} << true;
        QTest::newRow("fractional-frame-start") << 44100 << quint64 {1} << quint64 {1} << true;
        QTest::newRow("fractional-frame-end") << 44100 << quint64 {9} << quint64 {1} << true;
        QTest::newRow("whole-file-metadata-too-short") << 1000 << quint64 {0} << quint64 {2} << false;
        QTest::newRow("whole-file-duration-unknown") << 1000 << quint64 {0} << quint64 {0} << false;
    }

    void boundedSegments()
    {
        QFETCH(int, rate);
        QFETCH(quint64, offset);
        QFETCH(quint64, duration);
        QFETCH(bool, bounded);
        PreviewHarness h;
        auto plan = h.add(u"segment.wav"_s);
        plan->format.setSampleRate(rate);
        plan->seekRoundsUp = rate == 44100; // Reproduce FFmpeg's fractional-ms seek rounding.
        plan->totalFrames  = 2000;
        h.create();
        Track track {u"segment.wav"_s};
        track.setOffset(offset);
        track.setDuration(duration);
        if(bounded) {
            track.setCuePath(u"fixture.cue"_s);
        }
        h.controller->previewTrack(track);
        const uint64_t first = offset * rate / 1000;
        const uint64_t count = bounded ? (offset + duration) * rate / 1000 - first : plan->totalFrames;
        QTRY_COMPARE(h.output->stream.size(), count * sizeof(int16_t));
        for(size_t i = 0; i < count; ++i) {
            QCOMPARE(sampleAt(h.output->stream, i), static_cast<int16_t>((first + i) % 30000));
        }
        const auto anchorPeriod = 1000 / std::gcd(rate, 1000);
        QCOMPARE(plan->lastSeek.load(), offset - offset % anchorPeriod);
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Draining);
        h.output->canDrain = true;
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Stopped);
        QCOMPARE(h.controller->snapshot().positionMs, uint64_t {0});
    }

    void configuredStartSamples_data()
    {
        QTest::addColumn<int>("rate");
        QTest::addColumn<quint64>("sourceOffset");
        QTest::addColumn<quint64>("duration");
        QTest::addColumn<bool>("bounded");
        QTest::addColumn<bool>("seekable");
        QTest::addColumn<int>("seconds");
        QTest::addColumn<quint64>("effectiveMs");
        QTest::newRow("whole-file") << 1000 << quint64 {0} << quint64 {5000} << false << true << 1 << quint64 {1000};
        QTest::newRow("cue") << 1000 << quint64 {127} << quint64 {1700} << true << true << 1 << quint64 {1000};
        QTest::newRow("chapter-exact-preroll")
            << 44100 << quint64 {27} << quint64 {1701} << true << true << 1 << quint64 {1000};
        QTest::newRow("short-whole-file") << 1000 << quint64 {0} << quint64 {500} << false << true << 1 << quint64 {0};
        QTest::newRow("at-duration") << 1000 << quint64 {0} << quint64 {1000} << false << true << 1 << quint64 {0};
        QTest::newRow("short-segment") << 1000 << quint64 {127} << quint64 {500} << true << true << 1 << quint64 {0};
        QTest::newRow("unknown-duration") << 1000 << quint64 {0} << quint64 {0} << false << true << 1 << quint64 {1000};
        QTest::newRow("unseekable") << 1000 << quint64 {0} << quint64 {0} << false << false << 3600 << quint64 {0};
        QTest::newRow("unseekable-segment-at-source-start")
            << 1000 << quint64 {0} << quint64 {1700} << true << false << 1 << quint64 {0};
        QTest::newRow("negative-policy-defaults-to-zero")
            << 1000 << quint64 {0} << quint64 {5000} << false << true << -1 << quint64 {0};
    }

    void configuredStartSamples()
    {
        QFETCH(int, rate);
        QFETCH(quint64, sourceOffset);
        QFETCH(quint64, duration);
        QFETCH(bool, bounded);
        QFETCH(bool, seekable);
        QFETCH(int, seconds);
        QFETCH(quint64, effectiveMs);
        PreviewHarness h;
        auto plan = h.add(u"offset.wav"_s);
        plan->format.setSampleRate(rate);
        plan->seekable     = seekable;
        plan->seekRoundsUp = true;
        plan->totalFrames  = rate * 5;
        h.create();
        h.controller->setPreviewStartOffsetSeconds(seconds);
        Track track {u"offset.wav"_s};
        track.setOffset(sourceOffset);
        track.setDuration(duration);
        if(bounded) {
            track.setCuePath(u"fixture.cue"_s);
        }
        h.controller->previewTrack(track);
        const uint64_t first = (sourceOffset + effectiveMs) * rate / 1000;
        const uint64_t end   = bounded ? (sourceOffset + duration) * rate / 1000 : plan->totalFrames;
        QTRY_COMPARE(h.output->stream.size(), (end - first) * sizeof(int16_t));
        for(size_t i = 0; i < end - first; ++i) {
            QCOMPARE(sampleAt(h.output->stream, i), static_cast<int16_t>((first + i) % 30000));
        }
        QCOMPARE(h.controller->snapshot().positionMs, effectiveMs);
        QCOMPARE(h.controller->snapshot().seekable, seekable);
        const auto period = 1000 / std::gcd(rate, 1000);
        QCOMPARE(plan->lastSeek.load(), (sourceOffset + effectiveMs) - (sourceOffset + effectiveMs) % period);
        h.output->processed = 23;
        QTRY_COMPARE(h.controller->snapshot().positionMs, effectiveMs + 23);
        h.output->canDrain = true;
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Stopped);
        QCOMPARE(h.controller->snapshot().positionMs, uint64_t {0});
        QCOMPARE(h.controller->snapshot().track.filepath(), track.filepath());
    }

    void offsetChangesOnlyAffectFreshStarts()
    {
        PreviewHarness h;
        auto plan = h.add(u"offset.wav"_s);
        h.create();
        h.controller->setPreviewStartOffsetSeconds(1);
        const Track track {u"offset.wav"_s};
        h.controller->previewTrack(track);
        QTRY_VERIFY(!h.output->stream.empty());
        QCOMPARE(sampleAt(h.output->stream, 0), int16_t {1000});
        h.output->processed = 123;
        QTRY_COMPARE(h.controller->snapshot().positionMs, uint64_t {1123});
        const auto generation = h.controller->snapshot().generation;
        h.controller->setPreviewStartOffsetSeconds(2);
        QCOMPARE(h.controller->snapshot().generation, generation);
        QCOMPARE(h.controller->snapshot().positionMs, uint64_t {1123});
        QCOMPARE(plan->opens.load(), 1);
        QCOMPARE(sampleAt(h.output->stream, 0), int16_t {1000});
        h.controller->playPause();
        h.controller->setPreviewStartOffsetSeconds(3);
        h.controller->playPause();
        QCOMPARE(h.controller->snapshot().positionMs, uint64_t {1123});
        QCOMPARE(h.controller->snapshot().generation, generation);
        h.controller->playPause();
        QVERIFY(h.controller->seek(40));
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Paused);
        QCOMPARE(h.controller->snapshot().positionMs, uint64_t {40});
        h.controller->playPause();
        QTRY_VERIFY(!h.output->stream.empty());
        QCOMPARE(sampleAt(h.output->stream, 0), int16_t {40});
        h.controller->previewTrack(track);
        QTRY_VERIFY(!h.output->stream.empty());
        QCOMPARE(sampleAt(h.output->stream, 0), int16_t {3000});
        h.controller->stop();
        QCOMPARE(h.controller->snapshot().positionMs, uint64_t {0});
        h.controller->setPreviewStartOffsetSeconds(4);
        h.controller->playPause();
        QTRY_VERIFY(!h.output->stream.empty());
        QCOMPARE(sampleAt(h.output->stream, 0), int16_t {4000});
    }

    void configuredOffsetPastUnknownEof_data()
    {
        QTest::addColumn<int>("seconds");
        QTest::newRow("past-eof") << 2;
        QTest::newRow("maximum-seconds") << std::numeric_limits<int>::max();
    }

    void configuredOffsetPastUnknownEof()
    {
        QFETCH(int, seconds);
        PreviewHarness h;
        auto plan         = h.add(u"unknown.wav"_s);
        plan->totalFrames = 100;
        h.create();
        h.controller->setPreviewStartOffsetSeconds(seconds);
        h.controller->previewTrack(Track {u"unknown.wav"_s});
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Stopped);
        QCOMPARE(plan->lastSeek.load(), static_cast<uint64_t>(seconds) * 1000);
        QCOMPARE(h.controller->snapshot().positionMs, uint64_t {0});
        QVERIFY(h.controller->snapshot().error.isEmpty());
        QVERIFY(h.output->stream.empty());
        QCOMPARE(h.output->starts, 0);
        h.controller->setPreviewStartOffsetSeconds(0);
        h.controller->playPause();
        QTRY_VERIFY(!h.output->stream.empty());
        QCOMPARE(sampleAt(h.output->stream, 0), int16_t {0});
    }

    void configuredOffsetErrors_data()
    {
        QTest::addColumn<quint64>("sourceOffset");
        QTest::addColumn<quint64>("duration");
        QTest::addColumn<bool>("seekable");
        QTest::newRow("unseekable-segment") << quint64 {27} << quint64 {2000} << false;
        QTest::newRow("offset-addition-overflow")
            << quint64(std::numeric_limits<int64_t>::max() - 500) << quint64 {0} << true;
        QTest::newRow("segment-end-overflow")
            << quint64(std::numeric_limits<int64_t>::max() - 1500) << quint64 {2000} << true;
    }

    void configuredOffsetErrors()
    {
        QFETCH(quint64, sourceOffset);
        QFETCH(quint64, duration);
        QFETCH(bool, seekable);
        PreviewHarness h;
        auto plan      = h.add(u"offset-error.wav"_s);
        plan->seekable = seekable;
        h.create();
        h.controller->setPreviewStartOffsetSeconds(1);
        Track track {u"offset-error.wav"_s};
        track.setOffset(sourceOffset);
        track.setDuration(duration);
        track.setCuePath(u"fixture.cue"_s);
        h.controller->previewTrack(track);
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Error);
        QVERIFY(!h.controller->snapshot().error.isEmpty());
        QVERIFY(h.output->stream.empty());
        QCOMPARE(plan->lastSeek.load(), uint64_t {0});
        QCOMPARE(plan->reads.load(), 0);
    }

    void configuredOffsetReplacementDuringOpen()
    {
        PreviewHarness h;
        auto old      = h.add(u"old-offset.wav"_s);
        old->openGate = std::make_shared<Gate>();
        auto next  = h.add(u"new-offset.wav"_s);
        next->base = 9000;
        h.create();
        h.controller->setPreviewStartOffsetSeconds(1);
        h.controller->previewTrack(Track {u"old-offset.wav"_s});
        QTRY_VERIFY(old->openGate->entered.load());
        h.controller->setPreviewStartOffsetSeconds(2);
        h.controller->previewTrack(Track {u"new-offset.wav"_s});
        h.controller->playPause();
        old->openGate->release();
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Paused);
        QCOMPARE(h.controller->snapshot().positionMs, uint64_t {2000});
        QVERIFY(h.output->stream.empty());
        h.controller->playPause();
        QTRY_VERIFY(!h.output->stream.empty());
        QCOMPARE(sampleAt(h.output->stream, 0), int16_t {11000});
        QCOMPARE(old->reads.load(), 0);
    }

    void pausedSeekFlushesAndClamps()
    {
        PreviewHarness h;
        auto plan = h.add(u"seek.wav"_s);
        h.create();
        Track track {u"seek.wav"_s};
        track.setOffset(100);
        track.setDuration(300);
        track.setCuePath(u"fixture.cue"_s);
        h.controller->previewTrack(track);
        QTRY_VERIFY(!h.output->stream.empty());
        h.controller->playPause();
        QVERIFY(h.controller->seek(40));
        QVERIFY(h.output->stream.empty());
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Paused);
        QCOMPARE(h.controller->snapshot().positionMs, uint64_t {40});
        QCOMPARE(plan->lastSeek.load(), uint64_t {140});
        h.controller->playPause();
        QTRY_VERIFY(!h.output->stream.empty());
        QCOMPARE(sampleAt(h.output->stream, 0), int16_t {140});
        h.controller->playPause();
        QVERIFY(h.controller->seek(-999));
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Paused);
        QCOMPARE(h.controller->snapshot().positionMs, uint64_t {0});
        QVERIFY(h.controller->seek(std::numeric_limits<int64_t>::max()));
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Paused);
        QCOMPARE(h.controller->snapshot().positionMs, uint64_t {300});
        QCOMPARE(plan->lastSeek.load(), uint64_t {400});
        h.controller->shutdown();
        QCOMPARE(plan->opens.load(), plan->stops.load());
        QCOMPARE(plan->opens.load(), plan->destroys.load());
    }

    void coalescedSeeksAndUnseekableInput()
    {
        PreviewHarness h;
        auto plan      = h.add(u"seek.wav"_s);
        plan->readGate = std::make_shared<Gate>();
        h.create();
        h.controller->previewTrack(Track {u"seek.wav"_s});
        QTRY_VERIFY(plan->readGate->entered.load());
        QVERIFY(h.controller->seek(10));
        QVERIFY(h.controller->seek(20));
        QVERIFY(h.controller->seek(30));
        QTRY_VERIFY(!h.output->stream.empty());
        QCOMPARE(sampleAt(h.output->stream, 0), int16_t {30});
        QCOMPARE(h.controller->snapshot().positionMs, uint64_t {30});

        PreviewHarness unseekable;
        auto fixed      = unseekable.add(u"fixed.wav"_s);
        fixed->seekable = false;
        unseekable.create();
        Track track {u"fixed.wav"_s};
        unseekable.controller->previewTrack(track);
        QTRY_COMPARE(unseekable.controller->snapshot().state, PreviewState::Playing);
        const auto generation = unseekable.controller->snapshot().generation;
        QVERIFY(!unseekable.controller->seek(20));
        QCOMPARE(unseekable.controller->snapshot().generation, generation);
        track.setOffset(5);
        track.setCuePath(u"fixture.cue"_s);
        track.setDuration(30);
        unseekable.controller->previewTrack(track);
        QTRY_COMPARE(unseekable.controller->snapshot().state, PreviewState::Error);
        QVERIFY(unseekable.output->stream.empty());
    }

    void sourceErrors_data()
    {
        QTest::addColumn<QString>("kind");
        for(const auto& kind :
            {"open", "read", "malformed", "changed-format", "multichannel", "layout", "seek", "overflow", "starvation",
             "changed-layout", "partial-frame", "oversized", "too-many-channels"}) {
            QTest::newRow(kind) << QString::fromLatin1(kind);
        }
    }

    void sourceErrors()
    {
        QFETCH(QString, kind);
        PreviewHarness h;
        auto plan           = h.add(u"error.wav"_s);
        plan->openFails     = kind == u"open";
        plan->readFails     = kind == u"read";
        plan->malformed     = kind == u"malformed";
        plan->changedFormat = kind == u"changed-format";
        plan->seekFails     = kind == u"seek";
        if(kind == u"multichannel") {
            plan->format.setChannelCount(6);
            plan->format.clearChannelLayout();
        }
        if(kind == u"too-many-channels") {
            plan->format.setChannelCount(32);
        }
        if(kind == u"changed-layout" || kind == u"partial-frame" || kind == u"oversized") {
            plan->format.setChannelCount(8);
            plan->changedLayout        = kind == u"changed-layout";
            plan->partialFrame         = kind == u"partial-frame";
            plan->oversized            = kind == u"oversized";
            h.output->negotiatedFormat = AudioFormat {SampleFormat::F32, 48000, 2};
        }
        if(kind == u"layout") {
            plan->format.setChannelLayout({AudioFormat::ChannelPosition::LFE});
        }
        if(kind == u"starvation") {
            plan->starveReads = -1;
        }
        h.create();
        Track track {u"error.wav"_s};
        if(kind == u"seek") {
            track.setOffset(100);
        }
        if(kind == u"overflow") {
            track.setOffset(std::numeric_limits<uint64_t>::max());
        }
        h.controller->previewTrack(track);
        QTRY_COMPARE_WITH_TIMEOUT(h.controller->snapshot().state, PreviewState::Error, 4000);
        QVERIFY(!h.controller->snapshot().error.isEmpty());
        QVERIFY(h.output->stream.empty());
        h.controller->shutdown();
        QCOMPARE(plan->stops.load(), 1);
        QCOMPARE(plan->destroys.load(), 1);
        QVERIFY(!plan->wrongThread);
        if(kind == u"starvation") {
            QVERIFY(plan->reads.load() < 300);
        }
    }

    void starvationIsNotEofAndShortWritesAreExact()
    {
        PreviewHarness h;
        auto plan            = h.add(u"short.wav"_s);
        plan->totalFrames    = 71;
        plan->starveReads    = 3;
        h.output->zeroWrites = true;
        h.output->maxWrite   = 3; // Deliberately split frames.
        h.create();
        h.controller->previewTrack(Track {u"short.wav"_s});
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Draining);
        QVERIFY(h.output->stream.empty());
        h.output->zeroWrites = false;
        QTRY_COMPARE(h.output->stream.size(), size_t {142});
        for(size_t i = 0; i < 71; ++i) {
            QCOMPARE(sampleAt(h.output->stream, i), static_cast<int16_t>(i));
        }
        h.output->canDrain = true;
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Stopped);
    }

    void convertedSegmentsAndSeekDiscardResamplerHistory()
    {
        PreviewHarness h;
        auto plan                  = h.add(u"converted.wav"_s);
        h.output->negotiatedFormat = AudioFormat {SampleFormat::F32, 48000, 2};
        h.create();
        Track track {u"converted.wav"_s};
        track.setOffset(100);
        track.setDuration(71);
        track.setCuePath(u"fixture.cue"_s);
        h.controller->previewTrack(track);
        QTRY_COMPARE(h.output->stream.size(), size_t {3408 * 8});
        for(size_t frame = 128; frame + 128 < 3408; frame += 16) {
            float left {}, right {};
            std::memcpy(&left, h.output->stream.data() + frame * 8, 4);
            std::memcpy(&right, h.output->stream.data() + frame * 8 + 4, 4);
            QVERIFY(std::abs(left - static_cast<float>((100.0 + frame / 48.0) / 32768)) < 1.0F / 32768);
            QCOMPARE(left, right);
        }
        h.controller->playPause();
        QVERIFY(h.controller->seek(40));
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Paused);
        QVERIFY(h.output->stream.empty());
        h.controller->playPause();
        QTRY_COMPARE(h.output->stream.size(), size_t {1488 * 8});
        float first {};
        std::memcpy(&first, h.output->stream.data(), 4);
        QVERIFY(std::abs(first - 140.0F / 32768) < 1.0F / 32768);
        QCOMPARE(h.controller->snapshot().positionMs, uint64_t {40});
    }

    void stereoPcmSurvivesPartialFrameWrites()
    {
        PreviewHarness h;
        auto plan = h.add(u"stereo.wav"_s);
        plan->format.setChannelCount(2);
        plan->totalFrames  = 71;
        plan->base         = 100;
        h.output->maxWrite = 3;
        h.create();
        h.controller->previewTrack(Track {u"stereo.wav"_s});
        QTRY_COMPARE(h.output->stream.size(), size_t {284});
        for(size_t i = 0; i < 142; ++i) {
            QCOMPARE(sampleAt(h.output->stream, i), static_cast<int16_t>(100 + i / 2));
        }
        QCOMPARE(h.output->format.channelCount(), 2);
    }

    void convertedReplacementAndSeekIsolation_data()
    {
        QTest::addColumn<int>("rate");
        QTest::addColumn<int>("channels");
        for(int rate : {44100, 96000}) {
            for(int channels : {1, 6, 8}) {
                QTest::newRow(qPrintable(QStringLiteral("%1-to-48000-%2ch").arg(rate).arg(channels)))
                    << rate << channels;
            }
        }
    }

    void convertedReplacementAndSeekIsolation()
    {
        QFETCH(int, rate);
        QFETCH(int, channels);
        PreviewHarness h;
        auto old = h.add(u"old-converted.wav"_s);
        old->format.setSampleRate(rate);
        old->format.setChannelCount(channels);
        auto next = h.add(u"next-converted.wav"_s);
        next->format.setSampleRate(rate);
        next->format.setChannelCount(channels);
        next->base                 = 10000;
        h.output->negotiatedFormat = AudioFormat {SampleFormat::F32, 48000, 2};
        h.output->maxWrite         = 3; // Leave both a partial output frame and pending PCM.
        h.create();
        h.controller->previewTrack(Track {u"old-converted.wav"_s});
        QTRY_VERIFY(!h.output->stream.empty());
        h.output->zeroWrites = true;

        Track track {u"next-converted.wav"_s};
        track.setOffset(100);
        track.setDuration(50);
        track.setCuePath(u"fixture.cue"_s);
        h.controller->previewTrack(track);
        QVERIFY(h.output->stream.empty());
        h.output->maxWrite   = std::numeric_limits<qint64>::max();
        h.output->zeroWrites = false;
        QTRY_COMPARE(h.output->stream.size(), size_t {2400 * 8});
        // The new source is strictly positive and distinct from the old one.
        // Check every sample, including both resampler edges and both channels.
        const auto verifySignal = [&](uint64_t firstFrame) {
            for(size_t frame = 0; frame < h.output->stream.size() / 8; ++frame) {
                float left {}, right {};
                std::memcpy(&left, h.output->stream.data() + frame * 8, 4);
                std::memcpy(&right, h.output->stream.data() + frame * 8 + 4, 4);
                const auto expected = (10000.0 + firstFrame + frame * rate / 48000.0) / 32768.0;
                QVERIFY2(std::abs(left - expected) < 4.0 / 32768.0, "Stale or out-of-segment converted sample");
                QCOMPARE(left, right);
            }
        };
        verifySignal(rate / 10);
        h.controller->playPause();
        // Coalesced seeks must discard the old resampler tail and pending writes.
        QVERIFY(h.controller->seek(10));
        QVERIFY(h.controller->seek(20));
        QVERIFY(h.controller->seek(30));
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Paused);
        QCOMPARE(h.controller->snapshot().positionMs, uint64_t {30});
        QVERIFY(h.output->stream.empty());
        h.controller->playPause();
        QTRY_COMPARE(h.output->stream.size(), size_t {960 * 8});
        verifySignal(rate * 130 / 1000);
        h.controller->shutdown();
        QCOMPARE(old->opens.load(), old->destroys.load());
        QCOMPARE(next->opens.load(), next->destroys.load());
        QVERIFY(!old->wrongThread && !next->wrongThread);
    }

    void convertedQueueBoundedForLongSources_data()
    {
        QTest::addColumn<quint64>("frames");
        QTest::newRow("one-hour") << quint64 {96000ULL * 3600};
        QTest::newRow("one-week") << quint64 {96000ULL * 3600 * 24 * 7};
    }

    void convertedQueueBoundedForLongSources()
    {
        QFETCH(quint64, frames);
        PreviewHarness h;
        auto plan = h.add(u"long.wav"_s);
        plan->format.setSampleRate(96000);
        plan->format.setChannelCount(8);
        plan->totalFrames = frames;
        plan->blockFrames = 4096;
        auto session      = std::make_shared<PreviewSession>(1, Track {u"long.wav"_s}, 0);
        std::atomic<bool> failed {false};
        PreviewDecoderWorker worker {
            [library = h.library]() { return std::make_unique<FakeSource>(library); },
            [](const auto& opened, const auto&) { opened->approve(AudioFormat {SampleFormat::F32, 48000, 2}); },
            [&](auto, const auto&) {
                failed = true;
            }};
        worker.submit(session);
        QTRY_VERIFY(session->buffer());
        auto queue = session->buffer();
        QTRY_COMPARE(queue->size(), queue->capacity());
        QCOMPARE(queue->capacity(), size_t {48000 / 2 * 8});
        QVERIFY(!queue->finished());
        // At most a half-second queue plus one decoder block can be in flight.
        QVERIFY(plan->reads.load() <= 14);
        worker.shutdown(); // Must wake a producer blocked in converted queue push.
        QCOMPARE(queue->size(), size_t {0});
        QCOMPARE(plan->destroys.load(), 1);
        QVERIFY(!plan->wrongThread);
        QVERIFY(!failed);
    }

    void replacementDiscardsOldEofAndPendingBytes()
    {
        PreviewHarness h;
        auto old             = h.add(u"old.wav"_s);
        old->totalFrames     = 71;
        auto next            = h.add(u"next.wav"_s);
        next->base           = 15000;
        h.output->zeroWrites = true;
        h.create();
        h.controller->previewTrack(Track {u"old.wav"_s});
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Draining);
        h.controller->previewTrack(Track {u"next.wav"_s});
        h.output->zeroWrites = false;
        QTRY_VERIFY(!h.output->stream.empty());
        QCOMPARE(sampleAt(h.output->stream, 0), int16_t {15000});
        QCOMPARE(h.controller->snapshot().state, PreviewState::Playing);
        h.controller->previewTrack(Track {u"next.wav"_s}); // Same-track re-preview restarts at zero.
        QTRY_VERIFY(!h.output->stream.empty());
        QCOMPARE(sampleAt(h.output->stream, 0), int16_t {15000});
    }

    void recoverableErrorAndPauseDuringBuffering()
    {
        PreviewHarness h;
        auto plan                = h.add(u"recover.wav"_s);
        plan->readGate           = std::make_shared<Gate>();
        h.output->configureFails = true;
        h.create();
        h.controller->previewTrack(Track {u"recover.wav"_s});
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Error);
        h.output->configureFails = false;
        h.controller->playPause();
        QTRY_VERIFY(plan->readGate->entered.load());
        QCOMPARE(h.controller->snapshot().state, PreviewState::Buffering);
        h.controller->playPause();
        plan->readGate->release();
        QTRY_VERIFY(plan->reads.load() >= 8);
        QCOMPARE(h.output->starts, 0);
        QCOMPARE(h.controller->snapshot().state, PreviewState::Paused);
        h.controller->playPause();
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Playing);
    }

    void outputErrors_data()
    {
        QTest::addColumn<QString>("kind");
        for(const auto& kind : {"configure", "start", "write", "capacity", "backend"}) {
            QTest::newRow(kind) << QString::fromLatin1(kind);
        }
    }

    void outputErrors()
    {
        QFETCH(QString, kind);
        PreviewHarness h;
        auto plan                = h.add(u"out.wav"_s);
        h.output->configureFails = kind == u"configure";
        h.output->startFails     = kind == u"start";
        h.output->writeFails     = kind == u"write";
        if(kind == u"capacity") {
            h.output->freeBytes = -1;
        }
        if(kind == u"backend") {
            h.output->failure = u"Backend failed"_s;
        }
        h.create();
        h.controller->previewTrack(Track {u"out.wav"_s});
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Error);
        QVERIFY(h.output->stream.empty());
        h.controller->shutdown();
        QCOMPARE(plan->opens.load(), plan->stops.load());
    }

    void explicitDevicePolicyAndVolume()
    {
        PreviewHarness h;
        auto plan           = h.add(u"device.wav"_s);
        h.output->available = false;
        h.create();
        h.controller->previewTrack(Track {u"device.wav"_s});
        QCOMPARE(h.controller->snapshot().state, PreviewState::Error);
        QCOMPARE(plan->opens.load(), 0);
        h.output->available = true;
        h.controller->refreshOutput();
        QCOMPARE(h.controller->snapshot().state, PreviewState::Error);
        h.controller->playPause();
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Playing);
        h.controller->setVolume(2.0);
        QCOMPARE(h.output->volume, 1.0);
        h.controller->setVolume(-1.0);
        QCOMPARE(h.output->volume, 0.0);
        h.controller->setVolume(std::numeric_limits<double>::quiet_NaN());
        QCOMPARE(h.output->volume, 0.0);
        h.output->available = false;
        h.controller->refreshOutput();
        QCOMPARE(h.controller->snapshot().state, PreviewState::Error);
        QVERIFY(h.output->stream.empty());
        h.output->available = true;
        h.controller->refreshOutput();
        QCOMPARE(h.output->starts, 1);
        auto replacement = std::make_shared<OutputState>();
        h.controller->setOutput(std::make_unique<FakeOutput>(replacement));
        QCOMPARE(h.controller->snapshot().state, PreviewState::Stopped);
        QCOMPARE(replacement->starts, 0);
    }

    void unsupportedSourcesNeverOpen_data()
    {
        QTest::addColumn<QString>("path");
        QTest::newRow("invalid") << QString {};
        QTest::newRow("remote") << u"https://example.com/music.wav"_s;
        QTest::newRow("virtual") << u"cdda://disc/1"_s;
        QTest::newRow("archive") << u"unpack://zip|6|file:///a.zip!track.wav"_s;
    }

    void unsupportedSourcesNeverOpen()
    {
        QFETCH(QString, path);
        PreviewHarness h;
        auto plan = h.add(path);
        h.create();
        h.controller->previewTrack(Track {path});
        QCOMPARE(h.controller->snapshot().state, PreviewState::Error);
        QCOMPARE(plan->opens.load(), 0);
    }
};

QTEST_GUILESS_MAIN(PreviewEngineTest)
#include "previewenginetest.moc"
