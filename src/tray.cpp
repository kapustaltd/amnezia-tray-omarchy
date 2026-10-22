// SPDX-License-Identifier: GPL-3.0-only
#include "tray.h"
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <unistd.h>

namespace {
QString bytes(const QString &value) {
    double amount = value.toDouble();
    const QStringList units{"B", "KiB", "MiB", "GiB", "TiB"};
    int unit = 0;
    while (amount >= 1024 && unit < units.size() - 1) { amount /= 1024; ++unit; }
    return QString::number(amount, 'f', unit ? 1 : 0) + " " + units[unit];
}
}

Tray::Tray(QString client, QObject *parent) : QObject(parent), m_client(std::move(client)) {
    m_menu.setObjectName("amneziaTrayMenu");
    m_icon.setIcon(QIcon(":/amnezia-tray-omarchy.svg"));
    m_icon.setContextMenu(&m_menu);
    connect(&m_icon, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) page("home");
    });
    connect(&m_timer, &QTimer::timeout, this, &Tray::refresh);
    connect(&m_menu, &QMenu::aboutToHide, this, [this] { QTimer::singleShot(0, this, &Tray::rebuild); });
}

bool Tray::start() {
    if (!QDBusConnection::sessionBus().registerObject("/org/kapustaltd/AmneziaTrayShell1", this,
        QDBusConnection::ExportAllSlots)) return false;
    rebuild();
    m_icon.show();
    m_timer.start(2000);
    refresh();
    return true;
}

void Tray::startClientIfNeeded() {
    QTimer::singleShot(1000, this, [this] { if (!m_available) launch(); });
}

void Tray::notifyError(const QString &message) {
    m_icon.showMessage("Amnezia Tray", message, QSystemTrayIcon::Warning, 8000);
}

void Tray::request(QJsonObject data, std::function<void(QJsonObject)> done) {
    auto message = QDBusMessage::createMethodCall("org.kapustaltd.AmneziaTray1", "/org/kapustaltd/AmneziaTray1",
        "org.kapustaltd.AmneziaTray1", "Request");
    message << QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact));
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, 4000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, done, data] {
        QDBusPendingReply<QString> reply = *watcher;
        watcher->deleteLater();
        QJsonObject result;
        if (reply.isError()) result = {{"ok", false}, {"transportError", true},
            {"error", "The local AmneziaVPN API is unavailable. Start the bridge-enabled client; an already running unpatched copy must be closed first."}};
        else {
            const auto document = QJsonDocument::fromJson(reply.value().toUtf8());
            result = document.object();
            if (!result.contains("ok")) result = {{"ok", false}, {"error", "Invalid API response"}};
        }
        if (done) done(result);
        else if (!result["ok"].toBool()) notifyError(result["error"].toString());
        if (data["command"].toString() != "snapshot") refresh();
    });
}

void Tray::refresh() {
    if (m_polling) return;
    m_polling = true;
    request({{"command", "snapshot"}}, [this](QJsonObject result) {
        m_polling = false;
        m_available = result["ok"].toBool() && result["api"].toInt() == 1;
        m_snapshot = m_available ? result : QJsonObject{};
        m_icon.setToolTip(m_available
            ? "AmneziaVPN — " + result["state"].toString() + "\n" + result["serverName"].toString()
            : "AmneziaVPN — local API unavailable");
        rebuild();
        if (m_available && !m_imports.isEmpty() && !m_importing) {
            m_importing = true;
            const auto next = m_imports.dequeue();
            request(next, [this](QJsonObject response) {
                if (!response["ok"].toBool()) notifyError(response["error"].toString());
                m_importing = false;
            });
        }
    });
}

