// SPDX-License-Identifier: GPL-3.0-or-later
#include "fakes/plugincontexts.h"

#include <core/engine/audioloader.h>
#include <core/plugins/coreplugin.h>
#include <core/plugins/plugin.h>
#include <core/scripting/scriptregistry.h>
#include <gui/fywidget.h>
#include <gui/plugins/guiplugin.h>
#include <gui/plugins/pluginconfigguiplugin.h>
#include <gui/widgetprovider.h>
#include <utils/actions/actionmanager.h>
#include <utils/actions/command.h>
#include <utils/settings/settingsmanager.h>

#include <QApplication>
#include <QDialogButtonBox>
#include <QJsonArray>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMainWindow>
#include <QPluginLoader>
#include <QPushButton>
#include <QSlider>
#include <QSplitter>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>

using namespace Qt::StringLiterals;
using namespace Fooyin;
using namespace Fooyin::Headphones::Tests;

namespace {
QString pluginPath;
QStringList messages;
QtMessageHandler previousHandler {nullptr};

// Match the host's registry-before-plugin shutdown order, including failed tests.
class PluginLoader : public QPluginLoader
{
public:
    using QPluginLoader::QPluginLoader;
    ~PluginLoader() override
    {
        ScriptRegistry::instance().shutdown();
        QPluginLoader::unload();
    }

    bool unload()
    {
        ScriptRegistry::instance().shutdown();
        return QPluginLoader::unload();
    }
};

void captureMessages(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    if(QString::fromLatin1(context.category) == u"fy.headphones") {
        messages.append(message);
    }
    else if(previousHandler) {
        previousHandler(type, context, message);
    }
}
} // namespace

class PluginLifecycleTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init()
    {
        messages.clear();
        previousHandler = qInstallMessageHandler(captureMessages);
    }

    void cleanup()
    {
        qInstallMessageHandler(previousHandler);
        QLoggingCategory::setFilterRules({});
    }

    void metadataAndDefaultLogging()
    {
        PluginLoader loader {pluginPath};
        loader.setLoadHints({});
        QCOMPARE(loader.metaData().value(u"IID"_s).toString(), QString::fromLatin1(FOOYIN_PLUGIN_IID));
        const auto metadata = loader.metaData().value(u"MetaData"_s).toObject();
        QCOMPARE(metadata.value(u"Name"_s).toString(), u"Headphones Preview"_s);
        QVERIFY(!metadata.value(u"Version"_s).toString().isEmpty());
        QVERIFY(!metadata.value(u"Author"_s).toString().isEmpty());
        auto* root = loader.instance();
        QVERIFY2(root, qPrintable(loader.errorString()));
        auto* plugin = qobject_cast<Plugin*>(root);
        auto* core   = qobject_cast<CorePlugin*>(root);
        auto* gui    = qobject_cast<GuiPlugin*>(root);
        QVERIFY(plugin);
        QVERIFY(core);
        QVERIFY(gui);
        QLoggingCategory::setFilterRules({});
        core->initialise(coreContext());
        gui->initialise(guiContext());
        ScriptRegistry::instance().shutdown();
        plugin->shutdown();
        QVERIFY(root->children().isEmpty());
        QVERIFY(loader.unload());
        QVERIFY(messages.isEmpty());
    }

    void lifecycle_data()
    {
        QTest::addColumn<int>("stage");
        QTest::newRow("before-initialisation") << 0;
        QTest::newRow("core-only") << 1;
        QTest::newRow("core-and-gui") << 2;
    }

    void lifecycle()
    {
        QFETCH(int, stage);
        QLoggingCategory::setFilterRules(u"fy.headphones.debug=true"_s);
        PluginLoader loader {pluginPath};
        loader.setLoadHints({});
        auto* root = loader.instance();
        QVERIFY2(root, qPrintable(loader.errorString()));
        auto* plugin = qobject_cast<Plugin*>(root);
        auto* core   = qobject_cast<CorePlugin*>(root);
        auto* gui    = qobject_cast<GuiPlugin*>(root);
        QVERIFY(plugin && core && gui);
        if(stage >= 1) {
            core->initialise(coreContext());
            core->initialise(coreContext());
        }
        if(stage >= 2) {
            gui->initialise(guiContext());
            gui->initialise(guiContext());
        }
        QCOMPARE(messages.count(u"Core initialised"_s), stage >= 1 ? 1 : 0);
        QCOMPARE(messages.count(u"GUI initialised"_s), stage >= 2 ? 1 : 0);
        QVERIFY(root->children().isEmpty());
        ScriptRegistry::instance().shutdown();
        plugin->shutdown();
        plugin->shutdown();
        // Late callbacks cannot resurrect a stopped plugin.
        core->initialise(coreContext());
        gui->initialise(guiContext());
        QVERIFY(loader.unload());
        QCOMPARE(messages.count(u"Preview resources stopped"_s), 1);
        QCOMPARE(messages.count(u"Shutdown complete"_s), 1);
        QCOMPARE(messages.size(), stage + 2);
    }

    void guiBeforeCore()
    {
        QLoggingCategory::setFilterRules(u"fy.headphones.debug=true"_s);
        PluginLoader loader {pluginPath};
        loader.setLoadHints({});
        auto* root = loader.instance();
        QVERIFY2(root, qPrintable(loader.errorString()));
        auto* core = qobject_cast<CorePlugin*>(root);
        auto* gui  = qobject_cast<GuiPlugin*>(root);
        QVERIFY(core && gui);
        gui->initialise(guiContext());
        QCOMPARE(messages, QStringList {u"GUI initialisation requires core initialisation"_s});
        core->initialise(coreContext());
        gui->initialise(guiContext());
        QVERIFY(loader.unload()); // Destructor is also a shutdown path.
        QCOMPARE(messages.count(u"GUI initialised"_s), 1);
        QCOMPARE(messages.count(u"Shutdown complete"_s), 1);
    }

    void loadedPluginRegistersWidgetsActionsAndSharedConfiguration()
    {
        QTemporaryDir dir;
        SettingsManager settings {dir.filePath(u"settings.ini"_s)};
        QMainWindow mainWindow;
        ActionManager actions {&settings};
        actions.setMainWindow(&mainWindow);
        // The provider must release its plugin callbacks before module unload.
        auto widgets = std::make_unique<WidgetProvider>();
        PluginLoader loader {pluginPath};
        loader.setLoadHints({});
        auto* root = loader.instance();
        QVERIFY2(root, qPrintable(loader.errorString()));
        auto* core   = qobject_cast<CorePlugin*>(root);
        auto* gui    = qobject_cast<GuiPlugin*>(root);
        auto* plugin = qobject_cast<Plugin*>(root);
        auto* config = qobject_cast<PluginConfigGuiPlugin*>(root);
        QVERIFY(core && gui && plugin && config);
        auto coreServices            = coreContext();
        coreServices.settingsManager = &settings;
        coreServices.audioLoader     = std::make_shared<AudioLoader>();
        core->initialise(coreServices);
        auto guiServices           = guiContext();
        guiServices.actionManager  = &actions;
        guiServices.widgetProvider = widgets.get();
        gui->initialise(guiServices);
        const QStringList additionalIds {u"Headphones.Volume.Up"_s, u"Headphones.Volume.Down"_s,
                                         u"Headphones.Volume.Mute"_s, u"Headphones.Volume.Unmute"_s,
                                         u"Headphones.Volume.ToggleMute"_s, u"Headphones.Configure"_s,
                                         u"Headphones.Tracks.FocusPreviewed"_s};
        for(const auto& id : additionalIds) {
            auto* command = actions.command(Id {id});
            QVERIFY(command);
            QVERIFY(command->defaultShortcuts().isEmpty());
        }
        // Configuration and volume actions also work before creating any widgets.
        actions.command("Headphones.Volume.Mute")->action()->trigger();
        actions.command("Headphones.Volume.Unmute")->action()->trigger();
        actions.command("Headphones.Configure")->action()->trigger();
        const auto catalogue = widgets->widgetCatalogue();
        QCOMPARE(catalogue.size(), size_t {5});
        for(const auto& entry : catalogue) {
            QCOMPARE(entry.categories, QStringList {u"Headphones plugin"_s});
        }
        QVERIFY(!widgets->widgetExists(u"HeadphonesPreview"_s));
        QVERIFY(!widgets->createWidget(u"HeadphonesPreview"_s));
        const QStringList controlIds {u"HeadphonesPlayerControls"_s, u"HeadphonesSeekBar"_s,
                                       u"HeadphonesVolumeControls"_s, u"HeadphonesOutputDevice"_s,
                                       u"HeadphonesTrackInfo"_s};
        std::vector<QPointer<FyWidget>> controls;
        for(const auto& id : controlIds) {
            QVERIFY(widgets->widgetExists(id));
            auto* control = widgets->createWidget(id);
            QVERIFY(control);
            QCOMPARE(control->layoutName(), id);
            controls.emplace_back(control);
            auto* another = widgets->createWidget(id);
            QVERIFY(another);
            QVERIFY(another != control);
            delete another;
        }
        auto* command = actions.command("Headphones.Tracks.PreviewSelected");
        QVERIFY(command);
        QVERIFY(command->defaultShortcuts().isEmpty());
        auto* volume = controls[2]->findChild<QSlider*>(u"Headphones.Volume"_s);
        QVERIFY(volume);
        volume->setValue(37);
        QCOMPARE(settings.value(u"Headphones/Volume").toDouble(), 0.37);
        auto provider = config->settingsProvider();
        provider->showSettings(nullptr);
        QDialog* dialog {nullptr};
        for(auto* widget : QApplication::topLevelWidgets()) {
            if(widget->objectName() == u"Headphones.Settings") {
                dialog = qobject_cast<QDialog*>(widget);
            }
        }
        QVERIFY(dialog);
        auto* playerControls = controls.front().data();
        auto* configureButton = playerControls->findChild<QToolButton*>(u"Headphones.Configure"_s);
        QVERIFY(configureButton);
        QVERIFY(configureButton->isHidden());
        playerControls->loadLayoutData({{u"ShowConfigure"_s, true}});
        QVERIFY(!configureButton->isHidden());
        QVERIFY(configureButton->isEnabled());
        dialog->hide();
        configureButton->click();
        QVERIFY(dialog->isVisible());
        int count {0};
        for(auto* widget : QApplication::topLevelWidgets()) {
            count += widget->objectName() == u"Headphones.Settings";
        }
        QCOMPARE(count, 1);
        QPointer<FyWidget> oldVolume {controls[2]};
        delete controls[2].data();
        QVERIFY(oldVolume.isNull());
        controls[2] = widgets->createWidget(u"HeadphonesVolumeControls"_s);
        QVERIFY(controls[2]);
        QCOMPARE(controls[2]->findChild<QSlider*>(u"Headphones.Volume"_s)->value(), 37);
        ScriptRegistry::instance().shutdown();
        plugin->shutdown();
        for(const auto& control : controls) {
            QVERIFY(control.isNull());
        }
        for(const auto& id : controlIds) {
            QVERIFY(!widgets->createWidget(id));
        }
        QVERIFY(!actions.command("Headphones.Tracks.PreviewSelected"));
        for(const auto& id : additionalIds) {
            QVERIFY(!actions.command(Id {id}));
        }
        provider->showSettings(nullptr); // The retained host adapter safely handles a destroyed provider.
        QVERIFY(!widgets->createWidget(u"HeadphonesPreview"_s));
        provider.reset();
        widgets.reset();
        QVERIFY(loader.unload());
    }

    void loadedPluginRegistersScriptVariablesBeforeGui()
    {
        PluginLoader loader {pluginPath};
        loader.setLoadHints({});
        auto* root = loader.instance();
        QVERIFY2(root, qPrintable(loader.errorString()));
        auto* core = qobject_cast<CorePlugin*>(root);
        auto* gui = qobject_cast<GuiPlugin*>(root);
        QVERIFY(core && gui);
        auto& registry = ScriptRegistry::instance();
        const auto before = registry.variables().size();
        core->initialise(coreContext());
        QCOMPARE(registry.variables().size(), before + 10);
        core->initialise(coreContext());
        gui->initialise(guiContext());
        gui->initialise(guiContext());
        QCOMPARE(registry.variables().size(), before + 10);
        for(const auto* name : {"HEADPHONES_ALBUMARTIST", "HEADPHONES_ARTIST", "HEADPHONES_ALBUM",
                                "HEADPHONES_TITLE", "HEADPHONES_DISK", "HEADPHONES_TRACK", "HEADPHONESICON",
                                "HEADPHONES_ISPLAYING", "HEADPHONES_ISSTOPPED", "HEADPHONES_ISPAUSED"}) {
            const auto variable = QString::fromLatin1(name);
            const auto* invoker = registry.variableInvoker(variable.toLower());
            QVERIFY2(invoker, name);
            const auto result = (*invoker)({}, variable);
            const bool stopped = variable == u"HEADPHONES_ISSTOPPED";
            QCOMPARE(result.value, stopped ? u"1"_s : QString {});
            QCOMPARE(result.cond, stopped);
        }
        QVERIFY(loader.unload());
        QVERIFY(!registry.isVariable(u"HEADPHONESICON"_s));
    }

    void retainsLoaderUntilShutdown()
    {
        PluginLoader loader {pluginPath};
        loader.setLoadHints({});
        auto* root = loader.instance();
        QVERIFY2(root, qPrintable(loader.errorString()));
        auto* core   = qobject_cast<CorePlugin*>(root);
        auto* plugin = qobject_cast<Plugin*>(root);
        QVERIFY(core && plugin);
        auto context                          = coreContext();
        context.audioLoader                   = std::make_shared<AudioLoader>();
        std::weak_ptr<AudioLoader> weakLoader = context.audioLoader;
        core->initialise(context);
        context.audioLoader.reset();
        QVERIFY(!weakLoader.expired());
        ScriptRegistry::instance().shutdown();
        plugin->shutdown();
        QVERIFY(weakLoader.expired());
        QVERIFY(loader.unload());
    }

    void applicationQuitPreservesLayout_data()
    {
        QTest::addColumn<bool>("hostDestroysWidgets");
        QTest::newRow("host-destroys-widgets") << true;
        QTest::newRow("plugin-cleans-up-remaining-widgets") << false;
    }

    void applicationQuitPreservesLayout()
    {
        QFETCH(bool, hostDestroysWidgets);
        QTemporaryDir dir;
        SettingsManager settings {dir.filePath(u"settings.ini"_s)};
        QMainWindow mainWindow;
        ActionManager actions {&settings};
        actions.setMainWindow(&mainWindow);
        auto widgets = std::make_unique<WidgetProvider>();
        PluginLoader loader {pluginPath};
        loader.setLoadHints({});
        auto* root = loader.instance();
        QVERIFY2(root, qPrintable(loader.errorString()));
        auto* core   = qobject_cast<CorePlugin*>(root);
        auto* gui    = qobject_cast<GuiPlugin*>(root);
        auto* plugin = qobject_cast<Plugin*>(root);
        QVERIFY(core && gui && plugin);
        auto coreServices            = coreContext();
        coreServices.settingsManager = &settings;
        coreServices.audioLoader     = std::make_shared<AudioLoader>();
        std::weak_ptr<AudioLoader> weakLoader = coreServices.audioLoader;
        core->initialise(coreServices);
        coreServices.audioLoader.reset();
        auto guiServices           = guiContext();
        guiServices.actionManager  = &actions;
        guiServices.widgetProvider = widgets.get();
        gui->initialise(guiServices);

        auto layout = std::make_unique<QSplitter>();
        const QStringList ids {u"HeadphonesPlayerControls"_s,
                               u"HeadphonesSeekBar"_s, u"HeadphonesVolumeControls"_s,
                               u"HeadphonesOutputDevice"_s, u"HeadphonesTrackInfo"_s};
        std::vector<QPointer<FyWidget>> panels;
        QJsonArray beforeQuit;
        for(const auto& id : ids) {
            auto* panel = widgets->createWidget(id);
            QVERIFY(panel);
            layout->addWidget(panel);
            panels.emplace_back(panel);
            panel->saveLayout(beforeQuit);
        }
        QVERIFY(!weakLoader.expired());
        bool saved {false};
        // Like fooyin's main(), register host shutdown after plugin startup.
        // The host saves its layout before destroying GUI objects and unloading plugins.
        QObject hostShutdown;
        connect(qApp, &QCoreApplication::aboutToQuit, &hostShutdown, [&]() {
            QVERIFY(weakLoader.expired()); // Preview resources must already be released.
            QJsonArray atQuit;
            for(const auto& panel : panels) {
                QVERIFY2(panel, "Plugin deleted a layout widget before host layout saving");
                panel->saveLayout(atQuit);
            }
            QCOMPARE(atQuit, beforeQuit);
            QVERIFY(actions.command("Headphones.Tracks.PreviewSelected"));
            saved = true;
            if(hostDestroysWidgets) {
                layout.reset();
            }
            ScriptRegistry::instance().shutdown();
            plugin->shutdown();
            plugin->shutdown();
            for(const auto& panel : panels) {
                QVERIFY(panel.isNull());
            }
            QVERIFY(!actions.command("Headphones.Tracks.PreviewSelected"));
        });
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        QCOMPARE(qApp->exec(), 0);
        QVERIFY(saved);
        layout.reset();
        widgets.reset();
        QVERIFY(loader.unload());
    }

    void applicationQuit()
    {
        QLoggingCategory::setFilterRules(u"fy.headphones.debug=true"_s);
        PluginLoader loader {pluginPath};
        loader.setLoadHints({});
        auto* root = loader.instance();
        QVERIFY2(root, qPrintable(loader.errorString()));
        auto* core = qobject_cast<CorePlugin*>(root);
        auto* gui  = qobject_cast<GuiPlugin*>(root);
        QVERIFY(core && gui);
        core->initialise(coreContext());
        gui->initialise(guiContext());
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        QCOMPARE(qApp->exec(), 0);
        QCOMPARE(messages.count(u"Preview resources stopped"_s), 1);
        QCOMPARE(messages.count(u"Shutdown complete"_s), 0);
        QVERIFY(loader.unload());
        QCOMPARE(messages.count(u"Shutdown complete"_s), 1);
    }
};

int main(int argc, char** argv)
{
    QApplication app {argc, argv};
    if(argc < 2) {
        qCritical("Pass the plugin library path as the first argument");
        return 1;
    }
    pluginPath = QString::fromLocal8Bit(argv[1]);
    // Remove our argument before handing the remaining options to Qt Test.
    for(int i = 1; i < argc; ++i) {
        argv[i] = argv[i + 1];
    }
    --argc;
    PluginLifecycleTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "pluginlifecycletest.moc"
