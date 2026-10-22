// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QAbstractItemModel>
#include <QDBusContext>
#include <QDBusConnection>
#include <QJsonObject>
#include <QQmlContext>
#include <QSettings>
#include <functional>

// Embedded in the original GUI process, never in its privileged VPN service.
class TrayBridge final : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kapustaltd.AmneziaTray1")
public:
    using Navigator = std::function<bool(const QString &)>;
    explicit TrayBridge(QQmlContext *context, Navigator navigate, QObject *parent = nullptr);
    ~TrayBridge() override;
    bool start();
    void setState(const QString &state);
    void addTraffic(quint64 received, quint64 sent);
    void userDisconnected();

public slots:
    Q_SCRIPTABLE QString Request(const QString &json);
    void prepareForSleep(bool sleeping);

private slots:
    void importFinished() { m_previewActive = false; }
    void cancelResume() { userDisconnected(); }
    void syncConnectionState();
    void nativeConnectionError() { setState("Error"); }

private:
    QJsonObject execute(const QJsonObject &request);
    QJsonObject snapshot() const;
    QObject *object(const char *name) const;
    QJsonArray rows(const char *name, const QStringList &fields) const;
    bool busy() const;
    bool sameUser() const;
    QQmlContext *m_context;
    Navigator m_navigate;
    QSettings m_settings{"kapustaltd", "amnezia-tray-omarchy"};
    QString m_state = "Disconnected";
    quint64 m_received = 0, m_sent = 0;
    bool m_haveTraffic = false, m_resumePending = false;
    quint64 m_resumeGeneration = 0;
    bool m_previewActive = false;
    bool m_started = false;
};
