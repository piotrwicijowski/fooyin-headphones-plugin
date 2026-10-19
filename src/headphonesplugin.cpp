// SPDX-License-Identifier: GPL-3.0-or-later
#include "headphonesplugin.h"

#include "headphoneslog.h"
#include "previewcontroller.h"

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

    // One shared session service; the real explicitly-selected output is wired
    // in Phase 3. An absent output can never fall back to the system default.
    m_controller      = std::make_unique<PreviewController>(makePreviewSourceFactory(context.audioLoader), nullptr);
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
    qCDebug(HEADPHONES) << "GUI initialised";
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
    m_guiInitialised  = false;
    m_coreInitialised = false;
    qCDebug(HEADPHONES) << "Shutdown complete";
}

} // namespace Fooyin::Headphones
