// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QDateTime>
#include <QFuture>
#include <QFutureWatcher>
#include <QList>
#include <QString>

#include <atomic>
#include <optional>

#include "archive/ExportToZipTask.h"
#include "tasks/Task.h"

class MinecraftInstance;

/**
 * Backups of an instance, kept in <instance>/backups:
 *  - worlds/<world folder>/<stamp>_<reason>.zip: zipped copies of a world (the zip also works with "Add world")
 *  - restore-points/<stamp>_<reason>/: copies of the instance files (mods, configs, loader version) without the worlds
 */
namespace Backups {

enum class Kind { World, RestorePoint };
enum class Reason { Manual, BeforeUpdate, BeforeModpackUpdate, BeforeRestore };

/// What a restore point covers.
enum class Scope {
    Mods,          // the mods folder, including the mod metadata
    InstanceFiles  // everything except worlds, screenshots, logs and caches
};

struct Entry {
    Kind kind = Kind::World;
    Reason reason = Reason::Manual;
    QString path;         // the zip of a world backup, the folder of a restore point
    QString subject;      // world backups: the world folder name
    Scope scope = Scope::InstanceFiles;
    QString description;  // restore points: what they were made for
    QDateTime created;
    qint64 size = 0;
    int fileCount = 0;

    bool isAutomatic() const { return reason != Reason::Manual; }
};

QString rootDir(const MinecraftInstance* instance);
QList<Entry> list(const MinecraftInstance* instance);
QList<Entry> worldBackups(const MinecraftInstance* instance, const QString& worldFolder);

/// Paths of the worlds played since their newest backup (or never backed up).
QStringList worldsChangedSinceBackup(const MinecraftInstance* instance);

QString reasonText(Reason reason);
QString scopeText(Scope scope);

/// Deletes the oldest automatic backups beyond the "AutoBackupsToKeep" setting, per world and for restore points.
/// Manual backups and @p keepPath are never deleted.
void applyRetention(const MinecraftInstance* instance, const QString& keepPath = {});

bool remove(const Entry& entry);

/// Zips a world folder into the instance's backups.
class WorldBackupTask : public MMCZip::ExportToZipTask {
    Q_OBJECT
   public:
    static shared_qobject_ptr<WorldBackupTask> create(MinecraftInstance* instance, const QString& worldPath, Reason reason);

    QString outputPath() const { return m_outputPath; }

   protected:
    void emitSucceeded() override;

   private:
    WorldBackupTask(MinecraftInstance* instance, QString outputPath, const QString& worldPath, QFileInfoList files, Reason reason);

    MinecraftInstance* m_instance;
    QString m_outputPath;
    Reason m_reason;
};

/// Replaces a world with a backup. The current world is backed up first, so the restore can be undone.
class WorldRestoreTask : public Task {
    Q_OBJECT
   public:
    WorldRestoreTask(MinecraftInstance* instance, Entry backup);

   protected:
    void executeTask() override;
    bool abort() override;

   private:
    void extract();
    void swap();
    void runChild(Task::Ptr task, void (WorldRestoreTask::*next)());

    MinecraftInstance* m_instance;
    Entry m_backup;
    QString m_target;
    QString m_staging;
    Task::Ptr m_child;
};

/// Copies the instance files covered by a scope into a new restore point.
class RestorePointTask : public Task {
    Q_OBJECT
   public:
    RestorePointTask(MinecraftInstance* instance, Scope scope, Reason reason, QString description);
    ~RestorePointTask() override;

    QString outputPath() const { return m_outputPath; }

   protected:
    void executeTask() override;
    bool abort() override;

   private:
    using Result = std::optional<QString>;
    void finish();

    MinecraftInstance* m_instance;
    Scope m_scope;
    Reason m_reason;
    QString m_description;
    QString m_outputPath;
    std::atomic_bool m_cancel{ false };
    QFuture<Result> m_future;
    QFutureWatcher<Result> m_watcher;
};

/// Puts the files of a restore point back. The current files are saved in a new restore point first.
class RestorePointRestoreTask : public Task {
    Q_OBJECT
   public:
    RestorePointRestoreTask(MinecraftInstance* instance, Entry point);
    ~RestorePointRestoreTask() override;

   protected:
    void executeTask() override;

   private:
    using Result = std::optional<QString>;
    void finish();

    MinecraftInstance* m_instance;
    Entry m_point;
    QString m_safetyPath;
    QFuture<Result> m_future;
    QFutureWatcher<Result> m_watcher;
};

}  // namespace Backups
