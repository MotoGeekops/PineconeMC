// SPDX-License-Identifier: GPL-3.0-only
#include "Backups.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <QtConcurrent>

#include <algorithm>
#include <functional>

#include "Application.h"
#include "FileSystem.h"
#include "MMCZip.h"
#include "archive/ExtractZipTask.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "minecraft/World.h"
#include "minecraft/mod/ModFolderModel.h"
#include "settings/INIFile.h"

namespace Backups {

namespace {

const QString s_stampFormat = QStringLiteral("yyyyMMdd-HHmmss");
const QString s_manifestName = QStringLiteral("restore-point.json");

QString tr(const char* text)
{
    return QCoreApplication::translate("Backups", text);
}

struct ReasonId {
    Reason reason;
    const char* id;
};
const ReasonId s_reasonIds[] = {
    { Reason::Manual, "manual" },
    { Reason::BeforeUpdate, "before-update" },
    { Reason::BeforeModpackUpdate, "before-modpack-update" },
    { Reason::BeforeRestore, "before-restore" },
};

QString reasonId(Reason reason)
{
    for (const auto& entry : s_reasonIds) {
        if (entry.reason == reason)
            return entry.id;
    }
    return "manual";
}

std::optional<Reason> reasonFromId(const QString& id)
{
    for (const auto& entry : s_reasonIds) {
        if (id == entry.id)
            return entry.reason;
    }
    return std::nullopt;
}

QString scopeId(Scope scope)
{
    return scope == Scope::Mods ? "mods" : "instance";
}

/// "<stamp>_<reason>", with "-2", "-3"... after the stamp when two backups land in the same second
QString newBackupPath(const QString& dir, Reason reason, const QString& suffix)
{
    const QString stamp = QDateTime::currentDateTime().toString(s_stampFormat);
    QString path = FS::PathCombine(dir, QString("%1_%2%3").arg(stamp, reasonId(reason), suffix));
    for (int i = 2; QFileInfo::exists(path); ++i)
        path = FS::PathCombine(dir, QString("%1-%2_%3%4").arg(stamp, QString::number(i), reasonId(reason), suffix));
    return path;
}

bool parseBackupName(const QString& name, QDateTime& created, Reason& reason)
{
    static const QRegularExpression pattern(QStringLiteral("^(\\d{8}-\\d{6})(?:-\\d+)?_([a-z-]+)$"));
    const auto match = pattern.match(name);
    if (!match.hasMatch())
        return false;
    const auto parsedReason = reasonFromId(match.captured(2));
    created = QDateTime::fromString(match.captured(1), s_stampFormat);
    if (!parsedReason || !created.isValid())
        return false;
    reason = *parsedReason;
    return true;
}

/// The files a restore point covers, as paths relative to the instance folder.
struct ScopeSpec {
    QDir root;
    QStringList includes;  // "" is the instance folder itself
    QStringList excludes;  // excluded together with everything below them
};

ScopeSpec scopeSpec(const MinecraftInstance* instance, Scope scope)
{
    ScopeSpec spec;
    spec.root = QDir(instance->instanceRoot());
    const QString game = spec.root.relativeFilePath(instance->gameRoot());
    if (scope == Scope::Mods) {
        spec.includes << FS::PathCombine(game, "mods");
        return spec;
    }
    spec.includes << QString();
    // the backups themselves, and the instance settings (only the modpack version is restored from those)
    spec.excludes << "backups" << "instance.cfg";
    // worlds have their own backups; the rest is player data or caches that updates don't touch
    for (const char* dir : { "saves", "screenshots", "logs", "crash-reports", "backups", "downloads", ".cache", ".fabric", ".quilt", ".mixin.out",
                             "natives", "server-resource-packs", "journeymap", "xaero", "XaeroWorldMap", "XaeroWaypoints", ".bobby",
                             "replay_recordings", "schematics", "Distant_Horizons_server_data" }) {
        spec.excludes << FS::PathCombine(game, dir);
    }
    return spec;
}

bool isExcluded(const QString& path, const QStringList& excludes)
{
    for (const auto& excluded : excludes) {
        if (path.compare(excluded, Qt::CaseInsensitive) == 0 || path.startsWith(excluded + '/', Qt::CaseInsensitive))
            return true;
    }
    return false;
}

/// Appends the files below @p rel (relative to @p root) to @p out, skipping excluded folders and links to folders.
void collectFiles(const QDir& root, const QString& rel, const QStringList& excludes, QStringList& out)
{
    const QDir dir(rel.isEmpty() ? root.absolutePath() : root.filePath(rel));
    const auto entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
    for (const auto& entry : entries) {
        const QString path = rel.isEmpty() ? entry.fileName() : rel + '/' + entry.fileName();
        if (isExcluded(path, excludes))
            continue;
        if (entry.isDir()) {
            if (!entry.isSymLink() && !entry.isJunction())
                collectFiles(root, path, excludes, out);
        } else {
            out << path;
        }
    }
}

QStringList collectScope(const ScopeSpec& spec)
{
    QStringList files;
    for (const auto& include : spec.includes) {
        const QFileInfo info(include.isEmpty() ? spec.root.absolutePath() : spec.root.filePath(include));
        if (info.isDir())
            collectFiles(spec.root, include, spec.excludes, files);
        else if (info.isFile())
            files << include;
    }
    return files;
}

/// Removes the folders below @p rel that are empty now and are not in @p keep.
void removeEmptyFolders(const QDir& root, const QString& rel, const QStringList& excludes, const QSet<QString>& keep)
{
    const QDir dir(rel.isEmpty() ? root.absolutePath() : root.filePath(rel));
    for (const auto& entry : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
        const QString path = rel.isEmpty() ? entry.fileName() : rel + '/' + entry.fileName();
        if (isExcluded(path, excludes) || entry.isSymLink() || entry.isJunction())
            continue;
        removeEmptyFolders(root, path, excludes, keep);
        if (!keep.contains(path.toLower()) && QDir(entry.absoluteFilePath()).isEmpty())
            root.rmdir(path);
    }
}

bool copyFileOver(const QString& from, const QString& to)
{
    if (QFileInfo::exists(to)) {
        QFile::setPermissions(to, QFile::permissions(to) | QFileDevice::WriteOwner);
        if (!QFile::remove(to))
            return false;
    }
    return FS::ensureFilePathExists(to) && QFile::copy(from, to);
}

/// Reports progress; returns false when the work should stop.
using Progress = std::function<bool(int done, int total, const QString& file)>;

/// Copies the files of @p scope into a new restore point at @p outputDir. Returns an error message on failure.
/// When @p progress asks to stop, the partial copy is removed and no error is returned.
std::optional<QString> createRestorePoint(const MinecraftInstance* instance,
                                          Scope scope,
                                          Reason reason,
                                          const QString& description,
                                          const QString& outputDir,
                                          const Progress& progress)
{
    const auto spec = scopeSpec(instance, scope);
    const auto files = collectScope(spec);
    const QDir out(FS::PathCombine(outputDir, "files"));
    if (!FS::ensureFolderPathExists(out.absolutePath()))
        return tr("Could not create %1").arg(outputDir);

    qint64 bytes = 0;
    for (int i = 0; i < files.size(); ++i) {
        if (!progress(i, files.size(), files[i])) {
            FS::deletePath(outputDir);
            return std::nullopt;
        }
        const QString from = spec.root.filePath(files[i]);
        if (!copyFileOver(from, out.filePath(files[i]))) {
            FS::deletePath(outputDir);
            return tr("Could not copy %1").arg(files[i]);
        }
        bytes += QFileInfo(from).size();
    }

    // only the modpack version is restored from these, see restoreManagedPack()
    if (scope == Scope::InstanceFiles)
        QFile::copy(spec.root.filePath("instance.cfg"), FS::PathCombine(outputDir, "instance.cfg"));

    const QJsonObject manifest{
        { "formatVersion", 1 },          { "scope", scopeId(scope) }, { "reason", reasonId(reason) },
        { "description", description }, { "files", files.size() },   { "bytes", bytes },
    };
    QSaveFile file(FS::PathCombine(outputDir, s_manifestName));
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(manifest).toJson()) < 0 || !file.commit()) {
        FS::deletePath(outputDir);
        return tr("Could not write %1").arg(file.fileName());
    }
    return std::nullopt;
}

