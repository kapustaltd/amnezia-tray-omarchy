// SPDX-License-Identifier: GPL-3.0-only
#include "traybridge.h"
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QTimer>
#include <unistd.h>

namespace {
QJsonObject error(const QString &message) { return {{"ok", false}, {"error", message}}; }
QJsonObject success() { return {{"ok", true}}; }
bool call(QObject *object, const char *method) {
    return object && QMetaObject::invokeMethod(object, method, Qt::DirectConnection);
}
bool boolean(QObject *object, const char *method) {
    bool result = false;
    if (object) QMetaObject::invokeMethod(object, method, Qt::DirectConnection, Q_RETURN_ARG(bool, result));
    return result;
}
}

TrayBridge::TrayBridge(QQmlContext *context, Navigator navigate, QObject *parent)
    : QObject(parent), m_context(context), m_navigate(std::move(navigate)) {
    if (auto *importer = object("ImportController"))
        connect(importer, SIGNAL(importFinished()), this, SLOT(importFinished()));
    if (auto *page = object("PageController"))
        connect(page, SIGNAL(closePage()), this, SLOT(importFinished()));
    if (auto *connection = object("ConnectionController")) {
        if (connection->property("isConnected").toBool()) m_state = "Connected";
        else if (connection->property("isConnectionInProgress").toBool()) m_state = "Connecting";
        connect(connection, SIGNAL(connectionStateChanged()), this, SLOT(syncConnectionState()));
        if (connection->metaObject()->indexOfSignal("connectionErrorOccurred(ErrorCode)") >= 0)
            connect(connection, SIGNAL(connectionErrorOccurred(ErrorCode)), this, SLOT(nativeConnectionError()));
        syncConnectionState();
    }
}

TrayBridge::~TrayBridge() {
    if (m_started) {
        auto bus = QDBusConnection::sessionBus();
        bus.unregisterObject("/org/kapustaltd/AmneziaTray1");
        bus.unregisterService("org.kapustaltd.AmneziaTray1");
    }
}

bool TrayBridge::start() {
    auto bus = QDBusConnection::sessionBus();
    if (!bus.registerService("org.kapustaltd.AmneziaTray1")) return false;
    if (!bus.registerObject("/org/kapustaltd/AmneziaTray1", this, QDBusConnection::ExportScriptableSlots)) {
        bus.unregisterService("org.kapustaltd.AmneziaTray1");
        return false;
    }
    m_started = true;
    QDBusConnection::systemBus().connect("org.freedesktop.login1", "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager", "PrepareForSleep", this, SLOT(prepareForSleep(bool)));
    return true;
}

bool TrayBridge::sameUser() const {
    if (!calledFromDBus()) return true;
    auto *interface = connection().interface();
    if (!interface) return false;
    const auto uid = interface->serviceUid(message().service());
    return uid.isValid() && uid.value() == getuid();
}

QObject *TrayBridge::object(const char *name) const {
    return m_context ? m_context->contextProperty(QString::fromLatin1(name)).value<QObject *>() : nullptr;
}

QJsonArray TrayBridge::rows(const char *name, const QStringList &fields) const {
    QJsonArray result;
    auto *model = qobject_cast<QAbstractItemModel *>(object(name));
    if (!model) return result;
    const auto roles = model->roleNames();
    for (int row = 0; row < model->rowCount(); ++row) {
        QJsonObject item;
        for (auto it = roles.begin(); it != roles.end(); ++it) {
            const QString key = QString::fromUtf8(it.value());
            if (fields.contains(key)) {
                const auto value = model->data(model->index(row, 0), it.key());
                item[key] = (key == "dockerContainer" || key == "defaultContainer")
                    ? QJsonValue(value.toInt()) : QJsonValue::fromVariant(value);
            }
        }
        result.append(item);
    }
    return result;
}

