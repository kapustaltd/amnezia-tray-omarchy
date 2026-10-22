// SPDX-License-Identifier: GPL-3.0-only
#include "tray.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTimer>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("amnezia-tray-omarchy");
    app.setApplicationVersion("0.1.0");
    app.setOrganizationName("kapustaltd");
    app.setQuitOnLastWindowClosed(false);
    QCommandLineParser parser;
    parser.setApplicationDescription("An AmneziaVPN tray wrapper for Omarchy");
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption client("client", "Path to the bridge-enabled AmneziaVPN executable", "path");
    parser.addOption(client);
    QCommandLineOption startClient("start-client", "Start AmneziaVPN if its local API is unavailable");
    parser.addOption(startClient);
    parser.addPositionalArgument("url", "Optional vpn:// link to preview in AmneziaVPN", "[url]");
    parser.process(app);
    if (parser.positionalArguments().size() > 1) parser.showHelp(2);
    const QString url = parser.positionalArguments().value(0);
    if (!url.isEmpty() && (!url.startsWith("vpn://") || url.size() > 512 * 1024)) {
        QMessageBox::warning(nullptr, "Amnezia Tray", "Expected a vpn:// link, at most 512 KiB.");
        return 2;
    }
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        QMessageBox::critical(nullptr, "Amnezia Tray", "The desktop session D-Bus is unavailable.");
        return 1;
    }
    if (!bus.registerService("org.kapustaltd.AmneziaTrayShell1")) {
        QDBusInterface existing("org.kapustaltd.AmneziaTrayShell1", "/org/kapustaltd/AmneziaTrayShell1",
            "org.kapustaltd.AmneziaTrayShell1", bus);
        QDBusReply<bool> reply = existing.call("OpenUrl", url);
        return reply.isValid() && reply.value() ? 0 : 1;
    }
    QSettings preferences;
    if (parser.isSet(client)) preferences.setValue("clientExecutable", parser.value(client));
    QString executable = preferences.value("clientExecutable").toString();
    if (executable.isEmpty()) executable = QStandardPaths::findExecutable("AmneziaVPN");
    Tray tray(executable);
    if (!tray.start()) return 1;
    if (parser.isSet(startClient)) tray.startClientIfNeeded();
    if (!url.isEmpty()) QTimer::singleShot(0, &tray, [&] { tray.OpenUrl(url); });
    return app.exec();
}
