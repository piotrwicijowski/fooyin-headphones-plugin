// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <gui/fywidget.h>

#include <QSlider>

class QBoxLayout;

namespace Fooyin::Headphones {
// Common layout/orientation handling for the independent preview controls.
class HeadphonesControlWidget : public FyWidget
{
    Q_OBJECT
public:
    explicit HeadphonesControlWidget(QWidget* parent = nullptr);
    void saveLayoutData(QJsonObject& layout) override;
    void loadLayoutData(const QJsonObject& layout) override;
    void populateContextMenu(QMenu* menu) override;

protected:
    void contextMenuEvent(QContextMenuEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void updateOrientation();
    virtual void applyOrientation(Qt::Orientation orientation);

    QBoxLayout* m_layout;

private:
    Qt::Orientation m_orientation {Qt::Horizontal};
    bool m_autoOrientation {true};
};

// Absolute left-click positioning, independent of platform page-step defaults.
// A separate commit signal lets seekbars cancel a drag without seeking.
class HeadphonesSlider : public QSlider
{
    Q_OBJECT
public:
    explicit HeadphonesSlider(QWidget* parent = nullptr);
    void cancelDrag();

Q_SIGNALS:
    void positionCommitted(int value);
    void dragCancelled();

protected:
    bool event(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    int valueAt(const QPointF& position) const;
};
} // namespace Fooyin::Headphones
