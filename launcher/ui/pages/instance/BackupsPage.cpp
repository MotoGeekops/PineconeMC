// SPDX-License-Identifier: GPL-3.0-only
#include "BackupsPage.h"

#include <QCursor>
#include <QDir>
#include <QHash>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QTreeView>
#include <QVBoxLayout>

#include "Application.h"
#include "DesktopServices.h"
#include "FileSystem.h"
#include "StringUtils.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/World.h"
#include "tasks/SequentialTask.h"
#include "ui/dialogs/CustomMessageBox.h"
#include "ui/dialogs/ProgressDialog.h"
#include "ui/widgets/WideBar.h"

namespace {
enum Roles { EntryIndexRole = Qt::UserRole, SortRole };
enum Columns { NameColumn, ContentsColumn, ReasonColumn, DateColumn, SizeColumn };
}  // namespace

BackupsPage::BackupsPage(MinecraftInstance* instance, QWidget* parent) : QMainWindow(parent), m_instance(instance)
{
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);

    m_hint = new QLabel(central);
    m_hint->setObjectName("backupsHint");
    m_hint->setWordWrap(true);
    m_hint->setContentsMargins(2, 0, 2, 4);
    layout->addWidget(m_hint);

    m_model = new QStandardItemModel(this);
    m_proxy = new QSortFilterProxyModel(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setSortRole(SortRole);
    m_proxy->setSortCaseSensitivity(Qt::CaseInsensitive);

    m_view = new QTreeView(central);
    m_view->setModel(m_proxy);
    m_view->setRootIsDecorated(false);
    m_view->setItemsExpandable(false);
    m_view->setAlternatingRowColors(true);
    m_view->setUniformRowHeights(true);
    m_view->setAllColumnsShowFocus(true);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setIconSize(QSize(24, 24));
    m_view->setSortingEnabled(true);
    m_view->sortByColumn(DateColumn, Qt::DescendingOrder);
    m_view->header()->setStretchLastSection(false);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_view);
    setCentralWidget(central);

    m_toolBar = new WideBar(tr("Actions"), this);
    m_toolBar->setObjectName("toolBar");
    m_toolBar->setAllowedAreas(Qt::LeftToolBarArea | Qt::RightToolBarArea);
    m_toolBar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    addToolBar(Qt::RightToolBarArea, m_toolBar);
    createActions();

    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this, &BackupsPage::updateActions);
    connect(m_view, &QTreeView::customContextMenuRequested, this, [this](const QPoint& pos) {
        auto* menu = m_toolBar->createContextMenu(this, tr("Context menu"));
        menu->exec(m_view->viewport()->mapToGlobal(pos));
        delete menu;
    });

    retranslate();
}

void BackupsPage::createActions()
{
    m_backUpWorld = new QAction(this);
    m_backUpWorld->setObjectName("actionBackUpWorld");
    connect(m_backUpWorld, &QAction::triggered, this, &BackupsPage::backUpWorld);

    m_createRestorePoint = new QAction(this);
    m_createRestorePoint->setObjectName("actionCreateRestorePoint");
    connect(m_createRestorePoint, &QAction::triggered, this, &BackupsPage::createRestorePoint);

    m_restore = new QAction(this);
    m_restore->setObjectName("actionRestore");
    connect(m_restore, &QAction::triggered, this, &BackupsPage::restoreSelected);

    m_delete = new QAction(this);
    m_delete->setObjectName("actionDeleteBackup");
    connect(m_delete, &QAction::triggered, this, &BackupsPage::deleteSelected);

    m_viewFolder = new QAction(this);
    m_viewFolder->setObjectName("actionViewBackupsFolder");
    connect(m_viewFolder, &QAction::triggered, this, [this] {
        const QString dir = Backups::rootDir(m_instance);
        FS::ensureFolderPathExists(dir);
        DesktopServices::openPath(dir, true);
    });

    m_refresh = new QAction(this);
    m_refresh->setObjectName("actionRefreshBackups");
    connect(m_refresh, &QAction::triggered, this, &BackupsPage::refresh);

    m_toolBar->addAction(m_backUpWorld);
    m_toolBar->addAction(m_createRestorePoint);
    m_toolBar->addSeparator();
    m_toolBar->addAction(m_restore);
    m_toolBar->addAction(m_delete);
    m_toolBar->addSeparator();
    m_toolBar->addAction(m_refresh);
    m_toolBar->addAction(m_viewFolder);
    m_toolBar->insertSpacer(m_refresh);
}

