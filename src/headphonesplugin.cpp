// SPDX-License-Identifier: GPL-3.0-or-later
#include "headphonesplugin.h"

#include "headphonesactions.h"
#include "headphoneslog.h"
#include "headphonesoutputdevice.h"
#include "headphonesplayercontrol.h"
#include "headphonesseekbar.h"
#include "headphonessettings.h"
#include "headphonessettingsdialog.h"
#include "headphonestrackinfo.h"
#include "headphonesvariableprovider.h"
#include "headphonesvolumecontrol.h"
#include "previewcontroller.h"
#include "qtaudiooutput.h"

#include <QCoreApplication>

#include <core/playlist/playlisthandler.h>
#include <core/scripting/scriptregistry.h>
#include <gui/guiconstants.h>
#include <gui/playlist/playlistinteractor.h>
#include <gui/trackselectioncontroller.h>
#include <gui/widgetprovider.h>
#include <utils/actions/actionmanager.h>
#include <utils/settings/settingsmanager.h>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace Fooyin::Headphones {

Q_LOGGING_CATEGORY(HEADPHONES, "fy.headphones", QtInfoMsg)

HeadphonesPlugin::HeadphonesPlugin()
{
    // Stop preview before decoder providers unload, but leave layout widgets alive:
    // the host's later aboutToQuit handler saves the layout before destroying the GUI.
    if(auto* app = QCoreApplication::instance()) {
        QObject::connect(app, &QCoreApplication::aboutToQuit, this, &HeadphonesPlugin::stopPreview);
    }
}

HeadphonesPlugin::~HeadphonesPlugin()
{
    shutdown();
}

void HeadphonesPlugin::initialise(const CorePluginContext& context)
{
    if(m_stopping || m_coreInitialised) {
        return;
    }

    m_settingsManager = context.settingsManager;
    m_preferences     = std::make_unique<HeadphonesSettings>(context.settingsManager);
    m_controller      = std::make_unique<PreviewController>(makePreviewSourceFactory(context.audioLoader), nullptr);
    m_controller->setReplayGainSettingsManager(context.settingsManager);
    m_controller->setVolume(m_preferences->volume());
    m_controller->setPreviewStartOffsetSeconds(m_preferences->previewStartOffsetSeconds());
    m_variableProvider = std::make_unique<HeadphonesVariableProvider>(*m_controller);
    for(const auto& variable : m_variableProvider->variables()) {
        if(!ScriptRegistry::instance().registerVariable(variable)) {
            qCWarning(HEADPHONES) << "Failed to register Headphones Preview script variable:" << variable.name;
        }
    }
    QObject::connect(m_preferences.get(), &HeadphonesSettings::deviceChanged, this, [this]() {
        if(!m_stopping) {
            applyOutputDevice();
        }
    });
    QObject::connect(m_preferences.get(), &HeadphonesSettings::volumeChanged, this, [this]() {
        if(!m_stopping) {
            m_controller->setVolume(m_preferences->volume());
        }
    });
    QObject::connect(m_preferences.get(), &HeadphonesSettings::previewStartOffsetChanged, this, [this]() {
        if(!m_stopping) {
            m_controller->setPreviewStartOffsetSeconds(m_preferences->previewStartOffsetSeconds());
        }
    });
    QObject::connect(m_controller.get(), &PreviewController::snapshotChanged, this,
                     [this](const PreviewSnapshot& snapshot) {
                         if(!m_stopping && snapshot.volume != m_preferences->volume()) {
                             m_preferences->setVolume(snapshot.volume);
                         }
                     });
    m_coreInitialised = true;
    qCDebug(HEADPHONES) << "Core initialised";
}

