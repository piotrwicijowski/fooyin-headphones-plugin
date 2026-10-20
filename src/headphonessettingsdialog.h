// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <gui/plugins/pluginsettingsprovider.h>

#include <QPointer>

class QComboBox;
class QSpinBox;
namespace Fooyin::Headphones {
class HeadphonesSettings;
class HeadphoneDeviceCatalogue;

class HeadphonesSettingsDialog : public QDialog
{
    Q_OBJECT
public:
    HeadphonesSettingsDialog(HeadphonesSettings& settings, HeadphoneDeviceCatalogue& devices,
                             QWidget* parent = nullptr);

private:
    void done(int result) override;
    void showEvent(QShowEvent* event) override;
    void resetPending();
    void refreshDevices();
    void apply();
    QPointer<HeadphonesSettings> m_settings;
    QPointer<HeadphoneDeviceCatalogue> m_devices;
    QComboBox* m_combo;
    QSpinBox* m_startOffset;
    QByteArray m_pendingId;
    QString m_pendingDescription;
};

// Shared by the host's provider adapter and all player-control configure buttons.
class HeadphonesSettingsProvider : public PluginSettingsProvider
{
public:
    HeadphonesSettingsProvider(HeadphonesSettings& settings, HeadphoneDeviceCatalogue& devices);
    ~HeadphonesSettingsProvider() override;
    QDialog* createSettings(QWidget* parent) override;

private:
    QPointer<HeadphonesSettings> m_settings;
    QPointer<HeadphoneDeviceCatalogue> m_devices;
    QPointer<QDialog> m_dialog;
};

std::unique_ptr<PluginSettingsProvider> settingsProviderAdapter(std::weak_ptr<HeadphonesSettingsProvider> provider);

} // namespace Fooyin::Headphones