void BackupsPage::retranslate()
{
    m_toolBar->setWindowTitle(tr("Actions"));
    m_backUpWorld->setText(tr("Back Up World"));
    m_backUpWorld->setToolTip(tr("Save a copy of a world that you can restore later."));
    m_createRestorePoint->setText(tr("Create Restore Point"));
    m_createRestorePoint->setToolTip(tr("Save a copy of the mods, configs and loader version. Worlds are not included."));
    m_restore->setText(tr("Restore"));
    m_restore->setToolTip(tr("Go back to the selected backup. What is there now is saved first."));
    m_delete->setText(tr("Delete"));
    m_delete->setToolTip(tr("Delete the selected backup."));
    m_refresh->setText(tr("Refresh"));
    m_viewFolder->setText(tr("View Folder"));
    m_hint->setText(tr("Restore points keep the mods and configs, and are made automatically before mod and modpack updates. "
                       "World backups are zip files that also work with \"Add\" on the Worlds page. "
                       "Restoring saves the current state first, so you can always go back."));
    refresh();
}

void BackupsPage::openedImpl()
{
    m_wideBarSetting = APPLICATION->settings()->getOrRegisterSetting(QString("WideBarVisibility_%1").arg(id()));
    m_toolBar->setVisibilityState(QByteArray::fromBase64(m_wideBarSetting->get().toString().toUtf8()));
    refresh();
}

void BackupsPage::closedImpl()
{
    if (m_wideBarSetting)
        m_wideBarSetting->set(QString::fromUtf8(m_toolBar->getVisibilityState().toBase64()));
}

QMenu* BackupsPage::createPopupMenu()
{
    QMenu* filteredMenu = QMainWindow::createPopupMenu();
    filteredMenu->removeAction(m_toolBar->toggleViewAction());
    return filteredMenu;
}

QString BackupsPage::worldName(const QString& folder) const
{
    const World world{ QFileInfo(FS::PathCombine(m_instance->worldDir(), folder)) };
    return world.isValid() ? world.name() : folder;
}

void BackupsPage::refresh()
{
    const auto previous = selectedEntry();
    m_entries = Backups::list(m_instance);

    m_model->clear();
    m_model->setHorizontalHeaderLabels({ tr("Name"), tr("Contains"), tr("Reason"), tr("Date"), tr("Size") });

    QHash<QString, QString> worldNames;
    const QIcon restorePointIcon = QIcon::fromTheme("loadermods");
    const QIcon worldIcon = QIcon::fromTheme("worlds");
    for (int i = 0; i < m_entries.size(); ++i) {
        const auto& entry = m_entries[i];
        const bool isWorld = entry.kind == Backups::Kind::World;

        QString name;
        QIcon icon = restorePointIcon;
        if (isWorld) {
            if (!worldNames.contains(entry.subject))
                worldNames.insert(entry.subject, worldName(entry.subject));
            name = worldNames.value(entry.subject);
            const QString iconFile = FS::PathCombine(m_instance->worldDir(), entry.subject, "icon.png");
            icon = QFileInfo::exists(iconFile) ? QIcon(iconFile) : worldIcon;
        } else {
            name = entry.description.isEmpty() ? tr("Restore point") : entry.description;
        }

        auto* nameItem = new QStandardItem(icon, name);
        nameItem->setData(i, EntryIndexRole);
        nameItem->setData(name.toLower(), SortRole);
        nameItem->setToolTip(QDir::toNativeSeparators(entry.path));

        const QString contents = isWorld ? tr("World") : Backups::scopeText(entry.scope);
        auto* contentsItem = new QStandardItem(contents);
        contentsItem->setData(contents, SortRole);

        const QString reason = Backups::reasonText(entry.reason);
        auto* reasonItem = new QStandardItem(reason);
        reasonItem->setData(reason, SortRole);

        auto* dateItem = new QStandardItem(QLocale().toString(entry.created, QLocale::ShortFormat));
        dateItem->setData(entry.created.toMSecsSinceEpoch(), SortRole);

        QString size = StringUtils::humanReadableFileSize(entry.size, true);
        if (!isWorld)
            size = entry.fileCount == 1 ? tr("%1 in 1 file").arg(size) : tr("%1 in %2 files").arg(size).arg(entry.fileCount);
        auto* sizeItem = new QStandardItem(size);
        sizeItem->setData(entry.size, SortRole);

        m_model->appendRow({ nameItem, contentsItem, reasonItem, dateItem, sizeItem });
    }

    auto* header = m_view->header();
    header->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
    for (int column : { ContentsColumn, ReasonColumn, DateColumn, SizeColumn })
        header->setSectionResizeMode(column, QHeaderView::ResizeToContents);

    if (previous) {
        for (int row = 0; row < m_proxy->rowCount(); ++row) {
            const auto index = m_proxy->index(row, NameColumn);
            if (m_entries.value(index.data(EntryIndexRole).toInt()).path == previous->path) {
                m_view->setCurrentIndex(index);
                break;
            }
        }
    }
    updateActions();
}

