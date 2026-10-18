// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <core/plugins/coreplugin.h>
#include <core/plugins/plugin.h>
#include <gui/plugins/guiplugin.h>

#include <QObject>

namespace Fooyin::Headphones {

class HeadphonesPlugin : public QObject,
                         public Plugin,
                         public CorePlugin,
                         public GuiPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID FOOYIN_PLUGIN_IID FILE "headphones.json")
    Q_INTERFACES(Fooyin::Plugin Fooyin::CorePlugin Fooyin::GuiPlugin)

public:
    HeadphonesPlugin();
    ~HeadphonesPlugin() override;

    void initialise(const CorePluginContext& context) override;
    void initialise(const GuiPluginContext& context) override;
    void shutdown() override;

private:
    bool m_coreInitialised {false};
    bool m_guiInitialised {false};
    bool m_shutdown {false};
};

} // namespace Fooyin::Headphones
