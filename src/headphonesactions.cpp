// SPDX-License-Identifier: GPL-3.0-or-later
#include "headphonesactions.h"

#include <gui/trackselectioncontroller.h>
#include <utils/actions/actionmanager.h>
#include <utils/actions/command.h>

#include <QAction>
#include <QMenu>

#include <algorithm>
#include <limits>

using namespace Qt::StringLiterals;

namespace Fooyin::Headphones {
namespace {
constexpr const char* ActionIds[] {"Headphones.PreviewSelected", "Headphones.PlayPause", "Headphones.Stop",
                                   "Headphones.SeekBackward", "Headphones.SeekForward", "Headphones.Play",
                                   "Headphones.Pause"};
}

HeadphonesActions::HeadphonesActions(PreviewController& controller, ActionManager* manager)
    : m_controller {controller}
    , m_manager {manager}
{
    const QStringList labels {tr("Preview in headphones"), tr("Play/pause headphone preview"),
                              tr("Stop headphone preview"), tr("Seek headphone preview backward"),
                              tr("Seek headphone preview forward"), tr("Play headphone preview"),
                              tr("Pause headphone preview")};
    for(int i = 0; i < labels.size(); ++i) {
        auto* action = new QAction(labels[i], this);
        action->setObjectName(QString::fromLatin1(ActionIds[i]));
        action->setStatusTip(i == 0 ? tr("Preview the first selected track, independently of main playback")
                                    : labels[i]);
        m_actions.push_back(action);
        if(m_manager) {
            auto* command = m_manager->registerAction(action, ActionIds[i]);
            command->setCategories({tr("Headphones Preview")});
            command->setDescription(action->statusTip());
        }
    }
    connect(m_actions[0], &QAction::triggered, this, [this]() {
        if(previewReason(m_focused).isEmpty()) {
            m_controller.previewTrack(m_focused.front());
        }
    });
    connect(m_actions[1], &QAction::triggered, &m_controller, &PreviewController::playPause);
    connect(m_actions[2], &QAction::triggered, &m_controller, &PreviewController::stop);
    connect(m_actions[3], &QAction::triggered, this, [this]() { seekBy(-5000); });
    connect(m_actions[4], &QAction::triggered, this, [this]() { seekBy(5000); });
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
    return previewReason(m_display);
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
    m_focused = std::move(focused);
    m_display = std::move(display);
    updateActions();
}

void HeadphonesActions::previewDisplaySelection()
{
    if(displayPreviewReason().isEmpty()) {
        m_controller.previewTrack(m_display.front());
    }
}

void HeadphonesActions::renderMenu(QMenu* menu, const TrackSelection& selection)
{
    // A menu owns its action and immutable target. Never reuse the shortcut
    // action, whose ambient focus/selection may change while a menu is open.
    auto* action = menu->addAction(tr("Preview in headphones"));
    action->setObjectName(u"Headphones.PreviewSelected.Context"_s);
    const auto reason = previewReason(selection.tracks);
    action->setEnabled(reason.isEmpty());
    action->setToolTip(reason.isEmpty() ? tr("Preview the first selected track") : reason);
    action->setStatusTip(action->toolTip());
    const auto track = selection.tracks.empty() ? Track {} : selection.tracks.front();
    connect(action, &QAction::triggered, this, [this, track]() {
        if(previewReason({track}).isEmpty()) {
            m_controller.previewTrack(track);
        }
    });
}

void HeadphonesActions::seekBy(int64_t delta)
{
    const auto position
        = std::min<uint64_t>(m_controller.snapshot().positionMs, std::numeric_limits<int64_t>::max() - 5000);
    m_controller.seek(static_cast<int64_t>(position) + delta);
}

void HeadphonesActions::updateActions()
{
    const auto snapshot = m_controller.snapshot();
    const bool active   = snapshot.state != PreviewState::Stopped && snapshot.state != PreviewState::Error;
    const auto reason   = previewReason(m_focused);
    m_actions[0]->setEnabled(reason.isEmpty());
    m_actions[0]->setToolTip(reason.isEmpty() ? tr("Preview the first selected track") : reason);
    m_actions[1]->setEnabled(active || (snapshot.track.isValid() && snapshot.deviceAvailable));
    m_actions[2]->setEnabled(active || snapshot.state == PreviewState::Error || snapshot.positionMs != 0);
    m_actions[3]->setEnabled(active && snapshot.seekable);
    m_actions[4]->setEnabled(active && snapshot.seekable);
    playAction()->setEnabled(m_actions[1]->isEnabled());
    pauseAction()->setEnabled(active);
    Q_EMIT availabilityChanged();
}

} // namespace Fooyin::Headphones
