// SPDX-License-Identifier: GPL-3.0-or-later
#include "fakes/previewfakes.h"

#include <core/engine/audioloader.h>

#include <QDataStream>
#include <QFile>
#include <QPointer>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

using namespace Qt::StringLiterals;
using namespace Fooyin;
using namespace Fooyin::Headphones;
using namespace Fooyin::Headphones::Tests;

namespace {
struct DecoderState
{
    bool concurrent {true};
    bool validFormat {true};
    std::atomic<int> starts {0};
    std::atomic<int> stops {0};
    std::atomic<int> destroys {0};
    std::atomic<bool> wrongThread {false};
    std::atomic<bool> goodOptions {false};
    std::atomic<bool> inputAliveAtStop {false};
    std::atomic<bool> inputAliveAtDestroy {false};
    QPointer<QIODevice> device;
};

// A registered decoder that actually reads a generated mono S16 WAV. This tests
// the real AudioLoader's ownership/CarrierDecoder boundary using public APIs.
class FixtureDecoder final : public AudioDecoder
{
public:
    explicit FixtureDecoder(std::shared_ptr<DecoderState> state)
        : m_state {std::move(state)}
        , m_owner {std::this_thread::get_id()}
    {
    }

    ~FixtureDecoder() override
    {
        if(m_initialised) {
            checkThread();
            m_state->inputAliveAtDestroy = m_device && m_device->isOpen();
            ++m_state->destroys;
        }
    }
    QStringList extensions() const override
    {
        return {u"wav"_s};
    }
    bool isSeekable() const override
    {
        return true;
    }
    bool allowsConcurrentDecoding() const override
    {
        return m_state->concurrent;
    }
    std::optional<AudioFormat> init(const AudioSource& source, const Track&, DecoderOptions options) override
    {
        checkThread();
        m_initialised        = true;
        m_device             = source.device;
        m_state->device      = m_device;
        m_state->goodOptions = options == NoLooping && playbackHints() == NoHints;
        if(!m_device || m_device->thread() != QThread::currentThread() || m_device->read(4) != "RIFF") {
            m_state->wrongThread = true;
        }
        m_device->seek(44);
        return m_state->validFormat ? AudioFormat {SampleFormat::S16, 44100, 1} : AudioFormat {};
    }
    void start() override
    {
        checkThread();
        ++m_state->starts;
    }
    void stop() override
    {
        checkThread();
        m_state->inputAliveAtStop = m_device && m_device->isOpen();
        ++m_state->stops;
    }
    void seek(uint64_t ms) override
    {
        checkThread();
        m_frame = ms * 44100 / 1000;
        m_device->seek(static_cast<qint64>(44 + m_frame * 2));
    }
    ReadResult readAudio(size_t bytes) override
    {
        checkThread();
        const auto data = m_device->read(static_cast<qint64>(bytes));
        if(data.isEmpty()) {
            return ReadResult::endOfStream();
        }
        AudioBuffer buffer {reinterpret_cast<const uint8_t*>(data.constData()), static_cast<size_t>(data.size()),
                            AudioFormat {SampleFormat::S16, 44100, 1}, m_frame * 1000 / 44100};
        m_frame += data.size() / 2;
        return ReadResult::data(std::move(buffer));
    }
    AudioBuffer readBuffer(size_t bytes) override
    {
        return readAudio(bytes).buffer;
    }

private:
    void checkThread()
    {
        if(m_owner != std::this_thread::get_id()) {
            m_state->wrongThread = true;
        }
    }
    std::shared_ptr<DecoderState> m_state;
    QIODevice* m_device {nullptr};
    std::thread::id m_owner;
    uint64_t m_frame {0};
    bool m_initialised {false};
};

bool writeWave(const QString& path)
{
    QFile file {path};
    if(!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    QDataStream stream {&file};
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData("RIFF", 4);
    stream << quint32 {36 + 4000};
    stream.writeRawData("WAVEfmt ", 8);
    stream << quint32 {16} << quint16 {1} << quint16 {1} << quint32 {44100} << quint32 {88200} << quint16 {2}
           << quint16 {16};
    stream.writeRawData("data", 4);
    stream << quint32 {4000};
    for(qint16 i = 0; i < 2000; ++i) {
        stream << i;
    }
    return stream.status() == QDataStream::Ok;
}
} // namespace

class PreviewSourceTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void loaderBundleAndLogicalSegments_data()
    {
        QTest::addColumn<bool>("chapter");
        QTest::newRow("cue") << false;
        QTest::newRow("chapter") << true;
    }

