// SPDX-License-Identifier: GPL-3.0-only
#include "../bridge/traybridge.h"
#include "../src/tray.h"
#include <QApplication>
#include <QDir>
#include <QProcess>
#include <QCoreApplication>
#include <QDBusInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QTest>

class Connection : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool isConnected MEMBER connected)
    Q_PROPERTY(bool isConnectionInProgress MEMBER progress)
public:
    bool connected = false, progress = false;
    int opens = 0, closes = 0;
public slots:
    void openConnection() { ++opens; }
    void toggleConnection() { ++opens; }
    void closeConnection() { ++closes; }
signals:
    void connectionStateChanged();
};

class Servers : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString defaultServerId MEMBER id)
    Q_PROPERTY(QString defaultServerName MEMBER name)
    Q_PROPERTY(QString defaultServerDefaultContainerName MEMBER protocol)
    Q_PROPERTY(bool isDefaultServerFromApi MEMBER api)
public:
    QString id = "test-server", name = "Test server", protocol = "AmneziaWG";
    bool api = false;
    int selectedProtocol = -1;
public slots:
    void setDefaultServer(const QString &value) { id = value; }
    void setDefaultContainer(const QString &, int value) { selectedProtocol = value; }
};

class Settings : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString primaryDns MEMBER dns)
    Q_PROPERTY(QString secondaryDns MEMBER dns)
    Q_PROPERTY(bool isKillSwitchEnabled MEMBER enabled)
    Q_PROPERTY(bool autoStartEnabled MEMBER enabled)
    Q_PROPERTY(bool startMinimized MEMBER enabled)
public:
    QString dns = "9.9.9.9";
    bool enabled = false, autoConnect = false;
public slots:
    bool isAutoConnectEnabled() { return autoConnect; }
    void toggleAutoConnect(bool value) { autoConnect = value; }
    void toggleAutoStart(bool value) { enabled = value; }
    void toggleStartMinimized(bool value) { enabled = value; }
};

class Importer : public QObject {
    Q_OBJECT
public:
    QString last;
    QByteArray qr;
    int commits = 0;
public slots:
    bool extractConfigFromData(QString value) { last = value; return value == "vpn://valid"; }
    bool extractOmarchyQr(const QByteArray &value) { qr = value; return value == "binary-qr"; }
    void importConfig() { ++commits; }
signals:
    void importFinished();
};

class Pages : public QObject {
    Q_OBJECT
signals:
    void closePage();
};

class Model : public QAbstractListModel {
    Q_OBJECT
public:
    QList<QVariantMap> items;
    QHash<int, QByteArray> roles;
    int rowCount(const QModelIndex & = {}) const override { return items.size(); }
    QHash<int, QByteArray> roleNames() const override { return roles; }
    QVariant data(const QModelIndex &index, int role) const override {
        return items.value(index.row()).value(QString::fromUtf8(roles.value(role)));
    }
};

