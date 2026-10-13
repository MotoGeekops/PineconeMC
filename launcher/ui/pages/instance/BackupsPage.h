// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QMainWindow>

#include <optional>

#include "backup/Backups.h"
#include "settings/Setting.h"
#include "ui/pages/BasePage.h"

class MinecraftInstance;
class QLabel;
class QSortFilterProxyModel;
class QStandardItemModel;
class QTreeView;
class WideBar;

/// Instance page listing the world backups and restore points, with restore and delete.
class BackupsPage : public QMainWindow, public BasePage {
    Q_OBJECT

   public:
    explicit BackupsPage(MinecraftInstance* instance, QWidget* parent = nullptr);

    QString displayName() const override { return tr("Backups"); }
    QIcon icon() const override { return QIcon::fromTheme("backups"); }
    QString id() const override { return "backups"; }
    void openedImpl() override;
    void closedImpl() override;
    void retranslate() override;

    /**
     * Before mods or a modpack get updated: saves a restore point and backs up the worlds played since their last backup,
     * unless that is turned off in the settings. Returns false when the update should not go ahead.
     */
    static bool backUpBeforeUpdate(QWidget* parent, MinecraftInstance* instance, Backups::Scope scope, const QString& description);

   protected:
    QMenu* createPopupMenu() override;

   private:
    void createActions();
    void refresh();
    void updateActions();
    std::optional<Backups::Entry> selectedEntry() const;
    QString worldName(const QString& folder) const;
    bool refuseWhileRunning(const QString& title);
    bool runTask(Task::Ptr task, const QString& failTitle);

    void backUpWorld();
    void createRestorePoint();
    void restoreSelected();
    void deleteSelected();

    MinecraftInstance* m_instance;
    WideBar* m_toolBar;
    QLabel* m_hint;
    QTreeView* m_view;
    QStandardItemModel* m_model;
    QSortFilterProxyModel* m_proxy;
    QList<Backups::Entry> m_entries;

    QAction* m_backUpWorld;
    QAction* m_createRestorePoint;
    QAction* m_restore;
    QAction* m_delete;
    QAction* m_viewFolder;
    QAction* m_refresh;

    std::shared_ptr<Setting> m_wideBarSetting;
};