std::optional<Backups::Entry> BackupsPage::selectedEntry() const
{
    if (!m_view || !m_view->selectionModel())
        return std::nullopt;
    const auto rows = m_view->selectionModel()->selectedRows(NameColumn);
    if (rows.isEmpty())
        return std::nullopt;
    const int index = rows.first().data(EntryIndexRole).toInt();
    if (index < 0 || index >= m_entries.size())
        return std::nullopt;
    return m_entries[index];
}

void BackupsPage::updateActions()
{
    const bool selected = selectedEntry().has_value();
    m_restore->setEnabled(selected);
    m_delete->setEnabled(selected);
}

bool BackupsPage::refuseWhileRunning(const QString& title)
{
    if (!m_instance->isRunning())
        return false;
    QMessageBox::warning(this, title, tr("Close the game first: files that are in use can't be replaced."));
    return true;
}

bool BackupsPage::runTask(Task::Ptr task, const QString& failTitle)
{
    ProgressDialog dialog(this);
    dialog.setSkipButton(task->canAbort(), tr("Abort"));
    dialog.execWithTask(task.get());
    refresh();
    if (task->getState() == Task::State::Failed) {
        CustomMessageBox::selectable(this, failTitle, task->failReason(), QMessageBox::Critical)->exec();
        return false;
    }
    return task->wasSuccessful();
}