bool TrayBridge::busy() const {
    auto *controller = object("ConnectionController");
    return controller && (controller->property("isConnected").toBool()
        || controller->property("isConnectionInProgress").toBool());
}

void TrayBridge::setState(const QString &state) {
    if ((state == "Preparing" || state == "Connecting") && m_state != "Preparing"
        && m_state != "Connecting" && m_state != "Reconnecting") {
        m_received = m_sent = 0;
        m_haveTraffic = false;
    }
    m_state = state;
}
void TrayBridge::syncConnectionState() {
    if (auto *connection = object("ConnectionController")) {
        const auto state = connection->property("omarchyConnectionState").toString();
        if (!state.isEmpty()) setState(state);
    }
}
void TrayBridge::addTraffic(quint64 received, quint64 sent) {
    m_received += received;
    m_sent += sent;
    m_haveTraffic = true;
}
void TrayBridge::userDisconnected() { m_resumePending = false; ++m_resumeGeneration; }

void TrayBridge::prepareForSleep(bool sleeping) {
    if (sleeping) {
        m_resumePending = m_settings.value("resumeAfterSleep", true).toBool() && busy();
        ++m_resumeGeneration;
    } else if (m_resumePending) {
        const auto generation = m_resumeGeneration;
        QTimer::singleShot(5000, this, [this, generation] {
            if (!m_resumePending || generation != m_resumeGeneration) return;
            m_resumePending = false;
            if (!busy()) call(object("ConnectionController"), "toggleConnection");
        });
    }
}

QJsonObject TrayBridge::snapshot() const {
    auto *server = object("ServersUiController");
    auto *settings = object("SettingsController");
    QJsonObject result{{"ok", true}, {"api", 1}, {"backendVersion", "5.0.3.0"},
        {"state", m_state}, {"hasTraffic", m_haveTraffic},
        {"received", QString::number(m_received)}, {"sent", QString::number(m_sent)},
        {"resumeAfterSleep", m_settings.value("resumeAfterSleep", true).toBool()}};
    if (server) {
        result["serverId"] = server->property("defaultServerId").toString();
        result["serverName"] = server->property("defaultServerName").toString();
        result["protocolName"] = server->property("defaultServerDefaultContainerName").toString();
        result["isApiServer"] = server->property("isDefaultServerFromApi").toBool();
    }
    result["servers"] = rows("ServersModel", {"name", "hostName", "serverId", "isDefault", "defaultContainer"});
    result["protocols"] = rows("DefaultServerContainersModel",
        {"name", "dockerContainer", "isInstalled", "isSupported", "isVpnContainer"});
    if (settings) {
        for (const char *key : {"primaryDns", "secondaryDns", "isKillSwitchEnabled", "autoStartEnabled", "startMinimized"})
            result[QString::fromLatin1(key)] = QJsonValue::fromVariant(settings->property(key));
        result["autoConnect"] = boolean(settings, "isAutoConnectEnabled");
    }
    return result;
}

QString TrayBridge::Request(const QString &json) {
    QJsonObject result;
    if (!sameUser()) result = error("Access denied");
    else if (json.toUtf8().size() > 1024 * 1024) result = error("Request exceeds 1 MiB");
    else {
        QJsonParseError parse;
        const auto document = QJsonDocument::fromJson(json.toUtf8(), &parse);
        if (parse.error != QJsonParseError::NoError || !document.isObject()) result = error("Invalid request JSON");
        else result = execute(document.object());
    }
    return QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
}

