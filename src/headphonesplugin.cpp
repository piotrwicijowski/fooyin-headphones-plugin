// SPDX-License-Identifier: GPL-3.0-or-later
#include "headphonesplugin.h"

#include "headphoneslog.h"
#include "headphonessettings.h"
#include "previewcontroller.h"
#include "qtaudiooutput.h"

#include <QCoreApplication>

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

void HeadphonesPlugin::initialise(const GuiPluginContext& /*context*/)
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
    qCDebug(HEADPHONES) << "GUI initialised";
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
