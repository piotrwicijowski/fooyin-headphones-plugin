// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "previewcontroller.h"

#include <gui/trackselectioncontroller.h>

#include <QPointer>

class QAction;
class QMenu;
namespace Fooyin {
class ActionManager;
class SettingsManager;
class CurrentPlaylistController;
class PlaylistInteractor;
} // namespace Fooyin

namespace Fooyin::Headphones {
class HeadphonesSettingsProvider;

class HeadphonesActions : public QObject
{
    Q_OBJECT
public:
    HeadphonesActions(PreviewController& controller, ActionManager* manager, SettingsManager* settings = nullptr,
                      std::weak_ptr<HeadphonesSettingsProvider> provider = {},
                      CurrentPlaylistController* playlists = nullptr, PlaylistInteractor* interactor = nullptr,
                      TrackSelectionController* selection = nullptr);
    ~HeadphonesActions() override;
    void setSelection(TrackList focused, TrackList display);
    void setTrackSelections(const TrackSelection* focused, const TrackSelection* display);
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

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void updateActions();
    void seekByStep(bool forward, bool largeStep);
    void adjustVolume(double step);
    void previewSelection(const TrackSelection& selection);
    void focusPreviewedTrack();
    void showPlaylistTrack(const PlaylistTrack& track);
    void updateNavigation();
    void cancelNavigation();
    PreviewController& m_controller;
    QPointer<ActionManager> m_manager;
    QPointer<SettingsManager> m_settings;
    std::weak_ptr<HeadphonesSettingsProvider> m_provider;
    QPointer<CurrentPlaylistController> m_playlists;
    QPointer<PlaylistInteractor> m_interactor;
    QPointer<TrackSelectionController> m_selection;
    TrackSelection m_focused;
    TrackSelection m_display;
    std::optional<PlaylistTrack> m_previewOrigin;
    std::optional<PlaylistTrack> m_navigationTarget;
    QTimer m_navigationTimer;
    int m_navigationAttempts {0};
    int m_navigationMatches {0};
    std::vector<QAction*> m_actions;
};

} // namespace Fooyin::Headphones
