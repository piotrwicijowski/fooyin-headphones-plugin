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
    // 0 = automatic, positive odd values = 1-based left channel; -1 = invalid saved route.
    [[nodiscard]] int outputChannelPair() const;
    [[nodiscard]] double volume() const;
    [[nodiscard]] int previewStartOffsetSeconds() const;
    void setDevice(const QByteArray& id, const QString& description);
    void setDestination(const QByteArray& id, const QString& description, int firstChannel);
    void setVolume(double uiVolume);
    void setPreviewStartOffsetSeconds(int seconds);

Q_SIGNALS:
    void deviceChanged();
    void volumeChanged();
    void previewStartOffsetChanged();

private:
    void refreshDestination();
    SettingsManager* m_settings;
    QByteArray m_lastDeviceId;
    int m_lastChannelPair {0};
    bool m_updatingDestination {false};
};

} // namespace Fooyin::Headphones
