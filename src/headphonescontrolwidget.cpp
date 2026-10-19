// SPDX-License-Identifier: GPL-3.0-or-later
#include "headphonescontrolwidget.h"

#include <gui/guiutils.h>

#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QMenu>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QStyleOptionSlider>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace Fooyin::Headphones {
HeadphonesControlWidget::HeadphonesControlWidget(QWidget* parent)
    : FyWidget {parent}
    , m_layout {new QHBoxLayout(this)}
{
    m_layout->setContentsMargins({});
    m_layout->setSpacing(0);
}

void HeadphonesControlWidget::saveLayoutData(QJsonObject& layout)
{
    if(!m_autoOrientation) {
        layout["Orientation"_L1] = m_orientation;
    }
}

void HeadphonesControlWidget::loadLayoutData(const QJsonObject& layout)
{
    const auto orientation = static_cast<Qt::Orientation>(layout.value("Orientation"_L1).toInt());
    m_autoOrientation = orientation != Qt::Horizontal && orientation != Qt::Vertical;
    if(!m_autoOrientation) {
        m_orientation = orientation;
    }
    updateOrientation();
}

void HeadphonesControlWidget::populateContextMenu(QMenu* menu)
{
    Gui::addOrientationMenu(menu, this,
                           {.title = tr("Orientation"), .automatic = tr("Automatic"),
                            .horizontal = tr("Horizontal"), .vertical = tr("Vertical")},
                           m_autoOrientation, m_orientation, [this](std::optional<Qt::Orientation> orientation) {
                               m_autoOrientation = !orientation.has_value();
                               if(orientation) {
                                   m_orientation = *orientation;
                               }
                               updateOrientation();
                           });
}

void HeadphonesControlWidget::contextMenuEvent(QContextMenuEvent* event)
{
    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    populateContextMenu(menu);
    menu->popup(event->globalPos());
}

void HeadphonesControlWidget::resizeEvent(QResizeEvent* event)
{
    updateOrientation();
    FyWidget::resizeEvent(event);
}

void HeadphonesControlWidget::updateOrientation()
{
    const auto orientation = m_autoOrientation ? automaticOrientation() : m_orientation;
    m_layout->setDirection(orientation == Qt::Vertical ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    applyOrientation(orientation);
    updateOrientationConstraints(m_autoOrientation, m_layout->minimumSize());
}

void HeadphonesControlWidget::applyOrientation(Qt::Orientation /*orientation*/) {}

HeadphonesSlider::HeadphonesSlider(QWidget* parent)
    : QSlider {Qt::Horizontal, parent}
{
    setFocusPolicy(Qt::TabFocus);
}

int HeadphonesSlider::valueAt(const QPointF& position) const
{
    QStyleOptionSlider option;
    initStyleOption(&option);
    const auto groove = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
    const auto handle = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
    const bool horizontal = orientation() == Qt::Horizontal;
    const int handleSize = horizontal ? handle.width() : handle.height();
    const int start = horizontal ? groove.x() : groove.y();
    const int end = horizontal ? groove.right() : groove.bottom();
    const int pos = static_cast<int>(horizontal ? position.x() : position.y()) - handleSize / 2 - start + 1;
    return QStyle::sliderValueFromPosition(minimum(), maximum(), pos, std::max(0, end - handleSize - start + 1),
                                           option.upsideDown);
}

void HeadphonesSlider::cancelDrag()
{
    if(isSliderDown()) {
        setSliderDown(false);
        Q_EMIT dragCancelled();
    }
}

bool HeadphonesSlider::event(QEvent* event)
{
    if(event->type() == QEvent::UngrabMouse || event->type() == QEvent::WindowDeactivate
       || event->type() == QEvent::Hide || (event->type() == QEvent::EnabledChange && !isEnabled())) {
        cancelDrag();
    }
    return QSlider::event(event);
}

void HeadphonesSlider::mousePressEvent(QMouseEvent* event)
{
    if(event->button() == Qt::RightButton && isSliderDown()) {
        cancelDrag();
        event->accept();
        return;
    }
    if(event->button() != Qt::LeftButton || !isEnabled() || maximum() == minimum()) {
        event->ignore();
        return;
    }
    setSliderDown(true);
    if(focusPolicy() & Qt::ClickFocus) {
        setFocus(Qt::MouseFocusReason);
    }
    setSliderPosition(valueAt(event->position()));
    event->accept();
}

void HeadphonesSlider::mouseMoveEvent(QMouseEvent* event)
{
    if(isSliderDown() && (event->buttons() & Qt::LeftButton)) {
        setSliderPosition(valueAt(event->position()));
        event->accept();
    }
    else {
        event->ignore();
    }
}

void HeadphonesSlider::mouseReleaseEvent(QMouseEvent* event)
{
    if(event->button() != Qt::LeftButton || !isSliderDown()) {
        event->ignore();
        return;
    }
    setSliderPosition(valueAt(event->position()));
    setSliderDown(false);
    Q_EMIT positionCommitted(value());
    event->accept();
}
} // namespace Fooyin::Headphones
