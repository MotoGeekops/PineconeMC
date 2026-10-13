// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QDialog>
#include <QPointer>

#include <functional>
#include <memory>
#include <vector>

#include "BaseInstance.h"
#include "tasks/Task.h"

class QLabel;
class QPushButton;
class QVBoxLayout;

/// Help > Something's Wrong?: checks Java, memory, disk space, folders, the account, graphics and the servers the launcher needs.
class TroubleshooterDialog : public QDialog {
    Q_OBJECT
   public:
    explicit TroubleshooterDialog(BaseInstance* instance, QWidget* parent = nullptr);
    ~TroubleshooterDialog() override;

   private:
    enum class Status { Checking, Good, Warning, Problem };
    struct Row {
        QLabel* icon = nullptr;
        QLabel* title = nullptr;
        QLabel* detail = nullptr;
        QPushButton* fix = nullptr;
        Status status = Status::Checking;
        std::function<void()> fixAction;
    };

    Row* addRow(const QString& title);
    void setResult(Row* row, Status status, const QString& detail, const QString& fixLabel = {}, std::function<void()> fixAction = {});
    void runChecks();
    void updateSummary();
    QString report() const;

    void checkJava();
    void checkMemory();
    void checkDisk();
    void checkFolders();
    void checkAccount();
    void checkGraphics();
    void checkConnection(const QString& title, const QString& url, const QString& purpose);

    QPointer<BaseInstance> m_instance;
    QLabel* m_summary = nullptr;
    QWidget* m_rowsWidget = nullptr;
    QVBoxLayout* m_rowsLayout = nullptr;
    std::vector<std::unique_ptr<Row>> m_rows;
    std::vector<Task::Ptr> m_tasks;
    // results of an earlier run that arrive late are ignored
    int m_generation = 0;
};
