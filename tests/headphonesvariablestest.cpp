// SPDX-License-Identifier: GPL-3.0-or-later
#include "fakes/previewfakes.h"
#include "headphonesvariableprovider.h"

#include <core/scripting/scriptparser.h>
#include <core/scripting/scriptregistry.h>

#include <QSignalSpy>
#include <QTest>

#include <future>

using namespace Qt::StringLiterals;
using namespace Fooyin;
using namespace Fooyin::Headphones;
using namespace Fooyin::Headphones::Tests;

namespace {
struct ScriptHarness
{
    PreviewHarness preview;
    std::unique_ptr<HeadphonesVariableProvider> provider;
    ScriptRegistry registry;
    ScriptParser parser {&registry};

    ScriptHarness()
    {
        preview.create();
        provider = std::make_unique<HeadphonesVariableProvider>(*preview.controller);
        for(const auto& variable : provider->variables()) {
            if(!registry.registerVariable(variable)) {
                qFatal("Failed to register headphone script variable");
            }
        }
        registry.finalise();
    }

    ~ScriptHarness()
    {
        registry.shutdown();
    }
};
} // namespace

class HeadphonesVariablesTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void metadataUsesPreviewRatherThanScriptSubject()
    {
        ScriptHarness h;
        h.preview.add(u"preview.wav"_s);
        Track preview {u"preview.wav"_s};
        preview.setAlbumArtists({u"Album artist"_s});
        preview.setArtists({u"Artist"_s});
        preview.setAlbum(u"Album"_s);
        preview.setTitle(u"Title"_s);
        preview.setDiscNumber(u"02"_s);
        preview.setTrackNumber(u"07"_s);
        Track main {u"main.wav"_s};
        main.setTitle(u"Main title"_s);
        const auto script = h.parser.parse(u"[%headphones_albumartist%]|[%headphones_artist%]|[%headphones_album%]|"
                                           "[%headphones_title%]|[%headphones_disk%]|[%headphones_track%]"_s);
        QVERIFY(script.isValid());
        QCOMPARE(h.parser.evaluate(script, main), u"|||||"_s);
        h.preview.controller->previewTrack(preview);
        QTRY_COMPARE(h.preview.controller->snapshot().state, PreviewState::Playing);
        QCOMPARE(h.parser.evaluate(script, main), u"Album artist|Artist|Album|Title|02|07"_s);
        QCOMPARE(h.parser.evaluate(script), u"Album artist|Artist|Album|Title|02|07"_s);
        QCOMPARE(h.parser.evaluate(u"%HEADPHONES_DISC%/%Headphones_TrackNumber%"_s), u"02/07"_s);
        QCOMPARE(h.parser.evaluate(u"%title%"_s, main), u"Main title"_s);
        h.preview.controller->stop();
        QCOMPARE(h.parser.evaluate(script), u"Album artist|Artist|Album|Title|02|07"_s);
        h.preview.controller->shutdown();
        QCOMPARE(h.parser.evaluate(script), u"Album artist|Artist|Album|Title|02|07"_s);
    }

    void metadataFallbacksAndConditionals()
    {
        ScriptHarness h;
        h.preview.add(u"untagged.wav"_s);
        Track track {u"untagged.wav"_s};
        track.setArtists({u"Fallback artist"_s});
        h.preview.controller->previewTrack(track);
        QCOMPARE(h.parser.evaluate(u"%headphones_title%/%headphones_albumartist%"_s), u"untagged/Fallback artist"_s);
        QCOMPARE(h.parser.evaluate(u"[Album: %headphones_album%]$if(%headphones_track%,numbered,unnumbered)"_s),
                 u"unnumbered"_s);
        track.setArtists({});
        track.setAlbumArtists({u"Fallback album artist"_s});
        h.preview.controller->previewTrack(track);
        QCOMPARE(h.parser.evaluate(u"%headphones_artist%"_s), u"Fallback album artist"_s);
        track.setAlbumArtists({});
        track.setComposers({u"Composer"_s});
        h.preview.controller->previewTrack(track);
        QCOMPARE(h.parser.evaluate(u"%headphones_artist%/%headphones_albumartist%"_s), u"Composer/Composer"_s);
    }

    void statesIncludePendingPauseErrorAndEof()
    {
        ScriptHarness h;
        const auto script = h.parser.parse(u"[%headphones_isplaying%]/[%headphones_ispaused%]/"
                                           "[%headphones_isstopped%]"_s);
        QCOMPARE(h.parser.evaluate(script), u"//1"_s);
        const auto plan   = h.preview.add(u"preview.wav"_s);
        plan->openGate    = std::make_shared<Gate>();
        plan->totalFrames = 100;
        Track track {u"preview.wav"_s};
        h.preview.controller->previewTrack(track);
        QCOMPARE(h.preview.controller->snapshot().state, PreviewState::Loading);
        QCOMPARE(h.parser.evaluate(script), u"1//"_s);
        h.preview.controller->playPause();
        QCOMPARE(h.preview.controller->snapshot().state, PreviewState::Loading);
        QCOMPARE(h.parser.evaluate(script), u"/1/"_s);
        plan->openGate->release();
        QTRY_COMPARE(h.preview.controller->snapshot().state, PreviewState::Paused);
        QCOMPARE(h.parser.evaluate(script), u"/1/"_s);
        h.preview.controller->playPause();
        QTRY_COMPARE(h.preview.controller->snapshot().state, PreviewState::Draining);
        QCOMPARE(h.parser.evaluate(script), u"1//"_s);
        h.preview.output->canDrain = true;
        QTRY_COMPARE(h.preview.controller->snapshot().state, PreviewState::Stopped);
        QCOMPARE(h.parser.evaluate(script), u"//1"_s);
        QCOMPARE(h.parser.evaluate(u"[%headphonesicon%]"_s, track), QString {});
        h.preview.controller->setOutput(nullptr);
        h.preview.controller->previewTrack(track);
        QCOMPARE(h.preview.controller->snapshot().state, PreviewState::Error);
        QCOMPARE(h.parser.evaluate(script), u"//1"_s);
        QCOMPARE(h.parser.evaluate(u"[%headphonesicon%]"_s, track), QString {});
    }

    void iconFollowsIdentityReplacementPauseAndStop()
    {
        ScriptHarness h;
        h.preview.add(u"preview.wav"_s);
        h.preview.add(u"other.wav"_s);
        Track track {u"preview.wav"_s};
        track.setId(123);
        const auto script = h.parser.parse(u"[%headphonesicon%]"_s);
        h.preview.controller->previewTrack(track);
        QCOMPARE(h.parser.evaluate(script, track), u"\U0001F3A7"_s);
        Track duplicate = track;
        duplicate.setTitle(u"Changed metadata"_s);
        QCOMPARE(h.parser.evaluate(script, duplicate), u"\U0001F3A7"_s);
        QCOMPARE(h.parser.evaluate(script), QString {});
        Track other {u"other.wav"_s};
        QCOMPARE(h.parser.evaluate(script, other), QString {});
        h.preview.controller->playPause();
        QCOMPARE(h.parser.evaluate(script, track), u"\U0001F3A7"_s);
        h.preview.controller->previewTrack(other);
        QCOMPARE(h.parser.evaluate(script, track), QString {});
        QCOMPARE(h.parser.evaluate(script, other), u"\U0001F3A7"_s);
        h.preview.controller->stop();
        QCOMPARE(h.parser.evaluate(script, other), QString {});

        // Without database ids, logical segments in one physical file stay distinct.
        Track segment {u"preview.wav"_s};
        segment.setDuration(10000);
        segment.setOffset(10000);
        h.preview.controller->previewTrack(segment);
        Track nextSegment = segment;
        nextSegment.setOffset(20000);
        QCOMPARE(h.parser.evaluate(script, segment), u"\U0001F3A7"_s);
        QCOMPARE(h.parser.evaluate(script, nextSegment), QString {});
    }

    void refreshesOnlyWhenScriptValuesChange()
    {
        ScriptHarness h;
        h.preview.add(u"preview.wav"_s);
        QSignalSpy changed {h.provider.get(), &HeadphonesVariableProvider::variablesChanged};
        Track track {u"preview.wav"_s};
        h.preview.controller->previewTrack(track);
        QCOMPARE(changed.count(), 1);
        QTRY_COMPARE(h.preview.controller->snapshot().state, PreviewState::Playing);
        QCOMPARE(changed.count(), 1);
        h.preview.controller->setVolume(0.5);
        h.preview.output->processed = 100;
        QTRY_COMPARE(h.preview.controller->snapshot().positionMs, uint64_t {100});
        QCOMPARE(changed.count(), 1);
        h.preview.controller->playPause();
        QCOMPARE(changed.count(), 2);
        QVERIFY(h.preview.controller->seek(200));
        QTRY_COMPARE(h.preview.controller->snapshot().state, PreviewState::Paused);
        QCOMPARE(changed.count(), 2);
        h.preview.controller->stop();
        QCOMPARE(changed.count(), 3);
        h.preview.controller->stop();
        QCOMPARE(changed.count(), 3);
        track.setTitle(u"Updated title"_s);
        h.preview.controller->previewTrack(track);
        QCOMPARE(changed.count(), 4);
        QCOMPARE(h.parser.evaluate(u"%headphones_title%"_s), u"Updated title"_s);
    }

    void callbacksCanEvaluateDuringPreviewChanges()
    {
        ScriptHarness h;
        h.preview.add(u"first.wav"_s);
        h.preview.add(u"second.wav"_s);
        const Track first {u"first.wav"_s};
        const Track second {u"second.wav"_s};
        h.preview.controller->previewTrack(first);
        std::atomic<bool> started {false};
        std::atomic<bool> finished {false};
        auto reader = std::async(std::launch::async, [&]() {
            ScriptParser parser {&h.registry};
            const auto titleScript = parser.parse(u"%headphones_title%"_s);
            const auto iconScript  = parser.parse(u"[%headphonesicon%]"_s);
            bool valid {true};
            started = true;
            do {
                const auto title = parser.evaluate(titleScript);
                const auto icon  = parser.evaluate(iconScript, first);
                valid = valid && (title == u"first" || title == u"second") && (icon.isEmpty() || icon == u"\U0001F3A7");
            } while(!finished.load());
            return valid;
        });
        // Always join the reader before allowing a failing assertion to leave this scope.
        while(!started.load()) {
            std::this_thread::yield();
        }
        for(int i {0}; i < 100; ++i) {
            h.preview.controller->previewTrack(i % 2 ? first : second);
            h.preview.controller->playPause();
            h.preview.controller->stop();
        }
        finished = true;
        QVERIFY(reader.get());
    }
};

QTEST_GUILESS_MAIN(HeadphonesVariablesTest)
#include "headphonesvariablestest.moc"