/// Puts the files of @p point back: files added since are removed, saved files are copied over the current ones.
std::optional<QString> restoreFiles(const MinecraftInstance* instance, const Entry& point, const Progress& progress)
{
    const auto spec = scopeSpec(instance, point.scope);
    const QDir saved(FS::PathCombine(point.path, "files"));
    QStringList savedFiles;
    collectFiles(saved, QString(), {}, savedFiles);

    // compared without case: Windows and macOS don't tell "Mod.jar" and "mod.jar" apart
    QSet<QString> savedSet;
    QSet<QString> savedFolders;
    for (const auto& file : savedFiles) {
        savedSet.insert(file.toLower());
        for (QString folder = file.section('/', 0, -2); !folder.isEmpty(); folder = folder.section('/', 0, -2)) {
            savedFolders.insert(folder.toLower());
            if (!folder.contains('/'))
                break;
        }
    }

    for (const auto& file : collectScope(spec)) {
        if (savedSet.contains(file.toLower()))
            continue;
        const QString path = spec.root.filePath(file);
        QFile::setPermissions(path, QFile::permissions(path) | QFileDevice::WriteOwner);
        if (!QFile::remove(path))
            return tr("Could not remove %1. Close the game and try again.").arg(file);
    }

    for (int i = 0; i < savedFiles.size(); ++i) {
        progress(i, savedFiles.size(), savedFiles[i]);
        if (!copyFileOver(saved.filePath(savedFiles[i]), spec.root.filePath(savedFiles[i])))
            return tr("Could not restore %1. Close the game and try again.").arg(savedFiles[i]);
    }

    for (const auto& include : spec.includes)
        removeEmptyFolders(spec.root, include, spec.excludes, savedFolders);
    return std::nullopt;
}

