// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "previewcontroller.h"

#include <QPointer>

class QAction;
class QMenu;
namespace Fooyin {
class ActionManager;
class SettingsManager;
struct TrackSelection;
} // namespace Fooyin

namespace Fooyin::Headphones {

class HeadphonesActions : public QObject
{
    Q_OBJECT
public:
    HeadphonesActions(PreviewController& controller, ActionManager* manager, SettingsManager* settings = nullptr);
    ~HeadphonesActions() override;
    void setSelection(TrackList focused, TrackList display);
    void renderMenu(QMenu* menu, const TrackSelection& selection);
    void previewDisplaySelection();
    [[nodiscard]] QString previewReason(const TrackList& tracks) const;
    [[nodiscard]] QString displayPreviewReason() const;
    [[nodiscard]] QAction* playPauseAction() const;
    [[nodiscard]] QAction* stopAction() const;
    [[nodiscard]] QAction* playAction() const;
    [[nodiscard]] QAction* pauseAction() const;

Q_SIGNALS:
    void availabilityChanged();

private:
    void updateActions();
    void seekByStep(bool forward, bool largeStep);
    PreviewController& m_controller;
    QPointer<ActionManager> m_manager;
    QPointer<SettingsManager> m_settings;
    TrackList m_focused;
    TrackList m_display;
    std::vector<QAction*> m_actions;
};

} // namespace Fooyin::Headphones
