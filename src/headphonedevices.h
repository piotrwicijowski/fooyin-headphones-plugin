// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <core/engine/audioformat.h>

#include <QAudioDevice>
#include <QMediaDevices>
#include <QObject>

#include <functional>
#include <optional>
#include <vector>

namespace Fooyin::Headphones {

struct HeadphoneDevice
{
    QByteArray id;
    QString description;
    AudioFormat preferredFormat;
    std::function<bool(const AudioFormat&)> supportsFormat;
    QAudioDevice qtDevice; // Only real catalogue entries carry an actual Qt device.
    // Concrete formats with a known, ordered channel map, never inferred from a maximum count.
    std::vector<AudioFormat> pairFormats;
};

class HeadphoneDeviceCatalogue : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    [[nodiscard]] virtual std::vector<HeadphoneDevice> devices() const = 0;
    [[nodiscard]] std::optional<HeadphoneDevice> resolve(const QByteArray& id) const;

Q_SIGNALS:
    void devicesChanged();
};

class HeadphoneDevices final : public HeadphoneDeviceCatalogue
{
public:
    explicit HeadphoneDevices(QObject* parent = nullptr);
    [[nodiscard]] std::vector<HeadphoneDevice> devices() const override;

private:
    QMediaDevices m_mediaDevices;
};

[[nodiscard]] QString encodeDeviceId(const QByteArray& id);
[[nodiscard]] QByteArray decodeDeviceId(const QString& encoded);
[[nodiscard]] QAudioFormat qtPreviewFormat(const AudioFormat& format);
[[nodiscard]] bool isExplicitPairFormat(const AudioFormat& format, int firstChannel);
[[nodiscard]] std::vector<int> previewChannelPairs(const HeadphoneDevice& device);
[[nodiscard]] std::optional<AudioFormat> negotiatePreviewFormat(const HeadphoneDevice& device, const AudioFormat& input,
                                                                int firstChannel = 0);

} // namespace Fooyin::Headphones
