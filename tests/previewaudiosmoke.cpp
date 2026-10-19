// SPDX-License-Identifier: GPL-3.0-or-later
#include "previewcontroller.h"
#include "qtaudiooutput.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTextStream>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <numbers>

using namespace Fooyin;
using namespace Fooyin::Headphones;

namespace {
class ToneSource final : public PreviewSource
{
public:
    std::optional<PreviewSourceInfo> open(const Track&, QString&) override
    {
        return PreviewSourceInfo {AudioFormat {SampleFormat::F32, 44100, 1}, true};
    }
    bool seek(uint64_t ms, QString&) override
    {
        m_frame = ms * 44100 / 1000;
        return true;
    }
    AudioDecoder::ReadResult read(size_t maxBytes) override
    {
        if(m_aborted || m_frame >= 44100) {
            return AudioDecoder::ReadResult::endOfStream();
        }
        const size_t count = std::min<size_t>(maxBytes / sizeof(float), 44100 - m_frame);
        std::vector<float> signal(count);
        for(size_t i = 0; i < count; ++i) {
            signal[i] = static_cast<float>(0.2 * std::sin(2 * std::numbers::pi * 440 * (m_frame + i) / 44100));
        }
        AudioBuffer buffer {reinterpret_cast<const uint8_t*>(signal.data()),
                            count * sizeof(float),
                            {SampleFormat::F32, 44100, 1},
                            m_frame * 1000 / 44100};
        m_frame += count;
        return AudioDecoder::ReadResult::data(std::move(buffer));
    }
    void requestAbort() override
    {
        m_aborted = true;
    }
    void stop() override { }

private:
    uint64_t m_frame {0};
    std::atomic<bool> m_aborted {false};
};
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app {argc, argv};
    const auto args = app.arguments();
    HeadphoneDevices devices;
    if(args.size() == 2 && args[1] == QStringLiteral("--list-devices")) {
        QTimer::singleShot(500, &app, [&]() {
            QTextStream out {stdout};
            for(const auto& device : devices.devices()) {
                out << encodeDeviceId(device.id) << '\t' << device.description << '\n';
            }
            app.quit();
        });
        return app.exec();
    }
    if(args.size() != 3 || args[1] != QStringLiteral("--device-id") || decodeDeviceId(args[2]).isEmpty()) {
        qCritical("Usage: previewaudiosmoke --list-devices | --device-id BASE64_ID (plays a one-second tone)");
        return 1;
    }
    PreviewController controller {[]() { return std::make_unique<ToneSource>(); },
                                  std::make_unique<QtAudioOutput>(devices, decodeDeviceId(args[2]))};
    QElapsedTimer elapsed;
    bool started {false};
    QObject::connect(&controller, &PreviewController::snapshotChanged, &app, [&](const PreviewSnapshot& snapshot) {
        if(snapshot.state == PreviewState::Error) {
            qCritical() << snapshot.error;
            app.exit(2);
        }
        else if(snapshot.state == PreviewState::Playing || snapshot.state == PreviewState::Draining) {
            started = true;
        }
        else if(started && snapshot.state == PreviewState::Stopped) {
            started = false;
            qInfo() << "Explicit-device QAudioSink smoke passed; elapsed" << elapsed.elapsed() << "ms";
            app.exit(0);
        }
    });
    QTimer::singleShot(10000, &app, [&]() {
        qCritical("Audio smoke timed out");
        app.exit(3);
    });
    QTimer::singleShot(500, &app, [&]() {
        elapsed.start();
        controller.previewTrack(Track {QStringLiteral("generated-tone.wav")});
    });
    const int result = app.exec();
    QObject::disconnect(&controller, nullptr, &app, nullptr);
    controller.shutdown();
    return result;
}