class BridgeTests : public QObject {
    Q_OBJECT
    QQmlEngine *engine = nullptr;
    TrayBridge *bridge = nullptr;
    Connection connection;
    Servers servers;
    Settings settings;
    Importer importer;
    Pages pages;
    Model serverModel, protocolModel;
    QString navigated;
    QJsonObject request(QJsonObject value) {
        return QJsonDocument::fromJson(bridge->Request(QString::fromUtf8(
            QJsonDocument(value).toJson(QJsonDocument::Compact))).toUtf8()).object();
    }
private slots:
    void init() {
        connection.connected = connection.progress = false;
        connection.opens = connection.closes = 0;
        servers.id = "test-server"; servers.api = false; servers.selectedProtocol = -1;
        settings.autoConnect = false;
        importer.commits = 0;
        navigated.clear();
        serverModel.roles = {{257, "serverId"}, {258, "name"}, {259, "privateKey"}, {260, "hostName"}};
        serverModel.items = {{{"serverId", "test-server"}, {"name", "Test"}, {"privateKey", "secret"}, {"hostName", "192.0.2.1"}}};
        // Container IDs deliberately differ from row numbers.
        protocolModel.roles = {{257, "dockerContainer"}, {258, "name"}, {259, "isInstalled"}, {260, "isSupported"}, {261, "isVpnContainer"}};
        protocolModel.items = {{{"dockerContainer", 42}, {"name", "Test protocol"}, {"isInstalled", true}, {"isSupported", true}, {"isVpnContainer", true}},
            {{"dockerContainer", 43}, {"name", "Unavailable"}, {"isInstalled", true}, {"isSupported", false}, {"isVpnContainer", true}}};
        engine = new QQmlEngine;
        auto *context = engine->rootContext();
        context->setContextProperty("ConnectionController", &connection);
        context->setContextProperty("ServersUiController", &servers);
        context->setContextProperty("SettingsController", &settings);
        context->setContextProperty("ImportController", &importer);
        context->setContextProperty("PageController", &pages);
        context->setContextProperty("ServersModel", &serverModel);
        context->setContextProperty("DefaultServerContainersModel", &protocolModel);
        bridge = new TrayBridge(context, [this](const QString &page) {
            if (!QStringList{"home", "importPreview", "settings"}.contains(page)) return false;
            navigated = page; return true;
        });
    }
    void cleanup() { delete bridge; delete engine; }
    void malformedAndBoundedRequests() {
        for (const auto &value : QStringList{"[1]", "{", QString(1024 * 1024 + 1, 'a')})
            QVERIFY(!QJsonDocument::fromJson(bridge->Request(value).toUtf8()).object()["ok"].toBool());
        QVERIFY(!request({{"command", "shell"}, {"data", "touch /tmp/unwanted"}})["ok"].toBool());
    }
    void stateAndPrivacy() {
        bridge->setState("Error");
        auto result = request({{"command", "snapshot"}});
        QCOMPARE(result["state"].toString(), "Error");
        QVERIFY(!QJsonDocument(result).toJson().contains("secret"));
        QVERIFY(!result["hasTraffic"].toBool());
        bridge->addTraffic(20, 30); bridge->addTraffic(2, 3);
        result = request({{"command", "snapshot"}});
        QCOMPARE(result["received"].toString(), "22");
        QCOMPARE(result["sent"].toString(), "33");
        bridge->setState("Preparing");
        QVERIFY(!request({{"command", "snapshot"}})["hasTraffic"].toBool());
    }
    void connectionAndSelection() {
        QVERIFY(request({{"command", "connect"}})["ok"].toBool());
        QCOMPARE(connection.opens, 1);
        connection.progress = true;
        QVERIFY(!request({{"command", "server"}, {"id", "test-server"}})["ok"].toBool());
        QVERIFY(!request({{"command", "connect"}})["ok"].toBool());
        QVERIFY(request({{"command", "disconnect"}})["ok"].toBool());
        QCOMPARE(connection.closes, 1);
        connection.progress = false;
        QVERIFY(!request({{"command", "server"}, {"id", "unknown"}})["ok"].toBool());
        QVERIFY(request({{"command", "server"}, {"id", "test-server"}})["ok"].toBool());
        QVERIFY(!request({{"command", "protocol"}, {"id", 0}})["ok"].toBool());
        QVERIFY(!request({{"command", "protocol"}, {"id", 43}})["ok"].toBool());
        QVERIFY(request({{"command", "protocol"}, {"id", 42}})["ok"].toBool());
        QCOMPARE(servers.selectedProtocol, 42);
    }
    void importsRequireNativeConfirmation() {
        QVERIFY(!request({{"command", "import"}, {"data", "vpn://invalid"}})["ok"].toBool());
        QVERIFY(request({{"command", "import"}, {"data", "vpn://valid"}})["ok"].toBool());
        QCOMPARE(navigated, "importPreview");
        QCOMPARE(importer.commits, 0);
        QVERIFY(!request({{"command", "import"}, {"data", "vpn://valid"}})["ok"].toBool());
        emit pages.closePage();
        QVERIFY(request({{"command", "import"}, {"data", QString::fromLatin1(QByteArray("binary-qr").toBase64())}, {"qr", true}})["ok"].toBool());
        QCOMPARE(importer.qr, QByteArray("binary-qr"));
    }
    void settingsAreRestricted() {
        QVERIFY(!request({{"command", "setting"}, {"key", "autoConnect"}, {"value", "true"}})["ok"].toBool());
        QVERIFY(!request({{"command", "setting"}, {"key", "isKillSwitchEnabled"}, {"value", true}})["ok"].toBool());
        QVERIFY(request({{"command", "setting"}, {"key", "autoConnect"}, {"value", true}})["ok"].toBool());
        QVERIFY(settings.autoConnect);
        QVERIFY(!request({{"command", "page"}, {"page", "unknown"}})["ok"].toBool());
    }
    void resumeHonorsManualDisconnect() {
        QVERIFY(request({{"command", "setting"}, {"key", "resumeAfterSleep"}, {"value", true}})["ok"].toBool());
        connection.connected = true;
        bridge->prepareForSleep(true);
        connection.connected = false;
        bridge->prepareForSleep(false);
        bridge->userDisconnected();
        QTest::qWait(5400);
        QCOMPARE(connection.opens, 0);
        connection.connected = true;
        bridge->prepareForSleep(true);
        connection.connected = false;
        bridge->prepareForSleep(false);
        QTRY_COMPARE_WITH_TIMEOUT(connection.opens, 1, 6000);
    }
    void sessionBusRoundTripAndExportSurface() {
        QVERIFY(bridge->start());
        auto bus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "test-client");
        auto message = QDBusMessage::createMethodCall("org.kapustaltd.AmneziaTray1", "/org/kapustaltd/AmneziaTray1",
            "org.kapustaltd.AmneziaTray1", "Request");
        message << "{\"command\":\"snapshot\"}";
        QDBusPendingCallWatcher watcher(bus.asyncCall(message));
        QSignalSpy spy(&watcher, &QDBusPendingCallWatcher::finished);
        QVERIFY(spy.wait(3000));
        QDBusPendingReply<QString> reply = watcher;
        QVERIFY2(reply.isValid(), qPrintable(reply.error().message()));
        QVERIFY(QJsonDocument::fromJson(reply.value().toUtf8()).object()["ok"].toBool());
        auto introspect = QDBusMessage::createMethodCall("org.kapustaltd.AmneziaTray1", "/org/kapustaltd/AmneziaTray1",
            "org.freedesktop.DBus.Introspectable", "Introspect");
        QDBusPendingCallWatcher inspector(bus.asyncCall(introspect));
        QSignalSpy inspected(&inspector, &QDBusPendingCallWatcher::finished);
        QVERIFY(inspected.wait(3000));
        QDBusPendingReply<QString> xml = inspector;
        QVERIFY(xml.isValid());
        QVERIFY(xml.value().contains("method name=\"Request\""));
        QVERIFY(!xml.value().contains("method name=\"prepareForSleep\""));
        QVERIFY(!xml.value().contains("method name=\"cancelResume\""));
        QDBusConnection::disconnectFromBus("test-client");
    }
    void trayUsesNativeStateAndForwardsSecondInstance() {
        QVERIFY(bridge->start());
        auto bus = QDBusConnection::sessionBus();
        QVERIFY(bus.registerService("org.kapustaltd.AmneziaTrayShell1"));
        {
            Tray tray("/nonexistent/AmneziaVPN");
            QVERIFY(tray.start());
            QMenu *menu = nullptr;
            for (auto *widget : QApplication::topLevelWidgets())
                if (widget->objectName() == "amneziaTrayMenu") menu = qobject_cast<QMenu *>(widget);
            QVERIFY(menu);
            auto action = [menu](const QString &text) -> QAction * {
                for (auto *item : menu->actions()) if (item->text() == text) return item;
                return nullptr;
            };
            QVERIFY(action("Connect"));
            QVERIFY(!action("Connect")->isEnabled());
            QTRY_VERIFY_WITH_TIMEOUT(action("Connect")->isEnabled(), 3000);
            menu->resize(menu->sizeHint());
            QVERIFY(menu->grab().save("tray-menu.png"));
            action("Connect")->trigger();
            QTRY_COMPARE_WITH_TIMEOUT(connection.opens, 1, 3000);
            connection.connected = true;
            bridge->setState("Connected");
            QTRY_VERIFY_WITH_TIMEOUT(action("Disconnect")->isEnabled(), 4000);
            QVERIFY(!action("Connect")->isEnabled());
            action("Disconnect")->trigger();
            QTRY_COMPARE_WITH_TIMEOUT(connection.closes, 1, 3000);
            connection.connected = false;
            const auto previous = importer.last;
            importer.last.clear();
            QProcess second;
            second.setProgram(QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../tray/amnezia-tray-omarchy"));
            second.setArguments({"vpn://valid"});
            second.start();
            QTRY_COMPARE_WITH_TIMEOUT(second.state(), QProcess::NotRunning, 5000);
            QCOMPARE(second.exitCode(), 0);
            QTRY_COMPARE_WITH_TIMEOUT(importer.last, QString("vpn://valid"), 4000);
            QCOMPARE(importer.commits, 0);
            importer.last = previous;
        }
        bus.unregisterService("org.kapustaltd.AmneziaTrayShell1");
    }
};

QTEST_MAIN(BridgeTests)
#include "bridge_tests.moc"
