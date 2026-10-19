// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "previewcontroller.h"

#include <QPointer>

class QAction;
class QMenu;
namespace Fooyin {
class ActionManager;
struct TrackSelection;
} // namespace Fooyin

namespace Fooyin::Headphones {

class HeadphonesActions : public QObject
{
    Q_OBJECT
public:
    HeadphonesActions(PreviewController& controller, ActionManager* manager);
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
    void seekBy(int64_t delta);
    PreviewController& m_controller;
    QPointer<ActionManager> m_manager;
    TrackList m_focused;
    TrackList m_display;
    std::vector<QAction*> m_actions;
};

} // namespace Fooyin::Headphones