void Tray::rebuild() {
    if (m_menu.isVisible()) return;
    qDeleteAll(m_menu.findChildren<QMenu *>(QString(), Qt::FindDirectChildrenOnly));
    m_menu.clear();
    auto label = [this](const QString &text) { auto *action = m_menu.addAction(text); action->setEnabled(false); };
    const QString state = m_snapshot["state"].toString();
    const bool busy = state == "Connected" || state == "Preparing" || state == "Connecting"
        || state == "Reconnecting" || state == "Disconnecting";
    label(m_available ? "AmneziaVPN · " + state : "Local API unavailable");
    if (m_available) {
        label(m_snapshot["serverName"].toString() + " · " + m_snapshot["protocolName"].toString());
        for (const auto &row : m_snapshot["servers"].toArray()) {
            auto server = row.toObject();
            if (server["serverId"] == m_snapshot["serverId"]) {
                label("Server endpoint: " + server["hostName"].toString());
                break;
            }
        }
        label(m_snapshot["hasTraffic"].toBool()
            ? "Session ↓ " + bytes(m_snapshot["received"].toString()) + "  ↑ " + bytes(m_snapshot["sent"].toString())
            : "Traffic: not reported by this protocol");
    }
    m_menu.addSeparator();
    m_menu.addAction("Start AmneziaVPN", this, &Tray::launch);
    m_menu.addAction("Open AmneziaVPN", this, [this] { page("home"); });
    auto *connectAction = m_menu.addAction("Connect", this, [this] { request({{"command", "connect"}}); });
    connectAction->setEnabled(m_available && !busy && !m_snapshot["serverId"].toString().isEmpty());
    auto *disconnectAction = m_menu.addAction("Disconnect", this, [this] { request({{"command", "disconnect"}}); });
    disconnectAction->setEnabled(m_available && busy);
    auto *servers = m_menu.addMenu("Server");
    servers->setEnabled(m_available && !busy);
    auto *serverGroup = new QActionGroup(servers);
    for (const auto &row : m_snapshot["servers"].toArray()) {
        const auto item = row.toObject();
        const QString id = item["serverId"].toString();
        auto *action = servers->addAction(item["name"].toString(), this,
            [this, id] { request({{"command", "server"}, {"id", id}}); });
        action->setCheckable(true);
        action->setChecked(id == m_snapshot["serverId"].toString());
        serverGroup->addAction(action);
    }
    auto *protocols = m_menu.addMenu("Protocol");
    protocols->setEnabled(m_available && !busy);
    if (m_snapshot["isApiServer"].toBool()) {
        protocols->addAction("Choose in AmneziaVPN…", this, [this] { page("home"); });
    } else {
        auto *group = new QActionGroup(protocols);
        int selectedProtocol = -1;
        for (const auto &row : m_snapshot["servers"].toArray()) {
            const auto server = row.toObject();
            if (server["serverId"] == m_snapshot["serverId"]) selectedProtocol = server["defaultContainer"].toInt(-1);
        }
        for (const auto &row : m_snapshot["protocols"].toArray()) {
            const auto item = row.toObject();
            if (!item["isInstalled"].toBool() || !item["isVpnContainer"].toBool()) continue;
            const int id = item["dockerContainer"].toInt();
            const auto name = item["name"].toString();
            auto *action = protocols->addAction(name, this, [this, id] { request({{"command", "protocol"}, {"id", id}}); });
            action->setCheckable(true);
            action->setChecked(id == selectedProtocol);
            action->setEnabled(item["isSupported"].toBool());
            group->addAction(action);
        }
        protocols->addSeparator();
        protocols->addAction("Manage protocols…", this, [this] { page("protocols"); });
    }
    m_menu.addSeparator();
    auto *importMenu = m_menu.addMenu("Import");
    importMenu->addAction("Paste link or configuration…", this, &Tray::importText);
    importMenu->addAction("Configuration file…", this, [this] { importFile(false); });
    importMenu->addAction("QR image…", this, [this] { importFile(true); });
    auto *share = m_menu.addAction("Share / QR / export…", this, [this] { page("share"); });
    share->setEnabled(m_available);
    auto *add = m_menu.addAction("Add self-hosted server / SSH setup…", this, [this] { page("selfhost"); });
    add->setEnabled(m_available);
    m_menu.addAction("Settings…", this, &Tray::settings);
    m_menu.addSeparator();
    m_menu.addAction("Quit tray (keep VPN running)", qApp, &QApplication::quit);
}

