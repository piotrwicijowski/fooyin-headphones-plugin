// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "headphonescontrolwidget.h"

#include <QPointer>

namespace Fooyin {
class SettingsManager;
class ToolButton;
namespace Headphones {
class PreviewController;
class HeadphonesVolumeControl : public HeadphonesControlWidget
{
    Q_OBJECT
public:
    HeadphonesVolumeControl(PreviewController& controller, SettingsManager* settings, QWidget* parent = nullptr);
    QString name() const override;
    QString layoutName() const override;
    void saveLayoutData(QJsonObject& layout) override;
    void loadLayoutData(const QJsonObject& layout) override;
    void populateContextMenu(QMenu* menu) override;

protected:
    void applyOrientation(Qt::Orientation orientation) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void refresh();
    void updateDisplay();
    void showVolumeMenu();
    QPointer<PreviewController> m_controller;
    HeadphonesSlider* m_slider;
    ToolButton* m_icon;
    QWidget* m_volumeMenu;
    bool m_showSlider {true};
    bool m_showIcon {true};
};
} // namespace Headphones
} // namespace Fooyin