QJsonObject TrayBridge::execute(const QJsonObject &request) {
    auto *connection = object("ConnectionController");
    auto *servers = object("ServersUiController");
    if (!connection || !servers || !object("SettingsController")) return error("Native controllers unavailable");
    const auto command = request["command"].toString();
    if (command == "snapshot") return snapshot();
    if (command == "connect") {
        if (busy()) return error("A connection is already active or changing");
        if (servers->property("defaultServerId").toString().isEmpty()) return error("Select or import a server first");
        return call(connection, "toggleConnection") ? success() : error("Connect unavailable");
    }
    if (command == "disconnect") {
        userDisconnected();
        return call(connection, "closeConnection") ? success() : error("Disconnect unavailable");
    }
    if (command == "server") {
        if (busy()) return error("Disconnect before changing server");
        const QString id = request["id"].toString();
        bool exists = false;
        for (const auto &row : rows("ServersModel", {"serverId"}))
            if (row.toObject()["serverId"].toString() == id && !id.isEmpty()) exists = true;
        if (!exists) return error("Unknown server");
        return QMetaObject::invokeMethod(servers, "setDefaultServer", Qt::DirectConnection, Q_ARG(QString, id))
            ? success() : error("Server selection unavailable");
    }
    if (command == "protocol") {
        if (busy()) return error("Disconnect before changing protocol");
        if (servers->property("isDefaultServerFromApi").toBool()) {
            m_navigate("home");
            return error("Choose the subscription protocol in AmneziaVPN's home window");
        }
        const int id = request["id"].toInt(-1);
        bool valid = false;
        for (const auto &row : rows("DefaultServerContainersModel", {"dockerContainer", "isInstalled", "isSupported", "isVpnContainer"})) {
            const auto item = row.toObject();
            if (item["dockerContainer"].toInt(-2) == id && item["isInstalled"].toBool()
                && item["isSupported"].toBool() && item["isVpnContainer"].toBool()) valid = true;
        }
        if (!valid) return error("Protocol is not installed or supported");
        const QString serverId = servers->property("defaultServerId").toString();
        return QMetaObject::invokeMethod(servers, "setDefaultContainer", Qt::DirectConnection,
            Q_ARG(QString, serverId), Q_ARG(int, id)) ? success() : error("Protocol selection unavailable");
    }
    if (command == "page") {
        return m_navigate(request["page"].toString()) ? success() : error("Unknown page");
    }
    if (command == "import") {
        if (m_previewActive) return error("Finish or cancel the existing native import preview first");
        if (busy()) return error("Disconnect before importing a configuration");
        const auto data = request["data"].toString();
        if (data.isEmpty()) return error("Empty configuration");
        bool valid = false;
        auto *importer = object("ImportController");
        const bool qr = request["qr"].toBool();
        bool invoked;
        if (qr) {
            const auto bytes = QByteArray::fromBase64(data.toLatin1());
            invoked = importer && QMetaObject::invokeMethod(importer, "extractOmarchyQr", Qt::DirectConnection,
                Q_RETURN_ARG(bool, valid), Q_ARG(QByteArray, bytes));
        } else invoked = importer && QMetaObject::invokeMethod(importer, "extractConfigFromData", Qt::DirectConnection,
            Q_RETURN_ARG(bool, valid), Q_ARG(QString, data));
        if (!invoked || !valid) return error("AmneziaVPN rejected the configuration");
        // The original preview performs warnings and asks the user to import.
        m_previewActive = m_navigate("importPreview");
        return m_previewActive ? success() : error("Import preview unavailable");
    }
    if (command == "setting") {
        const auto key = request["key"].toString();
        const auto value = request["value"];
        if (!value.isBool()) return error("Setting requires a boolean");
        if (key == "resumeAfterSleep") {
            m_settings.setValue(key, value.toBool());
            if (!value.toBool()) userDisconnected();
            return success();
        }
        const QHash<QString, QByteArray> methods{{"autoConnect", "toggleAutoConnect"},
            {"autoStartEnabled", "toggleAutoStart"}, {"startMinimized", "toggleStartMinimized"}};
        if (!methods.contains(key)) return error("Use the native settings window for this setting");
        return QMetaObject::invokeMethod(object("SettingsController"), methods.value(key).constData(),
            Qt::DirectConnection, Q_ARG(bool, value.toBool())) ? success() : error("Setting unavailable");
    }
    return error("Unknown command");
}