void Tray::launch() {
    if (m_client.isEmpty() || !QFileInfo(m_client).isExecutable()) {
        QMessageBox::warning(nullptr, "Amnezia Tray", "Select a bridge-enabled AmneziaVPN executable in Settings.");
        return;
    }
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("AMNEZIA_TRAY_BRIDGE", "1");
    process.setProcessEnvironment(environment);
    process.setProgram(m_client);
    process.setArguments({});
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    if (!process.startDetached()) notifyError("Could not launch AmneziaVPN.");
}

void Tray::page(const QString &name) {
    if (!m_available) { launch(); return; }
    request({{"command", "page"}, {"page", name}});
}

bool Tray::OpenUrl(const QString &url) {
    if (calledFromDBus()) {
        auto *interface = connection().interface();
        if (!interface) return false;
        const auto uid = interface->serviceUid(message().service());
        if (!uid.isValid() || uid.value() != getuid()) return false;
    }
    if (url.isEmpty()) { page("home"); return true; }
    if (!url.startsWith("vpn://") || url.size() > 512 * 1024) return false;
    submitImport(url);
    return true;
}

void Tray::submitImport(const QString &text, bool qr) {
    if (text.isEmpty() || text.toUtf8().size() > 700 * 1024) {
        notifyError("Configuration is empty or too large (maximum 700 KiB)."); return;
    }
    // One preview at a time: a later URL must not overwrite a pending import.
    if (m_importing || !m_imports.isEmpty()) {
        notifyError("An import is already pending. Finish its preview before opening another configuration."); return;
    }
    m_imports.enqueue({{"command", "import"}, {"data", text}, {"qr", qr}});
    if (!m_available) launch();
    refresh();
}

void Tray::importText() {
    bool ok = false;
    const auto text = QInputDialog::getMultiLineText(nullptr, "Import into AmneziaVPN",
        "Paste a vpn:// link or configuration. AmneziaVPN will show a preview before import.", {}, &ok);
    if (ok) submitImport(text.trimmed());
}

void Tray::importFile(bool qr) {
    const auto path = QFileDialog::getOpenFileName(nullptr, qr ? "Import QR image" : "Import configuration", {},
        qr ? "Images (*.png *.jpg *.jpeg *.webp *.bmp)" : "VPN configurations (*.vpn *.conf *.ovpn *.json);;All files (*)");
    if (path.isEmpty()) return;
    if (!qr) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 700 * 1024) { notifyError("Cannot read configuration (maximum 700 KiB)."); return; }
        submitImport(QString::fromUtf8(file.readAll()));
        return;
    }
    const auto zbar = QStandardPaths::findExecutable("zbarimg");
    if (zbar.isEmpty()) { notifyError("Install zbar to import QR images."); return; }
    auto *process = new QProcess(this);
    auto *timeout = new QTimer(process);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, process, &QProcess::kill);
    connect(process, &QProcess::finished, this, [this, process, timeout](int code, QProcess::ExitStatus status) {
        timeout->stop();
        auto data = process->readAllStandardOutput();
        process->deleteLater();
        if (code != 0 || status != QProcess::NormalExit || data.isEmpty() || data.size() > 512 * 1024) {
            notifyError("Could not decode one QR code from this image."); return;
        }
        if (data.endsWith('\n')) data.chop(1);
        submitImport(QString::fromLatin1(data.toBase64()), true);
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) { notifyError("Could not start the QR decoder."); process->deleteLater(); }
    });
    process->setProgram(zbar);
    process->setArguments({"--quiet", "--nodbus", "--raw", "--set", "disable", "--set", "qrcode.enable", "--", path});
    process->setStandardErrorFile(QProcess::nullDevice());
    process->start();
    timeout->start(10000);
}

