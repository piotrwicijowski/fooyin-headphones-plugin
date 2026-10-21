// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <core/plugins/coreplugin.h>
#include <core/plugins/plugin.h>
#include <gui/plugins/guiplugin.h>
#include <gui/plugins/pluginconfigguiplugin.h>

#include <QObject>
#include <QPointer>

#include <memory>
#include <vector>

namespace Fooyin {
class FyWidget;
class SettingsManager;
}

namespace Fooyin::Headphones {

class PreviewController;
class HeadphoneDevices;
class HeadphonesSettings;
class HeadphonesActions;
class HeadphonesSettingsProvider;
class HeadphonesVariableProvider;

class HeadphonesPlugin : public QObject,
                         public Plugin,
                         public CorePlugin,
                         public GuiPlugin,
                         public PluginConfigGuiPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID FOOYIN_PLUGIN_IID FILE "headphones.json")
    Q_INTERFACES(Fooyin::Plugin Fooyin::CorePlugin Fooyin::GuiPlugin Fooyin::PluginConfigGuiPlugin)

public:
    HeadphonesPlugin();
    ~HeadphonesPlugin() override;

    void initialise(const CorePluginContext& context) override;
    void initialise(const GuiPluginContext& context) override;
    void shutdown() override;
    std::unique_ptr<PluginSettingsProvider> settingsProvider() const override;

private:
    void stopPreview();
    void applyOutputDevice();
    std::unique_ptr<HeadphonesSettings> m_preferences;
    std::unique_ptr<HeadphoneDevices> m_devices;
    std::unique_ptr<PreviewController> m_controller;
    std::unique_ptr<HeadphonesVariableProvider> m_variableProvider;
    std::unique_ptr<HeadphonesActions> m_actions;
    std::shared_ptr<HeadphonesSettingsProvider> m_settingsUi;
    std::shared_ptr<std::vector<QPointer<FyWidget>>> m_widgets;
    QPointer<SettingsManager> m_settingsManager;
    QByteArray m_selectedDevice;
    int m_selectedChannelPair {0};
    bool m_outputAttached {false};
    bool m_coreInitialised {false};
    bool m_guiInitialised {false};
    bool m_stopping {false};
    bool m_shutdown {false};
};

} // namespace Fooyin::Headphones