/// Only the modpack version comes back from the saved settings: play time, notes and the rest stay as they are.
void restoreManagedPack(MinecraftInstance* instance, const QString& cfgPath)
{
    INIFile saved;
    if (!QFileInfo::exists(cfgPath) || !saved.loadFile(cfgPath))
        return;
    for (const char* key : { "ManagedPack", "ManagedPackType", "ManagedPackID", "ManagedPackName", "ManagedPackVersionID",
                             "ManagedPackVersionName", "ManagedPackURL" }) {
        if (saved.contains(key))
            instance->settings()->set(key, saved.get(key, {}));
    }
}

}  // namespace

QString rootDir(const MinecraftInstance* instance)
{
    return FS::PathCombine(instance->instanceRoot(), "backups");
}

QList<Entry> worldBackups(const MinecraftInstance* instance, const QString& worldFolder)
{
    QList<Entry> entries;
    const QDir dir(FS::PathCombine(rootDir(instance), "worlds", worldFolder));
    for (const auto& zip : dir.entryInfoList({ "*.zip" }, QDir::Files)) {
        Entry entry;
        entry.kind = Kind::World;
        entry.path = zip.absoluteFilePath();
        entry.subject = worldFolder;
        entry.size = zip.size();
        if (parseBackupName(zip.completeBaseName(), entry.created, entry.reason))
            entries << entry;
    }
    return entries;
}

