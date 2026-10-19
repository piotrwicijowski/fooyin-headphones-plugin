// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "headphonescontrolwidget.h"

#include <QPointer>

namespace Fooyin {
class SettingsManager;
class ToolButton;
namespace Headphones {
class PreviewController;
class HeadphonesActions;
class HeadphonesPlayerControl : public HeadphonesControlWidget
{
    Q_OBJECT
public:
    HeadphonesPlayerControl(PreviewController& controller, HeadphonesActions& actions, SettingsManager* settings,
                            QWidget* parent = nullptr);
    QString name() const override;
    QString layoutName() const override;
    void saveLayoutData(QJsonObject& layout) override;
    void loadLayoutData(const QJsonObject& layout) override;
    void populateContextMenu(QMenu* menu) override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void updateIcons();
    QPointer<PreviewController> m_controller;
    ToolButton* m_stop;
    ToolButton* m_pause;
    ToolButton* m_play;
    ToolButton* m_playPause;
    bool m_showStop {true};
    bool m_showPause {false};
    bool m_showPlay {false};
    bool m_showPlayPause {true};
};
} // namespace Headphones
} // namespace Fooyin
