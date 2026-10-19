// SPDX-License-Identifier: GPL-3.0-or-later
#include "fakes/plugincontexts.h"

#include <core/engine/audioloader.h>
#include <core/plugins/coreplugin.h>
#include <core/plugins/plugin.h>
#include <gui/plugins/guiplugin.h>

#include <QApplication>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QPluginLoader>
#include <QTest>
#include <QTimer>

using namespace Qt::StringLiterals;
using namespace Fooyin;
using namespace Fooyin::Headphones::Tests;

namespace {
QString pluginPath;
QStringList messages;
QtMessageHandler previousHandler {nullptr};

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
        QPluginLoader loader {pluginPath};
        loader.setLoadHints({});
        QCOMPARE(loader.metaData().value(u"IID"_s).toString(), QString::fromLatin1(FOOYIN_PLUGIN_IID));
        const auto metadata = loader.metaData().value(u"MetaData"_s).toObject();
        QCOMPARE(metadata.value(u"Name"_s).toString(), u"Headphones Preview"_s);
        QVERIFY(!metadata.value(u"Version"_s).toString().isEmpty());
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
        QPluginLoader loader {pluginPath};
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
        plugin->shutdown();
        plugin->shutdown();
        // Late callbacks cannot resurrect a stopped plugin.
        core->initialise(coreContext());
        gui->initialise(guiContext());
        QVERIFY(loader.unload());
        QCOMPARE(messages.count(u"Shutdown complete"_s), 1);
        QCOMPARE(messages.size(), stage + 1);
    }

    void guiBeforeCore()
    {
        QLoggingCategory::setFilterRules(u"fy.headphones.debug=true"_s);
        QPluginLoader loader {pluginPath};
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

    void retainsLoaderUntilShutdown()
    {
        QPluginLoader loader {pluginPath};
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
        plugin->shutdown();
        QVERIFY(weakLoader.expired());
        QVERIFY(loader.unload());
    }

    void applicationQuit()
    {
        QLoggingCategory::setFilterRules(u"fy.headphones.debug=true"_s);
        QPluginLoader loader {pluginPath};
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
        QCOMPARE(messages.count(u"Shutdown complete"_s), 1);
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
