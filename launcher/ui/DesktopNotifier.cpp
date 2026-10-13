// SPDX-License-Identifier: GPL-3.0-only
#include "DesktopNotifier.h"

#include <QApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QPointer>

#include "Application.h"
#include "BuildConfig.h"
#include "settings/SettingsObject.h"

DesktopNotifier* DesktopNotifier::instance()
{
    static QPointer<DesktopNotifier> notifier;
    if (!notifier)
        notifier = new DesktopNotifier(qApp);
    return notifier;
}

DesktopNotifier::DesktopNotifier(QObject* parent) : QObject(parent), m_tray(new QSystemTrayIcon(this))
{
    m_tray->setIcon(APPLICATION->logo());
    m_tray->setToolTip(BuildConfig.LAUNCHER_DISPLAYNAME);
    connect(m_tray, &QSystemTrayIcon::messageClicked, this, &DesktopNotifier::clicked);
    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
            clicked();
    });
    // once the launcher is in front again, the notification has done its job
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive)
            dismiss();
    });
}

void DesktopNotifier::notify(const QString& title, const QString& message, std::function<void()> onClick)
{
    instance()->show(title, message, std::move(onClick));
}

void DesktopNotifier::taskFinished(const QString& title, const QString& message, std::function<void()> onClick)
{
    if (APPLICATION->settings()->get("NotifyOnDownloads").toBool() && launcherInBackground())
        notify(title, message, std::move(onClick));
}

bool DesktopNotifier::launcherInBackground()
{
    return QApplication::activeWindow() == nullptr;
}

void DesktopNotifier::show(const QString& title, const QString& message, std::function<void()> onClick)
{
    if (!QSystemTrayIcon::isSystemTrayAvailable() || !QSystemTrayIcon::supportsMessages()) {
        qDebug() << "Desktop notifications are not available here; skipped:" << title;
        return;
    }
    qDebug() << "Desktop notification:" << title << "-" << message;
    // the action belongs to the newest notification
    m_onClick = std::move(onClick);
    m_tray->show();
    m_tray->showMessage(title, message, APPLICATION->logo(), 10000);
}

void DesktopNotifier::clicked()
{
    auto onClick = std::move(m_onClick);
    dismiss();
    if (onClick)
        onClick();
    else
        APPLICATION->showMainWindow();
}

void DesktopNotifier::dismiss()
{
    m_onClick = nullptr;
    m_tray->hide();
}
