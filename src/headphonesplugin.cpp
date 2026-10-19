// SPDX-License-Identifier: GPL-3.0-or-later
#include "headphonesplugin.h"

#include "headphonesactions.h"
#include "headphoneslog.h"
#include "headphonessettings.h"
#include "headphonessettingsdialog.h"
#include "headphoneswidget.h"
#include "previewcontroller.h"
#include "qtaudiooutput.h"

#include <QCoreApplication>

#include <gui/guiconstants.h>
#include <gui/trackselectioncontroller.h>
#include <gui/widgetprovider.h>
#include <utils/actions/actionmanager.h>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace Fooyin::Headphones {

Q_LOGGING_CATEGORY(HEADPHONES, "fy.headphones", QtInfoMsg)

HeadphonesPlugin::HeadphonesPlugin()
{
    // Normal exit must release future preview resources before providers unload.
    if(auto* app = QCoreApplication::instance()) {
        QObject::connect(app, &QCoreApplication::aboutToQuit, this, &HeadphonesPlugin::shutdown);
    }
}

HeadphonesPlugin::~HeadphonesPlugin()
{
    shutdown();
}

void HeadphonesPlugin::initialise(const CorePluginContext& context)
{
    if(m_shutdown || m_coreInitialised) {
        return;
    }

    m_preferences = std::make_unique<HeadphonesSettings>(context.settingsManager);
    m_controller  = std::make_unique<PreviewController>(makePreviewSourceFactory(context.audioLoader), nullptr);
    m_controller->setVolume(m_preferences->volume());
    QObject::connect(m_preferences.get(), &HeadphonesSettings::deviceChanged, this, [this]() {
        if(!m_shutdown) {
            applyOutputDevice();
        }
    });
    QObject::connect(m_preferences.get(), &HeadphonesSettings::volumeChanged, this, [this]() {
        if(!m_shutdown) {
            m_controller->setVolume(m_preferences->volume());
        }
    });
    QObject::connect(m_controller.get(), &PreviewController::snapshotChanged, this,
                     [this](const PreviewSnapshot& snapshot) {
                         if(!m_shutdown && snapshot.volume != m_preferences->volume()) {
                             m_preferences->setVolume(snapshot.volume);
                         }
                     });
    m_coreInitialised = true;
    qCDebug(HEADPHONES) << "Core initialised";
}

void HeadphonesPlugin::initialise(const GuiPluginContext& context)
{
    if(m_shutdown || m_guiInitialised) {
        return;
    }
    if(!m_coreInitialised) {
        qCWarning(HEADPHONES) << "GUI initialisation requires core initialisation";
        return;
    }

    m_guiInitialised = true;
    m_devices        = std::make_unique<HeadphoneDevices>();
    applyOutputDevice();
    m_settingsUi = std::make_shared<HeadphonesSettingsProvider>(*m_preferences, *m_devices);
    m_actions    = std::make_unique<HeadphonesActions>(*m_controller, context.actionManager);
    if(context.trackSelection) {
        const QPointer<TrackSelectionController> selection {context.trackSelection};
        const QPointer<HeadphonesActions> actions {m_actions.get()};
        const auto syncSelection = [selection, actions]() {
            if(selection && actions) {
                actions->setSelection(selection->selectedTracks(), selection->displayTracks());
            }
        };
        connect(selection, &TrackSelectionController::selectionChanged, m_actions.get(), syncSelection);
        connect(selection, &TrackSelectionController::displaySelectionChanged, m_actions.get(), syncSelection);
        if(context.actionManager) {
            connect(context.actionManager, &ActionManager::contextChanged, m_actions.get(), syncSelection);
        }
        selection->registerTrackContextAction(
            m_actions.get(), TrackContextMenuArea::Track, Constants::Menus::Context::TrackSelection,
            "Headphones.PreviewSelected", tr("Preview in headphones"),
            [actions](QMenu* menu, const TrackSelection& tracks) {
                if(actions) {
                    actions->renderMenu(menu, tracks);
                }
            },
            Constants::Menus::Context::TrackFinalSeparator);
        syncSelection();
    }
    m_widgets = std::make_shared<std::vector<QPointer<HeadphonesWidget>>>();
    if(context.widgetProvider) {
        const QPointer<PreviewController> controller {m_controller.get()};
        const QPointer<HeadphonesActions> actions {m_actions.get()};
        const QPointer<HeadphonesSettings> preferences {m_preferences.get()};
        const QPointer<HeadphoneDeviceCatalogue> devices {m_devices.get()};
        const std::weak_ptr<HeadphonesSettingsProvider> provider {m_settingsUi};
        const std::weak_ptr<std::vector<QPointer<HeadphonesWidget>>> widgets {m_widgets};
        context.widgetProvider->registerWidget(
            u"HeadphonesPreview"_s,
            [controller, actions, preferences, devices, provider, widgets]() -> FyWidget* {
                const auto owners = widgets.lock();
                if(!owners || !controller || !actions || !preferences || !devices) {
                    return nullptr;
                }
                std::erase_if(*owners, [](const auto& widget) { return widget.isNull(); });
                auto* widget = new HeadphonesWidget(*controller, *actions, *preferences, *devices, provider);
                owners->push_back(widget);
                return widget;
            },
            tr("Headphones Preview"));
        context.widgetProvider->setSubMenus(u"HeadphonesPreview"_s, {tr("Playback")});
        context.widgetProvider->setLimit(u"HeadphonesPreview"_s, 1);
    }
    qCDebug(HEADPHONES) << "GUI initialised";
}

std::unique_ptr<PluginSettingsProvider> HeadphonesPlugin::settingsProvider() const
{
    return settingsProviderAdapter(m_settingsUi);
}

void HeadphonesPlugin::applyOutputDevice()
{
    if(!m_devices || !m_controller) {
        return;
    }
    const auto selected = m_preferences->deviceId();
    if(m_outputAttached && selected == m_selectedDevice) {
        return;
    }
    m_selectedDevice = selected;
    m_outputAttached = true;
    m_controller->setOutput(std::make_unique<QtAudioOutput>(*m_devices, selected));
}

void HeadphonesPlugin::shutdown()
{
    if(m_shutdown) {
        return;
    }

    m_shutdown = true;
    if(m_widgets) {
        for(const auto& widget : *m_widgets) {
            delete widget.data();
        }
        m_widgets.reset();
    }
    m_settingsUi.reset();
    m_actions.reset();
    if(m_controller) {
        m_controller->shutdown();
        m_controller.reset();
    }
    m_devices.reset();
    m_preferences.reset();
    m_guiInitialised  = false;
    m_coreInitialised = false;
    qCDebug(HEADPHONES) << "Shutdown complete";
}

} // namespace Fooyin::Headphones
