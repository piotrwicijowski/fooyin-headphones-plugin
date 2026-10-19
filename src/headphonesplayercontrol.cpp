// SPDX-License-Identifier: GPL-3.0-or-later
#include "headphonesplayercontrol.h"

#include "headphonesactions.h"
#include "headphonessettingsdialog.h"
#include "previewcontroller.h"

#include <gui/guiconstants.h>
#include <gui/guisettings.h>
#include <gui/iconloader.h>
#include <gui/widgets/toolbutton.h>
#include <utils/settings/settingsmanager.h>

#include <QAction>
#include <QBoxLayout>
#include <QContextMenuEvent>
#include <QJsonObject>
#include <QMenu>

using namespace Qt::StringLiterals;

namespace Fooyin::Headphones {
HeadphonesPlayerControl::HeadphonesPlayerControl(PreviewController& controller, HeadphonesActions& actions,
                                                SettingsManager* settings,
                                                std::weak_ptr<HeadphonesSettingsProvider> provider, QWidget* parent)
    : HeadphonesControlWidget {parent}
    , m_controller {&controller}
    , m_stop {settings ? new ToolButton(settings, this) : new ToolButton(this)}
    , m_pause {settings ? new ToolButton(settings, this) : new ToolButton(this)}
    , m_play {settings ? new ToolButton(settings, this) : new ToolButton(this)}
    , m_playPause {settings ? new ToolButton(settings, this) : new ToolButton(this)}
    , m_preview {settings ? new ToolButton(settings, this) : new ToolButton(this)}
    , m_configure {settings ? new ToolButton(settings, this) : new ToolButton(this)}
{
    m_stop->setObjectName(u"Headphones.Stop"_s);
    m_pause->setObjectName(u"Headphones.Pause"_s);
    m_play->setObjectName(u"Headphones.Play"_s);
    m_playPause->setObjectName(u"Headphones.PlayPause"_s);
    m_preview->setObjectName(u"Headphones.Preview"_s);
    m_configure->setObjectName(u"Headphones.Configure"_s);
    m_stop->setDefaultAction(actions.stopAction());
    m_pause->setDefaultAction(actions.pauseAction());
    m_play->setDefaultAction(actions.playAction());
    m_playPause->setDefaultAction(actions.playPauseAction());
    m_preview->setText(tr("Preview selected"));
    m_configure->setText(tr("Configure"));
    m_configure->setToolTip(tr("Configure Headphones Preview"));
    m_configure->setEnabled(!provider.expired());
    connect(m_preview, &ToolButton::clicked, &actions, &HeadphonesActions::previewDisplaySelection);
    const auto updatePreview = [this, &actions]() {
        const auto reason = actions.displayPreviewReason();
        m_preview->setEnabled(reason.isEmpty());
        m_preview->setToolTip(reason.isEmpty() ? tr("Preview the first selected track") : reason);
    };
    connect(&actions, &HeadphonesActions::availabilityChanged, this, updatePreview);
    updatePreview();
    connect(m_configure, &ToolButton::clicked, this, [this, provider]() {
        if(auto owner = provider.lock()) {
            owner->showSettings(window());
        }
    });
    for(auto* button : {m_preview, m_stop, m_pause, m_play, m_playPause, m_configure}) {
        button->installEventFilter(this);
        m_layout->addWidget(button);
    }
    m_pause->hide();
    m_play->hide();
    m_configure->hide();
    connect(&controller, &PreviewController::snapshotChanged, this, &HeadphonesPlayerControl::updateIcons);
    if(settings) {
        settings->subscribe<Settings::Gui::IconTheme>(this, [this]() { updateIcons(); });
    }
    updateIcons();
}

QString HeadphonesPlayerControl::name() const
{
    return tr("Headphones Player Controls");
}

QString HeadphonesPlayerControl::layoutName() const
{
    return u"HeadphonesPlayerControls"_s;
}

void HeadphonesPlayerControl::saveLayoutData(QJsonObject& layout)
{
    HeadphonesControlWidget::saveLayoutData(layout);
    layout["ShowStop"_L1]            = m_showStop;
    layout["ShowPause"_L1]           = m_showPause;
    layout["ShowPlay"_L1]            = m_showPlay;
    layout["ShowPlayPause"_L1]       = m_showPlayPause;
    layout["ShowPreviewSelected"_L1] = m_showPreview;
    layout["ShowConfigure"_L1]       = m_showConfigure;
}

void HeadphonesPlayerControl::loadLayoutData(const QJsonObject& layout)
{
    m_showStop      = layout.value("ShowStop"_L1).toBool(true);
    m_showPause     = layout.value("ShowPause"_L1).toBool(false);
    m_showPlay      = layout.value("ShowPlay"_L1).toBool(false);
    m_showPlayPause = layout.value("ShowPlayPause"_L1).toBool(true);
    m_showPreview   = layout.value("ShowPreviewSelected"_L1).toBool(true);
    m_showConfigure = layout.value("ShowConfigure"_L1).toBool(false);
    m_stop->setVisible(m_showStop);
    m_pause->setVisible(m_showPause);
    m_play->setVisible(m_showPlay);
    m_playPause->setVisible(m_showPlayPause);
    m_preview->setVisible(m_showPreview);
    m_configure->setVisible(m_showConfigure);
    HeadphonesControlWidget::loadLayoutData(layout);
}

void HeadphonesPlayerControl::populateContextMenu(QMenu* menu)
{
    const auto addButton = [this, menu](const QString& text, ToolButton* button, bool& visible) {
        auto* action = menu->addAction(text);
        action->setCheckable(true);
        action->setChecked(visible);
        connect(action, &QAction::triggered, this, [this, button, &visible](bool checked) {
            visible = checked;
            button->setVisible(checked);
            updateOrientation();
        });
    };
    addButton(tr("Show Stop"), m_stop, m_showStop);
    addButton(tr("Show Pause"), m_pause, m_showPause);
    addButton(tr("Show Play"), m_play, m_showPlay);
    addButton(tr("Show Play/Pause"), m_playPause, m_showPlayPause);
    addButton(tr("Show Preview Selected"), m_preview, m_showPreview);
    addButton(tr("Show Configure"), m_configure, m_showConfigure);
    menu->addSeparator();
    HeadphonesControlWidget::populateContextMenu(menu);
}

bool HeadphonesPlayerControl::eventFilter(QObject* watched, QEvent* event)
{
    // Disabled buttons consume context-menu events instead of propagating them.
    // Keep configuration accessible without enabling their playback actions.
    if((watched == m_stop || watched == m_pause || watched == m_play || watched == m_playPause
        || watched == m_preview || watched == m_configure)
       && event->type() == QEvent::ContextMenu) {
        auto* source = static_cast<QContextMenuEvent*>(event);
        QContextMenuEvent forwarded {source->reason(), mapFromGlobal(source->globalPos()), source->globalPos(),
                                     source->modifiers()};
        contextMenuEvent(&forwarded);
        event->accept();
        return true;
    }
    return HeadphonesControlWidget::eventFilter(watched, event);
}

void HeadphonesPlayerControl::updateIcons()
{
    if(!m_controller) {
        return;
    }
    const auto snapshot = m_controller->snapshot();
    const bool play = snapshot.pauseRequested || snapshot.state == PreviewState::Stopped
                   || snapshot.state == PreviewState::Error;
    m_stop->setIcon(Gui::iconFromTheme(Constants::Icons::Stop));
    m_pause->setIcon(Gui::iconFromTheme(Constants::Icons::Pause));
    m_play->setIcon(Gui::iconFromTheme(Constants::Icons::Play));
    auto previewIcon = Gui::iconFromTheme("audio-headphones");
    if(previewIcon.isNull()) {
        previewIcon = Gui::iconFromTheme(Constants::Icons::PlaylistPlay);
    }
    m_preview->setIcon(previewIcon);
    m_configure->setIcon(Gui::iconFromTheme(Constants::Icons::Settings));
    m_playPause->setIcon(Gui::iconFromTheme(play ? Constants::Icons::Play : Constants::Icons::Pause));
    m_playPause->setToolTip(play ? tr("Play headphone preview") : tr("Pause headphone preview"));
}
} // namespace Fooyin::Headphones
