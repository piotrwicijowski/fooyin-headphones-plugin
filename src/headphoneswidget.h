// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "previewtypes.h"

#include <gui/fywidget.h>

#include <QPointer>

#include <memory>

class QLabel;
class QPushButton;
class QSlider;
namespace Fooyin::Headphones {
class PreviewController;
class HeadphonesActions;
class HeadphonesSettings;
class HeadphoneDeviceCatalogue;
class HeadphonesSettingsProvider;

class HeadphonesWidget : public FyWidget
{
    Q_OBJECT
public:
    HeadphonesWidget(PreviewController& controller, HeadphonesActions& actions, HeadphonesSettings& settings,
                     HeadphoneDeviceCatalogue& devices, std::weak_ptr<HeadphonesSettingsProvider> provider,
                     QWidget* parent = nullptr);
    QString name() const override;
    QString layoutName() const override;

private:
    void refresh(const PreviewSnapshot& snapshot);
    void refreshDevice();
    void requestSeek(int sliderValue);
    void showTentative(int sliderValue);
    QPointer<PreviewController> m_controller;
    QPointer<HeadphonesActions> m_actions;
    QPointer<HeadphonesSettings> m_settings;
    QPointer<HeadphoneDeviceCatalogue> m_devices;
    QLabel* m_title;
    QLabel* m_device;
    QLabel* m_status;
    QLabel* m_position;
    QLabel* m_volumeValue;
    QPushButton* m_preview;
    QPushButton* m_playPause;
    QSlider* m_seek;
    QSlider* m_volume;
    PreviewSnapshot m_snapshot;
    bool m_updating {false};
    bool m_scrubbing {false};
    bool m_seekPending {false};
    uint64_t m_target {0};
};

} // namespace Fooyin::Headphones
