// SPDX-License-Identifier: GPL-3.0-or-later
// Opt-in real Qt output/converter probe; never installed or run by CTest.
#include "headphonedevices.h"
#include "previewconverter.h"
#include "qtaudiooutput.h"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QTimer>

#include <cmath>
#include <numbers>

using namespace Fooyin;
using namespace Fooyin::Headphones;
using namespace Qt::StringLiterals;

namespace {
struct Stream
{
    QtAudioOutput output;
    PreviewConverter converter;
    std::vector<std::byte> pending;
    size_t offset {0};
    uint64_t frame {0};
    double frequency;

    Stream(HeadphoneDeviceCatalogue& devices, const QByteArray& id, int pair, double frequency_)
        : output {devices, id, {}, pair}
        , frequency {frequency_}
    {
    }

    bool start(QString& error)
    {
        const auto format = output.configure({SampleFormat::F32, 44100, 2}, error);
        if(!format || !converter.configure({SampleFormat::F32, 44100, 2}, *format, error, 1.0, output.channelPair())) {
            return false;
        }
        pending.clear();
        offset = 0;
        output.setVolume(1.0);
        return output.start(error);
    }

    bool pump(QString& error)
    {
        if(!output.error().isEmpty()) {
            error = output.error();
            return false;
        }
        for(int attempt = 0; attempt < 4 && output.bytesFree() > 0; ++attempt) {
            if(offset == pending.size()) {
                std::vector<float> data(1024 * 2);
                for(size_t i = 0; i < 1024; ++i) {
                    data[i * 2]     = 0.1F * std::sin(2 * std::numbers::pi * frequency * (frame + i) / 44100);
                    data[i * 2 + 1] = 0.1F * std::sin(2 * std::numbers::pi * frequency * 1.5 * (frame + i) / 44100);
                }
                const AudioBuffer input {reinterpret_cast<const uint8_t*>(data.data()),
                                         data.size() * sizeof(float),
                                         {SampleFormat::F32, 44100, 2},
                                         0};
                auto converted = converter.convert(input, error);
                if(!converted) {
                    return false;
                }
                frame += 1024;
                pending = std::move(*converted);
                offset  = 0;
            }
            const auto count   = std::min<size_t>(pending.size() - offset, output.bytesFree());
            const auto written = output.write(std::span {pending}.subspan(offset, count));
            if(written < 0) {
                error = u"Probe output write failed."_s;
                return false;
            }
            offset += static_cast<size_t>(written);
        }
        return true;
    }
};
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app {argc, argv};
    const auto arguments = app.arguments();
    if(arguments.size() < 3) {
        qCritical() << "Usage: channelpairprobe BASE64_DEVICE_ID FIRST_CHANNEL [--concurrent]";
        return 2;
    }
    const auto id  = decodeDeviceId(arguments[1]);
    const int pair = arguments[2].toInt();
    HeadphoneDevices devices;
    std::vector<std::unique_ptr<Stream>> streams;
    QTimer timer;
    QElapsedTimer elapsed;
    bool paused = false, resumed = false, replaced = false;
    QTimer::singleShot(300, &app, [&] {
        const auto device = devices.resolve(id);
        if(!device || previewChannelPairs(*device) != std::vector<int> {1, 3}) {
            qCritical() << "Probe requires a concrete quad device with pairs 1/2 and 3/4.";
            app.exit(2);
            return;
        }
        streams.push_back(std::make_unique<Stream>(devices, id, pair, pair == 1 ? 220 : 440));
        if(arguments.contains(u"--concurrent"_s)) {
            streams.push_back(std::make_unique<Stream>(devices, id, 1, 220));
        }
        QString error;
        for(auto& stream : streams) {
            if(!stream->start(error)) {
                qCritical() << error;
                app.exit(2);
                return;
            }
        }
        elapsed.start();
        timer.start(5);
    });
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        QString error;
        const auto ms = elapsed.elapsed();
        // Preview pause/resume and sink replacement must preserve a concurrent main stream.
        auto& preview = *streams.front();
        if(ms >= 400 && !paused) {
            preview.output.setPaused(true);
            paused = true;
        }
        if(ms >= 650 && !resumed) {
            preview.output.setPaused(false);
            resumed = true;
        }
        if(ms >= 1100 && !replaced) {
            if(!preview.start(error)) {
                qCritical() << error;
                app.exit(2);
                return;
            }
            replaced = true;
        }
        for(auto& stream : streams) {
            if(!stream->pump(error)) {
                qCritical() << error;
                app.exit(2);
                return;
            }
        }
        if(ms >= 2200) {
            app.quit();
        }
    });
    QTimer::singleShot(10000, &app, [&] { app.exit(3); });
    return app.exec();
}