    void loaderBundleAndLogicalSegments()
    {
        QFETCH(bool, chapter);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath(u"fixture.wav"_s);
        QVERIFY(writeWave(path));
        auto state  = std::make_shared<DecoderState>();
        auto loader = std::make_shared<AudioLoader>();
        loader->addDecoder(u"Preview WAV fixture"_s, [state]() { return std::make_unique<FixtureDecoder>(state); });
        std::weak_ptr<AudioLoader> weakLoader = loader;
        auto output                           = std::make_shared<OutputState>();
        PreviewController controller {makePreviewSourceFactory(loader), std::make_unique<FakeOutput>(output)};
        loader.reset(); // Only plugin-owned source/factory references keep it alive.
        QVERIFY(!weakLoader.expired());
        Track track {path};
        track.setOffset(chapter ? 27 : 10);
        track.setDuration(chapter ? 13 : 17);
        if(chapter) {
            track.setIsChapter(true);
        }
        else {
            track.setCuePath(directory.filePath(u"fixture.cue"_s));
        }
        controller.previewTrack(track);
        const size_t FirstFrame = chapter ? 1190 : 441;
        const size_t FrameCount = chapter ? 574 : 749;
        QTRY_COMPARE(output->stream.size(), FrameCount * sizeof(int16_t));
        for(size_t i = 0; i < FrameCount; ++i) {
            QCOMPARE(sampleAt(output->stream, i), static_cast<int16_t>(FirstFrame + i));
        }
        controller.shutdown();
        QCOMPARE(state->starts.load(), 1);
        QCOMPARE(state->stops.load(), 1);
        QCOMPARE(state->destroys.load(), 1);
        QVERIFY(state->goodOptions);
        QVERIFY(state->inputAliveAtStop);
        QVERIFY(state->inputAliveAtDestroy);
        QVERIFY(state->device.isNull());
        QVERIFY(!state->wrongThread);
        QVERIFY(weakLoader.expired());
    }

    void rejectedDecoderStillStopsWithLiveInput_data()
    {
        QTest::addColumn<bool>("validFormat");
        QTest::newRow("concurrent-decoding-disabled") << true;
        QTest::newRow("invalid-format") << false;
    }

    void rejectedDecoderStillStopsWithLiveInput()
    {
        QFETCH(bool, validFormat);
        QTemporaryDir directory;
        const auto path = directory.filePath(u"fixture.wav"_s);
        QVERIFY(writeWave(path));
        auto state         = std::make_shared<DecoderState>();
        state->validFormat = validFormat;
        state->concurrent  = false;
        auto loader        = std::make_shared<AudioLoader>();
        loader->addDecoder(u"Rejected fixture"_s, [state]() { return std::make_unique<FixtureDecoder>(state); });
        auto output = std::make_shared<OutputState>();
        PreviewController controller {makePreviewSourceFactory(loader), std::make_unique<FakeOutput>(output)};
        controller.previewTrack(Track {path});
        QTRY_COMPARE(controller.snapshot().state, PreviewState::Error);
        controller.shutdown();
        QCOMPARE(state->starts.load(), 0);
        QCOMPARE(state->stops.load(), 1);
        QCOMPARE(state->destroys.load(), 1);
        QVERIFY(state->inputAliveAtStop);
        QVERIFY(state->inputAliveAtDestroy);
        QVERIFY(!state->wrongThread);
        QVERIFY(output->stream.empty());
    }
};

QTEST_GUILESS_MAIN(PreviewSourceTest)
#include "previewsourcetest.moc"