void BackupsPage::backUpWorld()
{
    QList<QPair<QString, QString>> worlds;  // name, folder path
    for (const auto& folder : QDir(m_instance->worldDir()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const World world(folder);
        if (world.isValid())
            worlds.append({ world.name(), folder.absoluteFilePath() });
    }
    if (worlds.isEmpty()) {
        QMessageBox::information(this, tr("Back Up World"), tr("This instance has no worlds yet."));
        return;
    }

    QStringList paths;
    if (worlds.size() == 1) {
        paths << worlds.first().second;
    } else {
        QMenu menu(this);
        for (const auto& [name, path] : worlds)
            menu.addAction(name)->setData(path);
        menu.addSeparator();
        auto* all = menu.addAction(tr("All Worlds"));
        // below the toolbar button when there is one (the action can also come from the context menu)
        QPoint where = QCursor::pos();
        if (auto* button = m_toolBar->widgetForAction(m_backUpWorld); button && button->isVisible() && button->underMouse())
            where = button->mapToGlobal(QPoint(0, button->height()));
        auto* chosen = menu.exec(where);
        if (!chosen)
            return;
        if (chosen == all) {
            for (const auto& world : worlds)
                paths << world.second;
        } else {
            paths << chosen->data().toString();
        }
    }

    if (m_instance->isRunning() &&
        QMessageBox::question(this, tr("Back Up World"),
                              tr("The game is running. A world that is open in the game might not be saved completely.\n"
                                 "Back up anyway?")) != QMessageBox::Yes) {
        return;
    }

    auto task = makeShared<SequentialTask>(tr("Backing up worlds"));
    task->setAbortable(true);
    for (const auto& path : paths)
        task->addTask(Backups::WorldBackupTask::create(m_instance, path, Backups::Reason::Manual));
    runTask(task, tr("Backup failed"));
}

void BackupsPage::createRestorePoint()
{
    bool ok = false;
    const QString note = QInputDialog::getText(this, tr("Create Restore Point"),
                                               tr("A restore point keeps a copy of the mods, configs and loader version. "
                                                  "Worlds are not included.\n\nNote (optional):"),
                                               QLineEdit::Normal, QString(), &ok)
                             .trimmed();
    if (!ok)
        return;
    runTask(makeShared<Backups::RestorePointTask>(m_instance, Backups::Scope::InstanceFiles, Backups::Reason::Manual,
                                                  note.isEmpty() ? tr("Restore point") : note),
            tr("Restore point failed"));
}

void BackupsPage::restoreSelected()
{
    const auto entry = selectedEntry();
    if (!entry || refuseWhileRunning(tr("Restore")))
        return;

    const QString date = QLocale().toString(entry->created, QLocale::ShortFormat);
    const bool isWorld = entry->kind == Backups::Kind::World;
    const QString question =
        isWorld ? tr("Restore the world \"%1\" to how it was on %2?\n\n"
                     "The world as it is now is backed up first, so you can undo this here.")
                      .arg(worldName(entry->subject), date)
                : tr("Put the files back to how they were on %1 (%2)?\n\n"
                     "Files changed since then are replaced and files added since are removed. "
                     "The current files are saved in a new restore point first, so you can undo this here.")
                      .arg(date, Backups::scopeText(entry->scope));
    if (CustomMessageBox::selectable(this, tr("Restore"), question, QMessageBox::Question, QMessageBox::Yes | QMessageBox::No,
                                     QMessageBox::No)
            ->exec() != QMessageBox::Yes) {
        return;
    }

    Task::Ptr task;
    if (isWorld)
        task = makeShared<Backups::WorldRestoreTask>(m_instance, *entry);
    else
        task = makeShared<Backups::RestorePointRestoreTask>(m_instance, *entry);
    if (runTask(task, tr("Restore failed")))
        QMessageBox::information(this, tr("Restore"), tr("Restored the backup from %1.").arg(date));
}

void BackupsPage::deleteSelected()
{
    const auto entry = selectedEntry();
    if (!entry)
        return;
    if (CustomMessageBox::selectable(this, tr("Delete Backup"), tr("Delete the backup from %1?\nThis can't be undone.")
                                         .arg(QLocale().toString(entry->created, QLocale::ShortFormat)),
                                     QMessageBox::Warning, QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            ->exec() != QMessageBox::Yes) {
        return;
    }
    if (!Backups::remove(*entry))
        QMessageBox::warning(this, tr("Delete Backup"), tr("Could not delete %1.").arg(QDir::toNativeSeparators(entry->path)));
    refresh();
}

bool BackupsPage::backUpBeforeUpdate(QWidget* parent, MinecraftInstance* instance, Backups::Scope scope, const QString& description)
{
    if (!instance || !APPLICATION->settings()->get("BackupBeforeUpdates").toBool())
        return true;

    const auto reason = scope == Backups::Scope::Mods ? Backups::Reason::BeforeUpdate : Backups::Reason::BeforeModpackUpdate;
    auto task = makeShared<SequentialTask>(tr("Backing up before the update"));
    task->setAbortable(true);
    task->addTask(makeShared<Backups::RestorePointTask>(instance, scope, reason, description));
    for (const auto& world : Backups::worldsChangedSinceBackup(instance))
        task->addTask(Backups::WorldBackupTask::create(instance, world, reason));

    ProgressDialog dialog(parent);
    dialog.setSkipButton(true, tr("Skip Backup"));
    dialog.execWithTask(task.get());

    // done, or skipped on purpose
    if (task->getState() != Task::State::Failed)
        return true;

    return CustomMessageBox::selectable(parent, tr("Backup failed"),
                                        tr("Backing up before the update failed:\n%1\n\nUpdate anyway?").arg(task->failReason()),
                                        QMessageBox::Warning, QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
               ->exec() == QMessageBox::Yes;
}
