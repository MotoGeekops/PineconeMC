// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QObject>
#include <QSystemTrayIcon>

#include <functional>

/**
 * Desktop notifications. They go through a tray icon that only appears while there is something to tell,
 * and goes away once the launcher is used again.
 */
class DesktopNotifier : public QObject {
    Q_OBJECT
   public:
    /// @p onClick runs when the notification or the tray icon is clicked.
    static void notify(const QString& title, const QString& message, std::function<void()> onClick = {});

    /// For downloads and updates that finished: notifies only while the launcher is in the background, if the setting allows it.
    static void taskFinished(const QString& title, const QString& message, std::function<void()> onClick = {});

    /// True while none of the launcher's windows is active, for example while the game is in front.
    static bool launcherInBackground();

   private:
    explicit DesktopNotifier(QObject* parent);
    static DesktopNotifier* instance();

    void show(const QString& title, const QString& message, std::function<void()> onClick);
    void clicked();
    void dismiss();

    QSystemTrayIcon* m_tray;
    std::function<void()> m_onClick;
};
