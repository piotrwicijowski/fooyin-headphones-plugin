// SPDX-License-Identifier: GPL-3.0-or-later
// Test-only access to the matching host's installer; never linked into the plugin.
#include "plugininstaller.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QTextStream>

namespace {
template <typename Result>
bool installed(const Result& result)
{
    // v0.14.0 returns an enum; newer development trees return std::expected.
    if constexpr(requires { result.has_value(); }) {
        return result.has_value();
    }
    else {
        return result == Result::Installed;
    }
}
} // namespace

int main(int argc, char* argv[])
{
    const QCoreApplication app {argc, argv};
    const auto args = app.arguments();
    if(args.size() != 3) {
        qCritical() << "Usage: pluginbundlecheck <bundle.zip> <temporary-plugin-directory>";
        return 1;
    }

    const QString destination = QDir {args.at(2)}.absolutePath();
    if(!installed(Fooyin::PluginInstaller::install(args.at(1), destination, false))) {
        qCritical() << "fooyin rejected the plugin bundle:" << args.at(1);
        return 1;
    }

    QDirIterator plugins {
        destination, {QStringLiteral("fyplugin_headphones.so")}, QDir::Files, QDirIterator::Subdirectories};
    if(!plugins.hasNext()) {
        qCritical() << "Installed bundle contains no Headphones plugin";
        return 1;
    }
    const QString path = plugins.next();
    if(plugins.hasNext()) {
        qCritical() << "Installed bundle contains multiple Headphones plugins";
        return 1;
    }

    QTextStream {stdout} << path << Qt::endl;
    return 0;
}
