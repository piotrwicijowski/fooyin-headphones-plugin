// SPDX-License-Identifier: GPL-3.0-or-later
#include "headphonesvolumecontrol.h"

#include "previewcontroller.h"

#include <gui/guiconstants.h>
#include <gui/guisettings.h>
#include <gui/iconloader.h>
#include <gui/widgets/toolbutton.h>
#include <utils/settings/settingsmanager.h>

#include <QApplication>
#include <QBoxLayout>
#include <QJsonObject>
#include <QMenu>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace Fooyin::Headphones {
HeadphonesVolumeControl::HeadphonesVolumeControl(PreviewController& controller, SettingsManager* settings, QWidget* parent)
    : HeadphonesControlWidget {parent}
    , m_controller {&controller}
    , m_slider {new HeadphonesSlider(this)}
    , m_icon {settings ? new ToolButton(settings, this) : new ToolButton(this)}
    , m_volumeMenu {new QWidget(this, Qt::ToolTip | Qt::FramelessWindowHint)}
{
    m_slider->setObjectName(u"Headphones.Volume"_s);
    m_slider->setAccessibleName(tr("Headphone volume"));
    m_slider->setRange(0, 100);
    m_icon->setObjectName(u"Headphones.Mute"_s);
    m_icon->setCheckable(true);
    m_layout->addWidget(m_slider, 1);
    m_layout->addWidget(m_icon);
    new QVBoxLayout(m_volumeMenu);
    auto* timer = new QTimer(this);
    timer->setInterval(1000);
    connect(timer, &QTimer::timeout, this, [this]() {
        if(!m_volumeMenu->underMouse() && !m_icon->underMouse() && !m_slider->isSliderDown()) {
            m_volumeMenu->hide();
        }
    });
    timer->start();
    connect(m_slider, &QSlider::valueChanged, this, [this](int value) {
        if(m_controller) {
            m_controller->setVolume(value / 100.0);
        }
    });
    connect(m_icon, &ToolButton::clicked, &controller, &PreviewController::toggleMute);
    connect(m_icon, &ToolButton::entered, this, &HeadphonesVolumeControl::showVolumeMenu);
    connect(&controller, &PreviewController::snapshotChanged, this, &HeadphonesVolumeControl::refresh);
    if(settings) {
        settings->subscribe<Settings::Gui::IconTheme>(this, [this]() { refresh(); });
    }
    updateDisplay();
    refresh();
}

QString HeadphonesVolumeControl::name() const
{
    return tr("Headphones Volume Controls");
}

QString HeadphonesVolumeControl::layoutName() const
{
    return u"HeadphonesVolumeControls"_s;
}

void HeadphonesVolumeControl::saveLayoutData(QJsonObject& layout)
{
    HeadphonesControlWidget::saveLayoutData(layout);
    layout["ShowSlider"_L1] = m_showSlider;
    layout["ShowIcon"_L1]   = m_showIcon;
}

void HeadphonesVolumeControl::loadLayoutData(const QJsonObject& layout)
{
    m_showSlider = layout.value("ShowSlider"_L1).toBool(true);
    m_showIcon   = layout.value("ShowIcon"_L1).toBool(true);
    if(!m_showSlider && !m_showIcon) {
        m_showIcon = true;
    }
    updateDisplay();
    HeadphonesControlWidget::loadLayoutData(layout);
}

void HeadphonesVolumeControl::populateContextMenu(QMenu* menu)
{
    const auto addOption = [this, menu](const QString& text, bool& option) {
        auto* action = menu->addAction(text);
        action->setCheckable(true);
        action->setChecked(option);
        connect(action, &QAction::triggered, this, [this, action, &option](bool checked) {
            option = checked;
            if(!m_showSlider && !m_showIcon) {
                option = true;
                action->setChecked(true);
            }
            updateDisplay();
        });
    };
    addOption(tr("Icon"), m_showIcon);
    addOption(tr("Slider"), m_showSlider);
    menu->addSeparator();
    HeadphonesControlWidget::populateContextMenu(menu);
}

void HeadphonesVolumeControl::updateDisplay()
{
    m_volumeMenu->hide();
    m_layout->removeWidget(m_slider);
    m_volumeMenu->layout()->removeWidget(m_slider);
    if(m_showSlider) {
        m_layout->insertWidget(0, m_slider, 1);
        m_slider->show();
    }
    else {
        m_volumeMenu->layout()->addWidget(m_slider);
        m_slider->show();
    }
    m_icon->setVisible(m_showIcon);
    updateOrientation();
}

void HeadphonesVolumeControl::applyOrientation(Qt::Orientation orientation)
{
    const auto sliderOrientation = m_showSlider ? orientation : Qt::Vertical;
    m_slider->setOrientation(sliderOrientation);
    m_slider->setMinimumSize(sliderOrientation == Qt::Horizontal ? QSize {75, 0} : QSize {0, 100});
    m_layout->setAlignment(m_icon, orientation == Qt::Horizontal ? Qt::AlignVCenter : Qt::AlignHCenter);
}

void HeadphonesVolumeControl::showVolumeMenu()
{
    if(!m_showSlider && m_showIcon && !m_volumeMenu->isVisible()) {
        m_volumeMenu->adjustSize();
        const QRect iconRect {m_icon->mapToGlobal(QPoint {}), m_icon->size()};
        const QRect windowRect {window()->mapToGlobal(QPoint {}), window()->size()};
        const auto size = m_volumeMenu->size();
        const int x = iconRect.right() + 10 + size.width() <= windowRect.right()
                    ? iconRect.right() + 10 : iconRect.left() - 10 - size.width();
        const int y = std::clamp(iconRect.center().y() - size.height() / 2, windowRect.top(),
                                 std::max(windowRect.top(), windowRect.bottom() - size.height()));
        m_volumeMenu->move(x, y);
        m_volumeMenu->show();
    }
}

void HeadphonesVolumeControl::wheelEvent(QWheelEvent* event)
{
    QApplication::sendEvent(m_slider, event);
}

void HeadphonesVolumeControl::refresh()
{
    if(!m_controller) {
        return;
    }
    const auto volume = m_controller->snapshot().volume;
    const QSignalBlocker blocker {m_slider};
    m_slider->setValue(static_cast<int>(std::lround(volume * 100)));
    m_slider->setToolTip(tr("Headphone volume: %1%").arg(m_slider->value()));
    m_slider->setAccessibleDescription(m_slider->toolTip());
    const auto* icon = volume >= 0.4 ? Constants::Icons::VolumeHigh
                     : volume >= 0.2 ? Constants::Icons::VolumeMed
                     : volume > 0.0 ? Constants::Icons::VolumeLow : Constants::Icons::VolumeMute;
    m_icon->setIcon(Gui::iconFromTheme(icon));
    m_icon->setChecked(volume == 0.0);
    m_icon->setToolTip(volume == 0.0 ? tr("Unmute headphone preview") : tr("Mute headphone preview"));
    m_icon->setAccessibleName(m_icon->toolTip());
}
} // namespace Fooyin::Headphones
