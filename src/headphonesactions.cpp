// SPDX-License-Identifier: GPL-3.0-or-later
#include "headphonesactions.h"
#include "headphonessettingsdialog.h"

#include <core/playlist/playlisthandler.h>
#include <gui/guisettings.h>
#include <gui/playlist/currentplaylistcontroller.h>
#include <gui/playlist/playlistinteractor.h>
#include <gui/trackselectioncontroller.h>
#include <utils/actions/actionmanager.h>
#include <utils/actions/command.h>
#include <utils/settings/settingsmanager.h>

#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QMenu>

#include <algorithm>
#include <limits>

using namespace Qt::StringLiterals;

namespace Fooyin::Headphones {
namespace {
constexpr const char* ActionIds[] {"Headphones.Tracks.PreviewSelected", "Headphones.Playback.PlayPause",
                                   "Headphones.Playback.Stop", "Headphones.Seek.Backward.Small",
                                   "Headphones.Seek.Forward.Small", "Headphones.Playback.Play",
                                   "Headphones.Playback.Pause", "Headphones.Seek.Backward.Large",
                                    "Headphones.Seek.Forward.Large", "Headphones.Volume.Up", "Headphones.Volume.Down",
                                    "Headphones.Volume.Mute", "Headphones.Volume.Unmute", "Headphones.Volume.ToggleMute",
                                    "Headphones.Configure", "Headphones.Tracks.FocusPreviewed"};

TrackSelection previewTarget(const TrackSelection& selection)
{
    TrackSelection target;
    if(!selection.tracks.empty()) {
        target.tracks = {selection.tracks.front()};
    }
    target.playlistId = selection.playlistId;
    if(!selection.playlistEntryIds.empty()) {
        target.playlistEntryIds = {selection.playlistEntryIds.front()};
    }
    if(!selection.playlistIndexes.empty()) {
        target.playlistIndexes = {selection.playlistIndexes.front()};
    }
    return target;
}
}

HeadphonesActions::HeadphonesActions(PreviewController& controller, ActionManager* manager, SettingsManager* settings,
                                    std::weak_ptr<HeadphonesSettingsProvider> provider,
                                    CurrentPlaylistController* playlists, PlaylistInteractor* interactor,
                                    TrackSelectionController* selection)
    : m_controller {controller}
    , m_manager {manager}
    , m_settings {settings}
    , m_provider {std::move(provider)}
    , m_playlists {playlists}
    , m_interactor {interactor}
    , m_selection {selection}
{
    const QStringList labels {tr("Preview selected"), tr("Play/Pause"), tr("Stop"),
                              tr("Seek backward (small step)"), tr("Seek forward (small step)"), tr("Play"), tr("Pause"),
                              tr("Seek backward (large step)"), tr("Seek forward (large step)"), tr("Volume up"),
                              tr("Volume down"), tr("Mute"), tr("Unmute"), tr("Toggle mute"), tr("Configure"),
                              tr("Show previewed track")};
    // The host keys shortcut categories by label, even across different parents.
    // Keep subgroup names unique to this plugin to avoid merging with main-player categories.
    const auto playback = tr("Headphone playback");
    const auto seek = tr("Headphone seek");
    const auto tracks = tr("Headphone tracks");
    const auto volume = tr("Headphone volume");
    const QStringList groups {tracks, playback, playback, seek, seek, playback, playback, seek, seek,
                              volume, volume, volume, volume, volume, QString {}, tracks};
    for(int i = 0; i < labels.size(); ++i) {
        auto* action = new QAction(labels[i], this);
        action->setObjectName(QString::fromLatin1(ActionIds[i]));
        action->setStatusTip(labels[i]);
        m_actions.push_back(action);
        if(m_manager) {
            auto* command = m_manager->registerAction(action, ActionIds[i]);
            QStringList categories {tr("Headphones Preview")};
            if(!groups[i].isEmpty()) {
                categories.append(groups[i]);
            }
            command->setCategories(categories);
            command->setDescription(labels[i]);
        }
    }
    connect(m_actions[0], &QAction::triggered, this, [this]() { previewSelection(m_focused); });
    connect(m_actions[1], &QAction::triggered, &m_controller, &PreviewController::playPause);
    connect(m_actions[2], &QAction::triggered, &m_controller, &PreviewController::stop);
    connect(m_actions[3], &QAction::triggered, this, [this]() { seekByStep(false, false); });
    connect(m_actions[4], &QAction::triggered, this, [this]() { seekByStep(true, false); });
    connect(m_actions[7], &QAction::triggered, this, [this]() { seekByStep(false, true); });
    connect(m_actions[8], &QAction::triggered, this, [this]() { seekByStep(true, true); });
    connect(m_actions[9], &QAction::triggered, this, [this]() { adjustVolume(0.05); });
    connect(m_actions[10], &QAction::triggered, this, [this]() { adjustVolume(-0.05); });
    connect(m_actions[11], &QAction::triggered, &m_controller, &PreviewController::mute);
    connect(m_actions[12], &QAction::triggered, &m_controller, &PreviewController::unmute);
    connect(m_actions[13], &QAction::triggered, &m_controller, &PreviewController::toggleMute);
    connect(m_actions[14], &QAction::triggered, this, [this]() {
        if(const auto provider = m_provider.lock()) {
            provider->showSettings(QApplication::activeWindow());
        }
    });
    connect(m_actions[15], &QAction::triggered, this, &HeadphonesActions::focusPreviewedTrack);
    connect(playAction(), &QAction::triggered, this, [this]() {
        const auto snapshot = m_controller.snapshot();
        if(snapshot.pauseRequested || snapshot.state == PreviewState::Stopped || snapshot.state == PreviewState::Error) {
            m_controller.playPause();
        }
        else {
            m_controller.previewTrack(snapshot.track);
        }
    });
    connect(pauseAction(), &QAction::triggered, this, [this]() {
        const auto snapshot = m_controller.snapshot();
        if(!snapshot.pauseRequested && snapshot.state != PreviewState::Stopped && snapshot.state != PreviewState::Error) {
            m_controller.playPause();
        }
    });
    connect(&m_controller, &PreviewController::snapshotChanged, this, [this]() { updateActions(); });
    m_navigationTimer.setInterval(50);
    connect(&m_navigationTimer, &QTimer::timeout, this, &HeadphonesActions::updateNavigation);
    if(m_playlists) {
        connect(m_playlists, &CurrentPlaylistController::currentPlaylistChanged, this, [this]() {
            if(m_navigationTarget && m_playlists->currentPlaylistId() != m_navigationTarget->playlistId) {
                cancelNavigation();
            }
        });
    }
    if(auto* app = QApplication::instance()) {
        app->installEventFilter(this);
    }
    updateActions();
}

HeadphonesActions::~HeadphonesActions()
{
    if(m_manager) {
        for(size_t i = 0; i < m_actions.size(); ++i) {
            m_manager->unregisterAction(m_actions[i], ActionIds[i]);
        }
    }
}

QString HeadphonesActions::previewReason(const TrackList& tracks) const
{
    if(tracks.empty()) {
        return tr("Select a track to preview.");
    }
    const auto error = previewTrackError(tracks.front());
    if(!error.isEmpty()) {
        return error;
    }
    if(!m_controller.snapshot().deviceAvailable) {
        return tr("Configure an available headphone output first.");
    }
    return {};
}

QString HeadphonesActions::displayPreviewReason() const
{
    return previewReason(m_display.tracks);
}
QAction* HeadphonesActions::playPauseAction() const
{
    return m_actions[1];
}
QAction* HeadphonesActions::stopAction() const
{
    return m_actions[2];
}

QAction* HeadphonesActions::playAction() const
{
    return m_actions[5];
}

QAction* HeadphonesActions::pauseAction() const
{
    return m_actions[6];
}

void HeadphonesActions::setSelection(TrackList focused, TrackList display)
{
    TrackSelection focusedSelection;
    focusedSelection.tracks = std::move(focused);
    TrackSelection displaySelection;
    displaySelection.tracks = std::move(display);
    setTrackSelections(&focusedSelection, &displaySelection);
}

void HeadphonesActions::setTrackSelections(const TrackSelection* focused, const TrackSelection* display)
{
    m_focused = focused ? previewTarget(*focused) : TrackSelection {};
    m_display = display ? previewTarget(*display) : TrackSelection {};
    updateActions();
}

void HeadphonesActions::previewDisplaySelection()
{
    previewSelection(m_display);
}

void HeadphonesActions::previewSelection(const TrackSelection& selection)
{
    if(!previewReason(selection.tracks).isEmpty()) {
        return;
    }
    m_previewOrigin.reset();
    const auto track = selection.tracks.front();
    if(m_interactor && m_interactor->handler() && selection.playlistId) {
        if(const auto* playlist = m_interactor->handler()->playlistById(*selection.playlistId)) {
            // Entry identity wins over a row, which may already have moved since menu creation.
            if(!selection.playlistEntryIds.empty()) {
                m_previewOrigin = playlist->playlistTrack(selection.playlistEntryIds.front());
            }
            else if(!selection.playlistIndexes.empty()) {
                m_previewOrigin = playlist->playlistTrack(selection.playlistIndexes.front());
            }
            if(m_previewOrigin && !m_previewOrigin->track.sameIdentityAs(track)) {
                m_previewOrigin.reset();
            }
        }
    }
    m_controller.previewTrack(track);
}

void HeadphonesActions::focusPreviewedTrack()
{
    const auto track = m_controller.snapshot().track;
    if(!track.isValid() || !m_playlists || !m_interactor || !m_interactor->handler()) {
        return;
    }
    const auto* handler = m_interactor->handler();
    if(m_previewOrigin && m_previewOrigin->track.sameIdentityAs(track)) {
        if(const auto* playlist = handler->playlistById(m_previewOrigin->playlistId)) {
            const auto entry = playlist->playlistTrack(m_previewOrigin->entryId);
            if(entry && entry->track.sameIdentityAs(track)) {
                showPlaylistTrack(*entry);
                return;
            }
        }
    }
    const auto focusMatch = [this, &track](const Playlist* playlist) {
        if(playlist) {
            for(const auto& entry : playlist->playlistTracks()) {
                if(entry.track.sameIdentityAs(track)) {
                    showPlaylistTrack(entry);
                    return true;
                }
            }
        }
        return false;
    };
    if(focusMatch(m_playlists->currentPlaylist())) {
        return;
    }
    for(const auto* playlist : handler->playlists()) {
        if(focusMatch(playlist)) {
            return;
        }
    }
}

void HeadphonesActions::showPlaylistTrack(const PlaylistTrack& track)
{
    cancelNavigation();
    if(m_playlists->currentPlaylistId() != track.playlistId) {
        m_playlists->changeCurrentPlaylist(track.playlistId);
    }
    m_navigationTarget = track;
    // Changing the playlist schedules an asynchronous view reset. Retry through
    // the public selection API until the target is selected and has settled.
    m_navigationTimer.start();
}

void HeadphonesActions::cancelNavigation()
{
    m_navigationTimer.stop();
    m_navigationTarget.reset();
    m_navigationAttempts = 0;
    m_navigationMatches = 0;
}

void HeadphonesActions::updateNavigation()
{
    if(!m_navigationTarget || !m_playlists || !m_interactor || !m_interactor->handler()
       || m_playlists->currentPlaylistId() != m_navigationTarget->playlistId) {
        cancelNavigation();
        return;
    }
    const auto* playlist = m_interactor->handler()->playlistById(m_navigationTarget->playlistId);
    const auto entry = playlist ? playlist->playlistTrack(m_navigationTarget->entryId) : std::nullopt;
    if(!entry || !entry->track.sameIdentityAs(m_controller.snapshot().track)) {
        cancelNavigation();
        return;
    }
    m_playlists->selectTracks({entry->track});
    if(!m_navigationTarget) {
        return;
    }
    m_interactor->handler()->ensurePlaylistItemVisible(entry->playlistId, entry->indexInPlaylist);
    const auto target = m_selection ? m_selection->playlistSelectionTarget(entry->playlistId) : std::nullopt;
    const bool selected = target && std::ranges::find(target->selection.playlistEntryIds, entry->entryId)
                                       != target->selection.playlistEntryIds.end();
    m_navigationMatches = selected ? m_navigationMatches + 1 : 0;
    if(m_navigationMatches >= 3 || ++m_navigationAttempts >= 40 || !m_selection) {
        cancelNavigation();
    }
}

bool HeadphonesActions::eventFilter(QObject* watched, QEvent* event)
{
    if(m_navigationTarget && (event->type() == QEvent::KeyPress || event->type() == QEvent::MouseButtonPress
                              || event->type() == QEvent::Wheel)) {
        // An explicit new user interaction takes precedence over delayed navigation.
        cancelNavigation();
    }
    return QObject::eventFilter(watched, event);
}

void HeadphonesActions::renderMenu(QMenu* menu, const TrackSelection& selection)
{
    // A menu owns its action and immutable target. Never reuse the shortcut
    // action, whose ambient focus/selection may change while a menu is open.
    auto* action = menu->addAction(tr("Preview in headphones"));
    action->setObjectName(u"Headphones.Tracks.PreviewSelected.Context"_s);
    const auto reason = previewReason(selection.tracks);
    action->setEnabled(reason.isEmpty());
    action->setToolTip(reason.isEmpty() ? tr("Preview the first selected track") : reason);
    action->setStatusTip(action->toolTip());
    connect(action, &QAction::triggered, this,
            [this, target = previewTarget(selection)]() { previewSelection(target); });
}

void HeadphonesActions::adjustVolume(double step)
{
    const auto volume = m_controller.snapshot().volume + step;
    // Repeated decimal steps must reach exact mute/full volume despite floating-point rounding.
    m_controller.setVolume(volume < 1e-12 ? 0.0 : (volume > 1.0 - 1e-12 ? 1.0 : volume));
}

void HeadphonesActions::seekByStep(bool forward, bool largeStep)
{
    const int configuredStep = m_settings ? (largeStep ? m_settings->value<Settings::Gui::SeekStepLarge>()
                                                      : m_settings->value<Settings::Gui::SeekStepSmall>())
                                          : (largeStep ? 30000 : 4000);
    const int64_t step = std::max(0, configuredStep);
    const auto position
        = std::min<uint64_t>(m_controller.snapshot().positionMs, std::numeric_limits<int64_t>::max() - step);
    m_controller.seek(static_cast<int64_t>(position) + (forward ? step : -step));
}

void HeadphonesActions::updateActions()
{
    const auto snapshot = m_controller.snapshot();
    const bool active   = snapshot.state != PreviewState::Stopped && snapshot.state != PreviewState::Error;
    const auto reason   = previewReason(m_focused.tracks);
    if(m_navigationTarget && !m_navigationTarget->track.sameIdentityAs(snapshot.track)) {
        cancelNavigation();
    }
    if(m_previewOrigin && !m_previewOrigin->track.sameIdentityAs(snapshot.track)) {
        m_previewOrigin.reset();
    }
    m_actions[0]->setEnabled(reason.isEmpty());
    m_actions[0]->setToolTip(reason.isEmpty() ? tr("Preview the first selected track") : reason);
    m_actions[1]->setEnabled(active || (snapshot.track.isValid() && snapshot.deviceAvailable));
    m_actions[2]->setEnabled(active || snapshot.state == PreviewState::Error || snapshot.positionMs != 0);
    for(auto* action : {m_actions[3], m_actions[4], m_actions[7], m_actions[8]}) {
        action->setEnabled(active && snapshot.seekable);
    }
    playAction()->setEnabled(m_actions[1]->isEnabled());
    pauseAction()->setEnabled(active);
    m_actions[14]->setEnabled(!m_provider.expired());
    m_actions[15]->setEnabled(snapshot.track.isValid() && m_playlists && m_interactor);
    Q_EMIT availabilityChanged();
}

} // namespace Fooyin::Headphones