void Tray::settings() {
    QDialog dialog;
    dialog.setWindowTitle("Amnezia Tray settings");
    dialog.resize(500, 0);
    auto *layout = new QVBoxLayout(&dialog);
    auto *description = new QLabel("VPN profiles, DNS, kill switch, sharing and SSH setup are managed by AmneziaVPN.\nThe tray uses its existing profiles and VPN engine.");
    description->setWordWrap(true);
    layout->addWidget(description);
    layout->addWidget(new QLabel("Bridge-enabled AmneziaVPN executable:"));
    auto *executable = new QLineEdit(m_client);
    layout->addWidget(executable);
    auto *browse = new QPushButton("Choose executable…");
    layout->addWidget(browse);
    layout->addWidget(new QLabel("Options apply immediately. Save updates the executable path."));
    connect(browse, &QPushButton::clicked, &dialog, [executable, &dialog] {
        const auto path = QFileDialog::getOpenFileName(&dialog, "Select AmneziaVPN");
        if (!path.isEmpty()) executable->setText(path);
    });
    const QString autostart = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
        + "/autostart/amnezia-tray-omarchy.desktop";
    auto *startTray = new QCheckBox("Start tray at login");
    startTray->setChecked(QFileInfo::exists(autostart));
    layout->addWidget(startTray);
    connect(startTray, &QCheckBox::toggled, &dialog, [this, autostart](bool enabled) {
        if (!enabled) {
            if (QFile::exists(autostart) && !QFile::remove(autostart)) notifyError("Could not disable tray autostart.");
            return;
        }
        if (!QDir().mkpath(QFileInfo(autostart).absolutePath())) { notifyError("Could not create autostart directory."); return; }
        QString executable = QCoreApplication::applicationFilePath();
        executable.replace('\\', "\\\\").replace('"', "\\\"").replace('`', "\\`").replace('$', "\\$").replace('%', "%%");
        QSaveFile file(autostart);
        const auto content = QString("[Desktop Entry]\nType=Application\nName=Amnezia Tray\nExec=\"%1\" --start-client\nIcon=amnezia-tray-omarchy\nTerminal=false\n").arg(executable).toUtf8();
        if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size() || !file.commit())
            notifyError("Could not save tray autostart.");
    });
    for (const auto &entry : QList<QPair<QString, QString>>{{"autoConnect", "Connect automatically (native setting)"},
        {"autoStartEnabled", "Start AmneziaVPN at login (native setting)"},
        {"startMinimized", "Start AmneziaVPN minimized (native setting)"},
        {"resumeAfterSleep", "Reconnect after suspend when VPN was active"}}) {
        auto *check = new QCheckBox(entry.second);
        check->setChecked(m_snapshot[entry.first].toBool());
        check->setEnabled(m_available);
        layout->addWidget(check);
        connect(check, &QCheckBox::toggled, &dialog, [this, key = entry.first](bool value) {
            request({{"command", "setting"}, {"key", key}, {"value", value}});
        });
    }
    for (const auto &entry : QList<QPair<QString, QString>>{{"settings", "All AmneziaVPN settings…"},
        {"dns", "DNS…"}, {"killSwitch", "Kill switch…"}, {"split", "IP split tunneling…"}, {"appSplit", "Application split tunneling…"}}) {
        auto *button = new QPushButton(entry.second);
        button->setEnabled(m_available);
        layout->addWidget(button);
        connect(button, &QPushButton::clicked, &dialog, [this, name = entry.first] { page(name); });
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Close);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const auto path = executable->text().trimmed();
        if (!QFileInfo(path).isExecutable() || !QFileInfo(path).isFile()) {
            QMessageBox::warning(&dialog, "Amnezia Tray", "Choose an executable file."); return;
        }
        m_client = QFileInfo(path).absoluteFilePath();
        QSettings().setValue("clientExecutable", m_client);
        dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}
