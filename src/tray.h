// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QDBusContext>
#include <QJsonObject>
#include <QMenu>
#include <QQueue>
#include <QSettings>
#include <QSystemTrayIcon>
#include <QTimer>
#include <functional>

class Tray final : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kapustaltd.AmneziaTrayShell1")
public:
    explicit Tray(QString client, QObject *parent = nullptr);
    bool start();
    void startClientIfNeeded();
    void submitImport(const QString &text, bool qr = false);
public slots:
    bool OpenUrl(const QString &url);
private:
    void request(QJsonObject request, std::function<void(QJsonObject)> done = {});
    void refresh();
    void rebuild();
    void launch();
    void settings();
    void importText();
    void importFile(bool qr);
    void page(const QString &name);
    void notifyError(const QString &message);
    QString m_client;
    QSystemTrayIcon m_icon;
    QMenu m_menu;
    QTimer m_timer;
    QJsonObject m_snapshot;
    bool m_available = false, m_polling = false, m_importing = false;
    QQueue<QJsonObject> m_imports;
};
