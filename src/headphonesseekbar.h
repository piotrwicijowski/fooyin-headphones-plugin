// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "headphonescontrolwidget.h"
#include "previewtypes.h"

#include <QPointer>

class QLabel;
namespace Fooyin {
class SettingsManager;
namespace Headphones {
class PreviewController;
class HeadphonesSeekBar : public HeadphonesControlWidget
{
    Q_OBJECT
public:
    HeadphonesSeekBar(PreviewController& controller, SettingsManager* settings, QWidget* parent = nullptr);
    QString name() const override;
    QString layoutName() const override;
    void saveLayoutData(QJsonObject& layout) override;
    void loadLayoutData(const QJsonObject& layout) override;
    void populateContextMenu(QMenu* menu) override;

protected:
    void applyOrientation(Qt::Orientation orientation) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void refresh(const PreviewSnapshot& snapshot);
    void updateLabels(uint64_t position);
    void requestSeek(int value);
    uint64_t timeAt(int value) const;

    QPointer<PreviewController> m_controller;
    QPointer<SettingsManager> m_settings;
    HeadphonesSlider* m_slider;
    QLabel* m_elapsed;
    QLabel* m_total;
    PreviewSnapshot m_snapshot;
    bool m_showLabels {true};
    bool m_showRemainingTime {false};
    bool m_seekPending {false};
};
} // namespace Headphones
} // namespace Fooyin