void HeadphonesPlugin::initialise(const GuiPluginContext& context)
{
    if(m_stopping || m_guiInitialised) {
        return;
    }
    if(!m_coreInitialised) {
        qCWarning(HEADPHONES) << "GUI initialisation requires core initialisation";
        return;
    }

    m_guiInitialised = true;
    if(auto* interactor = context.playlistInteractor) {
        connect(m_variableProvider.get(), &HeadphonesVariableProvider::variablesChanged, interactor, [interactor]() {
            if(auto* handler = interactor->handler()) {
                // Metadata and state variables are global, so scripts on any row can depend on them.
                for(const auto* playlist : handler->playlists()) {
                    interactor->refreshPlaylist(playlist->id());
                }
            }
        });
    }
    m_devices        = std::make_unique<HeadphoneDevices>();
    applyOutputDevice();
    m_settingsUi = std::make_shared<HeadphonesSettingsProvider>(*m_preferences, *m_devices);
    m_actions    = std::make_unique<HeadphonesActions>(*m_controller, context.actionManager, m_settingsManager,
                                                     m_settingsUi, context.playlistSelection,
                                                     context.playlistInteractor, context.trackSelection);
    if(context.trackSelection) {
        const QPointer<TrackSelectionController> selection {context.trackSelection};
        const QPointer<HeadphonesActions> actions {m_actions.get()};
        const auto syncSelection = [selection, actions]() {
            if(selection && actions) {
                actions->setTrackSelections(selection->selectedSelection(), selection->displaySelection());
            }
        };
        connect(selection, &TrackSelectionController::selectionChanged, m_actions.get(), syncSelection);
        connect(selection, &TrackSelectionController::displaySelectionChanged, m_actions.get(), syncSelection);
        if(context.actionManager) {
            connect(context.actionManager, &ActionManager::contextChanged, m_actions.get(), syncSelection);
        }
        selection->registerTrackContextAction(
            m_actions.get(), TrackContextMenuArea::Track, Constants::Menus::Context::TrackSelection,
            "Headphones.Tracks.PreviewSelected", tr("Preview in headphones"),
            [actions](QMenu* menu, const TrackSelection& tracks) {
                if(actions) {
                    actions->renderMenu(menu, tracks);
                }
            },
            Constants::Menus::Context::TrackFinalSeparator);
        syncSelection();
    }
    m_widgets = std::make_shared<std::vector<QPointer<FyWidget>>>();
    if(context.widgetProvider) {
        const QPointer<PreviewController> controller {m_controller.get()};
        const QPointer<HeadphonesActions> actions {m_actions.get()};
        const QPointer<HeadphonesSettings> preferences {m_preferences.get()};
        const QPointer<HeadphoneDeviceCatalogue> devices {m_devices.get()};
        const std::weak_ptr<HeadphonesSettingsProvider> provider {m_settingsUi};
        const std::weak_ptr<std::vector<QPointer<FyWidget>>> widgets {m_widgets};
        const auto registerControl = [widgetProvider = context.widgetProvider, widgets, controller](
                                         const QString& id, const QString& name, auto create) {
            widgetProvider->registerWidget(id, [widgets, controller, create]() -> FyWidget* {
                const auto owners = widgets.lock();
                if(!owners || !controller) {
                    return nullptr;
                }
                std::erase_if(*owners, [](const auto& widget) { return widget.isNull(); });
                auto* widget = create();
                if(widget) {
                    owners->push_back(widget);
                }
                return widget;
            }, name);
            widgetProvider->setSubMenus(id, {tr("Headphones plugin")});
        };
        const auto settings = m_settingsManager;
        registerControl(u"HeadphonesPlayerControls"_s, tr("Headphones Player Controls"),
                        [controller, actions, settings, provider]() -> FyWidget* {
                            return actions ? new HeadphonesPlayerControl(*controller, *actions, settings, provider)
                                           : nullptr;
                        });
        registerControl(u"HeadphonesSeekBar"_s, tr("Headphones Seekbar"), [controller, settings]() -> FyWidget* {
            return new HeadphonesSeekBar(*controller, settings);
        });
        registerControl(u"HeadphonesVolumeControls"_s, tr("Headphones Volume Controls"),
                        [controller, settings]() -> FyWidget* {
                            return new HeadphonesVolumeControl(*controller, settings);
                        });
        registerControl(u"HeadphonesOutputDevice"_s, tr("Headphones Output Device"),
                        [preferences, devices]() -> FyWidget* {
                            return preferences && devices ? new HeadphonesOutputDevice(*preferences, *devices) : nullptr;
                        });
        registerControl(u"HeadphonesTrackInfo"_s, tr("Headphones Track Info"), [controller]() -> FyWidget* {
            return new HeadphonesTrackInfo(*controller);
        });
    }
    qCDebug(HEADPHONES) << "GUI initialised";
}

std::unique_ptr<PluginSettingsProvider> HeadphonesPlugin::settingsProvider() const
{
    return settingsProviderAdapter(m_settingsUi);
}

void HeadphonesPlugin::applyOutputDevice()
{
    if(m_stopping || !m_devices || !m_controller) {
        return;
    }
    const auto selected = m_preferences->deviceId();
    const auto pair     = m_preferences->outputChannelPair();
    if(m_outputAttached && selected == m_selectedDevice && pair == m_selectedChannelPair) {
        return;
    }
    m_selectedDevice      = selected;
    m_selectedChannelPair = pair;
    m_outputAttached      = true;
    m_controller->setOutput(std::make_unique<QtAudioOutput>(*m_devices, selected, PreviewSinkFactory {}, pair));
}

void HeadphonesPlugin::stopPreview()
{
    if(m_stopping) {
        return;
    }
    m_stopping = true;
    if(m_controller) {
        m_controller->shutdown();
    }
    qCDebug(HEADPHONES) << "Preview resources stopped";
}

void HeadphonesPlugin::shutdown()
{
    if(m_shutdown) {
        return;
    }

    m_shutdown = true;
    stopPreview();
    if(m_widgets) {
        for(const auto& widget : *m_widgets) {
            delete widget.data();
        }
        m_widgets.reset();
    }
    m_settingsUi.reset();
    m_actions.reset();
    // The host drains ScriptRegistry callbacks before shutting down plugins.
    m_variableProvider.reset();
    m_controller.reset();
    m_devices.reset();
    m_preferences.reset();
    m_settingsManager.clear();
    m_guiInitialised  = false;
    m_coreInitialised = false;
    qCDebug(HEADPHONES) << "Shutdown complete";
}

} // namespace Fooyin::Headphones
