// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>

#include <limits>

namespace Fooyin {
class SettingsManager;
}

namespace Fooyin::Headphones {

// Persistent preview policy; no playback state is stored or restored.
class HeadphonesSettings : public QObject
{
    Q_OBJECT
public:
    static constexpr int MaxPreviewStartOffsetSeconds = std::numeric_limits<int>::max();

    explicit HeadphonesSettings(SettingsManager* settings, QObject* parent = nullptr);
    [[nodiscard]] QByteArray deviceId() const;
    [[nodiscard]] QString deviceDescription() const;
    [[nodiscard]] double volume() const;
    [[nodiscard]] int previewStartOffsetSeconds() const;
    void setDevice(const QByteArray& id, const QString& description);
    void setVolume(double uiVolume);
    void setPreviewStartOffsetSeconds(int seconds);

Q_SIGNALS:
    void deviceChanged();
    void volumeChanged();
    void previewStartOffsetChanged();

private:
    SettingsManager* m_settings;
};

} // namespace Fooyin::Headphones