QList<Entry> list(const MinecraftInstance* instance)
{
    QList<Entry> entries;
    const QDir root(rootDir(instance));

    for (const auto& world : QDir(root.filePath("worlds")).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
        entries << worldBackups(instance, world.fileName());

    for (const auto& folder : QDir(root.filePath("restore-points")).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        Entry entry;
        entry.kind = Kind::RestorePoint;
        entry.path = folder.absoluteFilePath();
        if (!parseBackupName(folder.fileName(), entry.created, entry.reason))
            continue;
        QFile file(FS::PathCombine(entry.path, s_manifestName));
        if (!file.open(QIODevice::ReadOnly))
            continue;  // still being written, or broken
        const auto manifest = QJsonDocument::fromJson(file.readAll()).object();
        entry.scope = manifest["scope"].toString() == "mods" ? Scope::Mods : Scope::InstanceFiles;
        entry.description = manifest["description"].toString();
        entry.size = manifest["bytes"].toInteger();
        entry.fileCount = manifest["files"].toInt();
        entries << entry;
    }
    return entries;
}

QStringList worldsChangedSinceBackup(const MinecraftInstance* instance)
{
    QStringList worlds;
    for (const auto& folder : QDir(instance->worldDir()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const World world(folder);
        if (!world.isValid())
            continue;
        QDateTime newest;
        for (const auto& backup : worldBackups(instance, folder.fileName())) {
            if (!newest.isValid() || backup.created > newest)
                newest = backup.created;
        }
        if (!newest.isValid() || world.lastPlayed() > newest)
            worlds << folder.absoluteFilePath();
    }
    return worlds;
}

QString reasonText(Reason reason)
{
    switch (reason) {
        case Reason::Manual:
            return tr("Manual");
        case Reason::BeforeUpdate:
            return tr("Before mod update");
        case Reason::BeforeModpackUpdate:
            return tr("Before modpack update");
        case Reason::BeforeRestore:
            return tr("Before restore");
    }
    return {};
}

QString scopeText(Scope scope)
{
    return scope == Scope::Mods ? tr("Mods") : tr("Mods, configs and loader");
}

void applyRetention(const MinecraftInstance* instance, const QString& keepPath)
{
    const int keep = APPLICATION->settings()->get("AutoBackupsToKeep").toInt();
    if (keep <= 0)
        return;

    // automatic world backups count per world, automatic restore points per instance
    QMap<QString, QList<Entry>> groups;
    for (const auto& entry : list(instance)) {
        if (entry.isAutomatic())
            groups[entry.kind == Kind::World ? "world:" + entry.subject : QString()].append(entry);
    }
    for (auto& group : groups) {
        std::sort(group.begin(), group.end(), [](const Entry& a, const Entry& b) { return a.created > b.created; });
        for (int i = keep; i < group.size(); ++i) {
            if (group[i].path != keepPath)
                remove(group[i]);
        }
    }
}

bool remove(const Entry& entry)
{
    if (entry.kind == Kind::World) {
        const bool removed = QFile::remove(entry.path);
        // drop the world's folder with its last backup (rmdir only removes empty folders)
        QDir().rmdir(QFileInfo(entry.path).absolutePath());
        return removed;
    }
    return FS::deletePath(entry.path);
}

// ---- WorldBackupTask ----

shared_qobject_ptr<WorldBackupTask> WorldBackupTask::create(MinecraftInstance* instance, const QString& worldPath, Reason reason)
{
    // session.lock is held by a running game, and Minecraft recreates it anyway
    QFileInfoList files;
    MMCZip::collectFileListRecursively(worldPath, nullptr, &files,
                                       [](const QFileInfo& file) { return file.fileName() == QLatin1String("session.lock"); });
    // level.dat first: reading a backup's world details can stop right there
    std::stable_partition(files.begin(), files.end(), [&worldPath](const QFileInfo& file) {
        return file.fileName() == QLatin1String("level.dat") && QDir(worldPath) == file.dir();
    });

    const QString dir = FS::PathCombine(rootDir(instance), "worlds", QFileInfo(worldPath).fileName());
    FS::ensureFolderPathExists(dir);
    return shared_qobject_ptr<WorldBackupTask>(new WorldBackupTask(instance, newBackupPath(dir, reason, ".zip"), worldPath, files, reason));
}

WorldBackupTask::WorldBackupTask(MinecraftInstance* instance, QString outputPath, const QString& worldPath, QFileInfoList files, Reason reason)
    // the world folder goes into the zip too, so "Add" on the Worlds page and Minecraft itself accept the backup
    : ExportToZipTask(outputPath, worldPath, std::move(files), QFileInfo(worldPath).fileName() + '/')
    , m_instance(instance)
    , m_outputPath(std::move(outputPath))
    , m_reason(reason)
{}

void WorldBackupTask::emitSucceeded()
{
    if (m_reason != Reason::Manual)
        applyRetention(m_instance, m_outputPath);
    ExportToZipTask::emitSucceeded();
}

// ---- WorldRestoreTask ----

WorldRestoreTask::WorldRestoreTask(MinecraftInstance* instance, Entry backup) : m_instance(instance), m_backup(std::move(backup)) {}

void WorldRestoreTask::executeTask()
{
    m_target = FS::PathCombine(m_instance->worldDir(), m_backup.subject);
    if (QFileInfo(m_target).isDir())
        runChild(WorldBackupTask::create(m_instance, m_target, Reason::BeforeRestore), &WorldRestoreTask::extract);
    else
        extract();
}

void WorldRestoreTask::runChild(Task::Ptr task, void (WorldRestoreTask::*next)())
{
    m_child = task;
    propagateFromOther(task.get());
    connect(task.get(), &Task::succeeded, this, next);
    connect(task.get(), &Task::failed, this, &WorldRestoreTask::emitFailed);
    connect(task.get(), &Task::aborted, this, &WorldRestoreTask::emitAborted);
    task->start();
}

bool WorldRestoreTask::abort()
{
    return m_child && m_child->canAbort() && m_child->abort();
}

void WorldRestoreTask::extract()
{
    const World saved{ QFileInfo(m_backup.path) };
    if (!saved.isValid()) {
        emitFailed(tr("%1 does not contain a Minecraft world.").arg(QFileInfo(m_backup.path).fileName()));
        return;
    }
    // unpacked next to the backups first, so a failed restore leaves the world as it was
    m_staging = FS::PathCombine(rootDir(m_instance), ".staging", QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!FS::ensureFolderPathExists(m_staging)) {
        emitFailed(tr("Could not create %1").arg(m_staging));
        return;
    }
    runChild(makeShared<MMCZip::ExtractZipTask>(m_backup.path, QDir(m_staging), saved.containerOffsetPath()), &WorldRestoreTask::swap);
}

void WorldRestoreTask::swap()
{
    const QString stagingRoot = QFileInfo(m_staging).absolutePath();
    auto cleanUp = [&] {
        FS::deletePath(m_staging);
        QDir().rmdir(stagingRoot);
    };

    QDir dir;
    QString replaced;
    if (QFileInfo::exists(m_target)) {
        replaced = m_staging + "-replaced";
        if (!dir.rename(m_target, replaced)) {
            cleanUp();
            emitFailed(tr("Could not move the current world out of the way. Close the game and try again."));
            return;
        }
    }
    if (!dir.rename(m_staging, m_target)) {
        if (!replaced.isEmpty())
            dir.rename(replaced, m_target);
        cleanUp();
        emitFailed(tr("Could not put the restored world in place."));
        return;
    }
    if (!replaced.isEmpty())
        FS::deletePath(replaced);
    cleanUp();
    emitSucceeded();
}

// ---- RestorePointTask ----

RestorePointTask::RestorePointTask(MinecraftInstance* instance, Scope scope, Reason reason, QString description)
    : m_instance(instance), m_scope(scope), m_reason(reason), m_description(std::move(description))
{
    setAbortable(true);
}

RestorePointTask::~RestorePointTask()
{
    // the worker uses this object until it notices the cancellation
    m_cancel = true;
    m_future.waitForFinished();
}

void RestorePointTask::executeTask()
{
    m_outputPath = newBackupPath(FS::PathCombine(rootDir(m_instance), "restore-points"), m_reason, QString());
    setStatus(tr("Saving a restore point (%1)").arg(scopeText(m_scope)));
    m_future = QtConcurrent::run(QThreadPool::globalInstance(), [this] {
        return createRestorePoint(m_instance, m_scope, m_reason, m_description, m_outputPath, [this](int done, int total, const QString&) {
            if (m_cancel)
                return false;
            setProgress(done, total);
            return true;
        });
    });
    connect(&m_watcher, &QFutureWatcher<Result>::finished, this, &RestorePointTask::finish);
    m_watcher.setFuture(m_future);
}

bool RestorePointTask::abort()
{
    m_cancel = true;
    return true;
}

void RestorePointTask::finish()
{
    if (m_cancel) {
        FS::deletePath(m_outputPath);
        emitAborted();
    } else if (auto error = m_future.result()) {
        emitFailed(*error);
    } else {
        if (m_reason != Reason::Manual)
            applyRetention(m_instance, m_outputPath);
        emitSucceeded();
    }
}

// ---- RestorePointRestoreTask ----

RestorePointRestoreTask::RestorePointRestoreTask(MinecraftInstance* instance, Entry point) : m_instance(instance), m_point(std::move(point))
{}

RestorePointRestoreTask::~RestorePointRestoreTask()
{
    m_future.waitForFinished();
}

void RestorePointRestoreTask::executeTask()
{
    m_safetyPath = newBackupPath(FS::PathCombine(rootDir(m_instance), "restore-points"), Reason::BeforeRestore, QString());
    const QString safetyDescription =
        tr("Before restoring the restore point from %1").arg(QLocale().toString(m_point.created, QLocale::ShortFormat));

    m_future = QtConcurrent::run(QThreadPool::globalInstance(), [this, safetyDescription]() -> Result {
        const auto report = [this](int done, int total, const QString&) {
            setProgress(done, total);
            return true;
        };
        setStatus(tr("Saving the current files first"));
        if (auto error = createRestorePoint(m_instance, m_point.scope, Reason::BeforeRestore, safetyDescription, m_safetyPath, report))
            return error;
        setStatus(tr("Restoring %1").arg(scopeText(m_point.scope)));
        if (auto error = restoreFiles(m_instance, m_point, report))
            return tr("%1\nThe files from before the restore are in a new restore point.").arg(*error);
        return std::nullopt;
    });
    connect(&m_watcher, &QFutureWatcher<Result>::finished, this, &RestorePointRestoreTask::finish);
    m_watcher.setFuture(m_future);
}

void RestorePointRestoreTask::finish()
{
    if (m_point.scope == Scope::InstanceFiles)
        restoreManagedPack(m_instance, FS::PathCombine(m_point.path, "instance.cfg"));
    // pick up the restored loader version and mods
    m_instance->getPackProfile()->reload(Net::Mode::Offline);
    m_instance->loaderModList()->update();
    applyRetention(m_instance, m_point.path);

    if (auto error = m_future.result())
        emitFailed(*error);
    else
        emitSucceeded();
}

}  // namespace Backups
