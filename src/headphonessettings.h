// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>

namespace Fooyin {
class SettingsManager;
}

namespace Fooyin::Headphones {

// Persistent device/volume policy; no playback state is stored or restored.
class HeadphonesSettings : public QObject
{
    Q_OBJECT
public:
    explicit HeadphonesSettings(SettingsManager* settings, QObject* parent = nullptr);
    [[nodiscard]] QByteArray deviceId() const;
    [[nodiscard]] QString deviceDescription() const;
    [[nodiscard]] double volume() const;
    void setDevice(const QByteArray& id, const QString& description);
    void setVolume(double uiVolume);

Q_SIGNALS:
    void deviceChanged();
    void volumeChanged();

private:
    SettingsManager* m_settings;
};

} // namespace Fooyin::Headphones
