// SPDX-License-Identifier: GPL-3.0-or-later
#include "headphonesseekbar.h"

#include "previewcontroller.h"

#include <gui/guisettings.h>
#include <gui/widgets/clickablelabel.h>
#include <utils/settings/settingsmanager.h>
#include <utils/stringutils.h>

#include <QApplication>
#include <QBoxLayout>
#include <QContextMenuEvent>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMenu>
#include <QSignalBlocker>
#include <QStyle>
#include <QToolTip>
#include <QWheelEvent>

#include <algorithm>
#include <limits>

using namespace Qt::StringLiterals;

namespace Fooyin::Headphones {
namespace {
constexpr int SeekScale = 1000000;
}

HeadphonesSeekBar::HeadphonesSeekBar(PreviewController& controller, SettingsManager* settings, QWidget* parent)
    : HeadphonesControlWidget {parent}
    , m_controller {&controller}
    , m_settings {settings}
    , m_slider {new HeadphonesSlider(this)}
    , m_elapsed {new QLabel(this)}
    , m_total {new ClickableLabel({}, this)}
{
    m_slider->setObjectName(u"Headphones.Seek"_s);
    m_slider->setAccessibleName(tr("Headphone preview position"));
    m_slider->installEventFilter(this);
    m_elapsed->setObjectName(u"Headphones.Elapsed"_s);
    m_total->setObjectName(u"Headphones.Total"_s);
    m_layout->setSpacing(style()->pixelMetric(QStyle::PM_LayoutHorizontalSpacing));
    m_layout->addWidget(m_elapsed);
    m_layout->addWidget(m_slider, 1);
    m_layout->addWidget(m_total);
    connect(static_cast<ClickableLabel*>(m_total), &ClickableLabel::clicked, this, [this]() {
        m_showRemainingTime = !m_showRemainingTime;
        updateLabels(timeAt(m_slider->value()));
    });
    connect(&controller, &PreviewController::snapshotChanged, this, &HeadphonesSeekBar::refresh);
    connect(m_slider, &HeadphonesSlider::positionCommitted, this, &HeadphonesSeekBar::requestSeek);
    connect(m_slider, &HeadphonesSlider::dragCancelled, this, [this]() {
        QToolTip::hideText();
        if(m_controller) {
            refresh(m_controller->snapshot());
        }
    });
    connect(m_slider, &QSlider::valueChanged, this, [this](int value) {
        if(m_slider->isSliderDown()) {
            updateLabels(timeAt(value));
            QToolTip::showText(QCursor::pos(), Utils::msToString(timeAt(value)), m_slider);
        }
        else {
            requestSeek(value);
        }
    });
    if(settings) {
        const auto setFocus = [this](bool enabled) {
            m_slider->setFocusPolicy(enabled ? Qt::StrongFocus : Qt::TabFocus);
        };
        setFocus(settings->value<Settings::Gui::SeekBarMouseFocus>());
        settings->subscribe<Settings::Gui::SeekBarMouseFocus>(this, setFocus);
    }
    refresh(controller.snapshot());
}

QString HeadphonesSeekBar::name() const
{
    return tr("Headphones Seekbar");
}

QString HeadphonesSeekBar::layoutName() const
{
    return u"HeadphonesSeekBar"_s;
}

void HeadphonesSeekBar::saveLayoutData(QJsonObject& layout)
{
    HeadphonesControlWidget::saveLayoutData(layout);
    layout["ShowLabels"_L1]        = m_showLabels;
    layout["ShowRemainingTime"_L1] = m_showRemainingTime;
}

void HeadphonesSeekBar::loadLayoutData(const QJsonObject& layout)
{
    m_showLabels        = layout.value("ShowLabels"_L1).toBool(true);
    m_showRemainingTime = layout.value("ShowRemainingTime"_L1).toBool(false);
    m_elapsed->setVisible(m_showLabels);
    m_total->setVisible(m_showLabels);
    updateLabels(timeAt(m_slider->value()));
    HeadphonesControlWidget::loadLayoutData(layout);
}

void HeadphonesSeekBar::populateContextMenu(QMenu* menu)
{
    auto* labels = menu->addAction(tr("Show labels"));
    labels->setCheckable(true);
    labels->setChecked(m_showLabels);
    connect(labels, &QAction::triggered, this, [this](bool checked) {
        m_showLabels = checked;
        m_elapsed->setVisible(checked);
        m_total->setVisible(checked);
        updateOrientation();
    });
    auto* remaining = menu->addAction(tr("Show remaining time"));
    remaining->setCheckable(true);
    remaining->setChecked(m_showRemainingTime);
    connect(remaining, &QAction::triggered, this, [this](bool checked) {
        m_showRemainingTime = checked;
        updateLabels(timeAt(m_slider->value()));
        updateOrientation();
    });
    menu->addSeparator();
    HeadphonesControlWidget::populateContextMenu(menu);
}

void HeadphonesSeekBar::applyOrientation(Qt::Orientation orientation)
{
    m_slider->setOrientation(orientation);
    // Match the main seekbar: vertical timelines progress from top to bottom.
    m_slider->setInvertedAppearance(orientation == Qt::Vertical);
    const auto alignment = orientation == Qt::Horizontal ? Qt::AlignVCenter : Qt::AlignHCenter;
    m_layout->setAlignment(m_elapsed, alignment);
    m_layout->setAlignment(m_total, alignment);
}

void HeadphonesSeekBar::contextMenuEvent(QContextMenuEvent* event)
{
    if(m_slider->isSliderDown()) {
        m_slider->cancelDrag();
        event->accept();
        return;
    }
    if(event->reason() == QContextMenuEvent::Mouse && (QApplication::mouseButtons() & Qt::LeftButton)) {
        event->accept();
        return;
    }
    HeadphonesControlWidget::contextMenuEvent(event);
}

void HeadphonesSeekBar::changeEvent(QEvent* event)
{
    HeadphonesControlWidget::changeEvent(event);
    if(event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
        updateLabels(timeAt(m_slider->value()));
    }
}

bool HeadphonesSeekBar::eventFilter(QObject* watched, QEvent* event)
{
    if(watched == m_slider && m_slider->isEnabled() && m_controller) {
        int direction = 0;
        if(event->type() == QEvent::KeyPress) {
            const auto key = static_cast<QKeyEvent*>(event)->key();
            if(key == Qt::Key_Right || key == Qt::Key_Up) {
                direction = 1;
            }
            else if(key == Qt::Key_Left || key == Qt::Key_Down) {
                direction = -1;
            }
        }
        else if(event->type() == QEvent::Wheel) {
            direction = static_cast<QWheelEvent*>(event)->angleDelta().y() < 0 ? -1 : 1;
        }
        if(direction) {
            const int step = m_settings ? m_settings->value<Settings::Gui::SeekStepSmall>() : 5000;
            const auto position = std::min<uint64_t>(m_snapshot.positionMs,
                                                     std::numeric_limits<int64_t>::max() - std::max(0, step));
            m_controller->seek(static_cast<int64_t>(position) + static_cast<int64_t>(direction) * step);
            event->accept();
            return true;
        }
    }
    return HeadphonesControlWidget::eventFilter(watched, event);
}

uint64_t HeadphonesSeekBar::timeAt(int value) const
{
    const auto duration = std::min<uint64_t>(m_snapshot.durationMs, std::numeric_limits<int64_t>::max());
    return std::min(duration, static_cast<uint64_t>(static_cast<long double>(duration)
                                                  * std::clamp(value, 0, SeekScale) / SeekScale));
}

void HeadphonesSeekBar::refresh(const PreviewSnapshot& snapshot)
{
    const bool reset = !snapshot.track.sameIdentityAs(m_snapshot.track) || snapshot.state == PreviewState::Stopped
                    || snapshot.state == PreviewState::Error;
    m_snapshot = snapshot;
    if(reset) {
        m_seekPending = false;
        m_slider->cancelDrag();
    }
    if(snapshot.state != PreviewState::Loading) {
        m_seekPending = false;
    }
    const bool active = snapshot.state != PreviewState::Stopped && snapshot.state != PreviewState::Error;
    const QSignalBlocker blocker {m_slider};
    m_slider->setRange(0, snapshot.durationMs ? SeekScale : 0);
    m_slider->setEnabled(active && snapshot.seekable && snapshot.durationMs != 0);
    const auto duration = std::min<uint64_t>(snapshot.durationMs, std::numeric_limits<int64_t>::max());
    m_slider->setSingleStep(duration ? static_cast<int>(std::clamp(5000.0L * SeekScale / duration, 1.0L,
                                                                static_cast<long double>(SeekScale))) : 1);
    m_slider->setPageStep(m_slider->singleStep());
    if(!m_slider->isSliderDown() && !m_seekPending) {
        const auto ratio = duration ? static_cast<long double>(snapshot.positionMs) / duration : 0.0L;
        m_slider->setValue(static_cast<int>(std::clamp(ratio, 0.0L, 1.0L) * SeekScale));
        updateLabels(snapshot.positionMs);
    }
}

void HeadphonesSeekBar::updateLabels(uint64_t position)
{
    const bool idle = !m_snapshot.track.isValid() || m_snapshot.state == PreviewState::Stopped
                   || m_snapshot.state == PreviewState::Error;
    if(idle) {
        position = 0;
    }
    m_elapsed->setText(Utils::msToString(position));
    const auto duration = idle ? 0 : m_snapshot.durationMs;
    m_total->setText(idle ? Utils::msToString(0)
                    : !duration ? tr("unknown")
                    : m_showRemainingTime ? u"-"_s + Utils::msToString(duration - std::min(position, duration))
                                          : Utils::msToString(duration));
    // Reserve the widest digits to avoid moving the slider as time advances.
    QString widthText = Utils::msToString(std::max(duration, position));
    for(auto& ch : widthText) {
        if(ch.isDigit()) {
            ch = u'8';
        }
    }
    m_elapsed->setFixedWidth(m_elapsed->fontMetrics().horizontalAdvance(widthText) + 2);
    const auto totalWidth = !idle && !duration ? tr("unknown")
                           : (!idle && m_showRemainingTime ? u"-"_s : QString {}) + widthText;
    m_total->setFixedWidth(m_total->fontMetrics().horizontalAdvance(totalWidth) + 2);
}

void HeadphonesSeekBar::requestSeek(int value)
{
    QToolTip::hideText();
    if(!m_controller || !m_slider->isEnabled()) {
        return;
    }
    const auto target = timeAt(value);
    m_seekPending = true;
    updateLabels(target);
    if(!m_controller->seek(static_cast<int64_t>(target))) {
        m_seekPending = false;
        refresh(m_controller->snapshot());
    }
}
} // namespace Fooyin::Headphones
