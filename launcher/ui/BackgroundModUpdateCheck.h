// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QList>
#include <QObject>

#include <vector>

#include "Version.h"
#include "tasks/Task.h"

class CheckUpdateTask;
class MinecraftInstance;
class Resource;

/**
 * Looks for mod updates of one instance in the background, at most once a day per instance,
 * and tells the user with a desktop notification when there are some.
 */
class BackgroundModUpdateCheck : public QObject {
    Q_OBJECT
   public:
    /// Checks @p instance, unless the notification is turned off, the game is running, or it was checked recently.
    static void start(MinecraftInstance* instance);
    /// Checks the instance that was played last.
    static void startForLastPlayed();

   private:
    explicit BackgroundModUpdateCheck(MinecraftInstance* instance);

    void modsLoaded();
    void checked();

    MinecraftInstance* m_instance;
    QList<Resource*> m_modrinth;
    QList<Resource*> m_flame;
    std::vector<Version> m_gameVersions;
    shared_qobject_ptr<CheckUpdateTask> m_modrinthCheck;
    shared_qobject_ptr<CheckUpdateTask> m_flameCheck;
    Task::Ptr m_task;
};
